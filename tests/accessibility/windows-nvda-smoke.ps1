[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$HostExe,

    [string]$ArtifactsDirectory = (Join-Path $PSScriptRoot "artifacts/windows-accessibility"),
    [int]$StartupTimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$InstallerUrl = "https://download.nvaccess.org/releases/2026.1.1/nvda_2026.1.1.exe"
$InstallerSha256 = "6e0289eb5a3aa076eb97ea99c5d5465cb48b5ecc6a3257dc3d811f881a1747c9"
$InstallerSignerThumbprint = "4E9A772A99C1A3B713766DAC4B3545236CC2AE72"

function Wait-Until {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Condition,
        [Parameter(Mandatory = $true)]
        [int]$TimeoutSeconds
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) {
            return $true
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    return [bool](& $Condition)
}

function Assert-InteractiveDesktop {
    param([string]$EvidencePath)

    $runnerProcess = Get-Process -Id $PID
    $sessionId = $runnerProcess.SessionId
    "commit=$($env:GITHUB_SHA)" | Out-File -FilePath $EvidencePath -Encoding utf8
    "image_os=$($env:ImageOS)" | Out-File -FilePath $EvidencePath -Append -Encoding utf8
    "image_version=$($env:ImageVersion)" | Out-File -FilePath $EvidencePath -Append -Encoding utf8
    "windows=$([Environment]::OSVersion.VersionString)" | Out-File -FilePath $EvidencePath -Append -Encoding utf8
    "username=$([Environment]::UserName)" | Out-File -FilePath $EvidencePath -Append -Encoding utf8
    "user_interactive=$([Environment]::UserInteractive)" | Out-File -FilePath $EvidencePath -Append -Encoding utf8
    "runner_pid=$PID" | Out-File -FilePath $EvidencePath -Append -Encoding utf8
    "runner_session=$sessionId" | Out-File -FilePath $EvidencePath -Append -Encoding utf8

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    "elevated=$elevated" | Out-File -FilePath $EvidencePath -Append -Encoding utf8

    if (-not [Environment]::UserInteractive -or $sessionId -eq 0) {
        throw "Installed NVDA requires an interactive Windows desktop outside session 0."
    }
    if (-not $elevated) {
        throw "Installed NVDA acceptance requires an elevated runner for silent installation."
    }

    if (-not ("NativeSdkInputDesktop" -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NativeSdkInputDesktop {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint desiredAccess);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SwitchDesktop(IntPtr desktop);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool CloseDesktop(IntPtr desktop);
}
'@
    }
    $desktop = [NativeSdkInputDesktop]::OpenInputDesktop(0, $false, 0x0100)
    $desktopSwitchable = $desktop -ne [IntPtr]::Zero -and [NativeSdkInputDesktop]::SwitchDesktop($desktop)
    "input_desktop_switchable=$desktopSwitchable" | Out-File -FilePath $EvidencePath -Append -Encoding utf8
    if ($desktop -ne [IntPtr]::Zero) {
        [void][NativeSdkInputDesktop]::CloseDesktop($desktop)
    }
    if (-not $desktopSwitchable) {
        throw "The runner has no unlocked switchable input desktop."
    }

    Add-Type -AssemblyName System.Windows.Forms
    $form = [System.Windows.Forms.Form]::new()
    try {
        $form.Text = "Native SDK NVDA desktop preflight"
        $form.Show()
        [void]$form.Activate()
        $activated = Wait-Until -TimeoutSeconds 5 -Condition {
            [System.Windows.Forms.Application]::DoEvents()
            return [System.Windows.Forms.Form]::ActiveForm -eq $form
        }
        "winforms_active=$activated" | Out-File -FilePath $EvidencePath -Append -Encoding utf8
        if (-not $activated) {
            throw "A WinForms window could not become active; this runner has no usable interactive desktop."
        }
    } finally {
        $form.Close()
        $form.Dispose()
    }
}

function Invoke-AccessibilityRun {
    param(
        [string]$Name,
        [string[]]$Checks,
        [string[]]$ProbeArguments,
        [bool]$NoBuild
    )

    $runArtifacts = Join-Path $ArtifactsDirectory $Name
    New-Item -ItemType Directory -Force -Path $runArtifacts | Out-Null
    $parameters = @{
        HostExe = $script:HostPath
        ProbeExe = $script:ProbeExe
        Checks = $Checks
        ProbeArguments = $ProbeArguments
        PostProbeDelayMilliseconds = 2000
        ArtifactsDirectory = $runArtifacts
    }
    if ($NoBuild) {
        $parameters.NoBuild = $true
    }
    & $script:RawRunner @parameters
    if ($LASTEXITCODE -ne 0) {
        throw "Windows accessibility run '$Name' failed with exit code $LASTEXITCODE."
    }
}

function Get-SpeechBlocks {
    param([string]$LogPath)

    $content = Get-Content -LiteralPath $LogPath -Raw
    return @([regex]::Matches($content, '(?ms)^IO - speech\.speech\.speak .*?(?=^[A-Z]+ - |\z)') |
        ForEach-Object { $_.Value } |
        Where-Object { $_ -match '(?m)^Speaking \[' })
}

function Find-SpeechBlock {
    param(
        [string[]]$Blocks,
        [string[]]$Tokens,
        [int]$StartIndex = 0,
        [string[]]$ExcludedTokens = @()
    )

    for ($index = $StartIndex; $index -lt $Blocks.Count; $index++) {
        $matches = $true
        foreach ($token in $Tokens) {
            if ($Blocks[$index].IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
                $matches = $false
                break
            }
        }
        foreach ($token in $ExcludedTokens) {
            if ($Blocks[$index].IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $matches = $false
                break
            }
        }
        if ($matches) {
            return $index
        }
    }
    return -1
}

if ($env:OS -ne "Windows_NT") {
    throw "windows-nvda-smoke.ps1 must run on Windows."
}
if ($StartupTimeoutSeconds -le 0) {
    throw "StartupTimeoutSeconds must be positive."
}

New-Item -ItemType Directory -Force -Path $ArtifactsDirectory | Out-Null
$ArtifactsDirectory = (Resolve-Path -LiteralPath $ArtifactsDirectory).Path
$script:HostPath = (Resolve-Path -LiteralPath $HostExe).Path
$script:RawRunner = Join-Path $PSScriptRoot "windows-accessibility-smoke.ps1"
$script:ProbeExe = Join-Path $ArtifactsDirectory "windows_uia_probe.exe"
$sessionEvidence = Join-Path $ArtifactsDirectory "session-evidence.txt"
$installerEvidence = Join-Path $ArtifactsDirectory "installer-evidence.txt"
$speechEvidence = Join-Path $ArtifactsDirectory "speech-evidence.txt"
$nvdaLog = Join-Path $ArtifactsDirectory "nvda.log"
$nvdaStdout = Join-Path $ArtifactsDirectory "nvda.stdout.log"
$nvdaStderr = Join-Path $ArtifactsDirectory "nvda.stderr.log"
$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
$installerPath = Join-Path $tempRoot "nvda_2026.1.1.exe"
$configPath = Join-Path $ArtifactsDirectory "nvda-config"
$cleanupEvidence = Join-Path $ArtifactsDirectory "cleanup-evidence.txt"
$uninstallKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NVDA"
$uninstallKeyWow64 = "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\NVDA"
$nvdaPath = $null
$uninstallerPath = $null
$installDirectory = $null
$ownsInstall = $false
$nvdaStarted = $false
$nvdaStopped = $false
$installRemoved = $false
$ownedNvdaPids = @()

Assert-InteractiveDesktop -EvidencePath $sessionEvidence
"" | Set-Content -LiteralPath $cleanupEvidence -Encoding utf8

$existingNvda = @(Get-Process -Name nvda -ErrorAction SilentlyContinue)
if ($existingNvda.Count -ne 0) {
    throw "A pre-existing NVDA process is running. Use a clean dedicated runner; the smoke will not stop a user's screen reader."
}
if ((Test-Path -LiteralPath $uninstallKey) -or (Test-Path -LiteralPath $uninstallKeyWow64) -or
    (Test-Path -LiteralPath (Join-Path ${env:ProgramFiles} "NVDA\nvda.exe"))) {
    throw "A pre-existing NVDA installation is present. Use a clean dedicated runner; the smoke will not overwrite it."
}

try {
    Invoke-WebRequest -Uri $InstallerUrl -OutFile $installerPath -UseBasicParsing
    $actualHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $installerSignature = Get-AuthenticodeSignature -LiteralPath $installerPath
    $installerSignerSubject = if ($installerSignature.SignerCertificate) {
        $installerSignature.SignerCertificate.Subject
    } else {
        ""
    }
    $installerSignerThumbprint = if ($installerSignature.SignerCertificate) {
        $installerSignature.SignerCertificate.Thumbprint
    } else {
        ""
    }
    "url=$InstallerUrl" | Out-File -FilePath $installerEvidence -Encoding utf8
    "sha256=$actualHash" | Out-File -FilePath $installerEvidence -Append -Encoding utf8
    "signature_status=$($installerSignature.Status)" | Out-File -FilePath $installerEvidence -Append -Encoding utf8
    "signature_subject=$installerSignerSubject" | Out-File -FilePath $installerEvidence -Append -Encoding utf8
    "signature_thumbprint=$installerSignerThumbprint" | Out-File -FilePath $installerEvidence -Append -Encoding utf8
    if ($actualHash -ne $InstallerSha256) {
        throw "NVDA installer SHA-256 is $actualHash, expected $InstallerSha256."
    }
    if ($installerSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $installerSignerThumbprint -ne $InstallerSignerThumbprint) {
        throw "NVDA installer does not have the pinned valid NV Access Authenticode signature."
    }

    $ownsInstall = $true
    $installer = Start-Process -FilePath $installerPath -ArgumentList @(
        "--install-silent",
        "--enable-start-on-logon=False"
    ) -PassThru -Wait
    if ($installer.ExitCode -ne 0) {
        throw "NVDA silent installation failed with exit code $($installer.ExitCode)."
    }
    $registered = Wait-Until -TimeoutSeconds 60 -Condition {
        return (Test-Path -LiteralPath $uninstallKey) -or (Test-Path -LiteralPath $uninstallKeyWow64)
    }
    if (-not $registered) {
        throw "NVDA silent installation did not create its uninstall registration."
    }

    $activeUninstallKey = if (Test-Path -LiteralPath $uninstallKey) { $uninstallKey } else { $uninstallKeyWow64 }
    $registration = Get-ItemProperty -LiteralPath $activeUninstallKey
    $installLocationProperty = $registration.PSObject.Properties["InstallLocation"]
    if ($installLocationProperty) {
        $installDirectory = $installLocationProperty.Value
    }
    if (-not $installDirectory) {
        $uninstallStringProperty = $registration.PSObject.Properties["UninstallString"]
        if (-not $uninstallStringProperty -or -not $uninstallStringProperty.Value) {
            throw "NVDA uninstall registration has neither InstallLocation nor UninstallString."
        }
        $installDirectory = Split-Path -Parent $uninstallStringProperty.Value.Trim('"')
    }
    $nvdaPath = Join-Path $installDirectory "nvda.exe"
    $uninstallerPath = Join-Path $installDirectory "uninstall.exe"
    if (-not (Test-Path -LiteralPath $nvdaPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $uninstallerPath -PathType Leaf)) {
        throw "Installed NVDA files are incomplete under $installDirectory."
    }

    $installedVersion = (Get-Item -LiteralPath $nvdaPath).VersionInfo.ProductVersion
    if ($installedVersion -notmatch '^2026\.1\.1(?:\.|$)') {
        throw "Installed NVDA version is $installedVersion, expected 2026.1.1."
    }
    $installedSignature = Get-AuthenticodeSignature -LiteralPath $nvdaPath
    $installedSignerSubject = if ($installedSignature.SignerCertificate) {
        $installedSignature.SignerCertificate.Subject
    } else {
        ""
    }
    if ($installedSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw "Installed NVDA executable does not have a valid Authenticode signature."
    }
    "uninstall_key=$activeUninstallKey" | Out-File -FilePath $installerEvidence -Append -Encoding utf8
    "installed_path=$nvdaPath" | Out-File -FilePath $installerEvidence -Append -Encoding utf8
    "installed_version=$installedVersion" | Out-File -FilePath $installerEvidence -Append -Encoding utf8
    "installed_signature_status=$($installedSignature.Status)" | Out-File -FilePath $installerEvidence -Append -Encoding utf8
    "installed_signature_subject=$installedSignerSubject" | Out-File -FilePath $installerEvidence -Append -Encoding utf8

    New-Item -ItemType Directory -Force -Path $configPath | Out-Null
    @(
        "[general]",
        "saveConfigurationOnExit = False",
        "startupNotification = False",
        "allowUsageStats = False",
        "[update]",
        "autoCheck = False",
        "[speech]",
        "synth = silence"
    ) | Set-Content -LiteralPath (Join-Path $configPath "nvda.ini") -Encoding utf8

    $nvdaLauncher = Start-Process -FilePath $nvdaPath -ArgumentList @(
        "-m",
        "-d",
        "-n", "en",
        "-c", "`"$configPath`"",
        "-f", "`"$nvdaLog`"",
        "-l", "12"
    ) -PassThru -RedirectStandardOutput $nvdaStdout -RedirectStandardError $nvdaStderr
    $nvdaStarted = $true

    $nvdaReady = Wait-Until -TimeoutSeconds $StartupTimeoutSeconds -Condition {
        if (-not (Test-Path -LiteralPath $nvdaLog -PathType Leaf)) {
            return $false
        }
        $log = Get-Content -LiteralPath $nvdaLog -Raw
        return $log -match 'Starting NVDA version 2026\.1\.1' -and
            $log -match 'Loaded synthDriver silence' -and
            $log -match 'NVDA initialized'
    }
    if (-not $nvdaReady) {
        throw "Installed NVDA did not initialize its silence synth and event loop within $StartupTimeoutSeconds seconds."
    }

    $runnerSession = (Get-Process -Id $PID).SessionId
    $nvdaProcesses = @(Get-Process -Name nvda -ErrorAction SilentlyContinue |
        Where-Object { $_.SessionId -eq $runnerSession })
    if ($nvdaProcesses.Count -eq 0) {
        throw "Installed NVDA did not remain in the runner's interactive session $runnerSession."
    }
    $ownedNvdaPids = @($nvdaProcesses | ForEach-Object { $_.Id })
    "nvda_launcher_pid=$($nvdaLauncher.Id)" | Out-File -FilePath $sessionEvidence -Append -Encoding utf8
    foreach ($process in $nvdaProcesses) {
        "nvda_pid=$($process.Id) nvda_session=$($process.SessionId)" |
            Out-File -FilePath $sessionEvidence -Append -Encoding utf8
    }

    Invoke-AccessibilityRun -Name "raw" -Checks @("all") -ProbeArguments @() -NoBuild $false
    Invoke-AccessibilityRun -Name "lesson-42" -Checks @("focus") `
        -ProbeArguments @("--text", "name:Lesson 42", "--no-close") -NoBuild $true
    Invoke-AccessibilityRun -Name "study-mode" -Checks @("focus", "toggle") `
        -ProbeArguments @("--text", "name:Study mode", "--no-close") -NoBuild $true

    $quit = Start-Process -FilePath $nvdaPath -ArgumentList @("--quit") -PassThru -Wait
    $nvdaStopped = Wait-Until -TimeoutSeconds 20 -Condition {
        return @(Get-Process -Name nvda -ErrorAction SilentlyContinue).Count -eq 0
    }
    "quit_exit_code=$($quit.ExitCode)" | Out-File -FilePath $sessionEvidence -Append -Encoding utf8
    "nvda_stopped=$nvdaStopped" | Out-File -FilePath $sessionEvidence -Append -Encoding utf8
    if (-not $nvdaStopped) {
        throw "Installed NVDA did not stop cleanly after the smoke."
    }

    if (-not (Test-Path -LiteralPath $nvdaLog -PathType Leaf)) {
        throw "Installed NVDA did not produce a log."
    }
    $nvdaLogContent = Get-Content -LiteralPath $nvdaLog -Raw
    if ($nvdaLogContent -match '(?im)Unhandled exception|Traceback \(most recent call last\)') {
        throw "NVDA logged an unhandled exception or Python traceback during the fixture runs."
    }
    $speechBlocks = @(Get-SpeechBlocks -LogPath $nvdaLog)
    $lessonBlock = Find-SpeechBlock -Blocks $speechBlocks -Tokens @("Lesson 42", "list item", "42 of 1000")
    if ($lessonBlock -lt 0) {
        throw "NVDA did not speak Lesson 42 as list item 42 of 1000 in one speech record."
    }
    $studyBlock = Find-SpeechBlock -Blocks $speechBlocks -Tokens @("Study mode", "check box")
    if ($studyBlock -lt 0) {
        throw "NVDA did not speak Study mode as a check box."
    }
    $checkedBlock = Find-SpeechBlock -Blocks $speechBlocks -Tokens @("'checked'") `
        -ExcludedTokens @("not checked") -StartIndex ($studyBlock + 1)
    if ($checkedBlock -lt 0) {
        throw "NVDA did not speak the checked state after the Study mode Toggle action."
    }
    @(
        $speechBlocks[$lessonBlock],
        $speechBlocks[$studyBlock],
        $speechBlocks[$checkedBlock]
    ) | Set-Content -LiteralPath $speechEvidence -Encoding utf8
} finally {
    if ($nvdaStarted -and -not $nvdaStopped -and $nvdaPath -and
        (Test-Path -LiteralPath $nvdaPath -PathType Leaf)) {
        $quitExitCode = -1
        try {
            $quit = Start-Process -FilePath $nvdaPath -ArgumentList @("--quit") -PassThru -Wait
            $quitExitCode = $quit.ExitCode
        } catch {
            $_ | Out-String | Out-File -FilePath $cleanupEvidence -Encoding utf8
        }
        $nvdaStopped = Wait-Until -TimeoutSeconds 20 -Condition {
            return @(Get-Process -Name nvda -ErrorAction SilentlyContinue).Count -eq 0
        }
        if (-not $nvdaStopped) {
            foreach ($ownedPid in $ownedNvdaPids) {
                Stop-Process -Id $ownedPid -Force -ErrorAction SilentlyContinue
            }
            $nvdaStopped = Wait-Until -TimeoutSeconds 5 -Condition {
                return @(Get-Process -Name nvda -ErrorAction SilentlyContinue).Count -eq 0
            }
        }
        "cleanup_quit_exit_code=$quitExitCode" | Out-File -FilePath $cleanupEvidence -Append -Encoding utf8
        "cleanup_nvda_stopped=$nvdaStopped" | Out-File -FilePath $cleanupEvidence -Append -Encoding utf8
    }
    if ($ownsInstall) {
        if (-not $installDirectory) {
            $activeUninstallKey = if (Test-Path -LiteralPath $uninstallKey) {
                $uninstallKey
            } elseif (Test-Path -LiteralPath $uninstallKeyWow64) {
                $uninstallKeyWow64
            } else {
                $null
            }
            if ($activeUninstallKey) {
                $registration = Get-ItemProperty -LiteralPath $activeUninstallKey
                $installLocationProperty = $registration.PSObject.Properties["InstallLocation"]
                if ($installLocationProperty) {
                    $installDirectory = $installLocationProperty.Value
                }
                if (-not $installDirectory) {
                    $uninstallStringProperty = $registration.PSObject.Properties["UninstallString"]
                    if ($uninstallStringProperty -and $uninstallStringProperty.Value) {
                        $installDirectory = Split-Path -Parent $uninstallStringProperty.Value.Trim('"')
                    }
                }
            }
        }
        if (-not $uninstallerPath -and $installDirectory) {
            $uninstallerPath = Join-Path $installDirectory "uninstall.exe"
        }
        if ($uninstallerPath -and (Test-Path -LiteralPath $uninstallerPath -PathType Leaf)) {
            $uninstaller = Start-Process -FilePath $uninstallerPath -ArgumentList @("/S") -PassThru -Wait
            "uninstall_exit_code=$($uninstaller.ExitCode)" | Out-File -FilePath $cleanupEvidence -Append -Encoding utf8
        }
        $installRemoved = Wait-Until -TimeoutSeconds 60 -Condition {
            $registrationGone = -not (Test-Path -LiteralPath $uninstallKey) -and
                -not (Test-Path -LiteralPath $uninstallKeyWow64)
            $directoryGone = -not $installDirectory -or -not (Test-Path -LiteralPath $installDirectory)
            return $registrationGone -and $directoryGone
        }
        "install_removed=$installRemoved" | Out-File -FilePath $cleanupEvidence -Append -Encoding utf8
        if (-not $installRemoved) {
            throw "The NVDA installation created by the smoke was not removed cleanly."
        }
    }
}

if (-not $nvdaStopped) {
    throw "Installed NVDA did not stop cleanly after the smoke."
}
if (-not $installRemoved) {
    throw "Installed NVDA was not removed after the smoke."
}
Get-Content -LiteralPath $installerEvidence | Write-Host
Get-Content -LiteralPath $sessionEvidence | Write-Host
Get-Content -LiteralPath $speechEvidence | Write-Host
Get-Content -LiteralPath $cleanupEvidence | Write-Host
Write-Host "PASS: installed NVDA announced virtual position and Toggle state from the UI Automation fixture"
