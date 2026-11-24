#ifdef SUPPORT_MS_EXTENSIONS
#pragma bss_seg(".user_audio_ctrl.data.bss")
#pragma data_seg(".user_audio_ctrl.data")
#pragma const_seg(".user_audio_ctrl.text.const")
#pragma code_seg(".user_audio_ctrl.text")
#endif

#include "app_config.h"
#include "system/includes.h"
#include "gpadc.h"
#include "gpio.h"
#include "jlstream.h"
#include "volume_node.h"
#include "effects/audio_autoduck.h"
#include "btstack/avctp_user.h"
#include "bt_event_func.h"
#include "pc_spk_player.h"
#include "uac_stream.h"
#include "user_audio_ctrl.h"

#define USER_WORK_LED_PIN          IO_PORTC_03
#define USER_BT_LED_PIN            IO_PORTA_06
#define USER_DUCK_LED_PIN          IO_PORTA_05
#define USER_OTG_LED_PIN           IO_PORTA_04
#define USER_DUCK_KEY_PIN          IO_PORTC_01
#define USER_VBUS_DET_PIN          IO_PORTA_02

#define USER_SCAN_PERIOD_MS        20
#define USER_ADC_PERIOD_TICKS      3
#define USER_BT_IDLE_TIMEOUT_MS    60000
#define USER_ADC_HYSTERESIS        12

struct user_volume_channel {
    u32 adc_ch;
    u32 filtered;
    s32 last_raw;
    u8 started;
};

struct user_audio_ctrl {
    u8 bt_connected;
    u8 bt_closed_by_timeout;
    u8 vbus_online;
    u8 usb_host_online;
    u8 duck_enable;
    u8 key_pressed;
    u8 key_debounce;
    u8 scan_tick;
    u8 force_volume_tick;
    u8 duck_sync_tick;
    u16 bt_timeout_timer;
    u16 scan_timer;
    u32 uac_tx_last_ms;
    user_work_state_t state;
    struct user_volume_channel mic;
    struct user_volume_channel music;
    struct user_volume_channel otg;
};

static struct user_audio_ctrl g_user_ctrl;

static void user_state_refresh(void)
{
    if (g_user_ctrl.usb_host_online) {
        g_user_ctrl.state = g_user_ctrl.bt_connected ? USER_STATE_PARALLEL : USER_STATE_OTG_DUPLEX;
    } else {
        g_user_ctrl.state = g_user_ctrl.bt_connected ? USER_STATE_BT_ONLY : USER_STATE_STANDBY;
    }
}

static int user_volume_set(const char *name, u32 raw)
{
    struct volume_cfg cfg = {0};
    u16 max_level = volume_ioc_get_max_level(name);
    u32 level;

    if (max_level == 0) {
        max_level = 100;
    }
    if (raw <= 25) {
        level = 0;
    } else if (raw >= 995) {
        level = max_level;
    } else {
        level = (raw - 25) * max_level / (995 - 25);
    }
    cfg.bypass = VOLUME_NODE_CMD_SET_VOL;
    cfg.cur_vol = level;
    return jlstream_set_node_param(NODE_UUID_VOLUME_CTRLER, name, &cfg, sizeof(cfg));
}

static void user_volume_update_all(void)
{
    user_volume_set("VolEff", g_user_ctrl.mic.filtered);
    user_volume_set("Vol_BtmMusic", g_user_ctrl.music.filtered);
    user_volume_set("Vol_LinMusic", g_user_ctrl.music.filtered);
    user_volume_set("Vol_PcspkMusic", g_user_ctrl.music.filtered);
    user_volume_set("Vol_PcMic", g_user_ctrl.otg.filtered);
}

static void user_duck_response_update(const char *name)
{
    autoduck_param_tool_set duck = {0};
    u8 bypass = !g_user_ctrl.duck_enable;

    if (!jlstream_get_node_param(NODE_UUID_AUTODUCK, name, &duck, sizeof(duck)) &&
        duck.is_bypass != bypass) {
        duck.is_bypass = bypass;
        jlstream_set_node_param(NODE_UUID_AUTODUCK, name, &duck, sizeof(duck));
    }
}

static void user_duck_update(void)
{
    static const char *const response_nodes[] = {
        "AutoDuckBt",
        "AutoDuckLinein",
        "AutoDuckPcSpk",
    };
    autoduck_trigger_param_tool_set trigger = {0};
    u8 bypass = !g_user_ctrl.duck_enable;

    for (u8 i = 0; i < ARRAY_SIZE(response_nodes); i++) {
        user_duck_response_update(response_nodes[i]);
    }
    if (!jlstream_get_node_param(NODE_UUID_AUTODUCK_TRIGGER, "AutoDuckTrigger", &trigger, sizeof(trigger)) &&
        trigger.is_bypass != bypass) {
        trigger.is_bypass = bypass;
        jlstream_set_node_param(NODE_UUID_AUTODUCK_TRIGGER, "AutoDuckTrigger", &trigger, sizeof(trigger));
    }
}

static u8 user_adc_sample(struct user_volume_channel *channel)
{
    u32 raw = adc_get_value(channel->adc_ch);
    u32 delta;

    if (!channel->started) {
        channel->filtered = raw;
        channel->last_raw = raw;
        channel->started = 1;
        return 1;
    }

    channel->filtered = (channel->filtered * 3 + raw) / 4;
    delta = (channel->filtered > channel->last_raw) ?
            channel->filtered - channel->last_raw :
            channel->last_raw - channel->filtered;
    if (delta < USER_ADC_HYSTERESIS) {
        return 0;
    }

    channel->last_raw = channel->filtered;
    return 1;
}

static void user_usb_online_task(void)
{
    user_state_refresh();
}

static int user_bt_idle_close_task(void)
{
    if (!g_user_ctrl.bt_connected && !bt_get_total_connect_dev()) {
        g_user_ctrl.bt_closed_by_timeout = 1;
        bt_close_discoverable_and_connectable();
        puts("user: bluetooth idle 60s, scan disabled\n");
    }
    return 0;
}

static void user_bt_idle_timeout(void *priv)
{
    g_user_ctrl.bt_timeout_timer = 0;
    os_taskq_post_type("app_core", Q_CALLBACK, 1, (int[]){(int)user_bt_idle_close_task});
}

static void user_bt_idle_timer_start(void)
{
    if (g_user_ctrl.bt_timeout_timer) {
        sys_timeout_del(g_user_ctrl.bt_timeout_timer);
    }
    g_user_ctrl.bt_timeout_timer = sys_timeout_add(NULL, user_bt_idle_timeout, USER_BT_IDLE_TIMEOUT_MS);
}

static void user_scan_timer(void *priv)
{
    u32 now = jiffies_msec();
    u8 pressed = !gpio_read(USER_DUCK_KEY_PIN);

    g_user_ctrl.vbus_online = !!gpio_read(USER_VBUS_DET_PIN);

    if (pressed != g_user_ctrl.key_pressed) {
        if (++g_user_ctrl.key_debounce >= 2) {
            g_user_ctrl.key_pressed = pressed;
            g_user_ctrl.key_debounce = 0;
            if (pressed) {
                g_user_ctrl.duck_enable = !g_user_ctrl.duck_enable;
                user_duck_update();
            }
        }
    } else {
        g_user_ctrl.key_debounce = 0;
    }
    gpio_write(USER_DUCK_LED_PIN, g_user_ctrl.duck_enable);

    if (++g_user_ctrl.duck_sync_tick >= 50) {
        g_user_ctrl.duck_sync_tick = 0;
        user_duck_update();
    }

    if (g_user_ctrl.bt_closed_by_timeout) {
        gpio_write(USER_BT_LED_PIN, 0);
    } else if (g_user_ctrl.bt_connected) {
        gpio_write(USER_BT_LED_PIN, 1);
    } else {
        gpio_write(USER_BT_LED_PIN, (now % 1300) < 1000);
    }

    if (!g_user_ctrl.usb_host_online) {
        gpio_write(USER_OTG_LED_PIN, 0);
    } else if (g_user_ctrl.uac_tx_last_ms && (now - g_user_ctrl.uac_tx_last_ms) < 200) {
        gpio_write(USER_OTG_LED_PIN, (now / 100) & 1);
    } else {
        gpio_write(USER_OTG_LED_PIN, 1);
    }

    if (++g_user_ctrl.scan_tick >= USER_ADC_PERIOD_TICKS) {
        u8 volume_changed = 0;

        g_user_ctrl.scan_tick = 0;
        volume_changed |= user_adc_sample(&g_user_ctrl.mic);
        volume_changed |= user_adc_sample(&g_user_ctrl.music);
        volume_changed |= user_adc_sample(&g_user_ctrl.otg);
        if (volume_changed || ++g_user_ctrl.force_volume_tick >= 50) {
            g_user_ctrl.force_volume_tick = 0;
            user_volume_update_all();
        }
    }
}

/* board_init 阶段只做引脚初始电平与输入/高阻配置 */
void user_audio_ctrl_board_init(void)
{
    memset(&g_user_ctrl, 0, sizeof(g_user_ctrl));
    g_user_ctrl.state = USER_STATE_STANDBY;

    gpio_set_mode(IO_PORT_SPILT(USER_WORK_LED_PIN), PORT_OUTPUT_HIGH);
    gpio_set_mode(IO_PORT_SPILT(USER_BT_LED_PIN), PORT_OUTPUT_LOW);
    gpio_set_mode(IO_PORT_SPILT(USER_DUCK_LED_PIN), PORT_OUTPUT_LOW);
    gpio_set_mode(IO_PORT_SPILT(USER_OTG_LED_PIN), PORT_OUTPUT_LOW);
    gpio_set_mode(IO_PORT_SPILT(USER_DUCK_KEY_PIN), PORT_INPUT_PULLUP_10K);
    /* 外部 100K/100K 分压，内部禁止上下拉，否则分压点被拉低 */
    gpio_set_mode(IO_PORT_SPILT(USER_VBUS_DET_PIN), PORT_INPUT_FLOATING);

    /* 电位器分压输入：高阻，避免数字输入/上下拉拉偏 10K 电位器 */
    gpio_set_mode(IO_PORT_SPILT(IO_PORTC_05), PORT_HIGHZ);
    gpio_set_mode(IO_PORT_SPILT(IO_PORTC_04), PORT_HIGHZ);
    gpio_set_mode(IO_PORT_SPILT(IO_PORTG_01), PORT_HIGHZ);

    g_user_ctrl.mic.adc_ch = adc_io2ch(IO_PORTC_05);
    g_user_ctrl.music.adc_ch = adc_io2ch(IO_PORTC_04);
    g_user_ctrl.otg.adc_ch = adc_io2ch(IO_PORTG_01);
    printf("user: adc ch mic=%d music=%d otg=%d\n",
           (int)g_user_ctrl.mic.adc_ch, (int)g_user_ctrl.music.adc_ch, (int)g_user_ctrl.otg.adc_ch);
    adc_add_sample_ch(g_user_ctrl.mic.adc_ch);
    adc_add_sample_ch(g_user_ctrl.music.adc_ch);
    adc_add_sample_ch(g_user_ctrl.otg.adc_ch);
}

/* 常驻 mic_effect / linein 流打开后再启动扫描与节点参数下发 */
void user_audio_ctrl_start(void)
{
    if (g_user_ctrl.scan_timer) {
        return;
    }

    printf("user: vol max eff=%d bt=%d lin=%d pc=%d mic=%d\n",
           volume_ioc_get_max_level("VolEff"),
           volume_ioc_get_max_level("Vol_BtmMusic"),
           volume_ioc_get_max_level("Vol_LinMusic"),
           volume_ioc_get_max_level("Vol_PcspkMusic"),
           volume_ioc_get_max_level("Vol_PcMic"));
    user_volume_update_all();
    user_duck_update();
    g_user_ctrl.scan_timer = sys_timer_add(NULL, user_scan_timer, USER_SCAN_PERIOD_MS);
}

void user_bt_connection_update(u8 connected)
{
    connected = !!connected;
    if (g_user_ctrl.bt_connected == connected && !g_user_ctrl.bt_closed_by_timeout) {
        if (!connected && !g_user_ctrl.bt_timeout_timer) {
            user_bt_idle_timer_start();
        }
        return;
    }
    g_user_ctrl.bt_connected = connected;
    g_user_ctrl.bt_closed_by_timeout = 0;
    user_state_refresh();

    if (connected) {
        if (g_user_ctrl.bt_timeout_timer) {
            sys_timeout_del(g_user_ctrl.bt_timeout_timer);
            g_user_ctrl.bt_timeout_timer = 0;
        }
        pcspk_close_player_by_taskq();
    } else {
        user_bt_idle_timer_start();
        if (g_user_ctrl.usb_host_online && uac_speaker_stream_status()) {
            pcspk_open_player_by_taskq();
        }
    }
}

void user_usb_slave_online(u8 online)
{
    online = !!online;
    if (g_user_ctrl.usb_host_online == online) {
        return;
    }
    /* USB_REQ_SET_CONFIGURATION 可能在中断上下文调用，这里只更新标志，
       状态刷新抛到 app_core 任务上下文执行 */
    g_user_ctrl.usb_host_online = online;
    if (!online) {
        g_user_ctrl.uac_tx_last_ms = 0;
    }
    os_taskq_post_type("app_core", Q_CALLBACK, 1, (int[]){(int)user_usb_online_task});
}

void user_uac_tx_activity(void)
{
    g_user_ctrl.uac_tx_last_ms = jiffies_msec();
}

u8 user_bt_is_connected(void)
{
    return g_user_ctrl.bt_connected;
}

user_work_state_t user_audio_ctrl_get_state(void)
{
    return g_user_ctrl.state;
}
