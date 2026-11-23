# x264 (software H.264 encoder) for Wii U - build notes

Result: `install/lib/libx264.a` + `install/include/{x264.h,x264_config.h}` (8-bit, 4:2:0, C only, no threads).
Used by `discord/src/venc.c` (encoder wrapper) and `discord/src/vrec.c` (video-message recorder).
Verified in Cemu 2.6 (2026-11-09/10) with `<workspace>\venctest` (VencTest.wuhb). Not run on real hardware.

## Source (downloaded with explicit user approval)
| what | URL | ref / commit |
|---|---|---|
| x264 | https://code.videolan.org/videolan/x264.git (official VideoLAN GitLab, TLS verified) | branch `stable`, commit `b35605ace3ddf7c1a5d67a2eb553f034aef41d55` (2025-04-02, "0.165.3222 b35605a", X264_BUILD 165) |

`git clone --branch stable --single-branch https://code.videolan.org/videolan/x264.git src; git -C src checkout b35605ace3ddf7c1a5d67a2eb553f034aef41d55`
No source patches.

## LICENSE - IMPORTANT
x264 is **GPL v2 or later** (`src/COPYING`). Any .rpx/.wuhb that links libx264 (MingusDiscord once venc.c/vrec.c are
linked in, VencTest) is a combined GPL work: distributing it requires distributing the complete corresponding source
of the whole app under GPL-compatible terms. (Commercial x264 licences exist from x264 LLC; not applicable here.)
The other engineers' libs: FFmpeg build is LGPL (no --enable-gpl), libdave/mlspp/OpenSSL 3 are Apache-2/BSD style -
compatible with GPLv3 only (OpenSSL 3 Apache-2.0 is not GPLv2-only compatible, but x264 is "v2 or later" so GPLv3 works).

## Build (Git Bash): `sh build-x264.sh`
1. configure (out of tree, `build/`):
   ```
   ../src/configure --host=powerpc-unknown-linux-gnu --cross-prefix=powerpc-eabi- --prefix=$DEPS/x264/install \
     --disable-asm --disable-thread --disable-cli --enable-static --disable-opencl --disable-lavf --disable-swscale \
     --disable-ffms --disable-gpac --disable-lsmash --disable-avs --disable-interlaced --bit-depth=8 --chroma-format=420 --enable-strip \
     --extra-cflags="-O1 -mcpu=750 -meabi -mhard-float -ffunction-sections -fdata-sections -DESPRESSO -D__WIIU__ -D__WUT__ -I$DEVKITPRO/wut/include" \
     --extra-ldflags="-specs=$DEVKITPRO/wut/share/wut.specs -L$DEVKITPRO/wut/lib -lwut"
   ```
   * `--host=powerpc-unknown-linux-gnu` (not powerpc-eabi): x264's configure dies on unknown OS "eabi". SYS_LINUX only
     enables malloc.h/memalign and a sched_getaffinity path that is compiled out by --disable-thread. Big-endian is
     detected (WORDS_BIGENDIAN 1); asm/AltiVec off (Espresso has none).
   * configure's link tests need `-specs=wut.specs -lwut` (and env `DEVKITPRO=$DEVKITPRO`).
2. `config.mak`: configure prepends `-O3 -ffast-math`; the script rewrites that to
   `-O1 -ffast-math -fno-isolate-erroneous-paths-dereference '-D__builtin_trap()=__builtin_abort()'` (Cemu notes in
   `third_party/dave/BUILD-NOTES.md`: GCC 16 -O2 CR spill miscompiled by Cemu's JIT; tw/twi halt Cemu).
3. make with the msys2 make: `$MSYS2/usr/bin/sh.exe -c '... make -j8 lib-static'` (PATH with
   $DEVKITPRO_CYG/devkitPPC/bin first).
4. copy `libx264.a`, `src/x264.h`, `build/x264_config.h` into `install/`.
5. check: `python tools/crscan.py build/libx264.a` -> conditional traps = 0 (22 `mfcr` CR spills remain
   even at -O1, in macroblock analyse/encode, me_search_ref, ratecontrol...; they ran correctly in Cemu: 1000+ frames
   encoded, output decodes bit-clean on the PC. If odd Cemu crashes appear in x264, bisect with `--force-interpreter`.)

libx264.a: 824 KB.

## Linking
build.json: lib `x264`, incdir `../third_party/x264/install/include`, libdir `../third_party/x264/install/lib`
(already added to `discord/build.json` and `venctest/build.json`).

## FFmpeg copy with the AAC encoder (for vrec.c)
`<deps>\ffmpeg-7.0.2-avenc` = copy of the media agent's `ffmpeg-7.0.2-av` sources (objects excluded),
configured with the SAME options plus `--enable-encoder=aac`, extra-cflags `-O1 -fno-isolate-erroneous-paths-dereference`
instead of `-O2`, and `-Os` (added by --enable-small) replaced by `-O1` in `ffbuild/config.mak`. Built with
`make -j8 libavcodec/libavcodec.a libavformat/libavformat.a libavutil/libavutil.a libswresample/libswresample.a`
(msys2 sh, log `third_party/ffavenc_configure.log` / `ffavenc_make.log`). Same lib names; a superset of the decoders/
demuxers of the original. `discord/build.json` incdir/libdirs for FFmpeg now point at `ffmpeg-7.0.2-avenc`
(lib list unchanged). If the media agent changes `ffmpeg-7.0.2-av`, redo the same change here (or add
`--enable-encoder=aac` there and switch build.json back). The MP4 container is written by vrec.c itself (no mov muxer).

## Measurements (Cemu 2.6, CPU mode 3 = multi-core recompiler, fast x86 host; REAL HARDWARE WILL DIFFER - the
## Espresso is a 1.24 GHz in-order PPC750 and is expected to be several times slower than these numbers)
Test pattern content (flat colour bars + moving box: easier than camera video), 75 frames each, 15 fps, ultrafast/zerolatency,
baseline, slice-max 1100, keyint 30:
| size | target | enc avg / p95 / max ms | ~core @15 fps | output | Y-PSNR after Wii U HW decode |
|---|---|---|---|---|---|
| 320x240 | 250 kbps | 6.8 / 8.9 / 65 (1st frame) | 10% | 198 kbps | 68.6 dB (min 52.5) |
| 426x240 | 300 kbps | 7.9 / 9.7 / 12 | 12% | 305 kbps | 58.9 dB (min 53.1) |
| 640x360 | 500 kbps | 15.5 / 19.6 / 22 | 23% | 391 kbps | 65.9 dB (min 53.2) |
Async (encoder thread, core 0) 320x240: 120/120 frames encoded, 0 dropped, UI-thread venc_submit max 0.1 ms.
Recorder (no slice limit): 480x360 13.3 ms/frame, 640x480 18.6 ms/frame; AAC-LC 32 kHz mono ("fast" coder) ~10 ms per
1024-sample frame (= ~30% of a core in Cemu - the costliest part of recording).
