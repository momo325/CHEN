#ifndef USER_AUDIO_CTRL_H
#define USER_AUDIO_CTRL_H

#include "cpu.h"

typedef enum {
    USER_STATE_STANDBY = 0,
    USER_STATE_BT_ONLY,
    USER_STATE_OTG_DUPLEX,
    USER_STATE_PARALLEL,
} user_work_state_t;

void user_audio_ctrl_board_init(void);
void user_audio_ctrl_start(void);
void user_bt_connection_update(u8 connected);
void user_usb_slave_online(u8 online);
void user_uac_tx_activity(void);
void user_pcspk_volume_update(void);
u8 user_bt_is_connected(void);
user_work_state_t user_audio_ctrl_get_state(void);

#endif
