# Vendored: stb_image_write.h

- Upstream: <https://github.com/nothings/stb> (`stb_image_write.h`)
- Version: **v1.16** (header banner: "stb_image_write - v1.16 - public domain").
- Fetched from: `https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h`
- License: dual **MIT / Public Domain (Unlicense)**; see the license block at the
  bottom of `stb_image_write.h`. No separate LICENSE file is required; the license
  text ships inside the header.

## Why it is here

Two programs of this stack write PNG files from pixels that they read back from the
GPU: the standalone Ogre sample (repository qar_ogre_adapter), which writes the colour
and depth images of its synthetic peers, and the offscreen capture of the RViz plugin
(repository qar_rviz_plugin), a debug path that proves that an offscreen Ogre
render texture holds real scene content. `stb_image_write` is a single self-contained
header with no external dependencies, which keeps the reusable `qar_ogre_streaming`
library free of heavy image-codec dependencies: the library itself never includes it.

## Usage

Exactly ONE translation unit of a program defines `STB_IMAGE_WRITE_IMPLEMENTATION`
before it includes `stb_image_write.h`. All other users include the header without the
define. The CMake file of each program that writes PNG files adds this folder to its
include path and compiles the implementation in its own source file; a repository holds
only the program or programs that it builds.
