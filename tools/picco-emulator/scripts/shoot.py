#!/usr/bin/env python3
"""Drive the Picco emulator over QMP. Waits for a console marker, then runs a
step list (touch taps / button presses) and converts each e-ink frame to a
portrait PNG. Steps file: a Python list of tuples, e.g.
  [('tap', 185, 107), ('wait', 2), ('key', 'up')]
'tap' holds the contact ~0.6s then releases; 'key' pulses a GPIO low."""
import json, os, re, socket, subprocess, sys, time
from PIL import Image

EMU = '/home/user/emu'
OUT = sys.argv[1] if len(sys.argv) > 1 else EMU + '/out/shoot'
STEPS = eval(open(sys.argv[2]).read()) if len(sys.argv) > 2 else []
QMP = '/tmp/claude-0/picco.qmp'
MARKER = os.environ.get('MARKER', 'Entering activity: Home')
KEYS = {'up': 21, 'down': 9, 'power': 2, 'select': 5}
os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT):
    os.remove(os.path.join(OUT, f))
env = dict(os.environ, PICCO_OUT=OUT, FLASH=EMU + '/img/flash-tiles.bin',
           SD=EMU + '/img/sd2.img', QMP=QMP,
           PSRAM_OPT='-global driver=ssi_psram,property=is_octal,value=true')
if os.path.exists(QMP):
    os.remove(QMP)
clog = open(OUT + '/console.log', 'w')
qemu = subprocess.Popen([EMU + '/scripts/run.sh'], env=env, stdout=clog, stderr=subprocess.STDOUT)

for _ in range(50):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(QMP); break
    except OSError:
        time.sleep(0.2)
f = s.makefile('rw')
def cmd(d):
    f.write(json.dumps(d) + '\n'); f.flush(); return json.loads(f.readline())
f.readline(); cmd({'execute': 'qmp_capabilities'})
def qset(prop, value):
    cmd({'execute': 'qom-set', 'arguments': {'path': '/machine/picco', 'property': prop, 'value': value}})

def wait_marker(timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if MARKER in open(OUT + '/console.log').read():
                return True
        except OSError:
            pass
        time.sleep(0.3)
    return False

print('waiting for marker %r...' % MARKER)
ok = wait_marker(float(os.environ.get('MARKER_TIMEOUT', '120')))
print('marker:', ok)
time.sleep(2)
for act in STEPS:
    k = act[0]
    if k == 'tap':
        qset('touch', '%d %d' % (act[1], act[2])); time.sleep(0.6)
        qset('touch', ''); time.sleep(0.6)
    elif k == 'key':
        qset('gpio-low', 1 << KEYS[act[1]]); time.sleep(1.0); qset('gpio-low', 0); time.sleep(0.6)
    elif k == 'wait':
        time.sleep(act[1])
time.sleep(2)
cmd({'execute': 'quit'}); qemu.wait()
pbms = sorted(x for x in os.listdir(OUT) if x.endswith('.pbm'))
for p in pbms:
    Image.open(os.path.join(OUT, p)).convert('L').rotate(-90, expand=True).save(os.path.join(OUT, p.replace('.pbm', '.png')))
print('frames:', pbms)
