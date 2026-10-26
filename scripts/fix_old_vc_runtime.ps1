# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Replaces the Visual C++ runtime files of the ROS 2 Humble Windows distribution that are
    older than version 14.40 with the same files of a newer runtime, so that the QarOS library
    loaded by rviz2.exe runs on a runtime it supports.

.DESCRIPTION
    The official ROS 2 Humble Windows distribution ships the Visual C++ runtime 14.29
    (msvcp140.dll, vcruntime140.dll and the other files of that family) in its bin folder, next
    to rviz2.exe. Windows searches the folder of the executable first, so RViz and every library
    it loads, the QarOS library included, run on that copy, and a runtime that a process has
    loaded stays in use until the process ends. The QarOS library needs the runtime 14.40 or
    newer: on the shipped copy it faults inside qar_library_init, with an access violation in
    MSVCP140.dll.

    The script
      1. looks below <Ros2Root> for the files of the runtime family (msvcp140.dll, msvcp140_1.dll,
         msvcp140_2.dll, msvcp140_atomic_wait.dll, msvcp140_codecvt_ids.dll, vcruntime140.dll,
         vcruntime140_1.dll, vcruntime140_threads.dll, concrt140.dll, vcomp140.dll) in every
         folder and keeps those whose file version is older than -MinimumVersion. When there are
         none, it says so and exits with 0;
      2. takes the source folder: of %SystemRoot%\System32 and, when -SdkRoot is given,
         <SdkRoot>\bin, the one whose msvcp140.dll has the highest version (System32 when they
         are equal). That version must reach -MinimumVersion; otherwise the script stops with
         exit status 1 and names the two remedies: the current "Microsoft Visual C++
         Redistributable" for x64 (winget package Microsoft.VCRedist.2015+.x64) or -SdkRoot;
      3. refuses, with exit status 1 and nothing changed, when rviz2 is running or when a file
         to replace cannot be opened for writing, which is the case while a running process has
         it loaded. It names the file and, when it finds one, the process; it never stops a
         process;
      4. keeps each original as <name>.vcrt.orig next to it (an existing backup is never
         overwritten) and copies the file of the source over it. A file is replaced only when the
         source holds a file of the same name with a version of at least -MinimumVersion, and a
         file the distribution does not have is never added.
    Each replaced file is printed with its old and new version and the source folder.

    Idempotent: a second run finds nothing older than the minimum and changes nothing. The exit
    status is 0 when no file is older than -MinimumVersion afterwards, and 1 otherwise (a
    refusal, no usable source, a copy that failed, or a file the source cannot replace).
    Start rviz2 again after the repair: a running process keeps the runtime it loaded.

.PARAMETER Ros2Root
    Root of the ROS 2 Windows distribution. Default: the environment variable
    ROS2_INSTALL_ROOT. Required.

.PARAMETER SdkRoot
    Optional. Root of the QarOS SDK; its bin folder is a second source of the newer runtime.
    Default: the environment variable QAR_SDK_ROOT. Without either, or when the folder does not
    exist yet (the SDK is unpacked later), only the system folder is a source.

.PARAMETER MinimumVersion
    File version below which a runtime file is replaced. Default: 14.40.

.EXAMPLE
    ./scripts/fix_old_vc_runtime.ps1 -Ros2Root C:\qar\deps\ros2_humble\ros2-windows

.EXAMPLE
    ./scripts/fix_old_vc_runtime.ps1 -Ros2Root C:\qar\deps\ros2_humble\ros2-windows -SdkRoot C:\QarOS
#>
param(
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root,
    [string]$SdkRoot,
    [version]$MinimumVersion = [version]'14.40'
)
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
    -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'
$SdkRoot = Resolve-QarLocation -Value $SdkRoot -Parameter 'SdkRoot' -EnvVar 'QAR_SDK_ROOT' `
    -What 'QarOS SDK' -Marker 'bin' -Section 'The QarOS SDK' -Optional -AllowMissing

$backupTag = 'vcrt'
$family = @(
    'concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
    'msvcp140_codecvt_ids.dll', 'vcomp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll',
    'vcruntime140_threads.dll'
)

function Stop-Repair {
    param([string[]]$Lines)
    foreach ($line in $Lines) { Write-Host $line -ForegroundColor Red }
    exit 1
}

# Four-part file version from the version resource of the file.
function Get-RuntimeVersion {
    param([string]$Path)
    $info = (Get-Item -LiteralPath $Path).VersionInfo
    return [version]::new($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart)
}

# Why the file cannot be opened for writing, or $null when it can. Opening without sharing fails
# while any process has the file loaded as a module or open.
function Get-WriteBlock {
    param([string]$Path)
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        $stream.Dispose()
        return $null
    } catch {
        $problem = $_.Exception
        if ($null -ne $problem.InnerException) { $problem = $problem.InnerException }
        if ($problem -is [System.UnauthorizedAccessException]) { return 'access denied (read-only file or missing permission)' }
        if ($problem -is [System.IO.IOException] -and (($problem.HResult -band 0xFFFF) -in 32, 33)) { return 'in use by another process' }
        return $problem.Message
    }
}

# Path -> names and ids of the processes that have the file loaded as a module.
function Get-LoadingProcess {
    param([string[]]$Paths)
    $holders = @{}
    foreach ($process in Get-Process) {
        try { $modules = @($process.Modules) } catch { continue }
        foreach ($module in $modules) {
            foreach ($path in $Paths) {
                if ($module.FileName -eq $path) {
                    if (-not $holders.ContainsKey($path)) { $holders[$path] = @() }
                    $holders[$path] += "$($process.ProcessName) (pid $($process.Id))"
                }
            }
        }
    }
    return $holders
}

# --- the runtime files of the distribution that are older than the minimum --------------------
# -Filter works with -LiteralPath in both PowerShell editions; -Include does not in Windows PowerShell.
$found = @(Get-ChildItem -LiteralPath $Ros2Root -Recurse -File -Filter '*140*.dll' -ErrorAction SilentlyContinue |
    Where-Object { $family -contains $_.Name } |
    Sort-Object FullName)
$old = @()
foreach ($file in $found) {
    $version = Get-RuntimeVersion $file.FullName
    if ($version -lt $MinimumVersion) {
        $old += [pscustomobject]@{
            Path     = $file.FullName
            Name     = $file.Name
            Relative = $file.FullName.Substring($Ros2Root.Length).TrimStart('\', '/')
            Version  = $version
        }
    }
}
if ($old.Count -eq 0) {
    if ($found.Count -eq 0) {
        Write-Host "No Visual C++ runtime file found below $Ros2Root -- nothing to do."
    } else {
        Write-Host "None of the $($found.Count) Visual C++ runtime file(s) below $Ros2Root is older than $MinimumVersion -- nothing to do."
    }
    exit 0
}

# --- the source of the newer files ------------------------------------------------------------
$systemDir = Join-Path $env:SystemRoot 'System32'
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    # A 32-bit host is redirected from System32 to the 32-bit files; Sysnative is the real folder.
    $systemDir = Join-Path $env:SystemRoot 'Sysnative'
}
$candidateDirs = @($systemDir)
if ($SdkRoot) { $candidateDirs += (Join-Path $SdkRoot 'bin') }

# The first strictly higher version wins, so on a tie the system folder stays.
$source = $null
$candidateNotes = @()
foreach ($dir in $candidateDirs) {
    $probe = Join-Path $dir 'msvcp140.dll'
    if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
        $candidateNotes += ("  {0}: no msvcp140.dll" -f $dir)
        continue
    }
    $version = Get-RuntimeVersion $probe
    $candidateNotes += ("  {0}: msvcp140.dll {1}" -f $dir, $version)
    if ($null -eq $source -or $version -gt $source.Version) {
        $source = [pscustomobject]@{ Dir = $dir; Version = $version }
    }
}
if ($null -eq $source -or $source.Version -lt $MinimumVersion) {
    Stop-Repair (@("Refused: no Visual C++ runtime of version $MinimumVersion or newer to copy from. Source folders:") +
        $candidateNotes +
        @("Install the current Microsoft Visual C++ Redistributable for x64 (winget install --id Microsoft.VCRedist.2015+.x64)",
          "or pass -SdkRoot <QarOS SDK folder whose bin holds a newer runtime>. Nothing was changed."))
}

$plan = @()
$unresolved = @()
foreach ($item in $old) {
    $from = Join-Path $source.Dir $item.Name
    $newVersion = $null
    if (Test-Path -LiteralPath $from -PathType Leaf) { $newVersion = Get-RuntimeVersion $from }
    if ($null -ne $newVersion -and $newVersion -ge $MinimumVersion) {
        $plan += [pscustomobject]@{
            Path = $item.Path; Name = $item.Name; Relative = $item.Relative
            Old = $item.Version; New = $newVersion; From = $from
        }
    } else {
        $unresolved += $item
    }
}
Write-Host ("{0} Visual C++ runtime file(s) older than {1} below {2}; source {3} (msvcp140.dll {4})." -f
    $old.Count, $MinimumVersion, $Ros2Root, $source.Dir, $source.Version)

# --- refusals, before anything changes --------------------------------------------------------
if ($plan.Count -gt 0) {
    $running = @(Get-Process -Name rviz2 -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        $ids = ($running | ForEach-Object { $_.Id }) -join ', '
        Stop-Repair @("Refused: rviz2 is running (pid $ids). Close RViz and run the script again; the script never stops a process.",
                      "Nothing was changed.")
    }

    $blocked = @()
    foreach ($item in $plan) {
        $reason = Get-WriteBlock $item.Path
        if ($reason) { $blocked += [pscustomobject]@{ Item = $item; Reason = $reason } }
    }
    if ($blocked.Count -gt 0) {
        Write-Host "Looking for the processes that have the file(s) loaded (takes several seconds) ..."
        $holders = Get-LoadingProcess @($blocked | ForEach-Object { $_.Item.Path })
        $lines = @("Refused: $($blocked.Count) file(s) cannot be opened for writing, so a running program has them loaded or open:")
        foreach ($entry in $blocked) {
            $lines += "  $($entry.Item.Relative): $($entry.Reason)"
            if ($holders.ContainsKey($entry.Item.Path)) {
                $lines += "    loaded by $($holders[$entry.Item.Path] -join ', ')"
            } else {
                $lines += "    no process with this file loaded was found; another program may hold it open"
            }
        }
        $lines += "Close the program(s) and run the script again; the script never stops a process. Nothing was changed."
        Stop-Repair $lines
    }

    # --- the replacement ----------------------------------------------------------------------
    $replaced = 0
    foreach ($item in $plan) {
        $backup = "$($item.Path).$backupTag.orig"
        try {
            if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $item.Path -Destination $backup }
            Copy-Item -LiteralPath $item.From -Destination $item.Path -Force
            $after = Get-RuntimeVersion $item.Path
            if ($after -ne $item.New) { throw "the file reads version $after after copying" }
        } catch {
            Stop-Repair @("Stopped at $($item.Relative): $($_.Exception.Message)",
                          "The original is kept as $backup. $replaced of $($plan.Count) file(s) were replaced before the failure;",
                          "run the script again once the cause is gone (files already replaced are skipped).")
        }
        Write-Host ("  replaced {0}  {1} -> {2}  (from {3})" -f $item.Relative, $item.Old, $item.New, $source.Dir)
        $replaced++
    }
    $lead = if ($unresolved.Count -eq 0) { 'Done. Replaced' } else { 'Replaced' }
    Write-Host ""
    Write-Host "$lead $replaced of $($old.Count) file(s); each original is kept as <name>.$backupTag.orig next to it."
    if ($unresolved.Count -eq 0) { Write-Host "Start rviz2 again: a running process keeps the runtime it loaded." }
}

if ($unresolved.Count -gt 0) {
    Write-Host ""
    Write-Host "Not replaced: the source folder $($source.Dir) holds no file of that name with version $MinimumVersion or newer:" -ForegroundColor Red
    foreach ($item in $unresolved) { Write-Host "  $($item.Relative)  $($item.Version)" -ForegroundColor Red }
    Write-Host "Install the current Microsoft Visual C++ Redistributable for x64 or pass -SdkRoot with the folder of a QarOS SDK whose bin holds them." -ForegroundColor Red
    exit 1
}
exit 0
