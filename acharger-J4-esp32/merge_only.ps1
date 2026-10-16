# ESP32-C3 一键合并 + 烧录（仅合并模式：仅跑 merge_bin，不烧录）
# 用法： .\merge_only.ps1
$ErrorActionPreference = "Stop"

$IDF    = "D:\ESP32\ESP_IDF\v5.3.3\frameworks\esp-idf-v5.3.3"
$PYTHON = "D:\ESP32\ESP_IDF\v5.3.3\python_env\idf5.3_py3.11_env\Scripts\python.exe"
$BUILD  = "$PSScriptRoot\build"
$TOOL   = "$IDF\components\esptool_py\esptool\esptool.py"

# 检查 Python
if (-not (Test-Path $PYTHON)) {
    Write-Host "[X] 找不到 ESP-IDF Python: $PYTHON" -ForegroundColor Red
    exit 1
}

# 检查4 个 bin
$bins = @(
    "$BUILD\bootloader\bootloader.bin",
    "$BUILD\partition_table\partition-table.bin",
    "$BUILD\ota_data_initial.bin",
    "$BUILD\acharge-us-esp.bin"
)
foreach ($f in $bins) {
    if (-not (Test-Path $f)) {
        Write-Host "[X] 缺少文件: $f" -ForegroundColor Red
        exit 1
    }
}

Write-Host "[*] 合并 4 个 bin ..." -ForegroundColor Cyan
# --chip 必须在 merge_bin 子命令之前（是 esptool 全局参数，不是 merge_bin 的参数）
& $PYTHON $TOOL --chip esp32c3 merge_bin `
    --output "$BUILD\merged.bin" `
    --flash_mode dio --flash_size 4MB --flash_freq 80m `
    --fill-flash-size 4MB `
    0x0     "$BUILD\bootloader\bootloader.bin" `
    0x8000  "$BUILD\partition_table\partition-table.bin" `
    0xd000  "$BUILD\ota_data_initial.bin" `
    0x10000 "$BUILD\acharge-us-esp.bin"

if ($LASTEXITCODE -eq 0) {
    Write-Host "[OK] 合并成功 -> $BUILD\merged.bin" -ForegroundColor Green
    $sz = (Get-Item "$BUILD\merged.bin").Length
    Write-Host ("[OK] 合并大小: {0:N0} bytes ({1:N2} KB)" -f $sz, ($sz/1KB)) -ForegroundColor Green
} else {
    Write-Host "[X] 合并失败" -ForegroundColor Red
    exit 1
}