# Trance Gate

A tempo-locked step gate for [Schwung](https://github.com/charlesvestal/schwung)
on Ableton Move, modelled on the Kilohearts Trance Gate.

- 8 pattern slots, length 1–32 steps, ties between steps
- Resolution as a musical division (1/1 … 1/64, incl. triplets)
- Per-step ADSR, a gate-length control, and one **Amount** at two scopes —
  global (dry/wet, 0% is a true bypass) and per step (an accent)
- Locked to song position via `get_beat_position()`, so it stays bar-aligned
- Pattern edited **on the pads** — press toggles a step, Shift+press ties it
  into the next one. Green is sound, red is a gap, brightness is that step's
  amount, and a **white pad sweeps with the playhead** while the transport runs
- Circular display: filled segments are sound on, hollow are off, and the band
  thickens with the step's amount

Chain `audio_fx` component.

## Requirements

**It needs a Schwung host that supports `page_knobs`.** That is a small
addition to the knob-grid planner letting a module-drawn page declare its own
knobs, and it is **not in any released Schwung** — it currently lives in
[graebe/schwung](https://github.com/graebe/schwung).

Without it the module still installs and still processes audio, but the ring
page **degrades silently**: it falls back to the level's first eight knobs, so
Slot and Step Amount are unreachable and the wrong controls sit under your
hands, with nothing on screen saying so. A companion host fix is needed for
`Slot` to display its own number rather than one higher.

There is no version gate on the install route below, so this section is the
only warning you get.

## Install

Open the Web Manager on your Move — `http://move.local:7700` — go to
**Modules**, and install from the GitHub URL:

```
graebe/schwung-trance-gate
```

It reads `release.json` from `main`, downloads the release asset and installs
to `modules/audio_fx/trance-gate/`. Then add it to a chain slot as an Audio FX.

## Build from source

```bash
./scripts/build.sh          # cross-compiles via Docker -> dist/
./scripts/install.sh        # scp to ableton@move.local
./tests/run.sh              # DSP + UI tests, no device needed
```

## Releasing

Tagging is what publishes. `src/module.json`, `release.json` and the tag must
all carry the same version — the workflow fails the build if they disagree,
before it builds anything.

```bash
git tag v0.1.0 && git push origin v0.1.0
```

## Licence

MIT. See `LICENSE`.
