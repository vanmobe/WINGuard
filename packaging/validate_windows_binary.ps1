param(
    [Parameter(Mandatory = $true)]
    [string]$PluginPath
)

$ErrorActionPreference = 'Stop'

$resolvedPluginPath = (Resolve-Path $PluginPath).Path

function Find-Dumpbin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        return $null
    }

    $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installationPath) {
        return $null
    }

    $candidate = Get-ChildItem -Path (Join-Path $installationPath 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    return $candidate.FullName
}

$dumpbin = Find-Dumpbin
if ($dumpbin) {
    $inspection = (& $dumpbin /headers /dependents $resolvedPluginPath 2>&1 | Out-String)
} else {
    $llvmReadobj = Get-Command llvm-readobj.exe -ErrorAction SilentlyContinue
    if (-not $llvmReadobj) {
        throw 'Neither dumpbin.exe nor llvm-readobj.exe was found; cannot validate the Windows plugin binary.'
    }
    $inspection = (& $llvmReadobj.Source --file-headers --coff-imports $resolvedPluginPath 2>&1 | Out-String)
}

if ($LASTEXITCODE -ne 0) {
    throw "Binary inspection failed for $resolvedPluginPath`n$inspection"
}

if ($inspection -notmatch '(?i)(8664 machine \(x64\)|IMAGE_FILE_MACHINE_AMD64)') {
    throw "Expected an x64 Windows plugin binary.`n$inspection"
}

$dynamicRuntimePattern = '(?i)\b(?:VCRUNTIME|MSVCP|CONCRT|UCRTBASED)[A-Z0-9_.-]*\.dll\b'
$dynamicRuntimeImports = [regex]::Matches($inspection, $dynamicRuntimePattern) |
    ForEach-Object { $_.Value.ToUpperInvariant() } |
    Sort-Object -Unique
if ($dynamicRuntimeImports) {
    throw "Plugin imports dynamic MSVC runtime DLLs: $($dynamicRuntimeImports -join ', ')"
}

Write-Host "Validated self-contained x64 plugin: $resolvedPluginPath"
