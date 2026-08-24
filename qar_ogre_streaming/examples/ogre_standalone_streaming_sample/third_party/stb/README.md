# Vendored: stb_image_write.h

- Upstream: <https://github.com/nothings/stb> (`stb_image_write.h`)
- Version: **v1.16** (header banner: "stb_image_write - v1.16 - public domain").
- Fetched from: `https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h`
- License: dual **MIT / Public Domain (Unlicense)** — see the license block at the
  bottom of `stb_image_write.h`. No separate LICENSE file is required; the license
  text ships inside the header.

## Why it is here

M4 needs a debug-only CPU-readback PNG writer to prove that offscreen Ogre
RenderTexture output contains real RViz scene content.
`stb_image_write` is a single self-contained public-domain header with no external
dependencies, which keeps the reusable `qar_ogre_streaming` library free of heavy
image-codec deps.

## Usage

Exactly ONE translation unit must define the implementation before including:

```cpp
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
```

All other users include the header without the define. In this repo the
implementation is compiled in the RViz plugin's capture path
(`qar_rviz_streaming`).
