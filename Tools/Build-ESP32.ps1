param(
    [ValidateSet('build', 'menuconfig')]
    [string]$Action = 'build',
    [ValidateSet('local', 'verify-bridge', 'verify-simulator', 'verify-home', 'verify-cloud')]
    [string]$Profile = 'local',
    [string]$IdfRoot = 'C:\esp\v6.1\esp-idf',
    [string]$ToolsRoot = 'C:\Espressif\tools'
)

$ErrorActionPreference = 'Stop'
if ($Action -eq 'menuconfig' -and $Profile -ne 'local') {
    throw 'menuconfig edits only the local profile.'
}
$repo = Split-Path -Parent $PSScriptRoot
$pythonEnv = Join-Path $ToolsRoot 'python\v6.1\venv'
$idfPy = Join-Path $IdfRoot 'tools\idf.py'
$python = Join-Path $pythonEnv 'Scripts\python.exe'
if (!(Test-Path -LiteralPath $idfPy) -or !(Test-Path -LiteralPath $python)) {
    throw 'ESP-IDF v6.1 or its Python environment was not found.'
}

# Native Windows IDF tools can fail when the project path contains Unicode.
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($repo))
    $suffix = ([BitConverter]::ToString($bytes, 0, 6)).Replace('-', '').ToLowerInvariant()
} finally {
    $sha.Dispose()
}
$stage = Join-Path ([IO.Path]::GetTempPath()) "edge-idf-$suffix"
$source = Join-Path $repo 'ESP32'
$project = Join-Path $stage 'ESP32'
$main = Join-Path $project 'main'
$wireInc = Join-Path $stage 'STM32\Core\Inc'
$wireSrc = Join-Path $stage 'STM32\Core\Src'
New-Item -ItemType Directory -Path $main,$wireInc,$wireSrc -Force | Out-Null
foreach ($file in @('CMakeLists.txt','sdkconfig.defaults','dependencies.lock')) {
    $path = Join-Path $source $file
    if (Test-Path -LiteralPath $path) {
        Copy-Item -LiteralPath $path -Destination $project -Force
    }
}
Get-ChildItem -LiteralPath (Join-Path $source 'main') -File |
    Copy-Item -Destination $main -Force
Copy-Item -LiteralPath (Join-Path $repo 'STM32\Core\Inc\edge_wire.h') -Destination $wireInc -Force
Copy-Item -LiteralPath (Join-Path $repo 'STM32\Core\Src\edge_wire.c') -Destination $wireSrc -Force
$sdkconfig = Join-Path $stage "sdkconfig-$Profile"
$build = "build-$Profile"
if ($Profile -eq 'local') {
    $localConfig = Join-Path $source 'sdkconfig'
    if (Test-Path -LiteralPath $localConfig) {
        Copy-Item -LiteralPath $localConfig -Destination $sdkconfig -Force
    } else {
        '# No local overrides' | Set-Content -LiteralPath $sdkconfig -Encoding ascii
    }
} else {
    $simulated = if ($Profile -eq 'verify-simulator') { 'y' } else { 'n' }
    $uartTx = if ($Profile -eq 'verify-cloud') { 25 } else { 17 }
    $mqttProfileLines = if ($Profile -eq 'verify-cloud') {
        @(
            'CONFIG_EDGE_HOME_MQTT=y'
            'CONFIG_EDGE_HOME_THINGSBOARD=y'
            'CONFIG_EDGE_TB_DEVICE_TOKEN="build-only"'
        )
    } elseif ($Profile -eq 'verify-home') {
        @(
            'CONFIG_EDGE_HOME_MQTT=y'
            'CONFIG_EDGE_HOME_THINGSBOARD=n'
            'CONFIG_EDGE_MQTT_USER="build-only"'
            'CONFIG_EDGE_MQTT_PASSWORD="build-only"'
        )
    } else {
        @('CONFIG_EDGE_HOME_MQTT=n')
    }
    @(
        'CONFIG_IDF_TARGET="esp32"'
        'CONFIG_EDGE_WIFI_SSID="BUILD_ONLY"'
        'CONFIG_EDGE_SERVER_IPV4="192.0.2.1"'
        'CONFIG_EDGE_DEVICE_ID="build-only"'
        "CONFIG_EDGE_UART_TX=$uartTx"
        'CONFIG_EDGE_UART_RX=16'
        "CONFIG_EDGE_SIMULATOR=$simulated"
        $mqttProfileLines
    ) | Set-Content -LiteralPath $sdkconfig -Encoding ascii
}

$previous = @{}
$environment = @{
    PYTHONUTF8 = '1'
    IDF_PATH = $IdfRoot
    IDF_TOOLS_PATH = $ToolsRoot
    IDF_PYTHON_ENV_PATH = $pythonEnv
    ESP_IDF_VERSION = '6.1'
    ESP_ROM_ELF_DIR = Join-Path $ToolsRoot 'esp-rom-elfs\20241011'
}
$bins = @(
    'python\v6.1\venv\Scripts',
    'cmake\4.0.3\bin',
    'ninja\1.12.1',
    'xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin',
    'esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin'
) | ForEach-Object { Join-Path $ToolsRoot $_ }
$environment['Path'] = ($bins -join ';') + ';' + $env:Path
foreach ($key in $environment.Keys) {
    $previous[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
    [Environment]::SetEnvironmentVariable($key, $environment[$key], 'Process')
}
Push-Location -LiteralPath $project
try {
    Write-Host "ESP-IDF source stage: $project"
    & $python $idfPy -B $build -D "SDKCONFIG=$sdkconfig" $Action
    if ($LASTEXITCODE -ne 0) {
        throw "idf.py $Action failed with code $LASTEXITCODE."
    }
    if ($Action -eq 'menuconfig') {
        Copy-Item -LiteralPath $sdkconfig -Destination (Join-Path $source 'sdkconfig') -Force
    }
    if (Test-Path -LiteralPath 'dependencies.lock') {
        Copy-Item -LiteralPath 'dependencies.lock' -Destination $source -Force
    }
    if ($Action -eq 'build' -and $Profile -eq 'local') {
        $output = Join-Path $source 'build-artifacts'
        New-Item -ItemType Directory -Path $output,"$output\bootloader","$output\partition_table" -Force | Out-Null
        foreach ($file in @('edge_gateway.elf','edge_gateway.bin','flash_args','flasher_args.json',
                            'bootloader\bootloader.bin','partition_table\partition-table.bin')) {
            Copy-Item -LiteralPath (Join-Path $build $file) -Destination (Join-Path $output $file) -Force
        }
        Write-Host "ESP32 artifacts: $output"
    }
} finally {
    Pop-Location
    foreach ($key in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($key, $previous[$key], 'Process')
    }
}
