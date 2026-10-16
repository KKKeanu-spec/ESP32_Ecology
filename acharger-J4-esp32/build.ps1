# ESP32-C3 一键编译（含 TinyML 正弦波模型）
#
# 用法:
#   .\build.ps1            增量编译
#   .\build.ps1 -Clean     全量重编（删 build 目录）
#   .\build.ps1 -Merge     编译后额外合并为 merged.bin（4MB 整片镜像）
#
# 说明: 本机 IDF 装在 D:\ESP32\ESP_IDF\v5.3.3，未写入系统 PATH，
#       故本脚本显式拼装工具链路径（cmake / ninja / riscv32 工具链 / git / ccache）。
param(
    [switch]$Clean,
    [switch]$Merge
)

$ErrorActionPreference = "Stop"

$IDF_BASE  = "D:\ESP32\ESP_IDF\v5.3.3"
$IDF_PATH  = "$IDF_BASE\frameworks\esp-idf-v5.3.3"
$IDF_TOOLS = "$IDF_BASE\tools"
$PYTHON    = "$IDF_BASE\python_env\idf5.3_py3.11_env\Scripts\python.exe"
$IDFPY     = "$IDF_PATH\tools\idf.py"

if (-not (Test-Path $PYTHON)) { Write-Host "[X] 找不到 IDF Python: $PYTHON" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $IDFPY))  { Write-Host "[X] 找不到 idf.py: $IDFPY"    -ForegroundColor Red; exit 1 }

# --- 环境变量 ---------------------------------------------------------------
$env:IDF_PATH              = $IDF_PATH
$env:IDF_PYTHON_ENV_PATH   = "$IDF_BASE\python_env\idf5.3_py3.11_env"
$env:ESP_ROM_ELF_DIR       = "$IDF_TOOLS\esp-rom-elfs\20240305"   # 缺失会导致 bootloader 子项目 configure 失败
$env:ESP_IDF_VERSION       = "5.3.3"

$toolPaths = @(
    "$IDF_TOOLS\riscv32-esp-elf\esp-13.2.0_20240530\riscv32-esp-elf\bin",
    "$IDF_TOOLS\cmake\3.30.2\bin",
    "$IDF_TOOLS\ninja\1.12.1",
    "$IDF_TOOLS\ccache\4.10.2\ccache-4.10.2-windows-x86_64",
    "$IDF_TOOLS\idf-git\2.44.0\cmd"
) | Where-Object { Test-Path $_ }

$env:PATH = ($toolPaths -join ";") + ";" + $env:PATH

Set-Location $PSScriptRoot

# --- 编译 -------------------------------------------------------------------
if ($Clean -and (Test-Path "build")) {
    Write-Host "[*] 删除 build 目录 ..." -ForegroundColor Cyan
    Remove-Item -Recurse -Force "build"
}

Write-Host "[*] idf.py build ..." -ForegroundColor Cyan
$logFile = Join-Path $PSScriptRoot "_idf_build.log"
& $PYTHON $IDFPY build 2>&1 | Tee-Object -FilePath $logFile
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] 编译失败，错误摘要:" -ForegroundColor Red
    Select-String -Path $logFile -Pattern "error:|Error |FAILED" | Select-Object -First 20 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    exit 1
}

Write-Host ""
Write-Host "[OK] 编译成功" -ForegroundColor Green
Write-Host "[*] 产物清单:" -ForegroundColor Cyan
$arts = @(
    "build\bootloader\bootloader.bin",
    "build\partition_table\partition-table.bin",
    "build\ota_data_initial.bin",
    "build\acharge-us-esp.bin",
    "build\acharge-us-esp.elf"
)
foreach ($f in $arts) {
    if (Test-Path $f) {
        $sz = (Get-Item $f).Length
        Write-Host ("    {0,-46} {1,10:N0} B  ({2,8:N2} KB)" -f $f, $sz, ($sz/1KB))
    } else {
        Write-Host ("    {0,-46} [缺失]" -f $f) -ForegroundColor Yellow
    }
}

# --- 合并整片镜像 -----------------------------------------------------------
if ($Merge) {
    Write-Host ""
    Write-Host "[*] 合并为 merged.bin ..." -ForegroundColor Cyan
    $TOOL = "$IDF_PATH\components\esptool_py\esptool\esptool.py"
    & $PYTHON $TOOL --chip esp32c3 merge_bin `
        --output "build\merged.bin" `
        --flash_mode dio --flash_size 4MB --flash_freq 80m `
        --fill-flash-size 4MB `
        0x0     "build\bootloader\bootloader.bin" `
        0x8000  "build\partition_table\partition-table.bin" `
        0xd000  "build\ota_data_initial.bin" `
        0x10000 "build\acharge-us-esp.bin"
    if ($LASTEXITCODE -eq 0) {
        $sz = (Get-Item "build\merged.bin").Length
        Write-Host ("[OK] merged.bin {0:N0} B ({1:N2} KB)" -f $sz, ($sz/1KB)) -ForegroundColor Green
    }
}
