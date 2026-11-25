param(
    [ValidateSet('build', 'menuconfig')]
    [string]$Action = 'build',
    [ValidateSet('local', 'verify-bridge', 'verify-simulator', 'verify-home', 'verify-cloud')]
    [string]$Profile = 'local',
    [string]$IdfRoot = 'C:\esp\v6.1\esp-idf',
    [string]$ToolsRoot = 'C:\Espressif\tools',
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,30}$')]
    [string]$Version,
    [switch]$DiagnosticRollback,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
if ($Action -eq 'menuconfig' -and $Profile -ne 'local') {
    throw 'menuconfig edits only the local profile.'
}
if ($Action -eq 'menuconfig' -and ($Version -or $DiagnosticRollback -or $OutputDirectory)) {
    throw 'Version, DiagnosticRollback and OutputDirectory apply only to builds.'
}
if ($DiagnosticRollback -and (!$Version -or $Profile -notin @('local','verify-cloud') -or
                            ($Profile -eq 'local' -and !$OutputDirectory))) {
    throw 'Use a unique Version, a cloud profile and a separate output directory for a local rollback test.'
}
$outputOverride = if ($OutputDirectory) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory) }
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
foreach ($file in @('CMakeLists.txt','sdkconfig.defaults','dependencies.lock','partitions.csv','version.txt')) {
    $path = Join-Path $source $file
    if (Test-Path -LiteralPath $path) {
        Copy-Item -LiteralPath $path -Destination $project -Force
    }
}
if ($Version) {
    Set-Content -LiteralPath (Join-Path $project 'version.txt') -Value $Version -Encoding ascii
}
Get-ChildItem -LiteralPath (Join-Path $source 'main') -File |
    Copy-Item -Destination $main -Force
Copy-Item -LiteralPath (Join-Path $repo 'STM32\Core\Inc\edge_wire.h') -Destination $wireInc -Force
Copy-Item -LiteralPath (Join-Path $repo 'STM32\Core\Src\edge_wire.c') -Destination $wireSrc -Force
$flavor = if ($DiagnosticRollback) { "$Profile-rollback" } else { $Profile }
$sdkconfig = Join-Path $stage "sdkconfig-$flavor"
$build = "build-$flavor"
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

if ($Action -eq 'build') {
    $setting = if ($DiagnosticRollback) { 'CONFIG_EDGE_OTA_DIAGNOSTIC_ROLLBACK=y' }
               else { '# CONFIG_EDGE_OTA_DIAGNOSTIC_ROLLBACK is not set' }
    $configLines = @(Get-Content -LiteralPath $sdkconfig | Where-Object {
        $_ -notmatch '^(CONFIG_EDGE_OTA_DIAGNOSTIC_ROLLBACK=|# CONFIG_EDGE_OTA_DIAGNOSTIC_ROLLBACK is not set)'
    })
    ($configLines + $setting) | Set-Content -LiteralPath $sdkconfig -Encoding utf8
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
    if ($DiagnosticRollback -and !(Select-String -LiteralPath $sdkconfig -SimpleMatch 'CONFIG_EDGE_OTA_DIAGNOSTIC_ROLLBACK=y' -Quiet)) {
        throw 'The diagnostic rollback image requires a ThingsBoard build with OTA enabled.'
    }
    if ($Action -eq 'menuconfig') {
        Copy-Item -LiteralPath $sdkconfig -Destination (Join-Path $source 'sdkconfig') -Force
    }
    if (Test-Path -LiteralPath 'dependencies.lock') {
        Copy-Item -LiteralPath 'dependencies.lock' -Destination $source -Force
    }
    if ($Action -eq 'build' -and $Profile -eq 'local') {
        $output = if ($outputOverride) { $outputOverride } else { Join-Path $source 'build-artifacts' }
        $flash = Get-Content -LiteralPath (Join-Path $build 'flasher_args.json') -Raw | ConvertFrom-Json
        $files = @('edge_gateway.elf','flash_args','flasher_args.json') + @($flash.flash_files.PSObject.Properties.Value)
        foreach ($file in $files) {
            $destination = Join-Path $output $file
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $build $file) -Destination $destination -Force
        }
        Write-Host "ESP32 artifacts: $output"
    }
} finally {
    Pop-Location
    foreach ($key in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($key, $previous[$key], 'Process')
    }
}
