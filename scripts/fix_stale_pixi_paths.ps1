# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Creates the absolute build-machine paths that the exported CMake targets of the ROS 2
    Humble Windows binary distribution refer to, so that downstream ament plugins pass CMake's
    generate-step existence checks.

.DESCRIPTION
    The official release-humble-20260220 Windows archive was built on a pixi CI machine, and
    its installed *Export.cmake files hard-code absolute paths under
        C:/pixi_ws/.pixi/envs/default/...
    into imported-target INTERFACE_INCLUDE_DIRECTORIES and link interfaces (eigen3, the conda
    "Library/include", numpy, Qt, mkspecs, python310.lib). Those paths do not exist on a normal
    machine, so when a downstream target pulls them into its link and generate closure, CMake
    fails at the generate step with
        "Imported target ... includes non-existent path ...".

    The plugin does not compile against these third-party trees (it uses Qt through Qt5_DIR and
    the vendored Eigen from fix_missing_eigen.ps1), so the paths only need to exist. The script
    creates that directory tree, fills eigen3 with the vendored Eigen headers, fills numpy with
    real headers when it has a source for them (optional, see -NumpySourceInclude) and copies a
    real python310.lib. The tree is created at the baked location (-PixiRoot, default
    C:\pixi_ws\.pixi\envs\default); the distribution names that path, so it cannot be moved.
    Delete that folder to undo the script. Idempotent.

.PARAMETER PixiRoot
    Baked CI prefix to create. Default: C:\pixi_ws\.pixi\envs\default
.PARAMETER Ros2Root
    Root of the ROS 2 Windows distribution; the eigen3_vendor headers that
    fix_missing_eigen.ps1 wrote there fill the eigen3 folder. Default: the environment
    variable ROS2_INSTALL_ROOT. Required.
.PARAMETER PythonLib
    Path of python310.lib. Default: <base prefix of "py -3.10">\libs\python310.lib.
.PARAMETER NumpySourceInclude
    Path of a folder that holds the numpy headers (the folder with the folder 'numpy' in it,
    '.../numpy/_core/include' or '.../numpy/core/include'). Optional: the numpy folder is
    created either way and stays empty without a source. Without this parameter, the script
    takes the headers from the numpy package of the virtual environment (the environment
    variable QAR_VENV; numpy 2 keeps them in Lib\site-packages\numpy\_core\include, numpy 1 in
    Lib\site-packages\numpy\core\include) and prints a warning when it finds none.
#>
param(
    [string]$PixiRoot = "C:\pixi_ws\.pixi\envs\default",
    [Alias('Ros2DistroRoot', 'Ros2InstallRoot')]
    [string]$Ros2Root,
    [string]$PythonLib = "",
    [string]$NumpySourceInclude = ""
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
    -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'

function Ensure-Dir { param([string]$Path) if (-not (Test-Path $Path)) { New-Item -ItemType Directory -Force -Path $Path | Out-Null; Write-Host "  mkdir $Path" -ForegroundColor Green } }

# --- Directories referenced as INTERFACE_INCLUDE_DIRECTORIES ----------------
$libInclude = Join-Path $PixiRoot "Library\include"
Ensure-Dir $libInclude
Ensure-Dir (Join-Path $PixiRoot "Library\mkspecs\win32-msvc")
foreach ($q in @("qt", "qt\QtWidgets", "qt\QtGui", "qt\QtCore", "qt\QtANGLE")) {
    Ensure-Dir (Join-Path $libInclude $q)
}

# eigen3 (populate with the vendored Eigen headers, if available) ------------
$eigenDst = Join-Path $libInclude "eigen3"
Ensure-Dir $eigenDst
$eigenSrc = Join-Path $Ros2Root "opt\eigen3_vendor\include\eigen3"
if ((Test-Path (Join-Path $eigenSrc "Eigen")) -and -not (Test-Path (Join-Path $eigenDst "Eigen"))) {
    Copy-Item -Path (Join-Path $eigenSrc "*") -Destination $eigenDst -Recurse -Force
    Write-Host "  populated eigen3 headers -> $eigenDst" -ForegroundColor Green
}

# numpy core/include (optional: from -NumpySourceInclude or the numpy of QAR_VENV) -----
$numpyDst = Join-Path $PixiRoot "lib\site-packages\numpy\core\include"
Ensure-Dir $numpyDst
if (-not (Test-Path (Join-Path $numpyDst "numpy"))) {
    $numpySrc = $null
    if ($NumpySourceInclude -and (Test-Path (Join-Path $NumpySourceInclude "numpy"))) {
        $numpySrc = $NumpySourceInclude
    } elseif ($env:QAR_VENV) {
        # numpy 2 keeps its C headers in numpy\_core\include, numpy 1 in numpy\core\include.
        foreach ($sub in "_core", "core") {
            $candidate = Join-Path $env:QAR_VENV "Lib\site-packages\numpy\$sub\include"
            if (Test-Path (Join-Path $candidate "numpy")) { $numpySrc = $candidate; break }
        }
    }
    if ($numpySrc) {
        Copy-Item -Path (Join-Path $numpySrc "*") -Destination $numpyDst -Recurse -Force
        Write-Host "  populated numpy headers -> $numpyDst (from $numpySrc)" -ForegroundColor Green
    } else {
        Write-Host ("  WARNING: no numpy C headers found; pass -NumpySourceInclude if a " +
            "downstream build needs them.") -ForegroundColor Yellow
    }
}

# --- python310.lib (a real link input referenced by *_rosidl_generator_py) --
$libsDir = Join-Path $PixiRoot "libs"
Ensure-Dir $libsDir
$py310lib = Join-Path $libsDir "python310.lib"
if (-not (Test-Path $py310lib)) {
    $srcLib = $PythonLib
    if ([string]::IsNullOrEmpty($srcLib)) {
        # Python 3.10 (the distribution's ABI) through the py launcher: <base prefix>\libs\python310.lib
        # The launcher runs to completion and only ITS exit code is read: a pipeline cut short by
        # Select-Object -First leaves $LASTEXITCODE unset in PowerShell 7 and at -1 in Windows
        # PowerShell 5.1, and an exit code left by an earlier command must not decide anything.
        $basePrefix = $null
        try {
            $global:LASTEXITCODE = $null
            $pyLines = @(& py -3.10 -c "import sys; print(sys.base_prefix)" 2>$null)
            if ($LASTEXITCODE -eq 0 -and $pyLines.Count -gt 0) { $basePrefix = [string]$pyLines[0] }
        } catch { }
        if ($basePrefix) { $srcLib = Join-Path $basePrefix "libs\python310.lib" }
    }
    if ($srcLib -and (Test-Path $srcLib)) {
        Copy-Item -Path $srcLib -Destination $py310lib -Force
        Write-Host "  copied python310.lib -> $py310lib" -ForegroundColor Green
    } else {
        Write-Host "  WARNING: python310.lib not found (pass -PythonLib <path of python310.lib>); python bindings link may fail." -ForegroundColor Yellow
    }
}

# --- tinyxml.lib (urdfdom's exported link interface hard-references it) ------
# urdfdomExport.cmake lists C:/pixi_ws/.pixi/envs/default/Library/lib/tinyxml.lib
# (tinyxml v1) in urdfdom::* INTERFACE_LINK_LIBRARIES, which propagates onto our
# plugin's link line via rviz_common -> urdf -> urdfdom. The plugin references no
# tinyxml symbols (the urdfdom_* DLLs import tinyxml.dll themselves, and nothing of
# ours links TinyXML 1), so a VALID but empty stub archive lets the link succeed.
# The stub is only the link-time file; the run-time tinyxml.dll comes from
# fix_missing_tinyxml_dll.ps1. Requires cl.exe + lib.exe on PATH (they are
# after build_plugin.ps1 imports vcvars64).
$libDir = Join-Path $PixiRoot "Library\lib"
Ensure-Dir $libDir
$tinyxmlLib = Join-Path $libDir "tinyxml.lib"
if (-not (Test-Path $tinyxmlLib)) {
    $cl = Get-Command cl -ErrorAction SilentlyContinue
    $libExe = Get-Command lib -ErrorAction SilentlyContinue
    if ($cl -and $libExe) {
        $tmp = Join-Path $env:TEMP ("qar_stub_" + [System.Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        # Empty translation unit -> valid (symbol-less) obj -> valid archive.
        Set-Content -Path (Join-Path $tmp "stub.c") -Value "/* qar tinyxml stub */`r`n" -Encoding ascii
        Push-Location $tmp
        try {
            & cl /nologo /c stub.c 2>&1 | Out-Null
            & lib /nologo /out:"$tinyxmlLib" stub.obj 2>&1 | Out-Null
        } finally { Pop-Location }
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
        if (Test-Path $tinyxmlLib) {
            Write-Host "  created stub tinyxml.lib -> $tinyxmlLib" -ForegroundColor Green
        } else {
            Write-Host "  WARNING: failed to create stub tinyxml.lib." -ForegroundColor Yellow
        }
    } else {
        Write-Host "  WARNING: cl/lib not on PATH; cannot create stub tinyxml.lib (run from build_plugin.ps1)." -ForegroundColor Yellow
    }
}

Write-Host "Stale pixi_ws paths materialized under $PixiRoot." -ForegroundColor Green
