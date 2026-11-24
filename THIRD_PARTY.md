# Third-party software and assets

*Discord: For the Nintendo Wii U* is licensed under the GNU GPL v3.0 (see [LICENSE](LICENSE)). It includes or links the following.
Their licenses are kept as they are.

## Linked libraries

| Component | License | Source |
|---|---|---|
| x264 | GPL-2.0-or-later | https://code.videolan.org/videolan/x264 |
| FFmpeg 7.0.2 (libavcodec, libavformat, libavutil, libswresample) | LGPL-2.1-or-later | https://ffmpeg.org |
| libdave (Discord Audio & Video End-to-End Encryption) | MIT | https://github.com/discord/libdave |
| mlspp | BSD-2-Clause | https://github.com/cisco/mlspp |
| OpenSSL 3.3 (libcrypto) | Apache-2.0 | https://github.com/openssl/openssl |
| nlohmann/json | MIT | https://github.com/nlohmann/json |
| SDL2, SDL2_ttf, SDL2_image | zlib | https://libsdl.org |
| FreeType | FreeType License (FTL) | https://freetype.org |
| HarfBuzz | MIT ("Old MIT") | https://harfbuzz.github.io |
| libpng, libjpeg-turbo, libwebp, zlib, bzip2, brotli | their respective permissive licenses | devkitPro portlibs |
| libcurl | curl license (MIT-style) | https://curl.se |
| Mbed TLS | Apache-2.0 | https://github.com/Mbed-TLS/mbedtls |
| Jansson | MIT | https://github.com/akheron/jansson |
| Opus | BSD-3-Clause | https://opus-codec.org |
| mpg123 | LGPL-2.1 | https://mpg123.org |
| wut | zlib | https://github.com/devkitPro/wut |
| QR Code generator (`src/qrcodegen.c`) | MIT | https://www.nayuki.io/page/qr-code-generator-library |

How the non-portlib libraries were built (versions, flags, patches) is described in [deps/README.md](deps/README.md).

## Fonts (in `content/`)

All fonts are under the **SIL Open Font License 1.1** (DejaVu Sans uses the Bitstream Vera / DejaVu license).

* **UI:** Noto Sans (`font.ttf`, `font_bold.ttf`)
* **Fallbacks for symbols and other scripts** (`content/fb/`): DejaVu Sans, Noto Sans Math, Symbols, Symbols 2, Cherokee, Egyptian Hieroglyphs, Gujarati, JP, Cham, and Noto Serif Tibetan
* **Display-name styles** (`content/names/`, matched to Discord's name-font ids): Bangers, BioRhyme, Cherry Bomb One, Chicle, Compagnon, MuseoModerno, Pirata One, Pixelify Sans, Ribes, Sinistre, Zilla Slab, Playpen Sans, Orbitron, New Rocker and Kalam

## CA certificates

`content/cacert.pem` is the Mozilla CA bundle as distributed by curl (MPL-2.0).

## Trademarks

"Discord" is a trademark of Discord Inc. "Nintendo" and "Wii U" are trademarks of Nintendo. This project is not affiliated with either company.
