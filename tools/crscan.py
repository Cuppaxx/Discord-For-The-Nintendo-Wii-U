"""Scan PPC objects/archives for GCC 'CR spilled to GPR' sequences (mfcr rX with X != 12, i.e. not the
ABI prologue save of cr2-cr4) and conditional traps (tw*/twi* other than unconditional 'trap').
Cemu 2.6's recompiler mis-captures CR0 in such mfcr sequences, and halts on any tw/twi.
usage: python crscan.py file.a|file.o ... [-v]"""
import re, subprocess, sys
OD = r'powerpc-eabi-objdump'
verbose = '-v' in sys.argv
tot_m = tot_t = 0
for path in [a for a in sys.argv[1:] if a != '-v']:
    out = subprocess.run([OD, '-d', path], capture_output=True, text=True).stdout
    fn = obj = ''
    hits_m, hits_t = [], []
    for line in out.splitlines():
        if line.endswith('file format elf32-powerpc'):
            obj = line.split(':')[0]
        m = re.match(r'^[0-9a-f]+ <(.+)>:', line)
        if m: fn = m.group(1); continue
        m = re.match(r'^\s+[0-9a-f]+:\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(\S*)', line)
        if not m: continue
        ins, ops = m.group(1), m.group(2)
        if ins == 'mfcr' and ops != 'r12':
            hits_m.append((obj, fn))
        if re.match(r'^tw', ins) and ins != 'trap':
            hits_t.append((obj, fn, ins))
    print('%s: CR spills (mfcr rX!=r12) = %d, conditional traps = %d' % (path, len(hits_m), len(hits_t)))
    if verbose:
        for h in sorted(set(hits_m)): print('   mfcr  %s %s' % h)
        for h in sorted(set(hits_t)): print('   trap  %s %s %s' % h)
    tot_m += len(hits_m); tot_t += len(hits_t)
print('TOTAL mfcr-spills=%d cond-traps=%d' % (tot_m, tot_t))
