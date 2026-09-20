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
const litPads = Object.create(null);
const params = Object.create(null);

for (const n of ['clear_screen', 'fill_rect', 'draw_rect', 'print', 'set_pixel',
                 'draw_line', 'draw_arc', 'fill_circle', 'draw_circle',
                 'host_pad_block', 'shadow_component_run_action']) {
    globalThis[n] = () => 0;
}
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
    if (k === 'chain_params') return readFileSync(process.env.TG_PARAMS, 'utf8');
    if (k === 'ui')      return `FFFFFFFF:0:${uiLength}:3.250:125.00:1:2:` + 'FF'.repeat(uiLength);
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
        const want = 'slot,amount,step_amount,attack,decay,sustain,release';
        if (!r.pages[0] || !r.pages[0].canvas) throw new Error('page 1 is not the canvas page');
        if (page(0) !== want) throw new Error(`got ${page(0)}`);
    });
    step('page 2 is the grid, and Len/Rate are only here', () => {
        const want = 'length,rate,amount,hold,attack,decay,sustain,release';
        if (page(1) !== want) throw new Error(`got ${page(1)}`);
    });
    step('page 3 holds what is left', () => {
        if (page(2) !== 'stopped') throw new Error(`got ${page(2)}`);
    });
    step('the two pages do NOT share a key list', () => {
        if (page(0) === page(1)) throw new Error('ring and grid collapsed onto one list');
    });
    step('the envelope group needed no reflow', () => {
        if ((r.realigned || []).length) throw new Error(JSON.stringify(r.realigned));
        if ((r.warnings || []).length) throw new Error(JSON.stringify(r.warnings));
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
