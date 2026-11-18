"""Generate the intro map's source paintings with fal (nano-banana-2, $0.08 each).

    python -I tools/intro_fal.py [name ...]

Reads art/intro/fal_prompts.json and writes art/intro/fal/<name>.png (+ .url). Skips names that already have a .png,
so a rerun only pays for what's missing; with names given, it regenerates just those.
"""
import json, os, pathlib, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHEET = json.loads((ROOT / 'art/intro/fal_prompts.json').read_text(encoding='utf-8'))
OUT = ROOT / 'art/intro/fal'
GEN = pathlib.Path.home() / '.claude/tools/fal/fal_gen.py'
OUT.mkdir(parents=True, exist_ok=True)

names = sys.argv[1:] or [n for n in SHEET if not n.startswith('_') and not (OUT / f'{n}.png').exists()]
print(f'{len(names)} to generate, about ${0.08 * len(names):.2f}:', ', '.join(names))

def run(n):
    e = SHEET[n]
    prompt = (SHEET['_style'] + ' ' + e['prompt']) if e['tile'] else e['prompt']
    r = subprocess.run([sys.executable, '-I', str(GEN), str(OUT / f'{n}.png'), e['aspect'], prompt],
                       capture_output=True, text=True)
    return n, r.returncode, (r.stdout + r.stderr).strip().splitlines()[-1:]

with ThreadPoolExecutor(8) as ex:
    for n, code, tail in ex.map(run, names):
        print(n, 'ok' if code == 0 else f'FAILED {tail}')
