#!/bin/sh
# A smoke test of the whole game, headless: the intro, the main menu, the
# campaign's first mission, the ship under way. Scripted input steers it
# (the clicks and keys below, at seconds from the start); screenshots are
# taken every 2000 frames and checked for a picture (not black) at each
# stage, and the run for no fault.
#
#   tools/test/smoke.sh BUILD/stl GAME [OUT]      # OUT: the shots and the log (default work/smoke)
#
# Exits 0 if every stage drew and nothing faulted. Takes about four minutes.
set -u
STL=${1:?the stl program}; GAME=${2:?the game folder}; OUT=${3:-work/smoke}
rm -rf "$OUT" && mkdir -p "$OUT/shots"
OUT=$(cd "$OUT" && pwd)                 # the game runs in its own folder: absolute paths
STL_SHOT="$OUT/shots" STL_SHOT_EVERY=2000 "$STL" --game "$GAME" --run --headless --mute --resolution 1024x768 --watchdog 230 \
    --click 512,408@75 --click 512,393@95 --key 32@125 \
    --key 0xBB@140 --key 0xBB@141 --key 0xBB@142 --key 0xBB@143 > "$OUT/run.log" 2>&1
code=$?
python3 -I - "$OUT" "$code" <<'PY'
import os, struct, sys
out, code = sys.argv[1], int(sys.argv[2])
log = open(os.path.join(out, 'run.log'), errors='replace').read()
def bright(p):
    b = open(p, 'rb').read(); w, h = struct.unpack_from('<ii', b, 18); off = struct.unpack_from('<I', b, 10)[0]
    row = (w * 3 + 3) & ~3
    return sum(b[off + y * row + x] for y in range(0, h, 16) for x in range(0, w * 3, 48)) / ((h // 16) * (w // 16) * 1.0)
shots = sorted(f for f in os.listdir(os.path.join(out, 'shots')) if f.endswith('.bmp'))
lit = [f for f in shots if bright(os.path.join(out, 'shots', f)) > 8]
checks = [
    ('ran until the watchdog (exit 4)', code == 4),
    ('no fault', '=== fault' not in log and 'FATAL' not in log),
    ('every shader translated and compiled', 'not translated' not in log and 'does not compile' not in log),
    ('pipelines all made', '[metal] pipeline:' not in log),
    ('the input reached the game', log.count('[input]') == 7),
    ('at least 8 screenshots with a picture', len(lit) >= 8),
    ('the last screenshot has a picture (the mission)', bool(shots) and shots[-1] in lit),
]
bad = 0
for name, ok in checks:
    print(('ok    ' if ok else 'FAIL  ') + name); bad += not ok
print('%d shots, %d with a picture; log and shots in %s' % (len(shots), len(lit), out))
sys.exit(1 if bad else 0)
PY
