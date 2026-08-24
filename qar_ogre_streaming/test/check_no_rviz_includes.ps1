#Requires -Version 5.1
<#
.SYNOPSIS
    Enforce the qar_ogre_streaming dependency-direction rule.

.DESCRIPTION
    Scans the reusable library and examples sources/headers for #include directives
    that pull in forbidden dependencies:
        * rviz_common / rviz_rendering / rviz_default_plugins / any rviz_* header
        * Qt          ( <Q...> angle-bracket includes, or paths containing "Qt")
        * ament       ( ament_* )
        * rclcpp / rcl / ROS client-library headers

    The reusable Ogre<->Qar library must depend ONLY on Ogre, the vendored Qar C
    header, and the C++ standard library, so it can be reused in non-RViz Ogre
    applications. This script exits 1 (with a list of offending lines) if any
    violation is found, and is wired as a CTest test (check_no_rviz_includes).

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's directory.

.PARAMETER ScanDirs
    Directories (relative to RepoRoot) to scan. Defaults to the reusable library
    and examples trees.
#>
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string[]]$ScanDirs = @('qar_ogre_streaming', 'examples')
)

$ErrorActionPreference = 'Stop'

# Forbidden #include patterns. Each entry: a label + a regex matched against the
# include target (the text inside <> or "").
$patterns = @(
    @{ Label = 'rviz';   Regex = '(^|[\\/])rviz[_/]' },
    @{ Label = 'rviz';   Regex = '^rviz_' },
    @{ Label = 'Qt';     Regex = '^Q[A-Z][A-Za-z0-9_]*$' },     # <QWidget>, <QObject>, ...
    @{ Label = 'Qt';     Regex = '(^|[\\/])(Qt[A-Za-z]+)([\\/]|$)' }, # <QtCore/...>, "QtWidgets/..."
    @{ Label = 'ament';  Regex = '(^|[\\/])ament_' },
    @{ Label = 'rclcpp'; Regex = '(^|[\\/])rclcpp([\\/]|$)|(^|[\\/])rclcpp\.hpp$' },
    @{ Label = 'rcl';    Regex = '(^|[\\/])rcl([\\/]|\.h$)' }
)

# Regex to capture an #include target from a source line.
$includeRegex = '^\s*#\s*include\s*[<"]([^">]+)[">]'

$violations = New-Object System.Collections.Generic.List[string]
$scannedFiles = 0

$extensions = @('.hpp', '.h', '.cpp', '.cc', '.cxx', '.hh')

foreach ($dir in $ScanDirs) {
    $full = Join-Path $RepoRoot $dir
    if (-not (Test-Path $full)) {
        continue  # examples/ may not exist yet — that's fine.
    }

    $files = Get-ChildItem -Path $full -Recurse -File |
        Where-Object { $extensions -contains $_.Extension.ToLower() } |
        Where-Object { $_.FullName -notmatch '[\\/](build|out|install)[\\/]' }

    foreach ($file in $files) {
        $scannedFiles++
        $lineNo = 0
        foreach ($line in [System.IO.File]::ReadAllLines($file.FullName)) {
            $lineNo++
            $m = [regex]::Match($line, $includeRegex)
            if (-not $m.Success) { continue }
            $target = $m.Groups[1].Value

            foreach ($p in $patterns) {
                if ($target -match $p.Regex) {
                    $rel = $file.FullName.Substring($RepoRoot.Length).TrimStart('\', '/')
                    $violations.Add(("[{0}] {1}:{2}: #include {3}" -f $p.Label, $rel, $lineNo, $target))
                    break
                }
            }
        }
    }
}

Write-Host "check_no_rviz_includes: scanned $scannedFiles file(s) under $($ScanDirs -join ', ')"

if ($violations.Count -gt 0) {
    Write-Host ""
    Write-Host "FORBIDDEN dependency includes found (dependency-direction rule violation):" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "The reusable qar_ogre_streaming library and examples must not include" -ForegroundColor Red
    Write-Host "rviz_*, Qt, ament, or rclcpp/rcl headers." -ForegroundColor Red
    exit 1
}

Write-Host "OK: no forbidden rviz/Qt/ament/rclcpp includes." -ForegroundColor Green
exit 0
