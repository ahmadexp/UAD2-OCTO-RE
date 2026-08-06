param(
    [switch]$ShutdownWhenDone,
    [switch]$FinalizeOnly
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$administrator = [Security.Principal.WindowsBuiltInRole]::Administrator
if (-not $principal.IsInRole($administrator)) {
    $elevationArguments = @(
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        "`"$PSCommandPath`""
    )
    if ($ShutdownWhenDone) {
        $elevationArguments += '-ShutdownWhenDone'
    }
    if ($FinalizeOnly) {
        $elevationArguments += '-FinalizeOnly'
    }
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $elevationArguments
    exit
}

$media = $PSScriptRoot
$capture = 'C:\UAD2Lab\plugin-host'
New-Item -ItemType Directory -Force -Path $capture | Out-Null

$artifacts = @(
    @{
        Name = 'plugins-64bit.msi'
        Hash = '4F3395BA7221E930CE10E8A8FA9632A2AA97794F110BA3FBAD76BE9E9C885ADC'
        Kind = 'msi'
    },
    @{
        Name = 'vst3-64bit.msi'
        Hash = '3FE5678548BBC706D32E0A49AF5CBC8E7369BBF010F348E04A8C9A074DC3B358'
        Kind = 'msi'
    },
    @{
        Name = 'reaper778_x64-install.exe'
        Hash = 'E7AD77BDD572C35D205034F871181C7B4D9A4110798131B60ACD54CD44453947'
        Kind = 'exe'
    }
)

$evidence = foreach ($artifact in $artifacts) {
    $path = Join-Path $media $artifact.Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing reference artifact: $path"
    }
    $observed = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
    if ($observed -ne $artifact.Hash) {
        throw "SHA-256 mismatch for $($artifact.Name): $observed"
    }
    $signature = Get-AuthenticodeSignature -FilePath $path
    [pscustomobject]@{
        Name = $artifact.Name
        SHA256 = $observed
        SignatureStatus = [string]$signature.Status
        Signer = if ($signature.SignerCertificate) {
            $signature.SignerCertificate.Subject
        } else {
            $null
        }
    }
}
$mediaEvidencePath = Join-Path $capture 'media-evidence.json'
$evidence | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $mediaEvidencePath

if (-not $FinalizeOnly) {
    foreach ($artifact in $artifacts | Where-Object Kind -eq 'msi') {
        $path = Join-Path $media $artifact.Name
        $log = Join-Path $capture ($artifact.Name + '.log')
        $arguments = @('/i', "`"$path`"", '/qn', '/norestart', '/l*v', "`"$log`"")
        $process = Start-Process -FilePath 'msiexec.exe' -Wait -PassThru -ArgumentList $arguments
        if ($process.ExitCode -notin @(0, 1641, 3010)) {
            throw "msiexec failed for $($artifact.Name): $($process.ExitCode)"
        }
    }

    $reaper = Join-Path $media 'reaper778_x64-install.exe'
    $process = Start-Process -FilePath $reaper -Wait -PassThru -ArgumentList '/S'
    if ($process.ExitCode -ne 0) {
        throw "REAPER installer failed: $($process.ExitCode)"
    }
}

$roots = @(
    'C:\Program Files\Common Files\VST3',
    'C:\Program Files\Steinberg\VstPlugins',
    'C:\Program Files\Universal Audio',
    'C:\Program Files (x86)\Universal Audio'
)
$inventory = foreach ($root in $roots) {
    if (Test-Path -LiteralPath $root) {
        Get-ChildItem -LiteralPath $root -Recurse -ErrorAction SilentlyContinue |
            Where-Object {
                ($_.PSIsContainer -and $_.Extension -eq '.vst3') -or
                (-not $_.PSIsContainer -and $_.Extension -eq '.dll')
            } |
            Select-Object FullName, Length, PSIsContainer, LastWriteTimeUtc
    }
}
$inventory | Sort-Object FullName | ConvertTo-Json -Depth 4 |
    Set-Content -Encoding UTF8 (Join-Path $capture 'plugin-inventory.json')

$reaperInstalled = Test-Path 'C:\Program Files\REAPER (x64)\reaper.exe'
$pluginFileCount = @($inventory).Count
if (-not $reaperInstalled -or $pluginFileCount -eq 0) {
    throw "plug-in host validation failed: reaper=$reaperInstalled plugins=$pluginFileCount"
}

[pscustomobject]@{
    CompletedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    Reaper = $reaperInstalled
    PluginFileCount = $pluginFileCount
    FinalizeOnly = [bool]$FinalizeOnly
    RebootRequired = $true
} | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $capture 'install-result.json')

if ($ShutdownWhenDone) {
    Stop-Computer -Force
}
