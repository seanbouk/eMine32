"""Our voice lines: art/voice/lines.json (the voice sheet, exported) -> fal -> game/sounds/voice/*.wav
Run: python tools/mkvoice.py        (only makes what's missing; delete art/voice/raw/<take>.* to redo a take)

Every line not marked cut becomes one or more takes (a line with "A / B" is two takes the game picks between).
Speech: fal ElevenLabs v3 in our designed voice (VOICE below), $0.10 per 1,000 characters. The two foley sounds
(knuckle crack, gulp): fal ElevenLabs sound effects, $0.002 per second. Each result's URL is written to
art/voice/raw/<take>.url before it's downloaded, so a failed download never means paying twice.
Then, locally: trim the silence at both ends, even out the loudness, and squeeze to 22 kHz mono 8-bit WAV (the web
build has no Ogg decoder, and it's the kind of sound Duke's own lines are).

TEXT below overrides what's spoken for a take: v3's [bracketed] directions for grunts and screams, and typo fixes.
"""
import json, os, subprocess, sys, concurrent.futures as cf

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
SHEET = os.path.join(ROOT, 'art', 'voice', 'lines.json')
RAW = os.path.join(ROOT, 'art', 'voice', 'raw')
OUT = os.path.join(ROOT, 'game', 'sounds', 'voice')
VOICE = 'm5pB7oZWGpxRPROPEp7z'  # "A3+ 3" on the voice sheet, saved as a voice (art/voice/design/harry_final.json)

# what's actually spoken, where it isn't just the sheet's line (keyed by take name)
TEXT = {
    'use-nothing': '[grunts] Hmph.',
    'use-nothing_2': 'Where is it?',
    'hurt': '[pained grunt] Ugh!',
    'hurt_2': '[grunts in pain] Agh!',
    'hurt_3': '[short pained grunt] Hngh!',
    'burning': '[screams in pain] Aaaargh!',
    'burning_2': '[gasping in agony] Ahh! Ahh!',
    'burning_3': '[pained yell] Gaaah!',
    'burning_4': '[groaning in pain] Nnngh... argh!',
    'land-hurt': '[pained grunt] Oof!',
    'fall-scream': '[screaming as he falls] Aaaaaaaaah!',
    'gasp': '[gasps deeply for air] Haaaah... haah.',
    'death-gibbed': '[death scream] Aaaaarrgh!',
    'medkit': '[relieved sigh] Ahhh.',
    'gib-1': 'How about a mesquite for that grill!?',
    'eat-cooked-first': 'Charred, tender, juicy....',
    'low-health': "A Western Double Chee would go down well about now.",
}
# foley, made with the sound-effects model: (description, seconds)
SFX = {
    'idle-crack': ('a man cracking his knuckles, several sharp knuckle cracks, close up, dry, no music', 1.2),
    'gulp': ('a single loud gulp, a man swallowing a mouthful of food, close up, dry, no music', 0.7),
}
TAKES_OF = {'hurt': 3, 'burning': 4}  # extra takes of a line with one text


def takes():
    """(take name, line id, spoken text or None for foley) for every line to record."""
    out = []
    for r in json.load(open(SHEET, encoding='utf-8')):
        if r['decision'] == 'cut':
            continue
        lid = r['id']
        if lid in SFX:
            out.append((lid, lid, None)); continue
        parts = [p.strip() for p in r['final'].split(' / ') if p.strip()] or [r['final']]
        n = max(len(parts), TAKES_OF.get(lid, 1))
        for k in range(n):
            name = lid if k == 0 else '%s_%d' % (lid, k + 1)
            out.append((name, lid, TEXT.get(name, parts[min(k, len(parts) - 1)])))
    return out


def fal_call(name, text):
    import fal_client, httpx, pathlib
    if not os.environ.get('FAL_KEY'):
        os.environ['FAL_KEY'] = (pathlib.Path.home() / '.fal_key').read_text(encoding='utf-8').strip()
    mp3, url_file = os.path.join(RAW, name + '.mp3'), os.path.join(RAW, name + '.url')
    if os.path.exists(mp3):
        return 'have'
    if os.path.exists(url_file):  # paid for, the download failed: fetch it again
        url = open(url_file).read().strip()
    else:
        if text is None:
            desc, secs = SFX[name]
            r = fal_client.subscribe('fal-ai/elevenlabs/sound-effects/v2', arguments={
                'text': desc, 'duration_seconds': secs, 'prompt_influence': 0.6})
        else:
            r = fal_client.subscribe('fal-ai/elevenlabs/tts/eleven-v3', arguments={'text': text, 'voice': VOICE})
        url = r['audio']['url']
        open(url_file, 'w').write(url + '\n')
    open(mp3, 'wb').write(httpx.get(url, timeout=120).raise_for_status().content)
    return 'made'


# a bare WAV: no metadata block before the samples (the game's sound library won't read past one)
PLAIN = ['-map_metadata', '-1', '-fflags', '+bitexact', '-flags:a', '+bitexact']


def process(name):
    """mp3 -> trimmed, levelled, 22 kHz mono 8-bit WAV."""
    trim = ('silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.03,areverse,'
            'silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.06,areverse,'
            'loudnorm=I=-15:TP=-1.5:LRA=11')
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-i', os.path.join(RAW, name + '.mp3'),
                    '-af', trim, '-ac', '1', '-ar', '22050', '-c:a', 'pcm_u8', *PLAIN, os.path.join(OUT, name + '.wav')],
                   check=True)


def main():
    os.makedirs(RAW, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    todo = takes()
    chars = sum(len(t) for _, _, t in todo if t and not os.path.exists(os.path.join(RAW, _ + '.mp3')))
    print('%d takes; %d characters of speech to make (about $%.2f)' % (len(todo), chars, chars / 1000 * 0.10))
    with cf.ThreadPoolExecutor(6) as ex:
        futs = {ex.submit(fal_call, n, t): n for n, _, t in todo}
        for f in cf.as_completed(futs):
            try:
                f.result()
            except Exception as e:
                print('FAILED', futs[f], e)
    for n, _, _ in todo:
        if os.path.exists(os.path.join(RAW, n + '.mp3')):
            process(n)
    # a moment of silence: what cut lines play, so none of Duke's old ones can
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                    'anullsrc=r=22050:cl=mono', '-t', '0.05', '-c:a', 'pcm_u8', *PLAIN, os.path.join(OUT, 'silence.wav')],
                   check=True)
    # what the game loads (emine_voice.cpp): "<take> <line>" for every take that was made
    made = [(n, l) for n, l, _ in todo if os.path.exists(os.path.join(OUT, n + '.wav'))]
    with open(os.path.join(OUT, 'takes.txt'), 'w', newline='\n') as f:
        f.writelines('%s %s\n' % m for m in made)
    missing = [n for n, _, _ in todo if not os.path.exists(os.path.join(OUT, n + '.wav'))]
    print('wrote %d takes to %s%s' % (len(made), os.path.relpath(OUT, ROOT), ('; missing: ' + ', '.join(missing)) if missing else ''))


if __name__ == '__main__':
    main()
