# Trance Gate

A tempo-locked step gate for [Schwung](https://github.com/charlesvestal/schwung)
on Ableton Move, modelled on the Kilohearts Trance Gate.

- 8 pattern slots, length 1–32 steps, ties between steps
- Resolution as a musical division (1/4 … 1/32, incl. triplets)
- Per-step ADSR, plus Depth (gate floor) and Mix
- Locked to song position via `get_beat_position()`, so it stays bar-aligned
- Pattern edited **on the pads** — pads mirror the pattern in their LEDs,
  press toggles a step, Shift+press ties it into the next one
- Circular display: filled segments are sound on, hollow are off

Chain `audio_fx` component. Requires Schwung 1.4.0 or newer.

## Build

```bash
./scripts/build.sh          # cross-compiles via Docker -> dist/
./scripts/install.sh        # scp to ableton@move.local
```

## Licence

MIT. See `LICENSE`.
