# Trance Gate — backlog

Checkable items. Ordered roughly by what unblocks what, not by priority.

---

## Requested

### [x] Randomise the pattern from the circle page

A generator that fills the current slot with a musically plausible gate rather
than 16 coin flips — uniform random reads as noise, not as a trance gate.

- [x] Decide the gesture. The circle page has no spare knob (see the eight-knob
      ceiling below) and plain click opens the section picker, so this probably
      wants **Shift+Click**, or a `access: "write"` trigger param (a click fires
      it, a knob cannot edit it).
- [x] Decide the algorithm. Candidates: density-weighted (a Density knob, so the
      result is steerable and repeatable-ish), Euclidean (evenly spread N hits
      over `length` — always musical, and a natural fit for a ring), or
      downbeat-biased random.
- [x] Keep it inside the RT contract: `set_param` runs on the audio callback, so
      the generator must be a bounded loop over ≤32 steps with no allocation.
      `rand()` is not RT-safe in the strict sense — use a small xorshift seeded
      per instance.
- [x] Do not disturb the playhead: regenerating mid-bar must not reset
      `step_pos`, or the gate jumps out of time on every press.

### [x] Per-step depth, applied before the global Depth

Each step carries its own depth; the existing global `depth` then scales the
whole sequence. So the per-step value is an accent and the global one is the
master amount.

- [x] DSP: gain becomes `1 - (stepDepth[i] * depth) * (1 - env)`. Per-step
      stored as one byte each (0–255), 32 per slot × 8 slots = 256 bytes.
- [x] State blob: extend the `p<N>` triple to carry the depth array as hex.
      **Bump `TG_STATE_VERSION` to 2** — a v1 blob has no depths and must load
      as "all full", not as "all zero", which would silently mute every gate.
- [x] UI: a knob in the circle view setting the depth of the step under the
      cursor, and the ring should show it — segment thickness or arc radius
      scaled by depth is the obvious reading, since the ring already uses
      thickness for on/off.
- [x] ~~Blocked on the eight-knob ceiling below~~ — resolved by the two-level split — this needs a ninth knob.

### [x] Circle page first, settings second

Today the bank bar shows `Main` then `Gate`; the module lands on `Gate` via
`goToPage`, so the *landing* is right but the *order* is not.

- [x] This is a HOST change, not a module one. `page_plan.mjs` pushes the
      level's knobs pages (~line 1068) and then emits canvas pages in a separate
      later pass (~line 1124), so a canvas page can never precede the grid
      whatever order `params` declares.
- [x] Options: emit canvas pages in `paramKeys(lvl)` order rather than in a
      trailing pass; or add an explicit `page_order` / `first` hint on the
      canvas param. The first is cleaner but reorders pages for every module
      that already ships an `as_page` canvas — check the fleet before changing
      it, and expect `tests/fixtures/movy-geom-baseline.txt` to move.

---

## Blocking the above

### [x] The eight-knob ceiling

A canvas page carries `authored.slice(0, perPage)` — the level's first eight
knobs — so the ring and the cells page always show the same eight, and a ninth
control means a second cells page (the page just removed).

Per-step depth needs a ninth. Ways out, cheapest first:

- [ ] ~~Drop `sustain`~~ — not needed (keep the param). Costs the envelope
      graphic one of its four cells — check whether `resolveViz` still forms the
      group with three.
- [x] Give the ring its **own level** with its own four or five knobs
      (cursor / gate / step-depth / length / rate) and leave root as the
      settings page. Plain jog still walks across levels, so it reads as two
      pages. More contract surface, no loss.
- [x] Accept a third page — yes, three now: Gate / Main / Settings.

---

## Known open, already flagged

- [ ] **TWO host changes are not on the device**, and two of the three items
      above do nothing without them: the canvas-page ordering (so the ring is
      page 1 rather than merely where the cursor lands) and the `fitDev`
      measurement fix. Deploying means `install.sh local`, which rebuilds and
      restarts the whole host.
- [ ] **The `fitDev` host fix is not on the device.** `render_page_movy.mjs`
      measured cell values in Tamzen while drawing them in font4x5, so anything
      ≥3 digits lost its last character ("100 ms" → "100 m"). Fixed on the
      schwung branch, 41 fleet baseline pages updated, all render tests green —
      but deploying it means `install.sh local`, which rebuilds and restarts the
      whole host. Until then 3-digit ms values still truncate on device.
- [x] ~~Depth, Mix, Slot and Stop have no cells~~ — back on the Settings page. They kept their metadata and
      still travel in presets, but are unreachable from the grid — the cost of
      collapsing to one settings page. Swap any back in on request.
- [ ] **"1000 ms" still will not fit** (32px against a 30px cell) even after the
      host fix, which is why decay and release are capped at 500 ms.
- [ ] **Pads do nothing yet.** `host_pad_block(1)` is called every frame so they
      are silent, but nothing acts on a press. Original plan: pad = toggle step,
      Shift+pad = tie, LEDs mirroring the pattern. The three-state `step` knob
      has since covered the same ground from the encoders, so this is now a
      convenience rather than the primary editor — decide whether it still earns
      its complexity.
- [ ] **Nothing is committed.** Both worktrees are dirty; `feature/trance-gate`
      in each repo has no commits beyond the empty root in the module repo.
- [ ] **No release workflow.** `.github/workflows/` is empty; `release.json` and
      `src/module.json` must agree on the version, and the catalog entry on the
      schwung branch already points at `graebe/schwung-trance-gate`.

---

## Verified working (do not re-litigate)

- Gate engine: bar-aligned from `get_beat_position()`, block-anchored with a
  per-sample local advance, survives seeks and tempo changes.
- Ties suppress the retrigger; `state` round-trips; 34 headless tests in
  `tests/run.sh`, no device needed.
- `ui_chain.js` + `createController` gives the real knob grid, enum peek,
  section picker, screen reader and the host's My Presets / Module pages.
- The ring is an `as_page` canvas: host draws the chrome, the drawer gets a
  clipped frame, so picture and text cannot collide.
- Playhead animates at frame rate by interpolating between `ui` reads using
  `ms_step`.
