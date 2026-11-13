# Voice: what Duke says now, and the plan for our own

> **Status (2026-11-13): done.** All 82 lines from the voice sheet are in, in our designed voice ("A3+ 3", ElevenLabs
> voice `m5pB7oZWGpxRPROPEp7z`), except two that fal's content checker refused (gib-9, gib-10; see below). Every one
> of the 69 slots in Duke's script that used his voice now plays our line or a moment of silence.
> - The sheet: https://claude.ai/artifact/B137hj684LeMQigvnjGRNt, exported to `art/voice/lines.json`.
> - `python tools/mkvoice.py` makes what's missing (fal, then trimmed and levelled to 22 kHz mono 8-bit WAV in
>   `game/sounds/voice/`, plus `takes.txt`). Raw takes and their URLs are kept in `art/voice/raw/`.
> - `engine/source/duke3d/src/emine_voice.cpp` maps Duke's slots and says the new lines (`Emine_Say`); dev console:
>   `emine_voice` (check the slots), `emine_voice <line>`, `emine_voice log`.
>
> The rest of this file is the original survey and plan.

Duke's lines are part of the shareware data, so all of them have to go before launch (see `ART-LAUNDRY.md`). This is
what's there, what makes each line play in eMine32 today, and how we make our own: an original gravel-voiced,
deadpan Dirty Harry type, made with fal.

The words for each line are from memory, from the file names and the well-known lines. Nobody has listened to them
to check.

## 1. Duke's voice lines in the shareware data

**Fires** says whether the line can still play in eMine32. Many of Duke's triggers lived in his game script on actors
and pickups we no longer use: our mobs are C++, Duke has every weapon from the start, and food heals directly.

### Still playing

| Sound | File | Line (approx.) | Trigger |
|---|---|---|---|
| BONUS_SPEECH1 | letsrk03 | "Let's rock!" | New Game (our skip of the skill menu) |
| DUKE_GET | getitm19 | pickup chime (not speech) | ammo and food pickups |
| DUKE_CRACK_FIRST | knuckle | knuckle crack (not speech) | standing still for a while |
| DUKE_CRACK | waitin03 | "What are you waiting for? Christmas?" | standing still longer |
| DUKE_CRACK2 | comeon02 | "Come on!" | standing still longer |
| DUKE_SEARCH / DUKE_SEARCH2 | pain87 / whrsit05 | grunt / "Where is it?" | pressing Use on nothing |
| DUKE_GRUNT | exert | grunt | hurt (mob hits, arrows) |
| DUKE_LONGTERM_PAIN (1-8) | gasps07, dscrem15-17, pain54/75/93/68 | pained gasps and screams | burning in lava |
| DUKE_LAND_HURT | pain39 | pained grunt | landing from a big fall |
| DUKE_SCREAM | dscrem04 | scream | falling a long way |
| DUKE_GASP | gasp | gasp for air | surfacing after a long swim |
| DUKE_KILLED1/2/3/5 | damn03, damnit04, thsuk13a, pisses01 | "Damn", "Damn it", "This sucks", "That really pisses me off" | Duke dies |
| DUKE_KILLED4, DUKE_DEAD | dscrem18, dmdeath | death scream | Duke is blown apart |
| DUKE_USEMEDKIT | ahh04 | "Ahhh" | using a medkit from the inventory |

### Not playing any more (the trigger is gone)

| Sound | File | Line (approx.) | Duke's trigger |
|---|---|---|---|
| JIBBED_ACTOR1 | amess06 | "What a mess" | an enemy blown to bits, 1 in 8 or so (`jib_sounds`) |
| JIBBED_ACTOR2 | bitchn04 | "Bitchin'!" | " |
| JIBBED_ACTOR3 | holycw01 | "Holy cow!" | " |
| JIBBED_ACTOR4 | holysh02 | "Holy sh*t!" | " |
| JIBBED_ACTOR5 | imgood12 | "Damn, I'm good" | " (also the "Damn I'm Good" skill) |
| JIBBED_ACTOR6 | piece02 | "Piece of cake" | " (also the "Piece of Cake" skill) |
| JIBBED_ACTOR7 | gothrt01 | "That's gotta hurt" | " |
| JIBBED_ACTOR8 | blowit01 | "Blow it out your ass" | " |
| JIBBED_ACTOR9 | eatsht01 | "Eat sh*t and die" | " |
| JIBBED_ACTOR10 | face01 | "In your face!" | " |
| JIBBED_ACTOR11 | inhell01 | "See you in hell" | " |
| JIBBED_ACTOR12 | sukit01 | "Suck it down" | " |
| JIBBED_ACTOR13 | letgod01 | "Let God sort 'em out" | " |
| DUKE_GETWEAPON1 | cool01 | "Cool" | picking up a weapon (`randgetweapsnds`) |
| DUKE_GETWEAPON2 | getsom1a | "Come get some" | " (also the "Come Get Some" skill) |
| DUKE_GETWEAPON3 | groovy02 | "Groovy" | " |
| DUKE_GETWEAPON4 | wansom4a | "Wanna dance?" | " |
| DUKE_GETWEAPON6 | hail01 | "Hail to the king, baby" | " |
| DUKE_GOTHEALTHATLOW | needed03 | "Ooh, I needed that" | Duke's health pickups when health is low |
| DUKE_PISSRELIEF / DUKE_URINATE | ahmuch03 / pissing | "Ahh, much better" | using a toilet |
| DUKE_DRINKING | drink18 | slurp | drinking from a toilet or fountain |
| DUKE_STEPONFECES | happen01 | "Sh*t happens" | stepping in dog mess (yes, the name) |
| DUKE_TIP1 / TIP2 | dance01 / shake2a | "Shake it, baby" | tipping a stripper |
| DUKE_HIT_STRIPPER1/2, DUKE_BOOBY | damnit04, damn03, booby04 | | strippers |
| DUKE_LOOKINTOMIRROR | lookin01 | "Lookin' good" | looking in a mirror |
| DUKE_TAKEPILLS | gulp01 | gulp | taking steroids |
| BONUS_SPEECH2/3/4 | ready2a, ripem08, rockin02 | "Ready for action", "Let's rip", "Rockin'!" | the end-of-level bonus screen |
| DUKETALKTOBOSS, DUKE_TALKTOBOSSFALL | duknuk14, diesob03 | boss taunts, "Die, you son of a b*tch" | boss fights |
| WAR_AMBIENCE9/10, GENERIC_AMBIENCE13-17, 23 | nobody01, chew05, pay02, onlyon03, rides09, doomed16, myself3a, 2ride06 | "Nobody steals our chicks and lives", "kick ass and chew bubblegum", and others | placed by hand in Duke's maps |

### Not Duke

The mobs still use Duke's enemy sounds (the zombie is the pig cop, the skeleton the trooper, and so on). Those go in
the sound pass with the rest of the effects, not here.

## 2. What fal has for voice

Prices are from fal's model pages (November 2026). Check them before a big run.

| Model | Endpoint | What it does | Price |
|---|---|---|---|
| ElevenLabs voice design (v3) | `fal-ai/elevenlabs/text-to-voice/design/eleven-v3` | invents a voice from a written description; returns a few previews, each with a voice id | $0.10 per 1,000 characters of preview |
| ElevenLabs TTS v3 | `fal-ai/elevenlabs/tts/eleven-v3` | speaks lines in a voice; v3 takes performance tags like `[growls]`, `[whispers]`, `[laughs]` | per character (not confirmed) |
| MiniMax voice design | `fal-ai/minimax/voice-design` | the same idea; returns a `custom_voice_id` | $3 per voice, plus $0.03 per 1,000 preview characters |
| MiniMax Speech 2.8 HD | `fal-ai/minimax/speech-2.8-hd` | speaks lines with a designed or stock voice | per character |
| Qwen 3 TTS | `fal-ai/qwen-3-tts/...` | has voice design and voice cloning | voice design about $0.09 per 1,000 characters |
| Chatterbox | `fal-ai/chatterbox/...` | expressive speech, an "exaggeration" control | about $0.025 per 1,000 characters |

**The pick:** ElevenLabs. Designing the voice costs pennies, v3 is the most expressive for a gravel-and-swagger
read, and its tags let us direct each take ("[low, menacing]", "[dry chuckle]").

**What we don't do:** clone a real person. No Clint Eastwood, and no Jon St. John (Duke's actor). That's a legal
problem and against fal's terms, and it's the one thing that would stop the game shipping. We describe an
*archetype*, and the voice we get is original.

## 3. The plan

### Step 1: design the voice (about $0.10)
A description along the lines of:

> A gravelly, low-pitched American man in his fifties. Deadpan, laconic, world-weary, quietly menacing. Clipped
> sentences, long pauses, a dry sarcastic edge, a little smoke in the throat. A 1970s tough-cop-movie hero, close to
> the microphone.

Run it two or three times with different seeds. Each run gives a few previews. Pick one, and keep its `voice_id` (and
the preview file) in `art/voice/`.

### Step 2: the script
Lines go in `art/voice/lines.json`: id, trigger, text, ElevenLabs tags, and how often it may play. Keep them short (one
to six words, as Duke's are) and original. We riff on the archetype rather than quote films ("Do you feel lucky?"
stays in the film). The setting gives the material: a city guy stuck in a block world at night, mining for bullets,
eating zombie. First draft, by trigger:

| Trigger (new or existing) | Example lines |
|---|---|
| New Game | "Let's get digging." / "Night shift." |
| Level start ("The Mines") | "Mines. Great." |
| Kill: zombie | "Stay dead this time." / "Rest in pieces." |
| Kill: skeleton | "Bone dry." / "Nothing personal. You're just in the way." |
| Kill: spider | "I hate spiders." / "Eight legs. Zero chance." |
| Creeper hisses near you | "Oh, you've got to be kidding me." |
| Creeper blows up near you | "...Rude." |
| Blowing a mob to bits (old jib quips) | "Messy." / "That'll leave a mark." / "Somebody call a mop." |
| Cooked a zombie (explosive kill) | "Well done." / "Dinner's served." |
| Eating raw meat | "Tastes like chicken." / "Don't ask." |
| Eating cooked meat | "Now that's a steak." |
| Eating melon / apple / mushroom | "Fruit. Great." / "Healthy." / "...That one's glowing. Eh." |
| Finding ore (ammo) | "Now we're talking." / "Bullets don't mine themselves." |
| Out of ammo | "Empty. Figures." |
| Low health | "Seen worse. Not much worse." |
| Idle (old knuckle crack) | "I'm not getting any younger." / "Waiting on something?" |
| Falling into lava | (screams: keep, re-recorded) |
| Death | "Aw, hell." / "Not like this." |
| Respawn | "Round two." |

About 60–80 lines, two takes each, comes to roughly 5,000–7,000 characters: **around $1–2** on top of the voice.

### Step 3: the tool
`tools/mkvoice.py`, like the other fal scripts:
- It reads `lines.json` and speaks each new or changed line with the saved `voice_id`.
- It logs each result's URL before downloading, so a failed download never means paying twice (the house fal rule).
- It only regenerates lines whose text or tags changed.
- It processes locally, for free: trim the silence, normalise the level, and squeeze to 22 kHz mono so it sits with the
  game's mid-90s sound (optionally a touch of grit). Then it writes `game/sounds/voice/<id>.ogg` and the sound
  definitions.
- A review page (an artifact, like the block texture one) plays each line next to its text, so you can pick takes.

### Step 4: hook it up
`emine_voice.cpp` with `Emine_Say(trigger)`:
- Each trigger has a chance to play (Duke's `SWEARFREQUENCY`) and a cooldown, and nothing plays over a line already
  playing. Kills say something maybe 1 in 4, with no repeats back to back.
- New triggers are called from where things happen: mob kills and creeper fuses (`emine_mobs.cpp`), eating
  (`emine_food.cpp`), ore drops and ammo (`emine.cpp`).
- Duke's existing slots (death, idle, pain, Use on nothing, "Let's rock") get our files under the same names, so
  nothing else changes.

### Step 5: check
Listen in the game. Tune how often each line fires. Then the `DUKE_*`, `JIBBED_*` and `BONUS_SPEECH*` sounds come off
the laundry list.

### Costs at a glance
- Voice design: about $0.10–0.30 for a few rounds of previews.
- The first full set of lines: about $1–2.
- Re-dos: only for the lines changed.
