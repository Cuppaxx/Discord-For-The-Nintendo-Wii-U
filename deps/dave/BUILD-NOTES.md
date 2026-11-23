# DAVE (Discord E2EE) for Wii U - build notes

Result: `install/lib/` holds static libs; `install/include/` the headers.
Verified in Cemu 2.6 (2026-11-09) with `<workspace>\davetest` (DaveTest.wuhb): OpenSSL/mlspp self-tests,
`dave_session_init`, key package, a full 2-member MLS flow (external-sender add proposal -> commit/welcome ->
key ratchets) and Opus frame encrypt/decrypt, on the main thread and on a 1 MB SDL thread. Not run on hardware.

## Cemu compatibility (IMPORTANT - read before changing flags)  [2026-11-09]
Two Cemu 2.6 emulation problems made `dave_session_init` die; both are worked around in the build flags:

1. **Recompiler mis-executes some GCC 16 `-O2` code -> host access violation (Cemu dies, "unknown module").**
   Pinpointed to OpenSSL `BN_usub()` (crypto/bn/bn_add.c) at -O2: GCC keeps `dif == 0` (from `subf.`) in cr0
   across two calls by saving CR with `mfcr r25` and restoring with `mtcrf 128,r25`. Under the JIT the
   captured cr0 is wrong when `dif == 0`, so the tail loop runs with ctr=0 -> `bdnz` loops ~2^32 times and
   writes far past the buffer. Evidence: identical-input loops crash at varying iterations only with the
   recompiler (`--force-interpreter` passes everything, RFC6979 test vector matches); `BN_usub` with equal
   `top` crashes in a 3000x micro-loop while every other bn primitive passes; replacing only that
   `mfcr` (or only that `mtcrf`) in the -O2 asm by an explicit compare makes everything pass. (Small asm
   replicas of the same shape did NOT reproduce, so the exact Cemu trigger is context-dependent; `-O1`
   avoids it - `-O1 -fgcse` or `-O1 -fcse-follow-jumps` re-create the spill.) Same class of issue as the
   QuickJS "-O2 miscompile" noted in wiibuild.py.
   **Fix: everything in the DAVE stack is built at `-O1`** (openssl-wiiu.conf CFLAGS, wiiu-toolchain.cmake
   WIIU_FLAGS and forced CMAKE_{C,CXX}_FLAGS_RELEASE, the dave_c wrapper).
2. **Conditional trap instructions halt the thread.** GCC 16 libstdc++ `shared_ptr` refcount-overflow checks
   (`if (count == INT_MAX) __builtin_trap();`, bits/shared_ptr_base.h) are if-converted into `tweq`.
   Cemu routes every `tw`/`twi` to the interpreter/debugger without evaluating the TO condition, so the
   first `shared_ptr` copy (Session::InitLeafNode) hangs forever (interpreter and JIT alike).
   **Fix: all C++ is compiled with `-D__builtin_trap()=__builtin_abort()`** (toolchain CXX flags, dave_c),
   same trick wiibuild.py already uses for app .cpp files. Check with
   `python tools/crscan.py install/lib/*.a` -> "conditional traps = 0" for our libs
   (libstdc++.a's own tweq's are only in filesystem/future/tzdb code that DAVE does not use).
Backups of the pre-fix state: `install/lib.bak-20261109/`, `openssl-wiiu.conf.bak-20261109`,
`wiiu-toolchain.cmake.bak-20261109`.

## Sources downloaded (official GitHub only, TLS verified)
| what | URL | ref / commit |
|---|---|---|
| libdave | https://github.com/discord/libdave | 8de72b1f8a2ac3c5a5270755bb8091a62e3c6169 (cpp/ subdir) |
| mlspp | https://github.com/cisco/mlspp | 1cc50a124a3bc4e143a787ec934280dc70c1034d (pinned by libdave vcpkg-alts overlay port) |
| OpenSSL | https://github.com/openssl/openssl branch openssl-3.3 | 7f7361872142a5676d299fec127c5af6f2542dba (reports 3.3.8-dev) |
| nlohmann/json | https://github.com/nlohmann/json tag v3.11.3 | (header-only, installed via cmake) |

Source edits: `-Werror` removed from `mlspp/CMakeLists.txt` and `libdave/cpp/CMakeLists.txt` (GCC 16 warnings).
No other source patches (the Cemu fixes below are compiler-flag only).

## Libraries produced (install/lib)
libcrypto.a (OpenSSL 3.3, no-asm), libbytes.a, libtls_syntax.a, libhpke.a, libmls_ds.a, libmls_vectors.a,
libmlspp.a, libdave.a (libdave cpp), libdave_c.a (C wrapper + POSIX/entropy stubs).

## Commands
Env helper `env.sh` (msys2 make needs /cygdrive paths). Toolchain: `wiiu-toolchain.cmake`.
CMake = <tools>\cmake-4.4.4-windows-x86_64\bin\cmake.exe, Ninja next to it.

1. OpenSSL: `sh build-openssl.sh` (Git Bash). Uses custom target `openssl-wiiu.conf`
   (CFLAGS incl. `-DB_ENDIAN -DNO_SYSLOG -Dtimezone=_timezone`), options
   `no-asm no-threads no-shared no-dso no-engine no-afalgeng no-tests no-apps no-docs no-module no-ui-console no-sock no-dgram no-async no-autoload-config`.
   Quirks: the msys2root perl is broken (no strict.pm); Git's perl lacks Pod::Usage / IPC::Cmd, so stubs live in `perl5/`
   and `perlw` is a wrapper passed as `PERL=`; Configure is run with `<absolute path>/...` source paths so the cygwin make can read them;
   only `build_generated libcrypto.a` is built (libssl not needed). `no-stdio` breaks err.h (va_list), so stdio stays on.
   Headers installed by copying `openssl/include/openssl/*.h` + `build-openssl/include/openssl/*.h` to `install/include/openssl`,
   and `libcrypto.a` to `install/lib`.
2. json: `cmake -S json -B build-json -G Ninja -DCMAKE_TOOLCHAIN_FILE=wiiu-toolchain.cmake -DCMAKE_INSTALL_PREFIX=install -DJSON_BuildTests=OFF` then `--install`.
   (2026-11-09 rebuild: `sh build-openssl.sh` from Git Bash with CFLAGS -O1 in openssl-wiiu.conf; then copy
   `build-openssl/libcrypto.a` to `install/lib`. Headers unchanged.)
3. mlspp (Git Bash; use ABSOLUTE paths - a relative toolchain/prefix path fails; Ninja is
   `<tools>\ninja.exe`, put that dir on PATH):
   `I=$DEPS/dave/install; TC=$DEPS/dave/wiiu-toolchain.cmake`
   `cmake -S mlspp -B build-mlspp -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$I -DDISABLE_GREASE=ON -DMLS_CXX_NAMESPACE=mlspp -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON -DCMAKE_PREFIX_PATH=$I -Dnlohmann_json_DIR=$I/share/cmake/nlohmann_json` ; build; `--install`.
4. libdave: `cmake -S libdave/cpp -B build-dave -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$I -DCMAKE_PREFIX_PATH=$I -DMLSPP_DIR=$I/share/MLSPP -Dnlohmann_json_DIR=$I/share/cmake/nlohmann_json` ; build; `--install`.
   (libdave has its own C API, `dave/dave.h`, already compiled into libdave.a.)
   Check the generated `build.ninja` FLAGS show `-O1 ... -D__builtin_trap()=__builtin_abort() -O1 -DNDEBUG`.
5. Wrapper + test (needs `DEVKITPRO=$DEVKITPRO`):
   ```
   F="-O1 -ffunction-sections -fdata-sections -DESPRESSO -D__WIIU__ -D__WUT__ -mcpu=750 -meabi -mhard-float"
   I="-Iinstall/include -Iinstall/include/mlspp -Ilibdave/cpp/includes -Ilibdave/cpp/src -I$DEVKITPRO/wut/include"   # mlspp + src: video test helpers (2026-11-09)
   powerpc-eabi-g++ $F -fexceptions -frtti '-D__builtin_trap()=__builtin_abort()' -std=c++17 $I -c dave_c.cpp ; powerpc-eabi-gcc $F $I -c wiiu_stubs.c
   powerpc-eabi-ar rcs install/lib/libdave_c.a dave_c.o wiiu_stubs.o
   powerpc-eabi-gcc $F $I -c dave_test.c
   powerpc-eabi-g++ $F -specs=$DEVKITPRO/wut/share/wut.specs dave_test.o -Linstall/lib \
     -Wl,--start-group -ldave_c -ldave -lmlspp -lmls_vectors -lhpke -ltls_syntax -lbytes -lcrypto -Wl,--end-group \
     -L$DEVKITPRO/wut/lib -lwut -o dave_test.elf
   ```
   Link order matters: libdave_c (stubs) must be inside the group / after libcrypto.

### 2026-11-09: video wrappers in dave_c
dave_c also has `dave_encrypt_video` / `dave_decrypt_video` (+ `_max_size_video`, `dave_frame_looks_encrypted`) and two test
helpers (`dave_test_make_pair`: real 2-member MLS group; `dave_test_get_key`: raw ratchet key for an independent check), which
need the extra include dirs above. Same -O1 / __builtin_trap flags; crscan: 0 CR spills / 0 conditional traps in dave_c.o.
Copy dave_c.h to install/include/dave_c.h after changing it. Previous lib: install/lib/libdave_c.a.bak-video.
Endianness on the video path: codec_utils.cpp / frame_processors.cpp are byte-oriented; the remaining host memcpy's are the
1-byte supplemental size and the 0xFAFA marker (endian-neutral); the nonce patches cover the rest. Verified in Cemu: a Wii U
encrypted H.264 frame decrypts with an independent little-endian AES-GCM (Python, nonce = 8 zero bytes + LE32 truncated
nonce); the big-endian nonce variant fails the tag.

## Test app (<workspace>\davetest)
`python wiibuild.py davetest "DaveTest" "DaveTest" "Mingus"` then
`powershell -File <workspace>\cemu.ps1 -App <workspace>\davetest\DaveTest.wuhb -Wait 40 -Out x.png`.
Log: `%APPDATA%\Cemu\sdcard\wiiu\apps\DaveTest\log.txt` (flushed per line). Stages: C++ runtime, single-
instruction/CR checks, bn word primitives + BN_uadd/usub micro-loops, deterministic OpenSSL stress (with
RFC6979 P-256 pubkey vector), OpenSSL keygen/ECDSA/X25519/Ed25519/HKDF/AES-GCM, mlspp keys + KeyPackage,
dave_c session/key package/passthrough encrypt, full 2-member MLS flow + encrypt/decrypt; all repeated on a
1 MB SDL thread; finally a never-taken `tweq` to show the Cemu trap halt (expected last line).
`davetest/crscan.py` lists CR-spill (`mfcr rX`) and conditional-trap sites in .a/.o files.
Debug tips: Cemu `--force-interpreter` (command-line) distinguishes JIT bugs from guest bugs.

## Untested / caveats
- Not run on real hardware. Big-endian correctness of OpenSSL is relied on (`-DB_ENDIAN`, generic C paths);
  verified in Cemu (RFC6979 vector, ECDSA verify, MLS flow between two sessions).
- The Cemu JIT issue was worked around by optimisation level, not fixed; the exact trigger is not fully
  characterised. If a crash in "unknown module" reappears after changing flags or code, bisect with
  `--force-interpreter` and `-O1` per object first (`davetest/rebuild_objs.sh "<flags>" crypto/bn/x.c ...`
  rebuilds chosen OpenSSL objects into a private davetest/lib/libcrypto.a; add "lib" first in
  davetest/build.json libdirs to use it).
- App code (e.g. discord/src at wiibuild's -O2) is not covered by this fix; the same Cemu JIT issue can
  in principle hit it too.
- Verified 2026-11-09: `python wiibuild.py discord ...` still links with these libs (MingusDiscord.wuhb
  built; its app.elf contains no conditional traps besides wut's unconditional abort/assert `twu`).
- Entropy: OpenSSL seeds via a weak `getentropy()`; `wiiu_stubs.c` provides one that calls an app hook
  (`dave_set_entropy_source`) and otherwise falls back to a WEAK timebase-jitter generator. Register a real source
  before generating keys (key package / signature keys). Security-critical.
- Wii U stack/heap: MLS operations are allocation heavy; run DAVE calls from a thread with a large stack (>=256KB suggested).
- Persistent keys are disabled (PERSISTENT_KEYS=OFF): the signing key is transient per session, which Discord accepts.
- Thread-safety: OpenSSL built `no-threads`; call all DAVE/OpenSSL functions from a single thread (or serialise).
- libdave.a ~0.6MB, libcrypto.a 9MB (unstripped; final .elf with gc-sections ~3MB text).
