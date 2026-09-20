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

## Install

Open the Web Manager on your Move — `http://move.local:7700` — go to
**Modules**, and install from the GitHub URL:

```
graebe/schwung-trance-gate
```

It installs to `modules/audio_fx/trance-gate/`; add it to a chain slot as an
Audio FX. **Nothing else is required** — the stable build runs on released
Schwung.

### Two tracks

| Channel | Needs | Difference |
|---|---|---|
| **Stable** (default) | released Schwung | Length and Rate sit on a page of their own, and the ring is the last page in the rotation — you still land on it when you open the module |
| **Beta** | a host carrying `page_knobs` | the ring leads the rotation and the grid behind it carries Length and Rate beside the envelope |

Pick a channel with the Stable / Beta toggle at the top of the Modules page.
The default is Stable and nobody is opted in automatically.

The beta track needs three host changes that are not in any released Schwung —
`page_knobs`, a canvas page leading its level, and an enum-resolver fix. They
are proposed upstream; until they land and ship, beta is for people running a
host built from those branches.

## Build from source

```bash
./scripts/build.sh          # cross-compiles via Docker -> dist/
./scripts/install.sh        # scp to ableton@move.local
./tests/run.sh              # DSP + UI tests, no device needed
```

## Releasing

Tagging is what publishes, and the **tag picks the channel**: a version
containing `-beta.` updates `channels.beta`, anything else updates
`channels.stable` and the top-level fields a channels-unaware manager reads.

`src/module.json` and the tag must carry the same version — the workflow fails
the build if they disagree, before it builds anything.

```bash
git tag v0.2.0        && git push origin v0.2.0          # stable, from main
git tag v0.3.0-beta.1 && git push origin v0.3.0-beta.1   # beta, from the beta branch
```

A beta is only offered when it is strictly newer than stable.

## Licence

MIT. See `LICENSE`.
