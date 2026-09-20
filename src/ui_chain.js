/*
 * trance-gate/ui_chain.js -- the module's editor.
 *
 * WHY THIS FILE EXISTS AT ALL. Pad editing needs host_pad_block(), which only
 * a component's own ui_chain.js can call -- and shadow_ui.js only loads one
 * for a component that serves NO ui_hierarchy (enterComponentEdit tries the
 * hierarchy editor first and returns). So the module refuses that key in C and
 * supplies the hierarchy here instead, to a controller it builds itself.
 *
 * WHAT THAT BUYS BACK. Everything the stock editor does: Schwung's own knob
 * grid, its widgets and envelope graphic, the enum peek, the section picker,
 * the screen reader, and the host's "My Presets" / "Module" pages -- none of
 * it reimplemented. `page_controller.mjs` is the same engine the shadow UI
 * drives; only the injected io differs.
 *
 * THE RING IS A PAGE, NOT A SECOND SCREEN. `gate` is declared in the module's
 * chain_params as a `canvas` param with `as_page: true`, so the planner emits
 * it as an ordinary knobs page that happens to carry a drawer. It sits in the
 * jog rotation beside the grid, the eight encoders work on it unchanged, and
 * the host paints the header, bank bar and footer AROUND the body it hands
 * us. A drawer is given a FRAME, not the screen -- which is what makes it
 * structurally impossible for the picture to land on top of the text.
 */

import {
    createController, LAYOUT_MOVY, LAYOUT_LIST
} from '/data/UserData/schwung/shared/param_pages/page_controller.mjs';
import {
    decodeInput, applyInput, isHardwarePadPress
} from '/data/UserData/schwung/shared/param_pages/page_input.mjs';
import { frameCtx } from '/data/UserData/schwung/shared/param_pages/frame_ctx.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';
import {
    setLED, invalidateLedCache
} from '/data/UserData/schwung/shared/input_filter.mjs';
import {
    Black, BrightRed, DeepRed, RustRed, PaleSalmon,
    DullGreen, NeonGreen, TealGreen, PaleGreen
} from '/data/UserData/schwung/shared/constants.mjs';

/* The prefix WE choose. The controller asks getParam for "<prefix>:<key>";
 * host_module_get_param already applies the real slot+component prefix, so
 * ours is stripped back off on the way through. */
const PREFIX = "tg";

/* How far outside the ring the cursor arc sits, in pixels. One would touch the
 * segments and read as part of them. */
const CURSOR_GAP = 2;

/* ------------------------------------------------------------------ pads --
 *
 * Note 68 is the BOTTOM-LEFT pad and they run left-to-right then upward, so
 * 92..99 is the top row. Steps do NOT follow that order -- see stepToNote.
 */
const PAD_FIRST = 68;
const PAD_COUNT = 32;
const PAD_COLS = 8;

/*
 * STEP ORDER IS READING ORDER; THE HARDWARE'S IS NOT.
 *
 * Move numbers its pads from the BOTTOM-left upward -- note 68 is the bottom
 * row's first pad and 92..99 is the top row. A step sequencer reads the other
 * way: step 1 belongs in the top-left corner, then along the top row, then the
 * row beneath it.
 *
 * Mapping here rather than anywhere else is deliberate. The pattern, the ring
 * and the knobs all speak in STEP indices; only these two functions know the
 * note numbers, so nothing downstream has to remember which way up the grid
 * is. It also means a 16-step pattern lights the top two rows, which is where
 * the eye starts.
 */
function stepToNote(step) {
    const row = (step / PAD_COLS) | 0;          /* 0 = top */
    const col = step % PAD_COLS;
    return PAD_FIRST + (3 - row) * PAD_COLS + col;
}

function noteToStep(note) {
    const idx = note - PAD_FIRST;
    if (idx < 0 || idx >= PAD_COUNT) return -1;
    const rowUp = (idx / PAD_COLS) | 0;         /* 0 = bottom */
    const col = idx % PAD_COLS;
    return (3 - rowUp) * PAD_COLS + col;
}

/* LEDs written per frame during the first paint. Eight is the house figure;
 * 32 pads is four frames. */
const LED_PER_FRAME = 8;

/*
 * MOVE STILL OWNS THESE LEDS.
 *
 * `pad_block` suppresses pad INPUT only. The shim strips Move's own LED
 * packets in overtake mode, and a ui_chain.js component is not overtake -- so
 * Move repaints pads on its own events (a track switch, clip playback, a view
 * change) right over ours, and setLED's cache then claims colours the hardware
 * stopped showing. Painting only on change can therefore never recover.
 *
 * One forced pad per frame walks the whole grid about twice a second, which
 * costs one packet a frame and is the only thing that heals a clobber.
 */
let ledPaintCursor = 0;      /* progressive first paint */
let ledHealCursor = 0;       /* rolling forced repaint */
let padsPainted = false;

/*
 * The hierarchy the DSP deliberately will not serve.
 *
 * Order is load-bearing twice. `gate` leads `params`, so the ring is the page
 * the planner emits first. And attack/decay/sustain/release are the first four
 * `knobs`, which puts them in positions 0-3 -- the TOP ROW of the 2x4 grid --
 * because a viz group whose members do not land on one row is dropped whole,
 * silently, and the envelope graphic is the point of grouping them.
 */
const HIERARCHY = JSON.stringify({
    modes: null,
    levels: {
        /*
         * TWO LEVELS, AND THE RING LEADS.
         *
         * A canvas page carries its LEVEL's knobs, and a level with knobs also
         * emits a cells page for them -- so one level could never hold both the
         * six pattern controls and the eight envelope/output ones without
         * spilling into a third grid page anyway. Splitting them is what makes
         * the pattern page self-contained and hands depth/mix/slot/stopped
         * their cells back.
         *
         * `gate` is FIRST in root's params, and the planner now emits a canvas
         * page ahead of its level's grids when it is declared ahead of every
         * cell-bearing param. So the ring is page 1 in the bank bar, not just
         * where the cursor happens to land.
         */
        root: {
            name: "Gate",
            children: null,
            /*
             * NO `cursor` AND NO `step` KNOB.
             *
             * The pads do both, and better: a pad IS the step, so selecting
             * one and toggling it is a single press on the thing itself
             * rather than two encoders spent walking an index. Leaving the
             * knobs in as a second route meant four of the six were spent on
             * what the grid already does, and the two surfaces could disagree
             * for a rotation at a time.
             *
             * Both keys keep their chain_params metadata -- the pads write
             * them, and the ring reads the cursor back out of `ui` -- they
             * simply have no cell. `step_amount` leads because it is what you
             * reach for straight after choosing a pad.
             */
            knobs: ["step_amount", "random", "length", "rate"],
            params: ["gate", { level: "settings", label: "Settings" }]
        },
        settings: {
            name: "Settings",
            children: null,
            /* attack/decay/sustain/release lead, so they are positions 0-3 --
             * the TOP ROW, contiguous. A viz group straddling two rows is
             * dropped whole and in silence, and the envelope graphic is the
             * point of grouping them. */
            knobs: ["attack", "decay", "sustain", "release",
                    "hold", "amount", "slot", "stopped"],
            params: ["attack", "decay", "sustain", "release",
                     "hold", "amount", "slot", "stopped"]
        }
    }
});

let ctl = null;
let needsRedraw = true;

/*
 * SHIFT IS READ, NOT WATCHED.
 *
 * This tracked CC 49 from onMidiMessageInternal, and the shim does not forward
 * it: the forwarded list is CC 3, 14, 51, 40-43, 71-78, 88 plus notes 0-7 and
 * (with pad_block) 68-99. The shim tracks Shift itself and publishes it in
 * shared memory, which is why the rest of shadow_ui.js reads this accessor
 * rather than listening for a CC.
 *
 * So the flag was permanently false and every Shift gesture the grid offers --
 * fine knobs, the section picker, reveal-values -- silently did nothing.
 */
function shiftHeld() {
    return typeof shadow_get_shift_held === "function" && !!shadow_get_shift_held();
}

/* The ring's local clock. `ui` arrives on the slow rotation (an extra key is
 * not in page.keys, so it never gets the live fast-read), which is about six
 * times a second -- visibly steppy for a 16th-note playhead. So the read is an
 * ANCHOR and the playhead is advanced locally between anchors at frame rate.
 * Exactly the trick the DSP uses against get_beat_position()'s block
 * interpolation, for the same reason. */
let anchor = null;   /* { phase, msStep, length, atMs, running } */

/*
 * THE CURSOR THE DEPENDENT KNOBS WERE LAST READ FOR.
 *
 * `step` and `step_amount` are not independent parameters -- they are VIEWS on
 * whichever step the cursor is over, and the host has no way to know that. It
 * refreshes them on its own staggered rotation, so for up to a full rotation
 * after the cursor moves they still show the PREVIOUS step's gate and amount,
 * while the ring has already moved its bracket. The two surfaces disagree
 * about which step you are editing, which is exactly what it looks like: an
 * off-by-one that is really a staleness.
 *
 * Worse than cosmetic: turn `step` during that window and the value shown is
 * the old step's, so the write lands somewhere the screen never described.
 */
let valuedCursor = -1;

function movy() {
    return {
        fillRect: fill_rect,
        print: print,
        textWidth: text_width,
        setPixel: set_pixel,
        line: typeof draw_line === "function" ? draw_line : undefined,
        fillCircle: typeof fill_circle === "function" ? fill_circle : undefined,
        drawCircle: typeof draw_circle === "function" ? draw_circle : undefined,
        drawArc: typeof draw_arc === "function" ? draw_arc : undefined,
    };
}

function stripPrefix(fullKey) {
    if (typeof fullKey !== "string") return "";
    return fullKey.startsWith(PREFIX + ":") ? fullKey.slice(PREFIX.length + 1) : fullKey;
}

/* Param View setting: 0 = list, 1 = knobs. The constant lives in
 * shadow_ui_param_pages.mjs, which is host-internal and not on a module's
 * import path -- only the getter is published, as globalThis. */
const PARAM_VIEW_KNOBS = 1;

function layout() {
    /* Mirror paramPagesLayout()'s own rule so this module reads like every
     * other screen: the screen reader and the user's Param View setting each
     * force the list, where a 30px cell can be spoken and a grid cannot. */
    try {
        if (typeof tts_get_enabled === "function" && tts_get_enabled()) return LAYOUT_LIST;
        if (typeof param_view_get_mode === "function") {
            return param_view_get_mode() === PARAM_VIEW_KNOBS ? LAYOUT_MOVY : LAYOUT_LIST;
        }
    } catch (e) { /* fall through to the grid */ }
    return LAYOUT_MOVY;
}

/* ------------------------------------------------------------- the ring -- */

/*
 * PARSED ONCE PER DISTINCT ANSWER, NOT ONCE PER FRAME.
 *
 * drawRing runs at frame rate and this builds an array; the `ui` string only
 * changes when the read rotation comes round, roughly six times a second. So
 * ~90% of the parses produced an identical object for the collector to throw
 * away. The cache is also what the pad painter reads, so the two surfaces
 * cannot disagree about the pattern.
 */
let uiCache = { raw: null, parsed: null };

function parseUiCached(raw) {
    if (raw === uiCache.raw) return uiCache.parsed;
    const parsed = parseUi(raw);
    uiCache = { raw, parsed };
    return parsed;
}

function parseUi(raw) {
    if (typeof raw !== "string" || !raw) return null;
    const f = raw.split(":");
    if (f.length < 7) return null;
    const steps  = parseInt(f[0], 16);
    const ties   = parseInt(f[1], 16);
    const length = parseInt(f[2], 10);
    const phase  = parseFloat(f[3]);
    const msStep = parseFloat(f[4]);
    const running = f[5] === "1";
    if (!isFinite(steps) || !isFinite(length) || length < 1) return null;
    const cursor = parseInt(f[6], 10);
    /* Per-step depths, two hex digits each. Absent (an older DSP) means full,
     * never zero -- a missing read must not draw a ring of silent steps. */
    const dRaw = f[7] || "";
    const depths = new Array(length);
    for (let i = 0; i < length; i++) {
        const v = parseInt(dRaw.substr(i * 2, 2), 16);
        depths[i] = isFinite(v) ? v / 255 : 1;
    }
    return {
        depths,
        steps,
        ties: isFinite(ties) ? ties : 0,
        length,
        phase: isFinite(phase) ? phase : 0,
        msStep: isFinite(msStep) && msStep > 0 ? msStep : 0,
        running,
        /* 0-based here; the `cursor` PARAM is 1-based because it is shown as a
         * step number. The two spellings meet only in the DSP. */
        cursor: isFinite(cursor) ? cursor : 0
    };
}

/* Where the playhead is NOW, extrapolated from the last anchor. */
function livePhase(nowMs) {
    if (!anchor) return null;
    if (!anchor.running || anchor.msStep <= 0) return anchor.phase;
    const steps = (nowMs - anchor.atMs) / anchor.msStep;
    let ph = anchor.phase + steps;
    ph = ph % anchor.length;
    return ph < 0 ? ph + anchor.length : ph;
}

/*
 * ctx is FRAME-SCOPED: (0,0) is the body band's top-left and ctx.width /
 * ctx.height are the band's, not the display's. There is no accessor here that
 * reaches a screen pixel, so nothing drawn can collide with the header or the
 * footer -- and every primitive is clipped to the frame besides.
 *
 * drawArc takes 0 degrees at TWELVE O'CLOCK, increasing clockwise, which is
 * already a step sequencer's reading order.
 */
function drawRing(ctx, o) {
    const w = ctx.width, h = ctx.height;
    if (w < 24 || h < 16) return;

    const vals = (o && o.values) || {};
    const u = parseUiCached(vals.ui);

    if (u) {
        /* Re-anchor whenever a fresh read lands. Comparing the raw string
         * rather than storing blindly keeps the local clock running smoothly
         * when the rotation hands back the same answer twice. */
        if (!anchor || anchor.raw !== vals.ui) {
            anchor = {
                raw: vals.ui,
                phase: u.phase, msStep: u.msStep, length: u.length,
                running: u.running, atMs: o.nowMs
            };
        }
        anchor.length = u.length;
        anchor.msStep = u.msStep;
        anchor.running = u.running;
    }

    /* A read that has not answered must not become a picture: say so and stop,
     * rather than draw a ring of a guessed length. */
    if (!u) {
        const msg = "...";
        ctx.print(Math.max(0, (w - ctx.textWidth(msg)) >> 1), (h >> 1) - 3, msg, 1);
        return;
    }

    /*
     * THE TEXT GETS ITS OWN COLUMNS, THE RING GETS WHAT IS LEFT.
     *
     * Centring the ring on the whole band and then printing beside it is how
     * the two collided before: the ring's radius came from the band and the
     * text was laid over it. Here the side columns are measured FIRST and the
     * ring is centred in the gap between them, so the radius can only shrink
     * to make room -- they cannot overlap whatever the band turns out to be.
     */
    const lenTxt  = String(u.length);
    const rateTxt = String(vals.rate === undefined || vals.rate === null ? "" : vals.rate);

    /*
     * THE SELECTED STEP'S AMOUNT, NAMED ON THE PAGE THAT EDITS IT.
     *
     * It is knob 1 here, but a canvas page draws no cells -- so the encoder
     * worked and nothing said what it was or what it had done, and the only
     * way to read it back was to touch the knob and watch the header. It costs
     * no read: `step_amount` is one of this page's own keys, so it is already
     * in the values map the drawer is handed.
     *
     * Drawn from the DSP's value rather than from the ring's `depths` run: the
     * depths are a snapshot on the slow rotation, while this key is on the
     * page and refreshes with the knob, so it is the one that keeps up with a
     * turn.
     */
    const rawAmt = vals.step_amount;
    let amtTxt = "";
    if (rawAmt !== undefined && rawAmt !== null && rawAmt !== "") {
        const n = Number(rawAmt);
        if (isFinite(n)) amtTxt = Math.round(n * 100) + "%";
    }

    const leftW  = Math.max(ctx.textWidth(lenTxt), ctx.textWidth(amtTxt));
    const rightW = ctx.textWidth(rateTxt);
    const pad = 2;

    ctx.print(0, 0, lenTxt, 1);
    if (rateTxt) ctx.print(Math.max(0, w - rightW), 0, rateTxt, 1);
    /* Bottom-left, under the length: the two numbers that describe the
     * pattern on the left, the one that describes time on the right. */
    if (amtTxt) ctx.print(0, h - 5, amtTxt, 1);

    const gapL = leftW + pad;
    const gapR = w - rightW - pad;
    const cx = (gapL + gapR) >> 1;
    const cy = h >> 1;
    /* The cursor arc lives OUTSIDE the ring, so the ring gives up its radius
     * to make room. Sizing the ring first and then drawing past it is how a
     * marker ends up clipped against the band edge. */
    const r = Math.max(5, Math.min((gapR - gapL) >> 1, cy) - 1 - CURSOR_GAP - 1);

    const n = u.length;
    const sweep = 360 / n;
    /* Visible between segments, but never so wide that a 32-step ring becomes
     * 32 dots. */
    const gap = Math.min(sweep * 0.3, 4);

    for (let i = 0; i < n; i++) {
        const on = (u.steps >> i) & 1;
        /* A tie closes the gap INTO the next segment, so a held pair reads as
         * one long arc -- which is what a tie sounds like. */
        const tiedOut = on && ((u.ties >> i) & 1) && ((u.steps >> ((i + 1) % n)) & 1);
        const a0 = i * sweep + gap / 2;
        const sw = sweep - gap + (tiedOut ? gap : 0);

        if (on) {
            /*
             * THICKNESS IS LEVEL, and the OUTER arc is what says "on".
             *
             * Scaling thickness alone would make a quiet ON step identical to
             * an OFF one at the bottom of the range -- the pattern would look
             * like it had holes that the audio does not have. The arc at r is
             * therefore unconditional for an on-step and the accent is drawn
             * INWARD from it, so on/off stays a different reading from loud/
             * quiet.
             */
            const d = u.depths[i] === undefined ? 1 : u.depths[i];
            ctx.drawArc(cx, cy, r, a0, sw, 1);
            if (d >= 0.34) ctx.drawArc(cx, cy, r - 1, a0, sw, 1);
            if (d >= 0.67) ctx.drawArc(cx, cy, r - 2, a0, sw, 1);
        } else {
            ctx.drawArc(cx, cy, r - 1, a0, sw, 1);
        }
    }

    /*
     * THE CURSOR IS AN ARC OUTSIDE THE RING, THE PLAYHEAD A DOT INSIDE IT.
     *
     * It was a three-pixel radial tick, which is nearly invisible next to a
     * three-pixel-thick segment and gave no sense of WHICH segment it belonged
     * to -- at 32 steps a tick sits between two of them as readily as on one.
     * A two-deep arc spanning the step's own sweep, at a LARGER radius than
     * the ring, is unambiguous: it brackets exactly the segment being edited
     * and reads as a selection rather than as another mark on the pattern.
     *
     * Outside also keeps it legible over an ON segment, where the band is
     * already thick, and the shape difference from the playhead dot is what
     * stops the two reading as one marker that occasionally splits in two.
     */
    if (u.cursor >= 0 && u.cursor < n) {
        const a0 = u.cursor * sweep + gap / 2;
        const sw = sweep - gap;
        ctx.drawArc(cx, cy, r + CURSOR_GAP, a0, sw, 1);
        ctx.drawArc(cx, cy, r + CURSOR_GAP + 1, a0, sw, 1);
    }

    const ph = livePhase(o.nowMs);
    if (ph !== null && isFinite(ph)) {
        const deg = (ph / n) * 360 - 90;
        const rad = deg * Math.PI / 180;
        const pr = r - 5;
        if (pr > 1) {
            ctx.fillCircle(Math.round(cx + Math.cos(rad) * pr),
                           Math.round(cy + Math.sin(rad) * pr), 2, 1);
        }
    }

    /* The step under the CURSOR, in the middle -- the thing being edited, not
     * the thing being played. The playhead already draws itself. */
    const label = String(u.cursor + 1);
    ctx.print(cx - (ctx.textWidth(label) >> 1), cy - 3, label, 1);
}

/* --------------------------------------------------------------- pad LEDs -- */

/*
 * KEEP THE PATTERN FRESH EVEN WHEN THE RING IS NOT ON SCREEN.
 *
 * The cache above is filled by drawRing, as a side effect of drawing -- and
 * `ui` is an extra_key of the CANVAS page, so on any other page the controller
 * does not even ask for it. The pads, though, are lit the whole time the
 * module is up. So editing Len or Rate from the cells page left the grid
 * showing the previous length: shorten a 32-step pattern and the pads that
 * should have gone dark stayed lit, because nothing had told the painter.
 *
 * One read, throttled, and only when the ring is not already doing it. At
 * ~7Hz that is a few percent of the read budget, which is the right price for
 * a surface that is always visible -- and it is skipped entirely on the page
 * where the ring keeps the cache current for free.
 */
const UI_READ_TICKS = 8;
let uiReadTick = 0;

function refreshUiOffRing() {
    if (ctl && ctl.onCanvasPage && ctl.onCanvasPage()) return;   /* drawRing has it */
    if ((++uiReadTick % UI_READ_TICKS) !== 0) return;
    if (typeof host_module_get_param !== "function") return;
    const raw = host_module_get_param("ui");
    /* null is a read that did not complete and "" is a key that produced
     * nothing -- neither is news about the pattern, so keep the last answer
     * rather than blanking the grid on a timeout. */
    if (raw !== null && raw !== undefined && raw !== "") parseUiCached(raw);
}

/*
 * BRIGHTNESS IS THE STEP'S AMOUNT; SELECTION IS ONE NOTCH LIGHTER.
 *
 * Two facts on one channel, so they have to be separable by eye. They are,
 * because the amount ramp deliberately stops short of the top: a selected pad
 * always sits one rung above where its own amount would have put it, and the
 * top rung is reachable ONLY by selection.
 *
 * The ramps are ordered by measured luminance rather than by name, because the
 * palette's names do not track brightness -- `BrightGreen` is an alias for
 * bright YELLOW, and the dim/dark partners are per-hue rather than a single
 * scale. As a percentage of DullGreen: TealGreen 55, NeonGreen 72, DullGreen
 * 100, PaleGreen 110. So an amount of zero still shows at about half
 * brightness and the pad stays readable -- a step that is ON is visibly on
 * however quiet it is, which is the point of lighting it at all.
 *
 * Reds run the same way against BrightRed: DeepRed 40, RustRed 72,
 * BrightRed 100, PaleSalmon well above it for the selected top rung.
 */
const GREEN_RAMP = [TealGreen, NeonGreen, DullGreen, PaleGreen];
const RED_RAMP   = [DeepRed,   RustRed,   BrightRed, PaleSalmon];

function padColour(u, i) {
    if (!u || i >= u.length) return Black;          /* past the pattern: dark */

    const on = (u.steps >> i) & 1;
    const selected = (i === u.cursor);

    /* Three rungs for the amount, and selection borrows the fourth. An absent
     * depth means full, never silent -- the same rule the state migration
     * uses, for the same reason. */
    const amount = (u.depths && u.depths[i] !== undefined) ? u.depths[i] : 1;
    let rung = amount >= 0.67 ? 2 : (amount >= 0.34 ? 1 : 0);
    if (selected) rung += 1;

    return (on ? GREEN_RAMP : RED_RAMP)[rung];
}

/*
 * Paint the pads.
 *
 * Three jobs in one pass, in the order that matters:
 *
 *   1. The FIRST paint covers all 32, including the ones that end up dark.
 *      loadModuleUi does no LED work of its own -- the clear-and-init
 *      lifecycle is the overtake path -- so init() inherits whatever Move
 *      happened to leave lit.
 *   2. After that, setLED's own cache suppresses unchanged pads, so the steady
 *      state costs nothing.
 *   3. One pad per frame is repainted FORCED regardless, which is what heals a
 *      Move clobber (see the note on ledHealCursor).
 *
 * The first paint is spread at LED_PER_FRAME because move_midi_internal_send
 * refuses a write when the buffer is full and setLED then caches -1 so the
 * next pass retries -- pacing on that is cheaper than discovering it.
 */
function paintPads(u) {
    if (!u) return;

    if (!padsPainted) {
        const end = Math.min(ledPaintCursor + LED_PER_FRAME, PAD_COUNT);
        for (let i = ledPaintCursor; i < end; i++) {
            setLED(stepToNote(i), padColour(u, i));
        }
        ledPaintCursor = end;
        if (ledPaintCursor >= PAD_COUNT) padsPainted = true;
        return;
    }

    for (let i = 0; i < PAD_COUNT; i++) setLED(stepToNote(i), padColour(u, i));

    setLED(stepToNote(ledHealCursor), padColour(u, ledHealCursor), true);
    ledHealCursor = (ledHealCursor + 1) % PAD_COUNT;
}

/*
 * A pad press. Toggles the step AND selects it, so the knobs edit what you
 * just hit.
 *
 * Both writes go through the same host_module_set_param the knobs use, so
 * there is ONE write path into the pattern and the ring, the cells and the
 * pads cannot end up disagreeing about it.
 */
function onPadPress(note) {
    const u = uiCache.parsed;
    const i = noteToStep(note);
    if (i < 0) return false;
    /* Past the end of the pattern: dark, and does nothing. */
    if (u && i >= u.length) return true;

    if (typeof host_module_set_param !== "function") return true;

    /* 1-based on the wire: the cursor is numbered the way the ring is. */
    host_module_set_param("cursor", String(i + 1));

    /*
     * PLAIN PRESS IS ON/OFF; SHIFT IS THE TIE.
     *
     * A three-way cycle under one finger would make the frequent gesture --
     * drawing a pattern -- the confusing one, and you would tap through Tie
     * every time you wanted to clear a step. Shift is the modifier Move uses
     * for "the other meaning" everywhere else, and it keeps the tie reachable
     * now that the `step` knob is gone: without it, removing that knob would
     * have quietly deleted the feature.
     *
     * Shift only means anything on a step that SOUNDS -- a tie is a hold into
     * the next step and an off step has nothing to hold -- so on an off step
     * it turns it on, which is what you wanted anyway.
     */
    const wasOn  = u ? ((u.steps >> i) & 1) : 0;
    const wasTie = u ? ((u.ties  >> i) & 1) : 0;

    let next;
    if (shiftHeld()) next = wasOn ? (wasTie ? "On" : "Tie") : "On";
    else             next = wasOn ? "Off" : "On";
    host_module_set_param("step", next);

    /* The next read brings the truth; moving the cache now keeps the ring and
     * the LEDs with the finger rather than with the rotation. */
    if (u) {
        const bit = 1 << i;
        if (next === "Off")      { u.steps &= ~bit; u.ties &= ~bit; }
        else if (next === "On")  { u.steps |=  bit; u.ties &= ~bit; }
        else                     { u.steps |=  bit; u.ties |=  bit; }
        u.cursor = i;
    }
    needsRedraw = true;
    return true;
}

/*
 * Repaint whatever Move believes it is showing.
 *
 * Outside overtake the shim's snapshot tracks MOVE's writes rather than ours,
 * so it is exactly the state to hand back. A note Move never lit has no cached
 * colour, and dark is the truthful answer for it.
 */
function restoreMovePads() {
    const snap = (typeof shadow_get_pad_led_snapshot === "function")
        ? shadow_get_pad_led_snapshot() : null;
    invalidateLedCache();
    for (let note = PAD_FIRST; note < PAD_FIRST + PAD_COUNT; note++) {
        const c = snap && typeof snap[note] === "number" ? snap[note] : 0;
        setLED(note, c, true);
    }
    padsPainted = false;
    ledPaintCursor = 0;
}

/* ------------------------------------------------------------- lifecycle -- */

function buildController() {
    ctl = createController({
        getParam(fullKey) {
            const key = stripPrefix(fullKey);
            /* The one key the DSP refuses, answered from here. */
            if (key === "ui_hierarchy") return HIERARCHY;
            if (typeof host_module_get_param !== "function") return null;
            return host_module_get_param(key);
        },
        setParam(fullKey, value) {
            const key = stripPrefix(fullKey);
            if (typeof host_module_set_param === "function") {
                host_module_set_param(key, String(value));
            }
        },
        announce,
        /* "My Presets" and "Module", bound by the host with our slot and
         * component already applied. A module building its own controller used
         * to get neither, which is why these shims exist. */
        trailingMenus() {
            return (typeof shadow_component_trailing_menus === "function")
                ? shadow_component_trailing_menus() : [];
        },
        /* Our own drawer, in this same file -- no loader needed. */
        drawCanvasPage(drawCtx, band, canvas, payload) {
            drawRing(frameCtx(drawCtx, band), payload || {});
        },
    });

    ctl.load({ slot: 0, component: PREFIX, prefix: PREFIX });
    ctl.setLayout(layout());
    landOnRing();
}

/*
 * HAVE WE ACTUALLY LANDED YET.
 *
 * landOnRing used to be called once, from here, and that is only correct if
 * the contract resolved on the first try. It need not: a param read can fail
 * or time out -- the whole reason the host has a tri-state read rule -- and
 * the controller then has NO pages for the scan to find. reloadIfChanged
 * plans them a moment later and leaves you on page 0, which is how "open the
 * module and you are on the settings page" happens intermittently and is
 * impossible to reproduce on demand.
 *
 * So it is an outcome, not an event: retry every tick until it succeeds once.
 */
let landed = false;

/* Open on the ring whatever order the planner emitted the pages in -- found by
 * what the page IS, not by an index that a contract change would shift. */
function landOnRing() {
    if (landed) return;
    const pages = ctl.pages || [];
    for (let i = 0; i < pages.length; i++) {
        if (pages[i] && pages[i].canvas) {
            ctl.goToPage(i);
            landed = true;
            return;
        }
    }
}

function init() {
    anchor = null;
    landed = false;
    valuedCursor = -1;
    uiCache = { raw: null, parsed: null };
    uiReadTick = 0;
    padsPainted = false;
    ledPaintCursor = 0;
    ledHealCursor = 0;
    /* The LED cache is module-level and SHARED with the on-screen keyboard and
     * the knob-grid LEDs, so on the way in it may describe somebody else's
     * painting. Drop it and let the first pass re-emit everything. */
    invalidateLedCache();
    needsRedraw = true;
    buildController();
}

function tick() {
    /* RESTATED every frame, never edged: the shim drops pad_block unilaterally
     * on the display-mode edge and at init, so a JS mirror of it latches and
     * leaves the pads dead with no gesture that puts them back. Same reason
     * shadow_ui.js reconciles it rather than remembering it. */
    if (typeof host_pad_block === "function") host_pad_block(1);

    if (!ctl) return;

    ctl.reloadIfChanged();
    ctl.tick();              /* exactly one param read */
    landOnRing();            /* no-op once it has succeeded */

    /* Driven from the same parsed answer the ring draws, so the two surfaces
     * cannot disagree -- and kept current off the ring page too. */
    refreshUiOffRing();
    const u = uiCache.parsed;
    paintPads(u);

    /*
     * The cursor moved, so re-read what depends on it. revalue() flushes any
     * due write, drops the value cache and re-warms the page -- the keys come
     * back for the step the ring is actually pointing at.
     *
     * Gated on a real change: it is a whole-page refresh, and running it every
     * frame would spend the read budget several times over.
     */
    if (u && u.cursor !== valuedCursor) {
        valuedCursor = u.cursor;
        ctl.revalue();
    }

    /* The ring animates, so it cannot wait for an input to ask for a repaint.
     * Everything else is cheap enough that redrawing with it costs nothing we
     * would otherwise save. */
    const ctx = movy();
    clear_screen();                       /* the frame is OURS, not the library's */
    ctl.render(ctx, { title: "Trance Gate" });
    /* The library never clears the screen -- that is what lets render() sit
     * inside a rect a caller owns -- so anything full-screen comes back to us.
     * Today that is the enum peek. Skipping this call is a SILENT bug: the
     * controller still tracks the peek and still swallows the Back that
     * dismisses it, it is simply painted nowhere. Two shipped modules had
     * exactly that. */
    ctl.renderOverlays(ctx, { clearScreen: clear_screen });
    needsRedraw = false;
}

function onMidiMessageInternal(data) {
    if (!ctl) return;

    /*
     * PADS FIRST. decodeInput answers null for notes 68-99 -- it knows about
     * the eight knob-touch notes and nothing else in the note range -- and the
     * null guard below returns, so routing pads through it drops them
     * silently. isHardwarePadPress is note-on with velocity, which is the
     * right edge: Move sends a release as either 0x80 or a 0x90 with velocity
     * 0, and neither should toggle anything.
     */
    if (isHardwarePadPress(data)) { onPadPress(data[1]); return; }

    const intent = decodeInput(data, { shift: shiftHeld() });
    if (!intent) return;

    const todo = applyInput(ctl, intent, { nowMs: Date.now() });
    if (!todo) { needsRedraw = true; return; }

    if (todo.action === "menu" && todo.entry && todo.entry.action) {
        /* The controller never performs an action -- what "Save" means is the
         * host's to know -- so this forwards which one was chosen. */
        if (typeof shadow_component_run_action === "function") {
            shadow_component_run_action(todo.entry.action);
        }
        return;
    }

    /*
     * `open` is deliberately unhandled.
     *
     * It fires when a click lands on a divable param -- here, only the two
     * enums with more than two options (Slot, Rate). The host answers it with
     * openEnumPicker, which is a shadow_ui screen a module cannot address. The
     * cost of letting it fall through is nil: turning the knob already cycles
     * the options and the controller's own enum peek rises over the grid to
     * show the list while you turn. Opening a second, worse picker of our own
     * would be the only way to make this worse.
     */

    needsRedraw = true;
}

/* Back is offered to us first. Consume it only while there is somewhere to go
 * back TO -- always returning truthy means the user can never leave, and Back
 * is the only host-processed exit on this screen. applyInput already unwinds
 * the peek, the picker and an entered menu one layer at a time and only then
 * answers "exit", so anything else here would be a second, disagreeing copy of
 * that rule. */
function handleBack() {
    /*
     * PUT MOVE'S PADS BACK.
     *
     * Move only writes a pad LED when its value CHANGES, so once we have
     * painted over the grid it has no reason to write anything and our
     * colours stay after we are gone.
     *
     * The host reconciles this on the ownership edge, which covers every exit
     * -- a Track tap, Menu, Shift+Vol, co-run -- and is the real fix. This
     * covers the common one without waiting for a host deploy, and costs
     * nothing when the host does it too: the same colours, written twice.
     */
    restoreMovePads();
    return false;
}

/* Pure helpers, exported for tests. The host never looks at this. */
globalThis.chain_ui_test = {
    parseUi,
    padColour,
    stepToNote,
    noteToStep,
    /* Which ring segment the cursor bracket is drawn over, given a `ui`
     * string. Must equal the cell's value minus one, always. */
    cursorSegment: (raw) => { const u = parseUi(raw); return u ? u.cursor : -1; },
    centreLabel: (raw) => { const u = parseUi(raw); return u ? String(u.cursor + 1) : "?"; },
};

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal,
    handleBack
};
