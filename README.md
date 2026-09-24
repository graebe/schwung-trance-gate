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
Audio FX. **Nothing else is required** — it runs on released Schwung
(1.3.0 or newer).

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
git tag v1.0.0        && git push origin v1.0.0          # stable, from main
git tag v1.1.0-beta.1 && git push origin v1.1.0-beta.1   # beta, from the beta branch
```

A beta is only offered when it is strictly newer than stable.

## Licence

**GPL-3.0-or-later**, for every crate here and for `src/ui_chain.js`. See
`LICENSE`, which also ships inside the module tarball -- what lands on a device
is a `.so` and a `.js` with no repository near them, so the terms have to
travel with them.

It was MIT until the Ableton Live plugin moved to
[nih-plug](https://github.com/robbert-vdh/nih-plug), whose **VST3 bindings are
GPLv3**. Anything linked into that binary has to be compatible with it, and
`tg-core` is linked into it.

Strictly, MIT would also have been compatible -- permissive code may be taken
into a GPL work. One licence across the whole project was chosen over two
because the alternative is a rule about which crate may be used where, and
that is the kind of rule that is remembered wrongly.

The Move module does not link nih-plug and would not have needed this. It
carries the same licence anyway, so that the engine on the device and the
engine in the plugin are the same thing in this respect as in every other.
