# Copyright 2026 QuaternAR
# SPDX-License-Identifier: MIT

<#
.SYNOPSIS
    Copies the QarOS streaming C API header from a QarOS SDK into third_party/qar/include/.

.DESCRIPTION
    The vendored header comes from the QarOS SDK install tree: <SdkRoot>\include\qar_streaming.h.
    It is a self-contained single-header amalgamation of the qar-streaming C API. It has no
    relative sibling includes, only system headers (stdbool, stdint, stdio, stdlib) and, when
    the matching backend is enabled, windows.h and d3d11.h (QAR_ENABLE_D3D11, automatic on
    Windows), GL/gl.h (QAR_ENABLE_OPENGL, automatic on Linux only) and the Vulkan headers
    (QAR_ENABLE_VULKAN). qar_streaming.h is therefore the only file that is vendored.

    The library is not linked. The adapter (qar_qar_adapter) compiles the header with
    QAR_ENABLE_DYNAMIC_LOADING: qar-streaming-c.dll is loaded at run time and every
    qar_impl_<name> symbol is resolved from it. On Windows, QAR_ENABLE_OPENGL must be defined
    before the header is included to expose the OpenGL types and functions (see
    qar_qar_adapter/include/qar_qar_adapter/qar_capi_render_sender_adapter.hpp).

    The script prints the SHA256 of the vendored header before and of the SDK's header, and
    copies only when they differ. third_party/qar/LICENSE is not touched: the SDK ships no
    licence file for the header, so check the SDK's third_party_licenses.md when the header
    changes.

.PARAMETER SdkRoot
    Root of the QarOS SDK install tree (the folder with include\qar_streaming.h). Default: the
    environment variable QAR_SDK_ROOT. Required.

.PARAMETER RepoRoot
    Root of the repository that receives the header: the script writes
    third_party\qar\include\qar_streaming.h below it. Default: the parent of the folder that
    holds this script.

.EXAMPLE
    ./scripts/update_qar_headers.ps1 -SdkRoot C:\QarOS\install
#>
param(
    [Alias('QarSdkRoot')]
    [string]$SdkRoot,
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "qar_locations.ps1")
$SdkRoot = Resolve-QarLocation -Value $SdkRoot -Parameter 'SdkRoot' -EnvVar 'QAR_SDK_ROOT' `
    -What 'QarOS SDK' -Marker 'include\qar_streaming.h' -Section 'The QarOS SDK'

$src = Join-Path $SdkRoot "include\qar_streaming.h"
$destInclude = Join-Path $RepoRoot "third_party\qar\include"
New-Item -ItemType Directory -Force -Path $destInclude | Out-Null
$out = Join-Path $destInclude "qar_streaming.h"

$oldHash = if (Test-Path $out) { (Get-FileHash -Path $out -Algorithm SHA256).Hash } else { "(none)" }
$newHash = (Get-FileHash -Path $src -Algorithm SHA256).Hash
Write-Host "Source        : $src"
Write-Host "Vendored copy : $out"
Write-Host "SHA256 before : $oldHash"
Write-Host "SHA256 SDK    : $newHash"
if ($oldHash -eq $newHash) {
    Write-Host "No change: the vendored header already equals the SDK's header."
} else {
    Copy-Item -Path $src -Destination $out -Force
    Write-Host "Vendored $src -> $out"
}
Write-Host "third_party\qar\LICENSE is kept as it is. The SDK ships no licence file for the header: check third_party_licenses.md of the SDK."
