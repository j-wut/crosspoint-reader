#!/usr/bin/env python3
"""Descend from a function into the call that never returns (boot hang finder).
Usage: findblock.py <func_addr> [depth]"""
import re, subprocess, sys, time
EMU = '/home/user/emu'
OD = EMU + '/tools/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump'
GDB = EMU + '/tools/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb'
ELF = EMU + '/img/stock.elf'

def calls(fn):
    out = subprocess.run([OD, '-d', '--start-address=%#x' % fn, '--stop-address=%#x' % (fn + 0x1800), ELF],
                         capture_output=True, text=True).stdout
    res = []; lits = {}
    for l in out.splitlines():
        m = re.match(r'([0-9a-f]+):\s+\S+\s+(\S+)\s*(.*)', l)
        if not m: continue
        a = int(m.group(1), 16); op, args = m.group(2), m.group(3)
        if op == 'l32r':
            mm = re.search(r'(a\d+), .*\((0x[0-9a-f]+)\)', args)
            if mm: lits[mm.group(1)] = int(mm.group(2), 16)
        if op in ('call8', 'call4', 'call12'):
            res.append((a + 3, int(args.split()[0], 16)))
        if op in ('callx8', 'callx4', 'callx12'):
            res.append((a + 3, lits.get(args.strip(), None)))
        if op in ('retw.n', 'retw') and res:
            break
    return res

def run(fn, cs, wait):
    g = ['set pagination off', 'target remote :1234', 'break *%#x' % fn, 'commands', 'silent', 'printf "ENTER\\n"', 'c', 'end']
    for r, _ in cs:
        g += ['break *%#x' % r, 'commands', 'silent', 'printf "RET %#x\\n"' % r, 'c', 'end']
    g += ['c']
    open('/tmp/claude-0/fb.gdb', 'w').write('\n'.join(g) + '\n')
    subprocess.run('pkill -f qemu-system-xtensa', shell=True)
    q = subprocess.Popen(EMU + '/scripts/run.sh -s -S', shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         env={'PATH': '/usr/bin:/bin', 'PICCO_OUT': '/tmp/claude-0', 'PSRAM_OPT': ''})
    time.sleep(1.5)
    try:
        out = subprocess.run([GDB, '-q', '-batch', '-x', '/tmp/claude-0/fb.gdb', ELF], capture_output=True, text=True, timeout=wait).stdout
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode()
    subprocess.run('pkill -f qemu-system-xtensa', shell=True)
    return [l for l in out.splitlines() if l.startswith('RET') or l.startswith('ENTER')]

fn = int(sys.argv[1], 16); depth = int(sys.argv[2]) if len(sys.argv) > 2 else 1
wait = int(sys.argv[3]) if len(sys.argv) > 3 else 25
for _ in range(depth):
    cs = calls(fn)
    hits = run(fn, cs, wait)
    rets = [int(h.split()[1], 16) for h in hits if h.startswith('RET')]
    print('fn %#x: entered=%s, %d calls returned' % (fn, 'ENTER' in hits, len(rets)))
    if 'ENTER' not in hits: break
    last = rets[-1] if rets else None
    idx = 0 if last is None else [r for r, _ in cs].index(last) + 1
    if idx >= len(cs): print('  all calls return (loops inside fn?) last=%s' % hex(last or 0)); break
    nxt = cs[idx]
    print('  blocking call at %#x -> %s' % (nxt[0] - 3, hex(nxt[1]) if nxt[1] else '?'))
    if not nxt[1]: break
    fn = nxt[1]
