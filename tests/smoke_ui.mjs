/*
 * Smoke test for ui_chain.js -- it RUNS the module, it does not merely load it.
 *
 * WHY THIS EXISTS. An earlier check imported the file and reported success,
 * which proved only that the syntax parsed and the imports resolved. The whole
 * pad constants block was missing at the time -- CURSOR_GAP, PAD_FIRST,
 * padsPainted and three others used but never declared -- and an import cannot
 * see that, because an undefined identifier is a RUNTIME error in the function
 * that reaches it. On the device it surfaced as `init()` throwing, and because
 * shadow_ui's stderr goes to /dev/null and loadModuleUi calls init() unguarded,
 * the only symptom was a component that would not open. Nothing was logged.
 *
 * So this calls every entry point the host calls, with the host's globals
 * stubbed, and fails on a throw. The stubs answer plausibly rather than
 * richly: the point is to execute every line, not to simulate Move.
 */
import { readFileSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { dirname, resolve } from 'node:path';

const SHARED = process.argv[2];
const OUT = process.argv[3];

/* The module imports the device's absolute paths; point them at real sources. */
mkdirSync(OUT, { recursive: true });
const src = readFileSync('src/ui_chain.js', 'utf8')
    .replaceAll('/data/UserData/schwung/shared', SHARED);
writeFileSync(resolve(OUT, 'ui_chain.mjs'), src);

let sent = 0;
let uiLength = 16;              /* what the DSP would report for `length` */
let uiPhase = 3.250;            /* where the playhead sits */
let uiMoving = 1;               /* the `advancing` field, not "transport on" */
let uiWithhold = 0;             /* reads of `ui` still to answer with null */
const litPads = Object.create(null);
const params = Object.create(null);

let clearCount = 0;
let chainParamReads = 0;
for (const n of ['clear_screen', 'fill_rect', 'draw_rect', 'print', 'set_pixel',
                 'draw_line', 'draw_arc', 'fill_circle', 'draw_circle',
                 'host_pad_block', 'shadow_component_run_action']) {
    globalThis[n] = () => 0;
}
/* What the LAST frame put on screen, so a test can assert on the picture the
 * user is actually looking at rather than on a variable near it. */
let frameText = [];
globalThis.clear_screen = () => { clearCount++; frameText = []; return 0; };
globalThis.print = (x, y, str) => { frameText.push(String(str)); return 0; };
globalThis.text_width = (s) => String(s).length * 6;
globalThis.tts_get_enabled = () => false;
globalThis.param_view_get_mode = () => 1;
globalThis.shadow_get_shift_held = () => 0;
globalThis.shadow_component_trailing_menus = () => [];
globalThis.move_midi_internal_send = (pkt) => {
    sent++;
    if (pkt && pkt.length === 4) litPads[pkt[2]] = pkt[3];   /* note -> colour */
    return true;
};
globalThis.host_module_set_param = (k, v) => { params[k] = String(v); return true; };
globalThis.host_module_get_param = (k) => {
    if (k in params) return params[k];
    /* Enough of the real contract to let the controller plan pages. */
    if (k === 'chain_params') { chainParamReads++; return readFileSync(process.env.TG_PARAMS, 'utf8'); }
    if (k === 'ui') {
        /* THE DEVICE ANSWERS ONE KEY PER TICK. The rotation is
         * keys + 1 + extraKeys = 10 stops, so `ui` lands around tick 9 -- long
         * after the first frame. This harness used to answer everything
         * instantly, which is precisely why it could not see the deadlock. */
        if (uiWithhold > 0) { uiWithhold--; return null; }
        return `FFFFFFFF:0:${uiLength}:${uiPhase.toFixed(3)}:125.00:${uiMoving}:2:` + 'FF'.repeat(uiLength);
    }
    if (k === 'state')   return '{"sv":3}';
    if (k === 'name')    return 'TRANCE GATE';
    return null;                       /* a read that did not answer */
};

let failures = 0;
function step(what, fn) {
    try { fn(); console.log(`  ${what.padEnd(44)} ok`); }
    catch (e) { failures++; console.log(`  ${what.padEnd(44)} THREW: ${e}`); }
}

const mod = await import(resolve(OUT, 'ui_chain.mjs'));
const ui = globalThis.chain_ui;

console.log('ui_chain.js smoke:');
step('exports chain_ui with init/tick/onMidi', () => {
    if (!ui || !ui.init || !ui.tick || !ui.onMidiMessageInternal) throw new Error('missing export');
});
/* init() is called UNGUARDED by loadModuleUi: a throw here means the component
 * silently never opens, which is exactly the failure this test was written for. */
step('init() does not throw', () => ui.init());
step('60 ticks do not throw', () => { for (let i = 0; i < 60; i++) ui.tick(); });
/* Step 5 of a 16-step pattern: the fifth pad of the TOP row, note 96. Using
 * 68 + 4 here would press the bottom row, which a 16-step pattern correctly
 * ignores -- and the test would then be asserting nothing. */
const STEP5_NOTE = 92 + 4;
step('a pad press does not throw', () => ui.onMidiMessageInternal([0x90, STEP5_NOTE, 100]));
step('a pad release does not throw', () => ui.onMidiMessageInternal([0x80, STEP5_NOTE, 0]));
step('a knob turn does not throw', () => ui.onMidiMessageInternal([0xB0, 71, 1]));
step('a knob touch does not throw', () => ui.onMidiMessageInternal([0x90, 0, 100]));
step('a jog turn does not throw', () => ui.onMidiMessageInternal([0xB0, 14, 1]));
step('a jog click does not throw', () => ui.onMidiMessageInternal([0xB0, 3, 127]));
step('ticks after input do not throw', () => { for (let i = 0; i < 20; i++) ui.tick(); });
step('a top-row pad selects the matching step', () => {
    if (params.cursor !== '4') throw new Error(`cursor=${params.cursor}, expected index 4`);
    if (!('step' in params)) throw new Error('step never written');
});
/*
 * SHIFT+PAD IS THE ONLY ROUTE TO A TIE now that the `step` knob is gone.
 * Removing that knob without this would have deleted the feature silently,
 * which is exactly the kind of loss a contract change hides.
 */
/* Drive to a known state first. Earlier steps in this file page away from the
 * ring, and the optimistic cache only refreshes while that page is drawn, so
 * asserting an absolute value here would be asserting the test's own history. */
const press = (shift) => {
    globalThis.shadow_get_shift_held = () => (shift ? 1 : 0);
    ui.onMidiMessageInternal([0x90, STEP5_NOTE, 100]);
    globalThis.shadow_get_shift_held = () => 0;
    return params.step;
};
step('a plain press toggles between Off and On only', () => {
    const a = press(false), b = press(false);
    if (!((a === 'On' && b === 'Off') || (a === 'Off' && b === 'On')))
        throw new Error(`plain presses gave ${a} then ${b}`);
});
step('shift+pad reaches Tie, which no knob can now', () => {
    if (press(false) === 'Off') press(false);     /* ensure the step sounds */
    if (press(true) !== 'Tie') throw new Error(`shift+press gave ${params.step}`);
});
step('shift+pad again clears the tie', () => {
    if (press(true) !== 'On') throw new Error(`shift+press gave ${params.step}`);
});
step('shift on a silent step just turns it on', () => {
    press(false);                                  /* -> Off */
    if (params.step !== 'Off') press(false);
    if (press(true) !== 'On') throw new Error(`shift on an off step gave ${params.step}`);
});

step('a pad past the pattern length is ignored', () => {
    const before = params.cursor;
    ui.onMidiMessageInternal([0x90, 68, 100]);   /* bottom-left = step 25 */
    if (params.cursor !== before)
        throw new Error(`cursor moved to ${params.cursor} for a pad past length 16`);
});
step('pad LEDs were actually painted', () => {
    if (sent === 0) throw new Error('no LED packets sent');
});

/*
 * THE PAD ORDER IS READING ORDER, and the hardware's is upside down relative
 * to it. Move numbers from the bottom-left upward (68 is the bottom row, 92-99
 * the top); a step sequencer starts at the top-left. Getting this backwards is
 * invisible in code and obvious the moment a pattern is played, so it is
 * pinned by the corners rather than by restating the formula.
 */
const T = globalThis.chain_ui_test;
const ctlPage = () => T.state() && T.state().pages ? T.state().pages[T.state().pageIndex] : null;
step('step 1 is the TOP-LEFT pad', () => {
    if (T.stepToNote(0) !== 92) throw new Error(`step 0 -> note ${T.stepToNote(0)}, expected 92`);
});
step('the top row runs left to right', () => {
    for (let c = 0; c < 8; c++)
        if (T.stepToNote(c) !== 92 + c) throw new Error(`step ${c} -> ${T.stepToNote(c)}`);
});
step('step 9 starts the second row', () => {
    if (T.stepToNote(8) !== 84) throw new Error(`step 8 -> ${T.stepToNote(8)}, expected 84`);
});
step('step 32 is the BOTTOM-RIGHT pad', () => {
    if (T.stepToNote(31) !== 75) throw new Error(`step 31 -> ${T.stepToNote(31)}, expected 75`);
});
step('a 16-step pattern lights the top two rows', () => {
    const notes = [];
    for (let i = 0; i < 16; i++) notes.push(T.stepToNote(i));
    notes.sort((a, b) => a - b);
    if (notes[0] !== 84 || notes[15] !== 99)
        throw new Error(`16 steps span ${notes[0]}..${notes[15]}, expected 84..99`);
});
step('note -> step is the exact inverse', () => {
    for (let i = 0; i < 32; i++)
        if (T.noteToStep(T.stepToNote(i)) !== i) throw new Error(`step ${i} does not round trip`);
});
step('a pad outside the grid is refused', () => {
    if (T.noteToStep(67) !== -1 || T.noteToStep(100) !== -1) throw new Error('out-of-range accepted');
});

/* The ring's bracket and the cell's number must name the SAME step. */
step('ring marker and cell value agree (0 vs 1 based)', () => {
    for (const oneBased of [1, 3, 8, 16, 32]) {
        const raw = `FFFF:0:32:0.000:125.00:0:${oneBased - 1}:` + 'FF'.repeat(32);
        const seg = T.cursorSegment(raw);
        const label = T.centreLabel(raw);
        if (seg !== oneBased - 1) throw new Error(`cursor ${oneBased}: marker on segment ${seg}`);
        if (label !== String(oneBased)) throw new Error(`cursor ${oneBased}: label "${label}"`);
    }
});

/*
 * THE PADS FOLLOW THE PATTERN FROM EVERY PAGE.
 *
 * The cache the painter reads is filled by the ring's DRAWER, and `ui` is an
 * extra_key of the canvas page -- so off that page nothing asked for it and
 * the grid kept showing the previous length. Shortening a 32-step pattern left
 * the pads that should have gone dark still lit.
 */
step('shortening the pattern darkens the freed pads', () => {
    uiLength = 32;
    for (let i = 0; i < 40; i++) ui.tick();          /* let it settle at 32 */
    const note = T.stepToNote(31);                   /* last step, bottom-right */
    if (!litPads[note]) throw new Error('step 32 was never lit at length 32');

    uiLength = 8;                                    /* as the Len knob would */
    for (let i = 0; i < 40; i++) ui.tick();
    if (litPads[note] !== 0)
        throw new Error(`step 32 still lit (colour ${litPads[note]}) after length 8`);
});
step('and growing it lights them again', () => {
    uiLength = 32;
    for (let i = 0; i < 40; i++) ui.tick();
    const note = T.stepToNote(31);
    if (litPads[note] === 0) throw new Error('step 32 stayed dark after growing to 32');
});

/*
 * PAD COLOUR CARRIES TWO FACTS AND THEY MUST STAY SEPARABLE: how loud the step
 * is, and whether it is the one being edited. Selection is one rung lighter,
 * and the top rung is reachable only by selection -- so no unselected pad can
 * ever look selected, whatever its amount.
 */
const mkUi = (steps, cursor, depths) =>
    `${steps.toString(16).toUpperCase()}:0:8:0.000:125.00:0:${cursor}:` +
    depths.map(d => Math.round(d * 255).toString(16).toUpperCase().padStart(2, '0')).join('');

step('a louder step is a brighter pad', () => {
    const u = T.parseUi(mkUi(0xFF, 7, [0.0, 0.5, 1.0, 1, 1, 1, 1, 1]));
    const [quiet, mid, loud] = [T.padColour(u, 0), T.padColour(u, 1), T.padColour(u, 2)];
    if (quiet === mid || mid === loud)
        throw new Error(`amount ramp collapsed: ${quiet}, ${mid}, ${loud}`);
});
step('an off step does NOT ramp -- its level does nothing', () => {
    const u = T.parseUi(mkUi(0x00, 7, [0.0, 0.5, 1.0, 1, 1, 1, 1, 1]));
    const [a, b, c] = [T.padColour(u, 0), T.padColour(u, 1), T.padColour(u, 2)];
    if (!(a === b && b === c))
        throw new Error(`gaps differ by level: ${a}, ${b}, ${c} -- the ear cannot hear that`);
});
step('the selected pad is lighter than the same step unselected', () => {
    for (const amt of [0.0, 0.5, 1.0]) {
        for (const steps of [0xFF, 0x00]) {   /* on and off both mark selection */
            const depths = new Array(8).fill(amt);
            const off = T.padColour(T.parseUi(mkUi(steps, 7, depths)), 0);  /* cursor elsewhere */
            const on  = T.padColour(T.parseUi(mkUi(steps, 0, depths)), 0);  /* cursor here */
            if (off === on)
                throw new Error(`amount ${amt}, steps ${steps}: selected looks identical`);
        }
    }
});
step('no unselected pad can wear the selected-only colour', () => {
    const selectedTop = new Set();
    for (const steps of [0xFF, 0x00]) {
        const d = new Array(8).fill(1.0);
        selectedTop.add(T.padColour(T.parseUi(mkUi(steps, 0, d)), 0));
    }
    for (const steps of [0xFF, 0x00]) {
        for (const amt of [0, 0.2, 0.4, 0.6, 0.8, 1.0]) {
            const d = new Array(8).fill(amt);
            const c = T.padColour(T.parseUi(mkUi(steps, 7, d)), 0);
            if (selectedTop.has(c))
                throw new Error(`unselected at amount ${amt} wears colour ${c}`);
        }
    }
});
step('a step past the pattern is dark, whatever its amount', () => {
    const u = T.parseUi(mkUi(0xFF, 0, new Array(8).fill(1)));
    if (T.padColour(u, 8) !== 0) throw new Error('past-the-end pad is not dark');
});

/*
 * THE GLOBAL AMOUNT METER. Rendered into a pixel grid rather than asserted on
 * call counts: the thing that can go wrong is geometric -- a bar drawn over
 * the ring, or a fill that does not track the value -- and only pixels show
 * that.
 */
const { frameCtx } = await import(SHARED + '/param_pages/frame_ctx.mjs');
function renderRing(amount, W = 128, Hh = 40) {
    const px = Array.from({ length: Hh }, () => new Array(W).fill(0));
    const put = (x, y, v) => {
        x = Math.round(x); y = Math.round(y);
        if (x >= 0 && x < W && y >= 0 && y < Hh) px[y][x] = v ? 1 : 0;
    };
    const parent = {
        fillRect: (x, y, w, h, v) => { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) put(x + i, y + j, v); },
        print: (x, y, t, v) => { const t2 = String(t); for (let i = 0; i < t2.length; i++) for (let dy = 0; dy < 5; dy++) for (let dx = 0; dx < 3; dx++) put(x + i * 4 + dx, y + dy, v); },
        textWidth: (t) => String(t).length * 4,
    };
    const ctx = frameCtx(parent, { x: 0, y: 0, w: W, h: Hh });
    const depths = 'FF'.repeat(16);
    T.drawRing(ctx, {
        values: { ui: `5555:0:16:3.2:125.00:1:4:` + depths,
                  rate: "1/16", step_amount: "0.75", amount },
        nowMs: 0,
    });
    return { px, clipped: ctx.clipped(), W, Hh };
}
const litInBar = (r) => {
    let n = 0;
    for (let y = 0; y < r.Hh; y++) for (let x = r.W - 5; x < r.W; x++) if (r.px[y][x]) n++;
    return n;
};

step('the meter draws a border even at zero', () => {
    const r = renderRing("0");
    if (litInBar(r) < 20) throw new Error('no border drawn at amount 0');
});
step('the fill tracks the value', () => {
    const [lo, mid, hi] = ["0", "0.5", "1"].map(a => litInBar(renderRing(a)));
    if (!(lo < mid && mid < hi))
        throw new Error(`fill does not increase: ${lo}, ${mid}, ${hi}`);
});
step('a read that did not answer draws the border and no fill', () => {
    const empty = litInBar(renderRing(null));
    const zero  = litInBar(renderRing("0"));
    if (empty !== zero) throw new Error(`unread ${empty} != zero ${zero}`);
});
step('the meter never draws over the ring', () => {
    /* The ring must not reach the bar's column. Compare a render with the bar
     * against the columns it occupies: the ring is centred left of them. */
    const r = renderRing("1");
    let ringPixelsInBarColumns = 0;
    for (let y = 0; y < r.Hh; y++) {
        for (let x = r.W - 5; x < r.W; x++) {
            /* everything in these columns should belong to the meter, which is
             * a solid rectangle: so every lit pixel is within its border */
            if (r.px[y][x] && y < 7) ringPixelsInBarColumns++;   /* above the bar top */
        }
    }
    if (ringPixelsInBarColumns > 20)
        throw new Error('something other than the rate text is in the meter column');
});
step('nothing drawn outside the frame', () => {
    for (const a of ["0", "0.5", "1"]) {
        const r = renderRing(a);
        if (r.clipped !== 0) throw new Error(`amount ${a}: ${r.clipped} pixels clipped`);
    }
});

/*
 * THE RUNNING PLAYHEAD.
 *
 * A white pad sweeps with the gate so the pad grid alone says which step is
 * sounding. Driven from the SAME local clock as the ring's dot -- one anchor,
 * one extrapolation -- so the two surfaces cannot disagree about where the
 * playhead is.
 *
 * The clock is driven here rather than waited on: a test that sleeps for a
 * step is a test that is flaky on a loaded machine.
 */
{
    const WHITE = T.PLAYHEAD_COLOUR;
    const MS_STEP = 125;
    /* 8 steps, all ON, cursor on step 3, full depth. moving/phase vary. */
    const mk = (phase, moving, len = 8) =>
        `FF:0:${len}:${phase.toFixed(3)}:${MS_STEP.toFixed(2)}:${moving}:3:` + 'FF'.repeat(len);
    const seed = (phase, moving, atMs, len = 8) => {
        const raw = mk(phase, moving, len);
        const u = T.parseUi(raw);
        T.anchorFrom(u, raw, atMs);
        return u;
    };
    const headAt = (u, nowMs) => {
        const real = Date.now;
        Date.now = () => nowMs;
        try { return T.playheadStep(u); } finally { Date.now = real; }
    };

    step('the playhead is the step the gate is on', () => {
        const u = seed(3.25, 1, 1000);
        if (headAt(u, 1000) !== 3) throw new Error(`head ${headAt(u, 1000)}`);
    });

    step('one step of elapsed time moves it exactly one pad', () => {
        const u = seed(3.25, 1, 1000);
        if (headAt(u, 1000 + MS_STEP) !== 4) throw new Error(`head ${headAt(u, 1000 + MS_STEP)}`);
        if (headAt(u, 1000 + 2 * MS_STEP) !== 5) throw new Error('second step wrong');
    });

    step('it wraps at Len rather than running past it', () => {
        const u = seed(7.5, 1, 1000);
        const h = headAt(u, 1000 + MS_STEP);      /* 7.5 + 1 = 8.5 -> wraps to 0 */
        if (h !== 0) throw new Error(`head ${h}, expected the wrap to 0`);
    });

    step('a pattern that is NOT moving has no playhead at all', () => {
        const u = seed(3.25, 0, 1000);
        if (headAt(u, 1000) !== -1) throw new Error('a stopped pattern lit a head');
        if (headAt(u, 1000 + 10 * MS_STEP) !== -1) throw new Error('it started sweeping while stopped');
    });

    step('a head past the pattern is refused (stale anchor length)', () => {
        const u = seed(3.25, 1, 1000);
        u.length = 2;                            /* Len knob just shrank it */
        if (headAt(u, 1000) !== -1) throw new Error('lit a pad past the pattern');
    });

    step('the playhead pad is White, and only that pad', () => {
        const u = seed(3.25, 1, 1000);
        const head = headAt(u, 1000);
        let whites = 0;
        for (let i = 0; i < u.length; i++) if (T.ledFor(u, i, head) === WHITE) whites++;
        if (whites !== 1) throw new Error(`${whites} white pads`);
        if (T.ledFor(u, head, head) !== WHITE) throw new Error('the head pad is not White');
    });

    step('White beats the selection rung while the head is over it', () => {
        const u = seed(3.25, 1, 1000);           /* cursor is 3, head is 3 */
        if (u.cursor !== 3) throw new Error('fixture: cursor moved');
        if (T.ledFor(u, 3, 3) !== WHITE) throw new Error('selection hid the head');
    });

    /*
     * THE HEAD IS PAINT, NOT AN EDIT. It must leave no trace once it passes --
     * neither in the pattern bits nor in the pad colour.
     */
    step('a pad returns to its own colour when the head moves on', () => {
        const u = seed(3.25, 1, 1000);
        const before = T.padColour(u, 3);
        const steps = u.steps, ties = u.ties;
        T.ledFor(u, 3, 3);                       /* head over it */
        if (T.ledFor(u, 3, 4) !== before) throw new Error('pad did not revert');
        if (u.steps !== steps || u.ties !== ties) throw new Error('the head edited the pattern');
    });

    /*
     * THE ANCHOR IS SHARED, and this is the case that forced it. It used to be
     * seeded inside drawRing, which runs only on the ring page -- so on the
     * cells pages the clock froze and the sweep with it. anchorFrom is called
     * from the off-ring read too, so a playhead survives paging away.
     */
    step('the clock runs with drawRing never called', () => {
        const u = seed(0.0, 1, 5000);            /* seeded WITHOUT drawing a ring */
        if (headAt(u, 5000 + 2 * MS_STEP) !== 2) throw new Error('frozen off the ring page');
    });

    /* And through the real tick loop, end to end. */
    step('a white pad appears on the grid while playing', () => {
        uiMoving = 1; uiPhase = 5.0; uiLength = 16;
        for (let i = 0; i < 40; i++) ui.tick();
        const white = Object.keys(litPads).filter((n) => litPads[n] === WHITE);
        if (white.length !== 1) throw new Error(`${white.length} white pads on the grid`);
        if (T.noteToStep(+white[0]) !== 5) throw new Error(`white pad is step ${T.noteToStep(+white[0])}`);
    });
}

/*
 * IDLE COST: the screen is painted at the STEP rate, not the frame rate.
 *
 * The only self-moving thing on this page is the ring's playhead, and at 1/16
 * and 120 BPM it advances eight times a second. Painting the whole page
 * forty-four times a second to show it is the bulk of the module's idle cost.
 */
{
    /* A clock we own, so nothing here waits on a real one. */
    const realNow = Date.now;
    /* AHEAD of the real clock, not behind it: the tests above stamped
     * lastInputMs with Date.now(), and a stub clock in the past makes every
     * elapsed time negative. */
    let NOW = realNow() + 10_000_000;
    const ticks = (n) => { for (let i = 0; i < n; i++) ui.tick(); };
    const PAST_TAIL = 5000;          /* > ANIM_TAIL_MS (1600) */

    try {
        Date.now = () => NOW;

        step('a parked playhead redraws nothing at all', () => {
            uiMoving = 0;                        /* nothing animating */
            NOW += PAST_TAIL; ticks(6);            /* let the tail expire */
            NOW += PAST_TAIL; ticks(4);
            clearCount = 0;
            ticks(20);
            if (clearCount !== 0) throw new Error(`${clearCount} redraws with nothing moving`);
        });

        step('an input redraws, and keeps redrawing while it animates', () => {
            clearCount = 0;
            ui.onMidiMessageInternal(new Uint8Array([0x90, T.stepToNote(0), 100]));
            ticks(10);
            if (clearCount < 10) throw new Error(`only ${clearCount} redraws inside the tail`);
        });

        step('...and stops once the animation can no longer be running', () => {
            NOW += PAST_TAIL; ticks(2);
            clearCount = 0;
            ticks(20);
            if (clearCount !== 0) throw new Error(`${clearCount} redraws after the tail expired`);
        });

        step('the playhead crossing a step earns exactly one redraw', () => {
            uiMoving = 1; uiPhase = 0.0;
            NOW += PAST_TAIL; ticks(10);           /* re-anchor, expire the tail */
            NOW += PAST_TAIL; ticks(4);
            clearCount = 0;
            ticks(5);                            /* same step, same clock */
            const idle = clearCount;
            NOW += 130;                            /* one 1/16 step at 120 BPM */
            ticks(5);
            if (clearCount - idle < 1) throw new Error('crossing a step did not redraw');
            if (clearCount - idle > 2) throw new Error(`${clearCount - idle} redraws for one crossing`);
        });

        step('a pattern edit redraws even with the playhead parked', () => {
            uiMoving = 0;
            NOW += PAST_TAIL; ticks(10);
            NOW += PAST_TAIL; ticks(4);
            clearCount = 0;
            ticks(4);
            uiLength = 8;                        /* as the Len knob would */
            ticks(20);                           /* the off-ring read is 1-in-8 */
            if (clearCount === 0) throw new Error('a pattern change drew nothing');
            uiLength = 16;
            ticks(20);
        });

        /*
         * reloadIfChanged is NOT a cheap guard: it reads chain_params over IPC
         * and runs a whole planPages() before it can compare fingerprints.
         * Per-tick it was the module's largest single cost.
         */
        step('the contract is re-read on an interval, not every tick', () => {
            NOW += PAST_TAIL; ticks(4);
            chainParamReads = 0;
            ticks(64);
            if (chainParamReads === 0) throw new Error('the contract is never re-read');
            if (chainParamReads > 4) throw new Error(`${chainParamReads} contract reads in 64 ticks`);
        });
    } finally {
        Date.now = realNow;
        uiMoving = 1; uiPhase = 3.250; uiLength = 16;
        ticks(4);
    }
}

/*
 * A COLD OPEN MUST REACH THE RING ON ITS OWN.
 *
 * drawRing prints "..." when it has no `ui` reading -- correct, a read that
 * did not answer must never become a picture. The bug was that it could get
 * STUCK there: `uiCache` was filled as a side effect of DRAWING, and the
 * redraw gate was derived from `uiCache`. Drawing was the only writer of the
 * state that decided whether to draw.
 *
 * First frame: needsRedraw, `ui` not yet read, dots, gate closes. The
 * rotation delivers `ui` eight ticks later and nothing ever looks at it.
 * Permanent -- until any input, which is why it read as "the first time".
 *
 * This is the whole reason the harness now withholds a read.
 */
{
    const ticks = (n) => { for (let i = 0; i < n; i++) ui.tick(); };

    step('a cold open leaves the dots with NO input at all', () => {
        /* Counted in READS, not ticks: the rotation reaches `ui` once every
         * ten ticks, so two withheld reads puts the first real answer at
         * about tick 29 -- well after the first frame, which is the point. */
        uiWithhold = 2;
        uiMoving = 1; uiPhase = 3.25; uiLength = 16;
        ui.init();                /* cold */
        clearCount = 0;
        ticks(60);                /* not one input in the whole window */

        if (!T.uiRaw()) throw new Error('the ui reading never reached the cache');
        if (frameText.indexOf('...') >= 0)
            throw new Error('the last frame is still the loading dots');
        if (clearCount === 0) throw new Error('nothing was ever drawn');
    });

    step('...and it did not fall back to drawing every frame', () => {
        if (clearCount > 40) throw new Error(`${clearCount} redraws in 60 ticks`);
    });

    /*
     * DRAWING MUST BE SIDE-EFFECT FREE, which is the property that makes the
     * gate sound. If a draw can still change what the gate reads, the
     * circularity is back and only its symptom was patched.
     */
    step('drawing the ring changes no state the gate reads', () => {
        const before = T.uiRaw();
        const r1 = renderRing("1");
        const after = T.uiRaw();
        if (after !== before) throw new Error('drawRing wrote to uiCache');
        if (!r1) throw new Error('the ring did not draw');
    });

    /*
     * The other half of the same shape: the entry warm and revalue() refill
     * the controller's values ASYNCHRONOUSLY. They were repainted only
     * because a pad press also happens to start the input tail.
     */
    step('a value arriving with no input repaints', () => {
        const realNow = Date.now;
        let NOW = realNow() + 10_000_000;
        Date.now = () => NOW;
        try {
            uiMoving = 0;                        /* park the playhead */
            NOW += 5000; ticks(8); NOW += 5000; ticks(8);
            clearCount = 0;
            ticks(6);
            if (clearCount !== 0) throw new Error('not settled: ' + clearCount);

            /* as the rotation would, with nobody touching anything */
            const page = ctlPage();
            if (!page) throw new Error('no page');
            globalThis.chain_ui_test.state().values[page.keys[0]] = "0.4242";
            ticks(6);
            if (clearCount === 0) throw new Error('a late value repainted nothing');

            /* AN EXTRA KEY IS A VALUE THE DRAWER READS WITH NO CELL -- `rate`
             * on the ring page. It is an input to the picture, so it has to be
             * a term in the gate; watching only page.keys leaves it able to
             * change with nothing to repaint it. */
            ticks(6);
            clearCount = 0;
            ticks(4);
            if (clearCount !== 0) throw new Error('not settled again: ' + clearCount);
            globalThis.chain_ui_test.state().values.rate = "1/8T";
            ticks(6);
            if (clearCount === 0) throw new Error('an extra key changed and nothing repainted');
        } finally {
            Date.now = realNow;
            uiMoving = 1; uiPhase = 3.250; uiLength = 16;
            ticks(4);
        }
    });
}

/*
 * EVERY VALUE THE RING DRAWS MUST BE ONE THE PAGE ACTUALLY FETCHES.
 *
 * The controller's read rotation fetches exactly `page.keys` plus the canvas
 * param's `extra_keys` -- nothing else reaches the payload the drawer is
 * handed. So a key drawRing reads and the page does not declare is simply
 * never there, and the label it feeds renders EMPTY. No error, no warning.
 *
 * That is not hypothetical: `rate` used to arrive because the canvas page took
 * the level's first eight knobs and `rate` was one of them. Declaring
 * `page_knobs` replaced that list and silently dropped it, and the Rate label
 * on the ring went blank -- a change about knob layout breaking a text label.
 *
 * Worse, it looked intermittent. `s.values` is not cleared when you page, so
 * one visit to page 2 cached `rate` and the ring looked correct from then on.
 * Cold open, blank; after a detour, fine.
 *
 * THE OTHER TESTS CANNOT SEE ANY OF THIS. renderRing() below builds the
 * payload it passes in, so it proves the drawer works GIVEN data and never
 * that the data arrives. This asserts the declaration instead of the picture,
 * which is the only place the answer lives.
 */
{
    const chainParams = JSON.parse(readFileSync(process.env.TG_PARAMS, 'utf8'));
    const canvas = chainParams.find((p) => p.type === 'canvas' && p.as_page);
    const src = readFileSync('src/ui_chain.js', 'utf8');

    /* drawRing's body, so a `vals.` in some other function cannot mask a
     * missing declaration here or invent one. */
    const from = src.indexOf('function drawRing(');
    const to = src.indexOf('\nfunction ', from + 1);
    const body = src.slice(from, to < 0 ? src.length : to);
    const reads = [...new Set([...body.matchAll(/\bvals\.([A-Za-z_][A-Za-z0-9_]*)/g)]
                              .map((m) => m[1]))].sort();

    step('drawRing reads at least one value (the scraper still works)', () => {
        if (!reads.length) throw new Error('found no vals.<key> reads -- scraper broke');
    });

    step('every value drawRing reads is declared on the canvas page', () => {
        if (!canvas) throw new Error('no as_page canvas param found');
        const declared = new Set([].concat(canvas.page_knobs || [], canvas.extra_keys || []));
        const missing = reads.filter((k) => !declared.has(k));
        if (missing.length)
            throw new Error(`${missing.join(', ')} -- read by drawRing, fetched by nothing, ` +
                            `so the label renders empty. Add to extra_keys.`);
    });

    step('and each of those is a real param, not a typo', () => {
        const keys = new Set(chainParams.map((p) => p.key));
        const bogus = reads.filter((k) => !keys.has(k));
        if (bogus.length) throw new Error(`${bogus.join(', ')} are not chain_params keys`);
    });
}

/*
 * THE PAGE LAYOUT, planned by the HOST'S OWN PLANNER against this module's
 * real chain_params and its real hierarchy.
 *
 * Two things are pinned here and they pull against each other:
 *
 *   The ring page and the grid must carry DIFFERENT keys. They are the same
 *   eight by default -- a canvas page takes the level's first eight knobs --
 *   and `page_knobs` is what separates them. Lose the declaration and the two
 *   pages silently collapse back onto one list, which looks like a layout
 *   preference rather than a bug.
 *
 *   attack/decay/sustain/release must stay on ONE ROW of the grid, i.e.
 *   positions 5-8. alignGroupsToRows reflows a page to keep a viz group
 *   together and DROPS one it cannot place -- whole, and in silence -- and
 *   that group is the envelope graphic. `realigned` being empty is the proof
 *   the authored order already satisfies it and nothing was moved.
 */
{
    const { planPages } = await import(resolve(SHARED, 'param_pages/page_plan.mjs'));
    const chainParams = JSON.parse(readFileSync(process.env.TG_PARAMS, 'utf8'));
    const hierarchy = JSON.parse(globalThis.chain_ui_test.HIERARCHY);
    const r = planPages({ hierarchy, chainParams });
    const page = (i) => (r.pages[i] ? (r.pages[i].keys || []).join(',') : '<missing>');

    step('page 1 is the ring, with its own knobs', () => {
        const want = 'slot,amount,step_amount,attack,decay,sustain,release,hold';
        if (!r.pages[0] || !r.pages[0].canvas) throw new Error('page 1 is not the canvas page');
        if (page(0) !== want) throw new Error(`got ${page(0)}`);
    });
    step('page 2 is the grid, and Len/Rate are only here', () => {
        const want = 'length,rate,amount,hold,attack,decay,sustain,release';
        if (page(1) !== want) throw new Error(`got ${page(1)}`);
    });
    /*
     * THERE IS NO THIRD PAGE. Asserted as a COUNT rather than as "page 3 is
     * not `stopped`", so a page coming back under any other name fails here
     * instead of passing a check that only knew the old one's name.
     * (The host appends My Presets / Module after the walk; those are not
     * this module's pages and planPages does not emit them here.)
     */
    step('the module plans exactly two pages', () => {
        if (r.pages.length !== 2)
            throw new Error(`${r.pages.length}: ` + r.pages.map((p) => p.name).join(', '));
    });

    /* A declaration left behind would put an orphan cell back on the grid
     * with no code behind it -- a knob that reads and writes nothing. */
    step('`stopped` is gone from chain_params entirely', () => {
        if (chainParams.some((p) => p.key === 'stopped'))
            throw new Error('stopped is still declared');
    });
    step('the two pages do NOT share a key list', () => {
        if (page(0) === page(1)) throw new Error('ring and grid collapsed onto one list');
    });
    step('the envelope group needed no reflow', () => {
        if ((r.realigned || []).length) throw new Error(JSON.stringify(r.realigned));
        if ((r.warnings || []).length) throw new Error(JSON.stringify(r.warnings));
    });
    /* A DECLARATION THE PLANNER DROPS IS THE SAME AS NO DECLARATION, and
     * extra_keys is capped -- so assert what the page actually CARRIES, not
     * what chain_params says. */
    step('the ring page carries ui and rate as extra keys', () => {
        const ek = (r.pages[0].canvas || {}).extraKeys || [];
        for (const k of ['ui', 'rate'])
            if (ek.indexOf(k) < 0) throw new Error(`${k} did not survive into canvas.extraKeys: ${ek}`);
    });

    step('every page_knobs key is declared in chain_params', () => {
        const declared = new Set(chainParams.map((p) => p.key));
        for (const k of r.pages[0].keys)
            if (!declared.has(k)) throw new Error(`${k} would be an invented 0..1 float`);
    });
}

/*
 * THE LENGTH WIRE, end to end, through the host's three resolvers AFTER the
 * learner has seen a device value. This is the exact sequence that shipped a
 * 16-step pattern reading 15 and writing 17: options ["1".."32"] are numerals,
 * so every index is also an option name, the learner latched "name" off the
 * first read, and the latch is permanent and on the SHARED meta object.
 */
{
    const F = await import(resolve(SHARED, 'param_format.mjs'));
    const M = await import(resolve(SHARED, 'param_pages/param_meta.mjs'));
    const chainParams = JSON.parse(readFileSync(process.env.TG_PARAMS, 'utf8'));
    const metaOf = (k) => chainParams.find((p) => p.key === k);

    step('length declares the index convention', () => {
        if (metaOf('length').wire_format !== 'index') throw new Error('undeclared');
        if (metaOf('cursor').wire_format !== 'index') throw new Error('cursor undeclared');
    });
    step('slot declares the NAME convention (it speaks 1-based)', () => {
        if (metaOf('slot').options_as_string !== true) throw new Error('undeclared');
    });
    step('a 16-step pattern reads 16 after the learner has run', () => {
        const meta = metaOf('length');
        F.learnEnumWireFormat(meta, '15');            /* what the device reports */
        const shown = F.formatParamValue('15', meta);
        if (shown !== '16') throw new Error(`displays ${JSON.stringify(shown)}`);
        if (M.enumIndexOf(meta, '15') !== 15) throw new Error('enumIndexOf disagrees');
        const wire = F.formatParamForSet(15, meta);
        if (wire !== '15') throw new Error(`writes ${JSON.stringify(wire)} -> atoi+1 = ${+wire + 1} steps`);
    });
    step('slot 1 still reads 1 and writes 1', () => {
        const meta = metaOf('slot');
        F.learnEnumWireFormat(meta, '1');
        if (F.formatParamValue('1', meta) !== '1') throw new Error(F.formatParamValue('1', meta));
        if (F.formatParamForSet(0, meta) !== '1') throw new Error(F.formatParamForSet(0, meta));
    });
}

rmSync(OUT, { recursive: true, force: true });
console.log(failures ? `\nFAILED (${failures})` : '\nPASS');
process.exit(failures ? 1 : 0);
