# Paths: set DEVKITPRO, DEPS (this deps/ folder holding the dependency source trees), MSYS2 (an msys2 root),
# and their /cygdrive/... forms DEVKITPRO_CYG / DEPS_CYG / MSYS2_CYG before running. See deps/README.md.
export PATH=$DEVKITPRO_CYG/devkitPPC/bin:$MSYS2_CYG/usr/bin:$PATH
export TMPDIR=$DEPS_CYG/dave/tmp
export DEVKITPRO=$DEVKITPRO_CYG
export DEVKITPPC=$DEVKITPRO_CYG/devkitPPC
