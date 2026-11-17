"""Our sound effects: art/sfx/sounds.json (the sound sheet) -> fal -> game/sounds/sfx/*.wav and sfx.txt
Run: python tools/mksfx.py          (only makes what's missing; delete art/sfx/raw/<name>.* to redo one)
     python tools/mksfx.py process  (no fal: just re-trim/convert the raw files and rewrite sfx.txt)

Each row of the sheet is one of the engine's sound slots (Duke's numbering, since his C code plays them by number)
that our game actually plays, with what plays it, our prompt, and the slot's playback settings (pitch range,
priority, flags, distance), kept from the slot it fills so it behaves as the engine expects.

Sound: fal ElevenLabs sound effects v2, $0.002 per second (the whole sheet is well under $0.20). Each result's URL
is written to art/sfx/raw/<name>.url before it's downloaded, so a failed download never means paying twice.
Then, locally: trim the silence, even out the loudness, 22 kHz mono 8-bit WAV (no Ogg decoder in the web build).
The game (emine_voice.cpp, Emine_SfxInit) reads sfx.txt and points each slot at our file.
"""
import json, os, subprocess, sys, concurrent.futures as cf

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
SHEET = os.path.join(ROOT, 'art', 'sfx', 'sounds.json')
RAW = os.path.join(ROOT, 'art', 'sfx', 'raw')
OUT = os.path.join(ROOT, 'game', 'sounds', 'sfx')
PLAIN = ['-map_metadata', '-1', '-fflags', '+bitexact', '-flags:a', '+bitexact']  # a bare WAV the game can read


def fal_call(row):
    import fal_client, httpx, pathlib
    if not os.environ.get('FAL_KEY'):
        os.environ['FAL_KEY'] = (pathlib.Path.home() / '.fal_key').read_text(encoding='utf-8').strip()
    name = row['name']
    mp3, url_file = os.path.join(RAW, name + '.mp3'), os.path.join(RAW, name + '.url')
    if os.path.exists(mp3):
        return 'have'
    if os.path.exists(url_file):  # paid for, the download failed: fetch it again
        url = open(url_file).read().strip()
    else:
        r = fal_client.subscribe('fal-ai/elevenlabs/sound-effects/v2', arguments={
            'text': row['prompt'], 'duration_seconds': max(0.5, row['seconds']), 'prompt_influence': 0.6})
        url = r['audio']['url']
        open(url_file, 'w').write(url + '\n')
    open(mp3, 'wb').write(httpx.get(url, timeout=120).raise_for_status().content)
    return 'made'


def process(row):
    """mp3 -> trimmed, levelled, 22 kHz mono 8-bit WAV. Loops keep their length (no trimming)."""
    name = row['name']
    loop = row['flags'] & 1 or name == 'underwater'
    trim = '' if loop else ('silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.01,areverse,'
                            'silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.04,areverse,')
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-i', os.path.join(RAW, name + '.mp3'),
                    '-af', trim + 'loudnorm=I=-14:TP=-1.0:LRA=11', '-ac', '1', '-ar', '22050', '-c:a', 'pcm_u8', *PLAIN,
                    os.path.join(OUT, name + '.wav')], check=True)


def main():
    rows = json.load(open(SHEET))
    os.makedirs(RAW, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    if not (len(sys.argv) > 1 and sys.argv[1] == 'process'):
        todo = [r for r in rows if not os.path.exists(os.path.join(RAW, r['name'] + '.mp3'))]
        secs = sum(max(0.5, r['seconds']) for r in todo if not os.path.exists(os.path.join(RAW, r['name'] + '.url')))
        print('%d sounds to make, %.1f s (about $%.2f)' % (len(todo), secs, secs * 0.002))
        with cf.ThreadPoolExecutor(6) as ex:
            futs = {ex.submit(fal_call, r): r['name'] for r in todo}
            for f in cf.as_completed(futs):
                try:
                    f.result()
                except Exception as e:
                    print('FAILED', futs[f], e)
    made = []
    for r in rows:
        if os.path.exists(os.path.join(RAW, r['name'] + '.mp3')):
            process(r)
            made.append(r)
    # what the game loads: "<slot> <file> <pitch lo> <pitch hi> <priority> <flags> <distance>" per row
    with open(os.path.join(OUT, 'sfx.txt'), 'w', newline='\n') as f:
        for r in made:
            f.write('%d %s %d %d %d %d %d\n' % (r['slot'], r['name'], r['pitch'][0], r['pitch'][1], r['priority'],
                                                r['flags'], r['distance']))
    missing = [r['name'] for r in rows if r not in made]
    print('wrote %d sounds%s' % (len(made), ('; missing: ' + ', '.join(missing)) if missing else ''))


if __name__ == '__main__':
    main()
