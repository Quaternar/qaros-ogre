# Vendored: the QarOS C header

- What the folder holds: `include/qar_streaming.h`, the C header of QAROS, and `LICENSE`.
- Origin of the header: `include/qar_streaming.h` is the header of the installed QAROS 0.9.0-dev.3008+65dc9aeb
  (`C:\Program Files\Quaternar\QAROS\include\qar_streaming.h`), the runtime against which this repository was built,
  tested and run. It is copied without a change.
  SHA-256: `55E6E3F48D940CA29331F0528FE475C1B7230FB19A59C31D5525C6E90B097F6C`.
- Origin of the licence: `LICENSE` is the MIT licence of the public QarOS repository,
  <https://github.com/Quaternar/qaros>, which publishes `qar_streaming.h` in `qar-streaming-c/include`. The installed QAROS
  ships no licence file for the header, and the header holds no licence text.
- Later headers: the public QarOS repository <https://github.com/Quaternar/qaros> publishes the header of its
  releases. This stack builds against the header of the installed QAROS named above.

## Why it is here

The libraries of this repository compile against the header, and the repository installs it with them, so that the
other repositories of the stack compile against the same file. It is the only part of the SDK that is copied into a
repository.

## Updating it

`scripts/update_qar_headers.ps1` copies the header of another installed QAROS and leaves `LICENSE` as it is. Which
version the stack supports and what to check after a change are in `docs/SDK_COMPATIBILITY.md`.
