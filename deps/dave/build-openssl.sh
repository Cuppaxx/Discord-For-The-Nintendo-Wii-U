#!/bin/sh
# Paths: set DEVKITPRO, DEPS (this deps/ folder holding the dependency source trees), MSYS2 (an msys2 root),
# and their /cygdrive/... forms DEVKITPRO_CYG / DEPS_CYG / MSYS2_CYG before running. See deps/README.md.
# run from Git Bash: configure + build libcrypto.a
set -e
cd $DEPS/dave
rm -rf build-openssl; mkdir build-openssl; cd build-openssl
export PERL5LIB=$DEPS/dave/perl5
export PATH=$DEVKITPRO/devkitPPC/bin:$PATH
perl $DEPS/dave/openssl/Configure --config=$DEPS/dave/openssl-wiiu.conf wiiu --prefix=$DEPS/dave/install no-asm no-threads no-shared no-dso no-engine no-afalgeng no-tests no-apps no-docs no-module no-ui-console no-sock no-dgram no-async no-autoload-config
$MSYS2/usr/bin/sh.exe -c '. ../env.sh; make -k -j8 PERL=$DEPS_CYG/dave/perlw SHELL=$MSYS2_CYG/usr/bin/sh.exe build_generated libcrypto.a'
