# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Patches the RViz2 binaries of the ROS 2 Humble Windows distribution so that they import
    the plain Qt5 DLL names (Qt5Core.dll, Qt5Gui.dll, Qt5Widgets.dll) instead of the
    "_conda"-suffixed names the official CI build baked in.

.DESCRIPTION
    The official release-humble-20260220 Windows distribution was built against a conda/pixi
    Qt whose module DLLs are named Qt5Core_conda.dll, Qt5Gui_conda.dll and
    Qt5Widgets_conda.dll. Four binaries import those names:
        rviz2.exe, rviz_common.dll, rviz_rendering.dll, rviz_default_plugins.dll
    The distribution ships no Qt at all, so an external Qt 5.15.2 (for example from
    aqtinstall) must be supplied.

    Supplying the "_conda" names as byte-identical copies of the Qt DLLs does not work: the
    copies' own import tables still reference the plain names (Qt5Widgets_conda.dll ->
    Qt5Gui.dll, Qt5Core.dll), so both a plain Qt5Core.dll and a Qt5Core_conda.dll are loaded
    into the same process. QApplication (built through the Qt5Widgets_conda copy) sets
    QCoreApplication::self in the plain Qt5Core.dll, while rviz_common.dll's
    VisualizerApp::init() reads QCoreApplication::arguments() from the Qt5Core_conda.dll copy,
    whose static is still null. Qt then prints
        "QCoreApplication::arguments: Please instantiate the QApplication object first"
        "QCommandLineParser: argument list cannot be empty ..."
    and rviz2.exe exits with code 1 before any window appears (two Qt copies in one process).

    This script binary-patches the four consumer binaries: in each file it replaces every
    occurrence of the byte strings "Qt5Core_conda.dll", "Qt5Gui_conda.dll" and
    "Qt5Widgets_conda.dll" with the plain name. In the four binaries of the release these
    strings occur only as the names in the import table (1, 3, 2 and 3 names), so the patch
    changes the import names and nothing else. Removing the literal "_conda" (6 characters)
    frees exactly 6 bytes, which are null-padded after the ".dll", so every string keeps its
    exact original byte length: no RVA shifts, no other PE structure changes, and no checksum
    concern (Windows does not validate the optional-header checksum of ordinary user-mode
    DLLs and executables). Afterwards every RViz binary imports the single plain Qt5Core.dll:
    one Qt5Core module and one consistent QCoreApplication::self.

    Idempotent: a file without a "_conda" Qt string is skipped. The first patch of a file keeps
    the original next to it as <name>.conda.orig; when that copy cannot be written, the script
    stops and leaves the file as it is.

.PARAMETER Ros2Root
    Root of the ROS 2 Windows distribution. Default: the environment variable
    ROS2_INSTALL_ROOT. Required.
#>
param(
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root
)
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
    -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'

$bin = Join-Path $Ros2Root "bin"
$targets = @("rviz2.exe", "rviz_common.dll", "rviz_rendering.dll", "rviz_default_plugins.dll")

# Each pair: the _conda name and its plain replacement. Both encoded ASCII, null-terminated.
# The replacement is padded with trailing NULs so total length == original (length-preserving).
$map = @(
    @{ Old = "Qt5Core_conda.dll";    New = "Qt5Core.dll" },
    @{ Old = "Qt5Gui_conda.dll";     New = "Qt5Gui.dll" },
    @{ Old = "Qt5Widgets_conda.dll"; New = "Qt5Widgets.dll" }
)

function Find-Bytes([byte[]]$hay, [byte[]]$needle, [int]$start) {
    for ($i = $start; $i -le $hay.Length - $needle.Length; $i++) {
        $ok = $true
        for ($j = 0; $j -lt $needle.Length; $j++) {
            if ($hay[$i + $j] -ne $needle[$j]) { $ok = $false; break }
        }
        if ($ok) { return $i }
    }
    return -1
}

foreach ($name in $targets) {
    $path = Join-Path $bin $name
    if (-not (Test-Path $path)) { Write-Warning "Not found (skipping): $path"; continue }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $totalPatched = 0

    foreach ($m in $map) {
        $old = [System.Text.Encoding]::ASCII.GetBytes($m.Old)
        # replacement: plain name + NUL padding to match old length exactly
        $newStr = [System.Text.Encoding]::ASCII.GetBytes($m.New)
        $repl = New-Object byte[] ($old.Length)
        [Array]::Copy($newStr, $repl, $newStr.Length)   # remaining bytes are already 0x00

        $idx = 0
        while (($idx = Find-Bytes $bytes $old $idx) -ge 0) {
            [Array]::Copy($repl, 0, $bytes, $idx, $repl.Length)
            $totalPatched++
            $idx += $repl.Length
        }
    }

    if ($totalPatched -gt 0) {
        $backup = "$path.conda.orig"
        if (-not (Test-Path $backup)) {
            # one-time pristine backup; without it the binary is not overwritten
            try {
                Copy-Item -LiteralPath $path -Destination $backup -ErrorAction Stop
            } catch {
                throw ("Cannot keep a copy of $path as ${backup}: $($_.Exception.Message) " +
                    "$name is not patched.")
            }
        }
        [System.IO.File]::WriteAllBytes($path, $bytes)
        Write-Host "Patched $name : $totalPatched '_conda' import name(s) -> plain Qt5 names."
    } else {
        Write-Host "Skipped $name : no '_conda' Qt import names found (already patched)."
    }
}

Write-Host ""
Write-Host "Done. rviz2 binaries now import plain Qt5Core/Qt5Gui/Qt5Widgets DLLs (single Qt copy)."
Write-Host "Ensure the Qt bin folder is on PATH (scripts/env_setup.ps1 handles this)."
