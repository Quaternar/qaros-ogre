<#
.SYNOPSIS
    (Re-)installs the vendored Qar streaming C API header into third_party/qar/include/.

.DESCRIPTION
    PROVENANCE CHANGE (M10, 2026-07-21): the vendored header is NO LONGER fetched from
    GitHub raw. It now comes from the delivered **QarOS SDK drop v0.1.1**, whose install
    tree lives (gitignored) at `QarOS_binary/install/`:

        QarOS_binary/install/include/qar_streaming.h          (the header)
        QarOS_binary/install/share/qar-streaming-c/            (CMake package config; v0.1.1)
        QarOS_binary/install/{bin,lib}/qar-streaming-c.*       (the real runtime DLL + import lib)

    The header is a self-contained single-header amalgamation ("Auto-generated single header
    for the qar-streaming C V0 API"). It has no "..."-relative sibling includes (only system
    headers: stdbool/stdint/stdio/stdlib, and — only when the matching backend is enabled —
    windows.h/d3d11.h (QAR_ENABLE_D3D11, auto on Windows), GL/gl.h (QAR_ENABLE_OPENGL on Linux),
    vulkan (QAR_ENABLE_VULKAN)). So qar_streaming.h remains the only file vendored.

    Dispatch model (important for the fake stub + real link): by DEFAULT (no
    QAR_ENABLE_DYNAMIC_LOADING) each public `qar_<name>()` is a `static inline` wrapper that
    calls `qar_impl_<name>()`, declared `__declspec(dllimport)` — i.e. you LINK the import lib
    `qar-streaming-c.dll` (loaded dynamically at run time), which exports the
    `qar_impl_<name>` symbols. If you instead `#define QAR_ENABLE_DYNAMIC_LOADING`, the header
    grows load helpers that `LoadLibraryExA("qar-streaming-c.dll")` (or `dlopen` on Linux) and
    `GetProcAddress` the `qar_impl_<name>` symbols into a per-module function table at load time.
    We use the default (link-time import-lib) path for both the mock and the real runtime.

    On Windows you MUST `#define QAR_ENABLE_OPENGL` BEFORE including the header to expose the
    OpenGL types/functions (D3D11 is auto-enabled on Windows; OpenGL is auto-enabled only on
    Linux). See qar_ogre_streaming/include/qar_ogre_streaming/qar_streaming_c.hpp.

.PARAMETER SdkInclude
    Path to the SDK drop's include dir that holds qar_streaming.h. Defaults to the repo-local
    gitignored QarOS_binary/install/include.

.EXAMPLE
    ./scripts/update_qar_headers.ps1
    ./scripts/update_qar_headers.ps1 -SdkInclude D:\some\other\qaros\install\include
#>
param(
    [string]$SdkInclude = (Join-Path (Resolve-Path "$PSScriptRoot\..").Path "QarOS_binary\install\include"),
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
)

$ErrorActionPreference = "Stop"

$src = Join-Path $SdkInclude "qar_streaming.h"
if (-not (Test-Path $src)) {
    throw "SDK header not found at $src. Point -SdkInclude at the QarOS SDK drop's include dir."
}

$destInclude = Join-Path $RepoRoot "third_party\qar\include"
New-Item -ItemType Directory -Force -Path $destInclude | Out-Null
$out = Join-Path $destInclude "qar_streaming.h"

Copy-Item -Path $src -Destination $out -Force
Write-Host "Vendored $src -> $out"
Write-Host "Source: QarOS SDK drop v0.1.1 (see QarOS_binary/install/share/qar-streaming-c/qar-streaming-cConfigVersion.cmake)."
Write-Host "NOTE: the old GitHub-raw pin (qaros @ 37eb9841...) is superseded by this SDK drop."
