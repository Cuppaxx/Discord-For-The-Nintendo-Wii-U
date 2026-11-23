#!/bin/sh
# Paths: set DEVKITPRO, DEPS (this deps/ folder holding the dependency source trees), MSYS2 (an msys2 root),
# and their /cygdrive/... forms DEVKITPRO_CYG / DEPS_CYG / MSYS2_CYG before running. See deps/README.md.
# Rebuild libx264.a for the Wii U (run from Git Bash). See BUILD-NOTES.md.
set -e
cd $DEPS/x264
rm -rf build; mkdir build; cd build
export DEVKITPRO=$DEVKITPRO PATH=$DEVKITPRO/devkitPPC/bin:$PATH
../src/configure --host=powerpc-unknown-linux-gnu --cross-prefix=powerpc-eabi- --prefix=$DEPS/x264/install \
  --disable-asm --disable-thread --disable-cli --enable-static --disable-opencl --disable-lavf --disable-swscale \
  --disable-ffms --disable-gpac --disable-lsmash --disable-avs --disable-interlaced --bit-depth=8 --chroma-format=420 --enable-strip \
  --extra-cflags="-O1 -mcpu=750 -meabi -mhard-float -ffunction-sections -fdata-sections -DESPRESSO -D__WIIU__ -D__WUT__ -I$DEVKITPRO/wut/include" \
  --extra-ldflags="-specs=$DEVKITPRO/wut/share/wut.specs -L$DEVKITPRO/wut/lib -lwut"
# configure puts "-O3 -ffast-math" first; make the Cemu-safe flags explicit (the later -O1 would win anyway)
sed -i "s/^CFLAGS=-O3 -ffast-math /CFLAGS=-O1 -ffast-math -fno-isolate-erroneous-paths-dereference '-D__builtin_trap()=__builtin_abort()' /" config.mak
$MSYS2/usr/bin/sh.exe -c 'export PATH=$DEVKITPRO_CYG/devkitPPC/bin:$MSYS2_CYG/usr/bin:$PATH; cd $DEPS_CYG/x264/build && make -j8 lib-static'
mkdir -p ../install/lib ../install/include
cp libx264.a ../install/lib/ && cp ../src/x264.h x264_config.h ../install/include/
python tools/crscan.py libx264.a
