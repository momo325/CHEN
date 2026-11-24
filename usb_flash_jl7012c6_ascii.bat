@echo off
setlocal EnableDelayedExpansion

rem ============================================================================
rem JL-7012C6 (BR28) USB Flash and Latest update.ufw Sync Script (Pure ASCII)
rem Place this file in the repo root (same folder as SDK and output)
rem ============================================================================

title JL-7012C6 USB Flash Tool

set "ROOT_DIR=%~dp0"
set "SDK_DIR=%ROOT_DIR%SDK"
set "OUT_DIR=%ROOT_DIR%output"
set "DL_DIR=%SDK_DIR%\cpu\br28\tools\download\soundbox"
set "SDK_CFG=%SDK_DIR%\apps\soundbox\board\br28\sdk_config.h"

echo ============================================================================
echo   JL-7012C6 (BR28) USB Flash Tool - Safety Check + Latest Firmware Sync
echo ============================================================================
echo [PATH] Repo Root: %ROOT_DIR%

if not exist "%SDK_DIR%\download.bat" (
    echo [ERROR] Cannot find "%SDK_DIR%\download.bat"
    echo         Please put this .bat file in the repo root next to SDK and output.
    pause
    exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

rem ----------------------------------------------------------------------------
rem 1. Safety Check: verify sdk_config.h was not overwritten by JLStudio
rem ----------------------------------------------------------------------------
echo.
echo [CHECK 1/3] Checking sdk_config.h VPWR debug UART setting...
findstr /C:"TCFG_DEBUG_UART_TX_PIN" "%SDK_CFG%" | findstr /C:"IO_PORT_LDOIN" >nul 2>&1
if errorlevel 1 (
    echo [WARN] TCFG_DEBUG_UART_TX_PIN is NOT IO_PORT_LDOIN!
    echo        JLStudio export may have overwritten sdk_config.h.
    echo        Recommended fix: git checkout -- SDK/apps/soundbox/board/br28/sdk_config.h
    set /p "CONT_CFG=Continue anyway? [Y/N]: "
    if /I not "!CONT_CFG!"=="Y" exit /b 1
) else (
    echo [PASS] sdk_config.h VPWR UART OK [IO_PORT_LDOIN].
)

rem ----------------------------------------------------------------------------
rem 2. Verify and sync latest stream.bin
rem ----------------------------------------------------------------------------
echo.
echo [CHECK 2/3] Checking and syncing latest stream.bin...
set "STREAM_OUT=%OUT_DIR%\stream.bin"
set "STREAM_DL=%DL_DIR%\stream.bin"

if exist "%STREAM_OUT%" (
    for %%F in ("%STREAM_OUT%") do echo   - output\stream.bin  : %%~tF  [%%~zF bytes]
) else (
    echo   - output\stream.bin  : NOT FOUND
)
if exist "%STREAM_DL%" (
    for %%F in ("%STREAM_DL%") do echo   - download\stream.bin: %%~tF  [%%~zF bytes]
) else (
    echo   - download\stream.bin: NOT FOUND
)

if exist "%STREAM_OUT%" (
    findstr /M "UserCoreMix" "%STREAM_OUT%" >nul 2>&1
    if errorlevel 1 (
        echo [WARN] UserCoreMix not found in output\stream.bin - may be old stream!
    ) else (
        echo [PASS] output\stream.bin contains new audio matrix [UserCoreMix].
    )
    copy /Y "%STREAM_OUT%" "%STREAM_DL%" >nul
)

rem ----------------------------------------------------------------------------
rem 3. Check latest update.ufw
rem ----------------------------------------------------------------------------
echo.
echo [CHECK 3/3] Checking latest update.ufw...
set "UFW_OUT=%OUT_DIR%\update.ufw"
set "UFW_DL=%DL_DIR%\update.ufw"
set "LATEST_UFW="

if exist "%UFW_OUT%" (
    for %%F in ("%UFW_OUT%") do echo   - output\update.ufw  : %%~tF  [%%~zF bytes]
)
if exist "%UFW_DL%" (
    for %%F in ("%UFW_DL%") do echo   - download\update.ufw: %%~tF  [%%~zF bytes]
)

if exist "%UFW_DL%" (
    xcopy /D /Y "%UFW_DL%" "%OUT_DIR%\" >nul 2>&1
)
if exist "%UFW_OUT%" (
    set "LATEST_UFW=%UFW_OUT%"
    for %%F in ("%UFW_OUT%") do echo [READY] Latest update.ufw: %%~fF [%%~tF, %%~zF bytes]
) else (
    echo [INFO] update.ufw not found yet; it will be generated during flash.
)

rem ----------------------------------------------------------------------------
rem 4. Select USB flash mode
rem ----------------------------------------------------------------------------
echo.
echo Connect board via USB in force-download mode and choose an option:
echo   [1] Normal USB Flash - keep VM data, recommended [Default]
echo   [2] Format VM + USB Flash - clears BT pairing and volume memory
echo   [3] Format ALL + USB Flash - full chip erase
echo   [4] Open folder and highlight latest update.ufw for standalone updater
echo   [0] Exit
echo.
set "MODE_CHOICE=1"
set /p "MODE_CHOICE=Enter choice [1/2/3/4/0] [Press Enter for 1]: "

if "%MODE_CHOICE%"=="0" exit /b 0

if "%MODE_CHOICE%"=="4" (
    if defined LATEST_UFW (
        explorer /select,"%LATEST_UFW%"
    ) else (
        explorer "%OUT_DIR%"
    )
    exit /b 0
)

set "DL_ACTION=download"
if "%MODE_CHOICE%"=="2" set "DL_ACTION=format_vm"
if "%MODE_CHOICE%"=="3" set "DL_ACTION=format_flash"

echo.
echo ============================================================================
echo [START] Running JieLi isd_download.exe chain: %DL_ACTION%
echo         Flashes latest app.bin + stream.bin via USB and updates output\update.ufw
echo ============================================================================

pushd "%SDK_DIR%"
call "%SDK_DIR%\download.bat" "%OUT_DIR%" %DL_ACTION%
set "FLASH_ERR=%ERRORLEVEL%"
popd

echo.
echo ============================================================================
if "%FLASH_ERR%"=="0" (
    if exist "%UFW_OUT%" (
        for %%F in ("%UFW_OUT%") do echo [DONE] Finished! Latest update.ufw: %%~tF [%%~zF bytes]
    ) else (
        echo [DONE] Finished!
    )
    echo        Note: If you see "Device Offline" above, check USB cable or force-download mode.
) else (
    echo [FAIL] Flash exited with error code: %FLASH_ERR%
)
echo ============================================================================
pause
exit /b %FLASH_ERR%
