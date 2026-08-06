[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path $_ -PathType Container })]
    [string]$MediaRoot,

    [ValidateSet('Install', 'PostReboot')]
    [string]$Phase = 'Install'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$work = 'C:\UAD2Lab'
$localScript = Join-Path $work 'Capture-Uad2Lifecycle.ps1'
$capture = Join-Path $work 'capture'
New-Item -ItemType Directory -Force -Path $work, $capture | Out-Null

function Get-OutputRoot {
    $volume = Get-Volume -FileSystemLabel UAD2OUT -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $volume -and $volume.DriveLetter) {
        $root = $volume.DriveLetter + ':\capture'
        New-Item -ItemType Directory -Force -Path $root | Out-Null
        return $root
    }
    return $capture
}

function Write-Snapshot {
    param([Parameter(Mandatory = $true)][string]$Name)

    $out = Get-OutputRoot
    $prefix = Join-Path $out $Name
    Get-Date -Format o | Set-Content ($prefix + '-timestamp.txt')
    Confirm-SecureBootUEFI | Out-File ($prefix + '-secure-boot.txt')
    Get-Tpm | Format-List * | Out-File ($prefix + '-tpm.txt') -Width 240
    Get-ComputerInfo | Format-List * | Out-File ($prefix + '-computer-info.txt') -Width 240
    Get-PnpDevice -PresentOnly | Sort-Object Class, FriendlyName |
        Format-Table -AutoSize -Wrap | Out-File ($prefix + '-pnp.txt') -Width 300
    Get-CimInstance Win32_PnPSignedDriver |
        Where-Object {
            $_.DeviceID -match 'VEN_1A00' -or
            $_.DeviceName -match 'UAD|Universal Audio'
        } |
        Select-Object DeviceName, DeviceID, DriverProviderName, DriverVersion,
            DriverDate, InfName, IsSigned, Signer |
        ConvertTo-Json -Depth 4 | Set-Content ($prefix + '-uad-drivers.json')
    Get-CimInstance Win32_PnPEntity |
        Where-Object {
            $_.PNPDeviceID -match 'VEN_1A00' -or
            $_.Name -match 'UAD|Universal Audio'
        } |
        Select-Object Name, PNPDeviceID, Status, ConfigManagerErrorCode |
        ConvertTo-Json -Depth 4 | Set-Content ($prefix + '-uad-devices.json')
    Get-Service | Where-Object {
        $_.Name -match 'UAD|UAudio|Universal' -or
        $_.DisplayName -match 'UAD|Universal Audio'
    } | Select-Object Name, DisplayName, Status, StartType |
        ConvertTo-Json -Depth 4 | Set-Content ($prefix + '-uad-services.json')
    driverquery.exe /v /fo csv | Out-File ($prefix + '-driverquery.csv') -Encoding utf8
    pnputil.exe /enum-devices /connected /deviceids |
        Out-File ($prefix + '-pnputil-connected.txt') -Width 300
    wevtutil.exe qe System /q:"*[System[(EventID=219 or EventID=20001 or EventID=20003 or EventID=12 or EventID=13)]]" /f:text /c:500 |
        Out-File ($prefix + '-system-events.txt') -Width 300
    reg.exe query 'HKLM\SOFTWARE\Universal Audio' /s |
        Out-File ($prefix + '-registry.txt') -Width 300
}

function Invoke-Msi {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Properties
    )

    $path = Join-Path $MediaRoot $Name
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Required package is absent: $path"
    }
    $log = Join-Path (Get-OutputRoot) ($Name + '.log')
    $arguments = @('/i', $path, '/qn', '/norestart', '/l*v', $log) + $Properties
    $process = Start-Process msiexec.exe -ArgumentList $arguments -Wait -PassThru
    if ($process.ExitCode -notin 0, 1641, 3010) {
        throw "$Name failed with MSI exit code $($process.ExitCode); see $log"
    }
    [pscustomobject]@{
        Name = $Name
        ExitCode = $process.ExitCode
        Log = $log
    }
}

function Copy-ResearchArtifacts {
    $out = Get-OutputRoot
    $sources = @(
        'C:\ProgramData\Universal Audio',
        'C:\Program Files\Universal Audio',
        'C:\Program Files (x86)\Universal Audio'
    )
    foreach ($source in $sources) {
        if (Test-Path $source) {
            $safe = ($source -replace '[:\\ ]', '_').Trim('_')
            $target = Join-Path $out $safe
            New-Item -ItemType Directory -Force -Path $target | Out-Null
            Get-ChildItem $source -Recurse -File -ErrorAction SilentlyContinue |
                Where-Object {
                    $_.Extension -in '.log', '.txt', '.json', '.xml', '.inf', '.cat', '.sys', '.exe', '.dll'
                } | ForEach-Object {
                    $relative = $_.FullName.Substring($source.Length).TrimStart('\')
                    $destination = Join-Path $target $relative
                    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
                    Copy-Item $_.FullName $destination -Force -ErrorAction SilentlyContinue
                }
        }
    }
}

function Send-HostMarker {
    param([Parameter(Mandatory = $true)][string]$Message)

    try {
        $port = [System.IO.Ports.SerialPort]::new('COM1', 115200, 'None', 8, 'One')
        $port.Open()
        $port.WriteLine($Message)
        $port.Close()
    } catch {
        $_ | Out-String | Set-Content (Join-Path (Get-OutputRoot) 'serial-marker-error.txt')
    }
}

Start-Transcript -Path (Join-Path (Get-OutputRoot) ("capture-$Phase-transcript.txt")) -Force
try {
    if ($Phase -eq 'Install') {
        Copy-Item $PSCommandPath $localScript -Force
        Get-ChildItem $MediaRoot -File | Get-FileHash -Algorithm SHA256 |
            Select-Object Path, Hash |
            ConvertTo-Json -Depth 3 | Set-Content (Join-Path (Get-OutputRoot) 'media-sha256.json')
        Get-ChildItem $MediaRoot -Filter '*.msi' -File |
            Get-AuthenticodeSignature |
            Select-Object Path, Status, StatusMessage, SignatureType,
                @{Name='SignerSubject'; Expression={$_.SignerCertificate.Subject}},
                @{Name='SignerThumbprint'; Expression={$_.SignerCertificate.Thumbprint}},
                @{Name='TimeStamperSubject'; Expression={$_.TimeStamperCertificate.Subject}} |
            ConvertTo-Json -Depth 4 | Set-Content (Join-Path (Get-OutputRoot) 'media-signatures.json')
        Write-Snapshot -Name '00-before-install'

        $common = @(
            'ARPSYSTEMCOMPONENT=1',
            'MSIFASTINSTALL=7',
            'UA_INSTALLDIR="C:\Program Files\Universal Audio\Powered Plugins\"'
        )
        $results = @()
        $results += Invoke-Msi -Name 'system-32bit.msi' -Properties ($common + @(
            ('SETUP_EXE_PATH=' + $MediaRoot),
            'SEND_USER_METRICS=0',
            'DO_DESKTOP_SHORTCUT_INSTALL=0',
            'INSTALLER_BUNDLE_UPGRADE_CODE={61724A5D-DCA9-440B-9A80-77144FFB843E}',
            'WINDOWS_MAJOR_VERSION=10'
        ))
        $results += Invoke-Msi -Name 'system-64bit.msi' -Properties ($common + @(
            'INSTALLER_BUNDLE_UPGRADE_CODE={61724A5D-DCA9-440B-9A80-77144FFB843E}'
        ))
        $results += Invoke-Msi -Name 'drivers.msi' -Properties @(
            'ARPSYSTEMCOMPONENT=1',
            'MSIFASTINSTALL=7',
            'DO_APOLLO_INSTALL=1'
        )
        $results | ConvertTo-Json -Depth 3 |
            Set-Content (Join-Path (Get-OutputRoot) 'msi-results.json')
        Write-Snapshot -Name '01-after-install-before-reboot'
        Copy-ResearchArtifacts

        $runOnce = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce'
        $command = 'powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "' +
            $localScript + '" -MediaRoot "C:\" -Phase PostReboot'
        New-ItemProperty -Path $runOnce -Name UAD2CaptureResume -Value $command -PropertyType String -Force | Out-Null
        shutdown.exe /r /t 10 /d p:4:1 /c "UAD-2 official driver lifecycle capture"
    } else {
        Start-Sleep -Seconds 45
        pnputil.exe /scan-devices | Out-File (Join-Path (Get-OutputRoot) 'pnputil-scan.txt')
        Start-Sleep -Seconds 30
        Write-Snapshot -Name '02-after-reboot'
        Copy-ResearchArtifacts
        Get-ChildItem 'C:\Windows\INF' -Filter 'setupapi*.log' -File |
            Copy-Item -Destination (Get-OutputRoot) -Force
        wevtutil.exe epl System (Join-Path (Get-OutputRoot) 'System.evtx') /ow:true
        wevtutil.exe epl Application (Join-Path (Get-OutputRoot) 'Application.evtx') /ow:true
        'complete' | Set-Content (Join-Path (Get-OutputRoot) 'CAPTURE-COMPLETE.txt')
        Send-HostMarker -Message 'UAD2_CAPTURE_COMPLETE'
    }
} finally {
    Stop-Transcript
}
