#!/usr/bin/env python3
"""Build DiscordWiiU.wuhb (no make needed).

    python tools/build.py            # release build -> DiscordWiiU.wuhb
    python tools/build.py --clean    # wipe build/ first

Needs devkitPro with devkitPPC, wut and the Wii U portlibs (see docs/BUILDING.md), plus the
prebuilt dependency bundle unpacked into deps/prebuilt/ (or your own build of it, see deps/README.md).
DEVKITPRO must point at the devkitPro install (default: /opt/devkitpro, or C:/devkitPro on Windows).
"""
import glob
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP_NAME, SHORT_NAME, AUTHOR = 'Discord: For the Nintendo Wii U', 'Discord', 'Cuppaxx'
EXE = '.exe' if os.name == 'nt' else ''


def devkitpro():
    d = os.environ.get('DEVKITPRO') or ('C:/devkitPro' if os.name == 'nt' else '/opt/devkitpro')
    if d.startswith('/') and os.name == 'nt' and len(d) > 2 and d[2] == '/':   # msys style /c/devkitPro
        d = d[1] + ':' + d[2:]
    if not os.path.isdir(os.path.join(d, 'devkitPPC')):
        sys.exit('devkitPro not found at %s - set DEVKITPRO' % d)
    return d


DKP = devkitpro()
BIN = os.path.join(DKP, 'devkitPPC', 'bin')
TOOLS = os.path.join(DKP, 'tools', 'bin')
ENV = dict(os.environ, DEVKITPRO=DKP, DEVKITPPC=os.path.join(DKP, 'devkitPPC'),
           PATH=BIN + os.pathsep + TOOLS + os.pathsep + os.environ.get('PATH', ''))


def run(cmd):
    r = subprocess.run(cmd, env=ENV, capture_output=True, text=True)
    for s in (r.stdout, r.stderr):
        if s.strip():
            print(s.strip())
    if r.returncode:
        sys.exit('FAILED: ' + ' '.join(cmd[:3]))


def main():
    cfg = json.load(open(os.path.join(ROOT, 'build.json')))
    out = os.path.join(ROOT, 'build')
    if '--clean' in sys.argv:
        shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out, exist_ok=True)
    if not os.path.isfile(os.path.join(ROOT, 'deps', 'prebuilt', 'lib', 'libdave_c.a')):
        sys.exit('deps/prebuilt is missing - download deps-prebuilt-wiiu.zip from the Releases page and unzip it into deps/ (see docs/BUILDING.md)')

    inc = ['-I' + os.path.join(DKP, 'wut', 'include'),
           '-I' + os.path.join(DKP, 'portlibs', 'wiiu', 'include'),
           '-I' + os.path.join(DKP, 'portlibs', 'ppc', 'include'),
           '-I' + os.path.join(DKP, 'portlibs', 'wiiu', 'include', 'SDL2'),
           '-I' + os.path.join(ROOT, 'src'), '-I' + os.path.join(ROOT, 'common')]
    inc += ['-I' + os.path.join(ROOT, d) for d in cfg.get('incdirs', [])]
    arch = ['-mcpu=750', '-meabi', '-mhard-float']
    cflags = ['-O2', '-Wall', '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
              '-DESPRESSO', '-D__WIIU__', '-D__WUT__'] + arch + cfg.get('cflags', [])

    sources = sorted(glob.glob(os.path.join(ROOT, 'src', '*.c'))) + sorted(glob.glob(os.path.join(ROOT, 'common', '*.c')))
    headers = glob.glob(os.path.join(ROOT, 'src', '*.h')) + glob.glob(os.path.join(ROOT, 'common', '*.h'))
    newest_header = max(os.path.getmtime(f) for f in headers)
    objs = []
    for src in sources:
        is_common = os.path.basename(os.path.dirname(src)) == 'common'
        o = os.path.join(out, ('c_' if is_common else '') + os.path.basename(src) + '.o')
        objs.append(o)
        if os.path.exists(o) and os.path.getmtime(o) > max(os.path.getmtime(src), newest_header):
            continue
        print('cc', os.path.relpath(src, ROOT))
        run([os.path.join(BIN, 'powerpc-eabi-gcc' + EXE), '-c', src, '-o', o] + cflags + inc)

    libdirs = ['-L' + os.path.join(DKP, 'wut', 'lib'), '-L' + os.path.join(DKP, 'portlibs', 'wiiu', 'lib'),
               '-L' + os.path.join(DKP, 'portlibs', 'ppc', 'lib')] + ['-L' + os.path.join(ROOT, d) for d in cfg.get('libdirs', [])]
    libs = [l if l.startswith('-') else '-l' + l for l in cfg['libs']]
    elf, rpx = os.path.join(out, 'app.elf'), os.path.join(out, 'app.rpx')
    print('link')
    run([os.path.join(BIN, 'powerpc-eabi-gcc' + EXE), '-specs=' + os.path.join(DKP, 'wut', 'share', 'wut.specs')]
        + objs + libdirs + libs + ['-o', elf] + arch)
    run([os.path.join(TOOLS, 'elf2rpl' + EXE), elf, rpx])

    wuhb = os.path.join(ROOT, cfg['output'] + '.wuhb')
    cmd = [os.path.join(TOOLS, 'wuhbtool' + EXE), rpx, wuhb, '--name=' + APP_NAME, '--short-name=' + SHORT_NAME,
           '--author=' + AUTHOR, '--content=' + os.path.join(ROOT, 'content')]
    for key, flag in (('icon', '--icon='), ('tv', '--tv-image='), ('drc', '--drc-image=')):
        p = os.path.join(ROOT, 'assets', key + '.png')
        if os.path.exists(p):
            cmd.append(flag + p)
    run(cmd)
    print('built', os.path.relpath(wuhb, ROOT), '(%.1f MB)' % (os.path.getsize(wuhb) / 1048576))


if __name__ == '__main__':
    main()
