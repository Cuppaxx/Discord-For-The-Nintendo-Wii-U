# Building Mingus Discord

## Requirements

| What | Notes |
|---|---|
| [devkitPro](https://devkitpro.org/wiki/Getting_Started) | Install **devkitPPC**, **wut** and the **Wii U portlibs**: `(dkp-)pacman -S wiiu-dev wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-curl wiiu-mbedtls ppc-jansson ppc-libopus ppc-mpg123 ppc-freetype ppc-harfbuzz ppc-libpng ppc-libjpeg-turbo ppc-libwebp ppc-zlib ppc-bzip2 ppc-brotli` |
| Python 3 | Runs the build script |
| `deps-prebuilt-wiiu.zip` | From the [Releases page](https://github.com/Cuppaxx/Discord-For-The-Nintendo-Wii-U/releases/latest): DAVE/libdave + mlspp + OpenSSL libcrypto, x264 and the FFmpeg libraries, built for the Wii U. To build these yourself, see [deps/README.md](../deps/README.md). |

## Build

```sh
git clone https://github.com/Cuppaxx/Discord-For-The-Nintendo-Wii-U.git
cd Discord-For-The-Nintendo-Wii-U
# unzip the bundle so that deps/prebuilt/lib and deps/prebuilt/include exist
unzip deps-prebuilt-wiiu.zip -d deps/
export DEVKITPRO=/opt/devkitpro     # Windows: set DEVKITPRO=C:/devkitPro
python tools/build.py
```

The result is `MingusDiscord.wuhb` in the repository root. `python tools/build.py --clean` rebuilds everything from scratch.

## Layout

```
src/        the app (main.c = UI + input, dc.c = REST API, gw.c = gateway, voice.c = voice/DAVE,
            vsend.c/vrecv.c = camera send/receive, golive.c = Go Live, overlay.c = menus/settings, ...)
common/     the shared Mingus UI layer (nx.c: SDL2 drawing, text, input, software keyboard; nxnet.c: HTTP)
content/    files packed into the .wuhb (fonts, CA certificates, bird)
assets/     Wii U Menu icon and launch images
deps/       how the third-party libraries were built + the prebuilt bundle (deps/prebuilt, not in git)
tools/      build.py, crscan.py (checks libraries for instructions Cemu can't run)
```

## Testing

* **Cemu 2.6** runs the app, including voice receive (no mic or camera). Put the `.wuhb` in Cemu's game path, or open it with *File → Load*.
* On a real console, `SD:/wiiu/apps/MingusDiscord/log.txt` is the debug log.
* **Cemu quirk:** the Cemu recompiler can mis-run some `-O2` code (see `deps/dave/BUILD-NOTES.md`). If Cemu dies with an "unknown module" error, try `--force-interpreter`.

### Test switches

Put these files in `SD:/wiiu/apps/MingusDiscord/` to turn the matching behaviour on:

| File / key | Effect |
|---|---|
| `nomic.txt` | Don't open the mic (receive-only voice) |
| `vsend_dry.txt` | Camera dry run: nothing is sent; your own frames loop back into your tile |
| `vsend_selftest.txt` | Offline camera-send self test |
| `gwdrop.txt` | Drop the gateway connection once, 20 s after connecting (tests auto-resume) |
| `vtest.h264`, `vtest720.h264` | Offline video-receive self test (encrypt → packetise → lossy network → decode) |
| `config.txt`: `fakemic=1` | A test tone replaces the mic in the voice-message / clip recorders |
| `config.txt`: `autojoin=<guild>:<channel>` | Join that voice channel automatically at startup |
| `config.txt`: `api=`, `cdn=`, `ra=` | Point the app at other API / CDN / remote-auth servers (e.g. a local mock) |
