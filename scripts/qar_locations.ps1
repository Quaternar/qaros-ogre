# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Helpers shared by the scripts in this folder: resolve the folders a build needs and set up
    the Visual Studio 2019 x64 build environment. Dot-source it; it only defines functions.

.DESCRIPTION
    Every script that needs a folder takes it from a parameter and, when the parameter is
    omitted, from an environment variable. The parameter wins. No script has a built-in
    default folder.

        Location                     Parameter    Environment variable
        ROS 2 Humble distribution    -Ros2Root    ROS2_INSTALL_ROOT
        Qt 5.15.2 msvc2019_64        -QtRoot      QAR_QT_ROOT
        QarOS SDK install tree       -SdkRoot     QAR_SDK_ROOT
        Install prefix               -Prefix      QAR_PREFIX
        Dependency root              -DepsRoot    QAR_DEPS_ROOT
        Python virtual environment   -Venv        QAR_VENV

    Functions:
      Resolve-QarLocation      returns the full path of a location or throws one of three
                               messages: "<what> not found: pass -<Parameter> <folder> or set
                               the environment variable <EnvVar> ...", "<what> folder does not
                               exist: <path> ..." or "<what> at <path> has no <marker>.".
                               -Marker names a file or folder (relative to the location) that
                               must exist, -Optional returns $null when neither the parameter
                               nor the variable is set, -AllowMissing accepts a folder that
                               does not exist yet (the caller creates it).
      Get-QarVsInstallPath     installation folder of Visual Studio 2019 with the C++ x64 tools.
      Import-QarBatchEnvironment
                               runs a batch file in cmd.exe and imports the variables it changed
                               into the current session, whatever code page the console uses.
      Import-QarVsDevEnvironment
                               runs vcvars64.bat and imports the resulting environment (cl.exe,
                               link.exe, the Windows SDK) into the current session.

    The file works in Windows PowerShell 5.1 and in PowerShell 7.

.EXAMPLE
    . "$PSScriptRoot\qar_locations.ps1"
    $Ros2Root = Resolve-QarLocation -Value $Ros2Root -Parameter 'Ros2Root' -EnvVar 'ROS2_INSTALL_ROOT' `
        -What 'ROS 2 distribution' -Marker 'local_setup.bat' -Section 'ROS 2 Humble for Windows'
#>

function Resolve-QarLocation {
    [CmdletBinding()]
    param(
        [string]$Value,
        [Parameter(Mandatory = $true)][string]$Parameter,
        [Parameter(Mandatory = $true)][string]$EnvVar,
        [Parameter(Mandatory = $true)][string]$What,
        [string]$Marker,
        [switch]$Optional,
        [switch]$AllowMissing,
        [Parameter(Mandatory = $true)][string]$Section
    )
    $path = $Value
    $origin = "-$Parameter"
    if ([string]::IsNullOrWhiteSpace($path)) {
        $path = [Environment]::GetEnvironmentVariable($EnvVar)
        $origin = "`$env:$EnvVar"
    }
    if ([string]::IsNullOrWhiteSpace($path)) {
        if ($Optional) { return $null }
        throw "$What not found: pass -$Parameter <folder> or set the environment variable $EnvVar (see docs/INSTALL.md, section '$Section')."
    }
    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($path.Trim().Trim('"'))
    if ($full.Length -gt 3) { $full = $full.TrimEnd('\', '/') }
    if (-not (Test-Path -LiteralPath $full -PathType Container)) {
        if ($AllowMissing -and -not (Test-Path -LiteralPath $full)) { return $full }
        throw "$What folder does not exist: $full (from $origin)."
    }
    if ($Marker -and -not (Test-Path -LiteralPath (Join-Path $full $Marker))) {
        throw "$What at $full has no $Marker."
    }
    return $full
}

function Get-QarVsInstallPath {
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe not found at ${vswhere}: install the Visual Studio 2019 Build Tools (see docs/INSTALL.md, section 'Toolchain')."
    }
    $vsPath = & $vswhere -version '[16.0,17.0)' -products '*' -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' -property installationPath -latest | Select-Object -First 1
    if (-not $vsPath) {
        throw "Visual Studio 2019 with the C++ x64 build tools (MSVC v142) not found by vswhere (see docs/INSTALL.md, section 'Toolchain')."
    }
    return $vsPath
}

# Runs `call "<Batch>"` in cmd.exe and copies the variables that it changed into this session.
# The batch file starts from -StartPath as its PATH, and the caller's PATH is put behind
# whatever the batch file left, so nothing the caller had is lost. -StopOnFailure makes a batch
# file that ends with a non-zero exit code an error; without it the variables are imported
# whatever the exit code is.
#
# A second cmd.exe, started with /u, writes the listing of `set` as UTF-16, so every value
# arrives as it is whatever code page the console has: a plain `set > file` is written in that
# code page, and a user name, a TEMP folder or a distribution folder with a letter outside
# ASCII would come back damaged. The batch file itself runs in a cmd.exe without /u: one with
# /u writes its own commands as UTF-16 and every program that the batch file starts as
# single-byte text, all into the same file. What the batch file prints goes into two files, in
# the code page of the console. Its stderr is shown as ordinary lines when it has ended (the
# warning that local_setup.bat of the ROS 2 distribution prints on every start); its stdout is
# not (the banner of vcvars64.bat). Both are part of the error when the environment cannot be
# captured. The paths travel on the command line, which cmd.exe gets as Unicode, and no batch
# text is written. A variable whose value is the session's already is not set again. The path
# of the batch file must not hold a caret or two percent signs, which cmd.exe reads as escapes.
function Import-QarBatchEnvironment {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Batch,
        [Parameter(Mandatory = $true)][string]$StartPath,
        [switch]$StopOnFailure
    )
    # The command line below has quotes of its own, which cmd.exe has to get unchanged. This
    # setting makes PowerShell 7 pass them as Windows PowerShell 5.1 always does.
    $PSNativeCommandArgumentPassing = 'Legacy'
    # cmd.exe writes the two files in the code page that the console has now. The Console
    # class keeps the one it found when the session started, so chcp is asked first.
    $sysDir = [Environment]::SystemDirectory
    $encoding = [Console]::OutputEncoding
    try {
        $codePage = [int](((& (Join-Path $sysDir 'chcp.com') 2>$null) -join '') -replace '\D', '')
        if ($codePage -gt 0) { $encoding = [System.Text.Encoding]::GetEncoding($codePage) }
    } catch {}
    $savedPath = $env:PATH
    # The second cmd.exe is started by its full name: cmd.exe looks in the current folder first.
    $cmdExe = Join-Path $sysDir 'cmd.exe'
    $stamp = [System.Guid]::NewGuid().ToString('N')
    $dump = Join-Path $env:TEMP "qar_env_dump_$stamp.txt"
    $outLog = Join-Path $env:TEMP "qar_env_out_$stamp.txt"
    $errLog = Join-Path $env:TEMP "qar_env_err_$stamp.txt"
    $next = if ($StopOnFailure) { '&&' } else { '&' }
    # A program that the batch file left running can still hold a log open for writing, so a
    # log is opened with sharing.
    $readLog = {
        param([string]$Path)
        if (-not (Test-Path -LiteralPath $Path)) { return '' }
        $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        try { return [System.IO.StreamReader]::new($stream, $encoding).ReadToEnd().TrimEnd() }
        finally { $stream.Dispose() }
    }
    try {
        $env:PATH = $StartPath
        $commandLine = "call `"$Batch`" > `"$outLog`" 2> `"$errLog`" $next " +
            "`"$cmdExe`" /u /c set > `"$dump`""
        cmd /c $commandLine | Out-Null
        if (-not (Test-Path -LiteralPath $dump)) {
            $said = @(& $readLog $outLog; & $readLog $errLog) | Where-Object { $_ }
            $message = "Failed to capture the environment of $Batch."
            if ($said) { $message += " The batch file wrote:`n" + ($said -join "`n") }
            throw $message
        }
        $shown = & $readLog $errLog
        if ($shown) { foreach ($line in $shown -split "\r?\n") { Write-Host $line.TrimEnd() } }
        $listing = [System.IO.File]::ReadAllText($dump, [System.Text.Encoding]::Unicode)
        foreach ($line in $listing -split "\r?\n") {
            if ($line -match '^([A-Za-z_][A-Za-z0-9_\(\)]*)=(.*)$') {
                $name = $matches[1]
                $value = $matches[2]
                if ([Environment]::GetEnvironmentVariable($name) -cne $value) {
                    try {
                        Set-Item -Path "Env:$name" -Value $value -ErrorAction SilentlyContinue
                    } catch {}
                }
            }
        }
        $env:PATH = "$env:PATH;$savedPath"
    } catch {
        $env:PATH = $savedPath
        throw
    } finally {
        Remove-Item -LiteralPath $dump, $outLog, $errLog -ErrorAction SilentlyContinue
    }
}

function Import-QarVsDevEnvironment {
    $vsPath = Get-QarVsInstallPath
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvars64.bat not found at $vcvars." }
    Write-Host "=== Importing the Visual Studio 2019 x64 build environment ===" -ForegroundColor Cyan
    Write-Host "  vcvars64: $vcvars"

    # vcvars64.bat fails with "The input line is too long" when the PATH it starts from is
    # already large, because its own `set PATH=<additions>;%PATH%` line exceeds the 8191
    # character limit of cmd.exe. Run it against the short machine PATH, then put the
    # caller's PATH behind whatever it set, so nothing the caller had is lost.
    Import-QarBatchEnvironment -Batch $vcvars -StopOnFailure `
        -StartPath ([Environment]::GetEnvironmentVariable('PATH', 'Machine'))

    # Some ROS and ament CMake modules call vswhere by its bare name.
    $vswhereDir = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer'
    if ($env:PATH -notlike "*$vswhereDir*") { $env:PATH = "$vswhereDir;$env:PATH" }
}
