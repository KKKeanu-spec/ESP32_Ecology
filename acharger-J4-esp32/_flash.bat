@echo off
set IDF_BASE=D:\ESP32\ESP_IDF\v5.3.3
set IDF_PATH=%IDF_BASE%\frameworks\esp-idf-v5.3.3
set IDF_TOOLS=%IDF_BASE%\tools
set IDF_PY=%IDF_BASE%\python_env\idf5.3_py3.11_env\Scripts\python.exe
set PATH=%IDF_TOOLS%\riscv32-esp-elf\esp-13.2.0_20240530\riscv32-esp-elf\bin;%IDF_TOOLS%\cmake\3.30.2\bin;%IDF_TOOLS%\ninja\1.12.1;%IDF_TOOLS%\idf-git\2.44.0\cmd;%PATH%
set IDF_PYTHON_ENV_PATH=%IDF_BASE%\python_env\idf5.3_py3.11_env
set ESP_ROM_ELF_DIR=%IDF_TOOLS%\esp-rom-elfs\20240305
cd /d c:\Users\huangguoyuan\Desktop\fcu_pwm_710\TinyML_ESP32C3\acharger-J4-esp32
echo === chip_id on COM5 === > "%~dp0_flash_out.txt"
"%IDF_PY%" "%IDF_PATH%\components\esptool_py\esptool\esptool.py" --chip esp32c3 -p COM5 chip_id >> "%~dp0_flash_out.txt" 2>&1
echo === flash === >> "%~dp0_flash_out.txt"
"%IDF_PY%" "%IDF_PATH%\components\esptool_py\esptool\esptool.py" --chip esp32c3 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0xd000 build\ota_data_initial.bin 0x10000 build\acharge-us-esp.bin >> "%~dp0_flash_out.txt" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%~dp0_flash_out.txt"
