[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$HostExe,

    [string]$ProbeExe = (Join-Path $PSScriptRoot "windows_uia_probe.exe"),
    [string]$ProbeSource = (Join-Path $PSScriptRoot "windows_uia_probe.cpp"),
    [string[]]$Checks = @("all"),
    [string[]]$HostArguments = @(),
    [string[]]$ProbeArguments = @(),
    [int]$StartupTimeoutSeconds = 20,
    [int]$ProbeTimeoutSeconds = 90,
    [int]$CheckTimeoutMilliseconds = 8000,
    [int]$PostProbeDelayMilliseconds = 0,
    [int]$ShutdownTimeoutSeconds = 10,
    [string]$ArtifactsDirectory = (Join-Path $PSScriptRoot "artifacts/windows-accessibility"),
    [switch]$NoBuild,
    [switch]$AllowNonInteractive
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

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

function Stop-ProcessTree {
    param([System.Diagnostics.Process]$Process)

    if ($null -eq $Process) {
        return
    }
    try {
        $Process.Refresh()
        if ($Process.HasExited) {
            return
        }
        [void]$Process.CloseMainWindow()
        if (-not (Wait-Until -TimeoutSeconds $ShutdownTimeoutSeconds -Condition {
            $Process.Refresh()
            return $Process.HasExited
        })) {
            & taskkill.exe /PID $Process.Id /T /F 2>&1 | Out-File -FilePath (Join-Path $ArtifactsDirectory "taskkill.log") -Encoding utf8
        }
    } catch {
        $_ | Out-String | Out-File -FilePath (Join-Path $ArtifactsDirectory "cleanup-error.log") -Encoding utf8
    }
}

function Write-ProcessDiagnostics {
    param(
        [System.Diagnostics.Process]$Process,
        [System.Diagnostics.Process]$ProbeProcess
    )

    $diagnostics = Join-Path $ArtifactsDirectory "diagnostics.txt"
    "timestamp=$([DateTime]::UtcNow.ToString('o'))" | Out-File -FilePath $diagnostics -Encoding utf8
    "user_interactive=$([Environment]::UserInteractive)" | Out-File -FilePath $diagnostics -Append -Encoding utf8
    "runner_session=$((Get-Process -Id $PID).SessionId)" | Out-File -FilePath $diagnostics -Append -Encoding utf8
    if ($null -ne $Process) {
        try {
            $Process.Refresh()
            "host_pid=$($Process.Id)" | Out-File -FilePath $diagnostics -Append -Encoding utf8
            "host_exited=$($Process.HasExited)" | Out-File -FilePath $diagnostics -Append -Encoding utf8
            "host_window_handle=$($Process.MainWindowHandle)" | Out-File -FilePath $diagnostics -Append -Encoding utf8
            "host_window_title=$($Process.MainWindowTitle)" | Out-File -FilePath $diagnostics -Append -Encoding utf8
            & tasklist.exe /V /FI "PID eq $($Process.Id)" 2>&1 | Out-File -FilePath $diagnostics -Append -Encoding utf8
            Get-CimInstance Win32_Process -Filter "ProcessId = $($Process.Id)" |
                Select-Object ProcessId, ParentProcessId, ExecutablePath, CommandLine |
                Format-List | Out-File -FilePath $diagnostics -Append -Encoding utf8
        } catch {
            $_ | Out-String | Out-File -FilePath $diagnostics -Append -Encoding utf8
        }
    }
    if ($null -ne $ProbeProcess) {
        try {
            $ProbeProcess.Refresh()
            "probe_pid=$($ProbeProcess.Id)" | Out-File -FilePath $diagnostics -Append -Encoding utf8
            "probe_session=$($ProbeProcess.SessionId)" | Out-File -FilePath $diagnostics -Append -Encoding utf8
            "probe_exited=$($ProbeProcess.HasExited)" | Out-File -FilePath $diagnostics -Append -Encoding utf8
        } catch {
            $_ | Out-String | Out-File -FilePath $diagnostics -Append -Encoding utf8
        }
    }
}

function Build-Probe {
    $sourcePath = (Resolve-Path -LiteralPath $ProbeSource).Path
    $probeDirectory = Split-Path -Parent $ProbeExe
    New-Item -ItemType Directory -Force -Path $probeDirectory | Out-Null
    $probePath = [System.IO.Path]::GetFullPath($ProbeExe)

    $compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($null -eq $compiler) {
        $compiler = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
    }
    if ($null -eq $compiler) {
        throw "Neither cl.exe nor clang-cl.exe is on PATH. Run from a Visual Studio Developer PowerShell."
    }

    $objectPath = Join-Path $ArtifactsDirectory "windows_uia_probe.obj"
    $compileLog = Join-Path $ArtifactsDirectory "compile.log"
    $arguments = @(
        "/nologo",
        "/EHsc",
        "/std:c++17",
        "/W4",
        "/DUNICODE",
        "/D_UNICODE",
        "/Fo$objectPath",
        "/Fe$probePath",
        $sourcePath,
        "/link",
        "ole32.lib",
        "oleaut32.lib",
        "uiautomationcore.lib"
    )
    & $compiler.Source @arguments 2>&1 | Tee-Object -FilePath $compileLog
    if ($LASTEXITCODE -ne 0) {
        throw "UI Automation probe compilation failed with exit code $LASTEXITCODE. See $compileLog"
    }
    return $probePath
}

if ($env:OS -ne "Windows_NT") {
    throw "windows-accessibility-smoke.ps1 must run on Windows."
}
if (-not $AllowNonInteractive) {
    if (-not [Environment]::UserInteractive) {
        throw "UI Automation requires an interactive desktop. Use an interactive self-hosted runner."
    }
    if ((Get-Process -Id $PID).SessionId -eq 0) {
        throw "UI Automation cannot be validated from Windows session 0. Use an interactive user session."
    }
}
if ($StartupTimeoutSeconds -le 0 -or $ProbeTimeoutSeconds -le 0 -or
    $CheckTimeoutMilliseconds -le 0 -or $PostProbeDelayMilliseconds -lt 0 -or
    $ShutdownTimeoutSeconds -le 0) {
    throw "All timeout values must be positive and PostProbeDelayMilliseconds must be nonnegative."
}

New-Item -ItemType Directory -Force -Path $ArtifactsDirectory | Out-Null
$ArtifactsDirectory = (Resolve-Path -LiteralPath $ArtifactsDirectory).Path
$hostPath = (Resolve-Path -LiteralPath $HostExe).Path
$probePath = [System.IO.Path]::GetFullPath($ProbeExe)
if (-not $NoBuild) {
    $probePath = Build-Probe
} elseif (-not (Test-Path -LiteralPath $probePath -PathType Leaf)) {
    throw "Probe executable does not exist: $probePath"
}

$hostStdout = Join-Path $ArtifactsDirectory "host.stdout.log"
$hostStderr = Join-Path $ArtifactsDirectory "host.stderr.log"
$probeStdout = Join-Path $ArtifactsDirectory "probe.stdout.log"
$probeStderr = Join-Path $ArtifactsDirectory "probe.stderr.log"
$appProcess = $null
$probeProcess = $null
$exitCode = 1

try {
    $appProcess = Start-Process -FilePath $hostPath -ArgumentList $HostArguments -PassThru `
        -RedirectStandardOutput $hostStdout -RedirectStandardError $hostStderr

    $windowReady = Wait-Until -TimeoutSeconds $StartupTimeoutSeconds -Condition {
        $appProcess.Refresh()
        if ($appProcess.HasExited) {
            return $false
        }
        return $appProcess.MainWindowHandle -ne 0
    }
    if (-not $windowReady) {
        $appProcess.Refresh()
        if ($appProcess.HasExited) {
            throw "Accessibility smoke host exited during startup with code $($appProcess.ExitCode)."
        }
        throw "Accessibility smoke host did not create a top-level window within $StartupTimeoutSeconds seconds."
    }

    $arguments = @(
        "--pid", [string]$appProcess.Id,
        "--timeout-ms", [string]$CheckTimeoutMilliseconds,
        "--check", ($Checks -join ",")
    ) + $ProbeArguments
    $probeProcess = Start-Process -FilePath $probePath -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput $probeStdout -RedirectStandardError $probeStderr
    Write-ProcessDiagnostics -Process $appProcess -ProbeProcess $probeProcess

    $probeFinished = Wait-Until -TimeoutSeconds $ProbeTimeoutSeconds -Condition {
        $probeProcess.Refresh()
        return $probeProcess.HasExited
    }
    if (-not $probeFinished) {
        & taskkill.exe /PID $probeProcess.Id /T /F 2>&1 | Out-File -FilePath (Join-Path $ArtifactsDirectory "probe-timeout-taskkill.log") -Encoding utf8
        throw "UI Automation probe exceeded its $ProbeTimeoutSeconds second process timeout."
    }

    $probeProcess.Refresh()
    Get-Content -LiteralPath $probeStdout -ErrorAction SilentlyContinue | Write-Host
    Get-Content -LiteralPath $probeStderr -ErrorAction SilentlyContinue | Write-Host
    if ($probeProcess.ExitCode -ne 0) {
        throw "UI Automation probe failed with exit code $($probeProcess.ExitCode)."
    }
    if ($PostProbeDelayMilliseconds -gt 0) {
        Start-Sleep -Milliseconds $PostProbeDelayMilliseconds
    }
    $exitCode = 0
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-ProcessDiagnostics -Process $appProcess -ProbeProcess $probeProcess
    if (Test-Path -LiteralPath $probeStdout) {
        Get-Content -LiteralPath $probeStdout | Write-Host
    }
    if (Test-Path -LiteralPath $probeStderr) {
        Get-Content -LiteralPath $probeStderr | Write-Host
    }
} finally {
    if ($null -ne $probeProcess) {
        try {
            $probeProcess.Refresh()
            if (-not $probeProcess.HasExited) {
                & taskkill.exe /PID $probeProcess.Id /T /F | Out-Null
            }
        } catch {
        }
    }
    Stop-ProcessTree -Process $appProcess
}

exit $exitCode
