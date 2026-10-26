# Vendored: the QarOS C header

- What the folder holds: `include/qar_streaming.h`, the C header of the QarOS SDK, and `LICENSE`.
- Origin of the header: `include/qar_streaming.h` is the header of the QarOS SDK drop of 2026-09-28, the drop
  against which this repository was built, tested and run. It is copied from that drop without a change.
  SHA-256: `5223B56251F3085B376A8BDE5ECEFCF661D6E8F8B800F6AA46414ADC1F4DB6EA`.
- Origin of the licence: `LICENSE` is the MIT licence of the public QarOS repository,
  <https://github.com/Quaternar/qaros>, which publishes `qar_streaming.h` in `qar-streaming-c/include`. The SDK drop
  ships no licence file for the header, and the header holds no licence text.
- Later headers: at the time of writing, the public QarOS repository <https://github.com/Quaternar/qaros> publishes a
  later generation of the C API. This stack does not build against it: the session set-up (create, serialise, join
  and invite), the hand of a gesture, the time points and the init structures of volumes and panels would have to be
  ported.

## Why it is here

The libraries of this repository compile against the header, and the repository installs it with them, so that the
other repositories of the stack compile against the same file. It is the only part of the SDK that is copied into a
repository.

## Updating it

`scripts/update_qar_headers.ps1` copies the header of another SDK drop and leaves `LICENSE` as it is. Which drops
the stack supports and what to check after a change are in `docs/SDK_COMPATIBILITY.md`.
