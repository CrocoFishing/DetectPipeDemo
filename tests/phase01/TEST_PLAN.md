# Phase 1 Test Plan

- Purpose: validate ESP-IDF target, PSRAM, OV3660 initialization, one RGB565 XGA capture, metadata, frame return, and heap logs.
- Preconditions: XIAO ESP32-S3 Sense camera cable seated; data USB cable; BOOT released; ESP-IDF v6.0 installed.
- Hardware: XIAO ESP32-S3 Sense with OV3660; no button/UART adapter/SD required.
- Firmware: `firmware/apps/phase01_camera`.
- Build: `powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase01_camera build`.
- Flash: `powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase01_camera -p COM5 flash`.
- Run: `powershell.exe -ExecutionPolicy Bypass -File .\tools\idf.ps1 -App phase01_camera -p COM5 monitor`.
- Steps: power-cycle with GPIO0 released; save full log; confirm PSRAM size; confirm sensor PID/OV3660 line; confirm width 1024, height 768, and 1,572,864 RGB565 bytes; confirm before/after heap log and PASS_LOCAL_CHECK marker; repeat reset 10 times.
- Data: serial log plus a CSV row per boot containing timestamp, PSRAM/free/largest, frame bytes, init/capture status.
- Expected: lines matching `expected_output.txt`; no panic, watchdog, null frame, or persistent heap decline.
- Pass: all 10 boots capture and return one frame; PSRAM present; frame legal; no reboot/panic; largest blocks remain sufficient.
- Fail: any init/capture failure, wrong sensor, PSRAM missing, invalid frame, panic, or repeatable memory decline.
- Recovery: release BOOT, reseat camera cable, power-cycle, erase flash only if sdkconfig/NVS is suspect, rebuild, and record failure before retry.
- Results: copy `RESULT_TEMPLATE.md` and logs to `results/phase01/actual/`.
