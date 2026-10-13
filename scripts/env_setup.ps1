# Copyright 2026 Quaternar
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Dot-source this script to prepare a PowerShell session for building and running against the
    ROS 2 Humble Windows distribution, Qt 5.15.2 and the project's Python virtual environment.

.DESCRIPTION
    Usage (the locations come from the parameters or from the environment variables below):

        . .\scripts\env_setup.ps1 -Ros2Root <distribution> [-QtRoot <Qt>] [-Venv <venv>]

    The leading ". " is required: a script run without it cannot change the environment of
    the session that started it.

    What it does:
      1. Runs the distribution's local_setup.bat (through cmd.exe), shows what it writes on
         stderr, and imports the resulting environment (PATH, AMENT_PREFIX_PATH,
         CMAKE_PREFIX_PATH, PYTHONPATH, ...) into the current session.
      2. Puts the virtual environment's Scripts folder (Python 3.10, the version the
         distribution's compiled Python modules need) and the Qt bin folder in front of PATH.
      3. Sets QT_QPA_PLATFORM_PLUGIN_PATH and QT_PLUGIN_PATH to the Qt plugins folder. RViz
         is linked against renamed Qt libraries and cannot find the plugin folder the usual
         way (relative to its own DLLs).
      4. Prints what it set.

    The distribution of the official release has gaps that must be repaired once before
    rviz2.exe and the ROS tools start: run the fix_*.ps1 scripts of this folder
    (fetch_dependencies.ps1 downloads the distribution). The section 'ROS 2 Humble for
    Windows' of docs/INSTALL.md leads to the repairs that this repository needs.

    The variables that local_setup.bat sets are copied exactly, whatever code page the
    console uses and whatever letters the folders have. The script restores
    $ErrorActionPreference before it returns and removes the temporary files it writes to
    %TEMP%.

.PARAMETER Ros2Root
    Root of the ROS 2 Humble Windows distribution (the folder with local_setup.bat).
    Default: the environment variable ROS2_INSTALL_ROOT. Required.

.PARAMETER QtRoot
    Qt 5.15.2 msvc2019_64 folder (the folder with bin\Qt5Core.dll). Default: the environment
    variable QAR_QT_ROOT. Without either, the Qt part is skipped.

.PARAMETER Venv
    Python virtual environment (the folder with Scripts\python.exe). Default: the environment
    variable QAR_VENV. Without either, the virtual environment part is skipped.

.EXAMPLE
    . .\scripts\env_setup.ps1 -Ros2Root $env:ROS2_INSTALL_ROOT -QtRoot $env:QAR_QT_ROOT -Venv $env:QAR_VENV
#>
param(
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root,
    [Alias('QtVersionDir')]
    [string]$QtRoot,
    [string]$Venv
)

$qarSavedErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Stop"
try {
    . (Join-Path $PSScriptRoot "qar_locations.ps1")
    $Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
        -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'
    $QtRoot = Resolve-QarLocation -Value $QtRoot -Parameter 'QtRoot' -EnvVar 'QAR_QT_ROOT' `
        -What 'Qt 5.15.2' -Marker 'bin\Qt5Core.dll' -Section 'Qt 5.15.2' -Optional
    $Venv = Resolve-QarLocation -Value $Venv -Parameter 'Venv' -EnvVar 'QAR_VENV' `
        -What 'Python virtual environment' -Marker 'Scripts\python.exe' -Section 'Python environment' -Optional

    $venvScripts = $null
    $qtBin = $null
    $qtPlugins = $null
    if ($Venv) { $venvScripts = Join-Path $Venv "Scripts" } else { Write-Host "Python virtual environment skipped: neither -Venv nor QAR_VENV is set." }
    if ($QtRoot) {
        $qtBin = Join-Path $QtRoot "bin"
        $qtPlugins = Join-Path $QtRoot "plugins"
    } else { Write-Host "Qt skipped: neither -QtRoot nor QAR_QT_ROOT is set." }

    # Put the venv Python and the Qt bin folder on PATH FIRST (so `python` resolves to the
    # venv's 3.10, matching the distribution's compiled ABI, before local_setup.bat's own
    # PATH additions).
    #
    # local_setup.bat prepends to PATH itself. When the CALLER's PATH is already large
    # (after importing the Visual Studio environment, or just from a long user PATH) that
    # line can exceed cmd.exe's 8191-character limit ("The input line is too long").
    # Start the batch file from the short machine PATH instead of the live one, then merge
    # the caller's real PATH back in afterwards so nothing on it is lost.
    $machineBasePath = [Environment]::GetEnvironmentVariable('PATH', 'Machine')
    $pathHead = @()
    if ($venvScripts) { $pathHead += $venvScripts }
    if ($qtBin) { $pathHead += $qtBin }
    $pathHead += $machineBasePath
    Import-QarBatchEnvironment -Batch (Join-Path $Ros2Root 'local_setup.bat') `
        -StartPath ($pathHead -join ';')

    # Qt plugin discovery for the patched RViz binaries (see fix_qt_conda_imports.ps1).
    if ($qtPlugins) {
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtPlugins "platforms"
        $env:QT_PLUGIN_PATH = $qtPlugins
    }

    Write-Host "=== Qar RViz2 environment set up in this PowerShell session ==="
    Write-Host "PATH prepended with: venv Scripts, Qt bin, ROS 2 distribution bin folders (via local_setup.bat)"
    Write-Host "AMENT_PREFIX_PATH: $env:AMENT_PREFIX_PATH"
    Write-Host "QT_QPA_PLATFORM_PLUGIN_PATH: $env:QT_QPA_PLATFORM_PLUGIN_PATH"
    Write-Host ""
    Write-Host "python  -> $(Get-Command python -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)"
    Write-Host ""
    Write-Host "REMINDER: the 'ros2' command-line wrapper exe has a broken shebang (a build-machine path)." -ForegroundColor Yellow
    Write-Host "  Use: python `"$Ros2Root\Scripts\ros2-script.py`" <args>   (NOT ros2.exe / ros2 directly)" -ForegroundColor Yellow
    Write-Host @"
rviz2.exe works: launch it directly (rviz2.exe).
  It needs the repaired distribution: run the fix_*.ps1 scripts of this folder once.
  docs/INSTALL.md, section 'ROS 2 Humble for Windows', leads to the repairs this repository needs.
"@ -ForegroundColor Green
} finally {
    $ErrorActionPreference = $qarSavedErrorAction
}
