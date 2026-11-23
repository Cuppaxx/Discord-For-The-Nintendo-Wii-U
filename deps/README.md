# Third-party libraries for the Wii U

The app links a few libraries that aren't in devkitPro's portlibs. Most people should just download the
prebuilt bundle **`deps-prebuilt-wiiu.zip`** from the Releases page and unzip it here, so that
`deps/prebuilt/lib` and `deps/prebuilt/include` exist. This folder documents exactly how that bundle was made,
so it can be rebuilt from source (and to meet the GPL's source requirements).

| Library | Version / commit | License | Used for |
|---|---|---|---|
| [libdave](https://github.com/discord/libdave) (`cpp/`) | `8de72b1f8a2ac3c5a5270755bb8091a62e3c6169` + [`patches/libdave-wiiu.patch`](patches/libdave-wiiu.patch) | MIT | Discord voice/video end-to-end encryption (DAVE) |
| [mlspp](https://github.com/cisco/mlspp) | `1cc50a124a3bc4e143a787ec934280dc70c1034d` + [`patches/mlspp-wiiu.patch`](patches/mlspp-wiiu.patch) | BSD-2-Clause | MLS group key agreement for DAVE |
| [OpenSSL](https://github.com/openssl/openssl) | branch `openssl-3.3` @ `7f7361872142a5676d299fec127c5af6f2542dba` (libcrypto only) | Apache-2.0 | Crypto for mlspp/libdave |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 (headers only) | MIT | mlspp |
| [x264](https://code.videolan.org/videolan/x264) | `0.165.3222 b35605a` | GPL-2.0-or-later | Camera video encoding |
| [FFmpeg](https://ffmpeg.org/) | 7.0.2 (`libavcodec`, `libavformat`, `libavutil`, `libswresample`), LGPL build | LGPL-2.1-or-later | Audio decoding (voice messages, media) and AAC encoding for video clips |
| `dave/dave_c.cpp` | this project | GPL-3.0 | Small C wrapper over libdave for the C app |

## Why the special flags

The Wii U CPU (Espresso, PowerPC 750) and the Cemu emulator have two quirks that every library here works around:

1. **Built with `-O1`, not `-O2`.** Cemu's recompiler mis-runs a condition-register spill pattern GCC emits at
   `-O2` (found in OpenSSL's `BN_usub`), which corrupts memory. The full analysis is in [`dave/BUILD-NOTES.md`](dave/BUILD-NOTES.md).
2. **`-D__builtin_trap()=__builtin_abort()`.** GCC turns some checks (e.g. in `shared_ptr`) into conditional
   trap instructions, which Cemu doesn't evaluate correctly. `tools/crscan.py` checks a library for them.

The libdave patch fixes **endianness**: libdave copied a nonce and the user-id ratchet context with
host-endian `memcpy`. On the big-endian Wii U that produced keys no other client could decrypt.

## Rebuilding

These scripts are the ones that were used. They were run from Git Bash plus an msys2 `make` on Windows, so set the
path variables at the top of each script for your machine:

* `dave/build-openssl.sh` with `dave/openssl-wiiu.conf`: OpenSSL libcrypto
* `dave/wiiu-toolchain.cmake`: CMake toolchain for nlohmann/json, mlspp and libdave; the exact `cmake` command lines are in `dave/BUILD-NOTES.md`
* `dave/dave_c.cpp`, `dave/wiiu_stubs.c`: the C wrapper (`libdave_c.a`) and the libc stubs it needs
* `x264/build-x264.sh`: x264 (see `x264/BUILD-NOTES.md`)
* `ffmpeg/configure-flags.txt`: the exact FFmpeg `configure` line

Put the resulting `.a` files in `deps/prebuilt/lib` and the headers in `deps/prebuilt/include`. FFmpeg headers go in their
`libavcodec/`, `libavformat/`, … sub-folders, the same as the FFmpeg source tree.
