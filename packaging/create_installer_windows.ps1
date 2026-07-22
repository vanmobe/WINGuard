param(
    [string]$Version = '1.2.0.0',
    [string]$StageDir = 'stage',
    [string]$OutDir = 'releases'
)

$ErrorActionPreference = 'Stop'

$pluginName = 'reaper_wingconnector.dll'
$configName = 'config.json'
$logoName = 'wingguard-logo.png'
$iconDirName = 'ui-icons'
$appName = 'WINGuard'

$stagePath = (Resolve-Path $StageDir).Path
if (-not (Test-Path (Join-Path $stagePath $pluginName))) {
    throw "Missing $pluginName in $stagePath"
}
if (-not (Test-Path (Join-Path $stagePath $logoName))) {
    throw "Missing $logoName in $stagePath"
}
if (-not (Test-Path (Join-Path $stagePath $iconDirName))) {
    throw "Missing $iconDirName in $stagePath"
}

$validatorPath = Join-Path $PSScriptRoot 'validate_windows_binary.ps1'
& $validatorPath -PluginPath (Join-Path $stagePath $pluginName)

$hasConfig = Test-Path (Join-Path $stagePath $configName)
$configFileEntry = ''
if ($hasConfig) {
    $configFileEntry = "Source: `"$stagePath\$configName`"; DestDir: `"{userappdata}\REAPER\UserPlugins`"; Flags: onlyifdoesntexist uninsneveruninstall"
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$outPath = (Resolve-Path $OutDir).Path

$iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue
if (-not $iscc) {
    $defaultIscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    if (Test-Path $defaultIscc) {
        $iscc = @{ Source = $defaultIscc }
    } else {
        throw "ISCC.exe not found. Install Inno Setup 6 first."
    }
}

$tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("audiolab-virtual-soundcheck-win-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmpRoot -Force | Out-Null
$issPath = Join-Path $tmpRoot 'audiolab-virtual-soundcheck.iss'

$iss = @"
[Setup]
AppId={{6EAB471F-8FB0-4EC1-8EF9-529A58C337A1}
AppName=$appName
AppVersion=$Version
AppPublisher=CO LAB
DefaultDirName={userappdata}\REAPER\UserPlugins
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=$outPath
OutputBaseFilename=WINGuard-WIN-v$Version
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayName=$appName

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "$stagePath\$pluginName"; DestDir: "{userappdata}\REAPER\UserPlugins"; Flags: ignoreversion
Source: "$stagePath\$logoName"; DestDir: "{userappdata}\REAPER\UserPlugins"; Flags: ignoreversion
Source: "$stagePath\$iconDirName\*.png"; DestDir: "{userappdata}\REAPER\UserPlugins\$iconDirName"; Flags: ignoreversion
$configFileEntry

[Icons]
Name: "{autoprograms}\$appName\Uninstall $appName"; Filename: "{uninstallexe}"

[Run]
Filename: "{cmd}"; Parameters: "/c echo Installed to %APPDATA%\REAPER\UserPlugins"; Flags: runhidden
"@

Set-Content -Path $issPath -Value $iss -NoNewline

& $iscc.Source $issPath | Out-Host

$exePath = Join-Path $outPath "WINGuard-WIN-v$Version.exe"
if (-not (Test-Path $exePath)) {
    throw "Expected installer not found: $exePath"
}

Remove-Item $tmpRoot -Recurse -Force
Write-Host "Created $exePath"
