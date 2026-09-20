/*
 * Headless tests for the Trance Gate engine.
 *
 * The module's real verification is on hardware, but the two things most
 * likely to be wrong -- where a step boundary falls, and whether a tie
 * suppresses the retrigger -- are pure functions of a fake transport and do
 * not need a Move to find out.
 *
 * Build and run:  ./tests/run.sh
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "audio_fx_api_v2.h"

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source);

#define SR 44100.0

static int    g_failures = 0;
static double g_beats    = -1.0;   /* < 0 == transport stopped */
static float  g_bpm      = 120.0f;

static float  fake_get_bpm(void)           { return g_bpm; }
static double fake_get_beat_position(void) { return g_beats; }

static host_api_v1_t g_fake_host;

static void check(const char *what, int ok) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_failures++;
}

static void check_near(const char *what, double got, double want, double tol) {
    int ok = fabs(got - want) <= tol;
    printf("  %-58s %s (got %.4f want %.4f)\n", what, ok ? "ok" : "FAIL", got, want);
    if (!ok) g_failures++;
}

/* Run `frames` frames of DC and report the mean absolute output level, while
 * advancing the fake transport as a real host would. */
static double run_dc(audio_fx_api_v2_t *api, void *inst, int frames, int16_t dc) {
    int16_t buf[128 * 2];
    double acc = 0.0;
    int done = 0;
    while (done < frames) {
        int n = (frames - done) > 128 ? 128 : (frames - done);
        for (int i = 0; i < n; i++) { buf[i * 2] = dc; buf[i * 2 + 1] = dc; }
        api->process_block(inst, buf, n);
        for (int i = 0; i < n; i++) acc += fabs((double)buf[i * 2]);
        if (g_beats >= 0.0) g_beats += (n / SR) * (g_bpm / 60.0);
        done += n;
    }
    return acc / (double)frames;
}

static void set(audio_fx_api_v2_t *api, void *inst, const char *k, const char *v) {
    api->set_param(inst, k, v);
}

/*
 * `length` and `cursor` speak the OPTION INDEX, not the number they display --
 * index 15 is the option named "16". Spelling that out at every call site is
 * how an off-by-one hides, so the two helpers do the conversion once and the
 * tests read in steps.
 */
static void set_length(audio_fx_api_v2_t *api, void *inst, int steps) {
    char v[8]; snprintf(v, sizeof(v), "%d", steps - 1);
    api->set_param(inst, "length", v);
}

static void set_cursor(audio_fx_api_v2_t *api, void *inst, int step1) {
    char v[8]; snprintf(v, sizeof(v), "%d", step1 - 1);
    api->set_param(inst, "cursor", v);
}

static const char *get(audio_fx_api_v2_t *api, void *inst, const char *k) {
    static char buf[8192];
    buf[0] = '\0';
    int n = api->get_param(inst, k, buf, (int)sizeof(buf));
    return (n >= 0) ? buf : "<refused>";
}

int main(void) {
    memset(&g_fake_host, 0, sizeof(g_fake_host));
    g_fake_host.api_version = 1;
    g_fake_host.sample_rate = 44100;
    g_fake_host.frames_per_block = 128;
    g_fake_host.get_bpm = fake_get_bpm;
    g_fake_host.get_beat_position = fake_get_beat_position;

    audio_fx_api_v2_t *api = move_audio_fx_init_v2(&g_fake_host);
    if (!api || api->api_version != 2) { printf("init failed\n"); return 1; }

    /* --- contract ------------------------------------------------------- */
    printf("contract:\n");
    void *inst = api->create_instance(NULL, NULL);
    check("create_instance returns an instance", inst != NULL);
    {
        /* SERVED-EMPTY, not refused. -1 reads as "the read did not complete"
         * and makes the component entry gate hold before falling back; ""
         * means "I have none", which is what routes the editor to
         * ui_chain.js immediately. */
        char h[64] = { 'x', 0 };
        int hn = api->get_param(inst, "ui_hierarchy", h, sizeof(h));
        check("serves ui_hierarchy EMPTY, never -1 (no entry hold)",
              hn == 0 && h[0] == '\0');
    }
    {
        const char *st = get(api, inst, "state");
        check("serves state (or the slot file is never written)",
              strstr(st, "\"sv\":") != NULL);
        /* The blob grew when per-step depths landed (8 slots x 32 bytes as
         * hex). A truncated blob still parses -- the reader just stops early
         * -- so the failure would be a patch that loads with its last slots
         * missing and nothing reporting it. Assert it ends the way it should. */
        size_t n = strlen(st);
        check("state blob is complete, not truncated",
              n > 400 && st[n - 1] == '}' && strstr(st, "\"p7\":") != NULL);
    }
    check("serves chain_params", get(api, inst, "chain_params")[0] == '[');
    check("rate reports its LABEL, not an index",
          strcmp(get(api, inst, "rate"), "1/16") == 0);

    /* --- stopped transport ---------------------------------------------- */
    printf("stopped transport:\n");
    g_beats = -1.0;
    double lvl = run_dc(api, inst, 4410, 10000);
    check_near("Open: passes through untouched", lvl, 10000.0, 1.0);

    /* --- a plain alternating gate --------------------------------------- */
    printf("gate, 1/16 at 120 BPM, alternating steps:\n");
    set(api, inst, "attack", "0");  set(api, inst, "decay", "0");
    set(api, inst, "sustain", "1"); set(api, inst, "release", "0");
    set(api, inst, "pattern", "5555");   /* steps 0,2,4,... on */
    set_length(api, inst, 16);

    /* One 1/16 step at 120 BPM is 0.25 beats = 0.125 s = 5512.5 samples. */
    g_beats = 0.0;
    double step_samples = (60.0 / 120.0) * SR * 0.25;
    check_near("step length in samples", step_samples, 5512.5, 0.6);

    /* Step 0 is ON -> full level. Measure just inside it. */
    lvl = run_dc(api, inst, 5000, 10000);
    check_near("step 0 (on) passes full level", lvl, 10000.0, 60.0);

    /* Cross into step 1, which is OFF -> silence. */
    run_dc(api, inst, 600, 10000);                 /* straddle the boundary */
    lvl = run_dc(api, inst, 4000, 10000);
    check_near("step 1 (off) is silent", lvl, 0.0, 60.0);

    /* --- depth and mix --------------------------------------------------- */
    printf("amount:\n");
    set(api, inst, "amount", "0.5");
    run_dc(api, inst, 1200, 10000);                /* into step 2 (on) */
    set(api, inst, "pattern", "0");                /* all steps off */
    set_length(api, inst, 16);
    run_dc(api, inst, 6000, 10000);                /* settle into a closed step */
    lvl = run_dc(api, inst, 4000, 10000);
    check_near("amount 0.5 closes to half level", lvl, 5000.0, 60.0);

    set(api, inst, "amount", "0.25");
    lvl = run_dc(api, inst, 4000, 10000);
    check_near("amount 0.25 leaves 75% through a shut gate", lvl, 7500.0, 60.0);

    api->destroy_instance(inst);

    /* --- ties ------------------------------------------------------------ */
    printf("ties:\n");
    inst = api->create_instance(NULL, NULL);
    set(api, inst, "attack", "50");     /* 50 ms, ~2205 samples */
    set(api, inst, "decay", "0");
    set(api, inst, "sustain", "1");
    set(api, inst, "release", "0");
    set_length(api, inst, 16);
    set(api, inst, "amount", "1");
    set(api, inst, "depth", "1");
    set(api, inst, "pattern", "3");     /* steps 0 and 1 both ON */
    set(api, inst, "ties", "0");        /* ...but NOT tied */
    g_beats = 0.0;

    run_dc(api, inst, (int)step_samples, 10000);          /* all of step 0 */
    double untied = run_dc(api, inst, 1000, 10000);       /* first ms of step 1 */

    api->destroy_instance(inst);
    inst = api->create_instance(NULL, NULL);
    set(api, inst, "attack", "50");  set(api, inst, "decay", "0");
    set(api, inst, "sustain", "1");  set(api, inst, "release", "0");
    set_length(api, inst, 16);  set(api, inst, "amount", "1");
    set(api, inst, "depth", "1");
    set(api, inst, "pattern", "3");
    set(api, inst, "ties", "1");        /* step 0 ties INTO step 1 */
    g_beats = 0.0;

    run_dc(api, inst, (int)step_samples, 10000);
    double tied = run_dc(api, inst, 1000, 10000);

    check("untied step 1 restarts the attack (dips)", untied < 4000.0);
    check("tied step 1 does NOT retrigger (stays open)", tied > 9500.0);
    check("the tie is the whole difference", tied > untied * 2.0);
    api->destroy_instance(inst);

    /* --- bar alignment --------------------------------------------------- */
    printf("bar alignment:\n");
    inst = api->create_instance(NULL, NULL);
    set_length(api, inst, 16);
    set(api, inst, "pattern", "FFFF");
    g_beats = 0.0;
    run_dc(api, inst, 1, 0);                      /* anchor */
    /* Jump the transport a long way, as a loop or a seek would. */
    g_beats = 129.0 + 0.5 * 0.25;                 /* 129 beats + half a 1/16 */
    run_dc(api, inst, 256, 0);
    double phase = atof(get(api, inst, "phase"));
    /* 129 beats / 0.25 = 516 steps; 516 mod 16 = 4. */
    check_near("phase follows song position after a seek", floor(phase), 4.0, 0.0);
    api->destroy_instance(inst);

    /* --- state round trip ------------------------------------------------ */
    printf("state round trip:\n");
    inst = api->create_instance(NULL, NULL);
    set(api, inst, "pattern", "DEAD");
    set(api, inst, "ties", "BEEF");
    set_length(api, inst, 13);
    set(api, inst, "rate", "1/8T");
    set(api, inst, "attack", "12.5");
    char saved[8192];
    strncpy(saved, get(api, inst, "state"), sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';
    api->destroy_instance(inst);

    inst = api->create_instance(NULL, NULL);
    set(api, inst, "state", saved);
    check("pattern survives",  strcmp(get(api, inst, "pattern"), "DEAD") == 0);
    check("ties survive",      strcmp(get(api, inst, "ties"), "BEEF") == 0);
    /* The wire is the option index, so a 13-step pattern reports "12". */
    check("length survives",   strcmp(get(api, inst, "length"), "12") == 0);
    check("rate survives",     strcmp(get(api, inst, "rate"), "1/8T") == 0);
    check("attack survives",   strcmp(get(api, inst, "attack"), "12.5") == 0);
    api->destroy_instance(inst);

    /* A numeric rate in a blob is an INDEX. Resolving it as "unknown" would
     * reset the rate on load while still reporting a plausible one. */
    inst = api->create_instance(NULL, NULL);
    set(api, inst, "state", "{\"sv\":1,\"slot\":0,\"rate\":5}");
    check("numeric rate in a blob resolves as an index",
          strcmp(get(api, inst, "rate"), "1/8") == 0);
    api->destroy_instance(inst);

    /* --- cursor / step editing ------------------------------------------- */
    printf("cursor and step editing:\n");
    inst = api->create_instance(NULL, NULL);
    set_length(api, inst, 16);
    set(api, inst, "pattern", "0");

    set(api, inst, "cursor", "2");                 /* the option INDEX */
    check("cursor reads back in the units it accepts",
          strcmp(get(api, inst, "cursor"), "2") == 0);
    check("a cleared step reads Off", strcmp(get(api, inst, "step"), "Off") == 0);

    set(api, inst, "step", "On");
    check("step On sets the bit under the cursor",
          strcmp(get(api, inst, "pattern"), "4") == 0);      /* bit 2 */
    check("step reads back On", strcmp(get(api, inst, "step"), "On") == 0);

    set(api, inst, "step", "Tie");
    check("Tie sets the step AND the tie",
          strcmp(get(api, inst, "pattern"), "4") == 0 &&
          strcmp(get(api, inst, "ties"), "4") == 0);
    check("step reads back Tie", strcmp(get(api, inst, "step"), "Tie") == 0);

    set(api, inst, "step", "On");
    check("On clears a tie it replaces", strcmp(get(api, inst, "ties"), "0") == 0);

    set(api, inst, "step", "Off");
    check("Off clears both bits",
          strcmp(get(api, inst, "pattern"), "0") == 0 &&
          strcmp(get(api, inst, "ties"), "0") == 0);

    /* A cursor past the end would edit a step the ring never draws. */
    set(api, inst, "cursor", "30");
    set_length(api, inst, 8);
    check("shortening the pattern pulls the cursor inside it",
          atoi(get(api, inst, "cursor")) <= 8);
    api->destroy_instance(inst);

    /* --- the compound UI readout ------------------------------------------ */
    printf("ui readout:\n");
    inst = api->create_instance(NULL, NULL);
    set_length(api, inst, 16);
    set(api, inst, "pattern", "5555");
    set(api, inst, "cursor", "4");
    g_beats = 0.0;
    run_dc(api, inst, 256, 0);
    {
        const char *ui = get(api, inst, "ui");
        unsigned st, ti; int len, cur; float ph, ms;
        int n = sscanf(ui, "%x:%x:%d:%f:%f:%*d:%d", &st, &ti, &len, &ph, &ms, &cur);
        check("ui carries all seven fields", n == 6);
        check("ui reports the pattern", st == 0x5555u);
        check("ui reports the length", len == 16);
        check("ui reports the cursor 0-based", cur == 4);
        /* 1/16 at 120 BPM is 125 ms -- what lets the page animate between
         * reads instead of stepping at the rotation rate. */
        check("ui reports ms per step", ms > 124.0f && ms < 126.0f);
    }
    api->destroy_instance(inst);

    /* --- per-step depth --------------------------------------------------- */
    printf("per-step amount:\n");
    inst = api->create_instance(NULL, NULL);
    set(api, inst, "attack", "0");  set(api, inst, "decay", "0");
    set(api, inst, "sustain", "1"); set(api, inst, "release", "0");
    set_length(api, inst, 16); set(api, inst, "amount", "1");
    set(api, inst, "depth", "1");   set(api, inst, "pattern", "FFFF");
    check("a fresh pattern is full depth",
          strcmp(get(api, inst, "step_amount"), "1.00") == 0);

    /*
     * `cursor` CARRIES THE OPTION INDEX. "1" is index 1, i.e. the SECOND step
     * -- the option names supply the step numbers, so index 1 displays as "2".
     * It was the 1-based name for a while and displayed one too high, because
     * the host's three enum resolvers disagree about names vs indices; see the
     * note in trance_gate.c.
     */
    set(api, inst, "cursor", "1");                 /* index 1 */
    set(api, inst, "step_amount", "0.5");
    check("sdepth reads back", strcmp(get(api, inst, "step_amount"), "0.50") == 0);
    set(api, inst, "cursor", "2");                 /* index 2 */
    check("the accent is PER STEP, not global",
          strcmp(get(api, inst, "step_amount"), "1.00") == 0);

    /*
     * A STEP'S AMOUNT IS HOW LOUD IT IS. One 1/16 step at 120 BPM is 5512
     * samples, so every window below stays inside one step -- a window that
     * crosses a boundary averages two and reads as a wrong gain rather than as
     * a bad measurement.
     */
    g_beats = 0.0;
    lvl = run_dc(api, inst, 4000, 10000);          /* step 0, full level */
    check_near("a full step passes everything", lvl, 10000.0, 60.0);

    run_dc(api, inst, 2000, 10000);                /* cross into step 1 */
    lvl = run_dc(api, inst, 3000, 10000);
    check_near("a half-level step is half as loud", lvl, 5019.0, 120.0);

    /* THE REPORTED BUG: amount at zero has to be silence, not full signal. */
    set(api, inst, "cursor", "1");                 /* index 1 */
    set(api, inst, "step_amount", "0");
    g_beats = 0.0;
    run_dc(api, inst, 6000, 10000);                /* settle inside step 1 */
    lvl = run_dc(api, inst, 3000, 10000);
    check_near("a zero-level step is SILENT, not loud", lvl, 0.0, 60.0);

    /* A gap has no loudness: env is zero there, so the level cannot matter. */
    set(api, inst, "step_amount", "1");
    set(api, inst, "pattern", "FFFD");             /* step index 1 OFF */
    g_beats = 0.0;
    run_dc(api, inst, 6000, 10000);
    lvl = run_dc(api, inst, 3000, 10000);
    check_near("an off step is a gap at any level", lvl, 0.0, 60.0);
    set(api, inst, "cursor", "1");
    set(api, inst, "step_amount", "0");
    lvl = run_dc(api, inst, 2000, 10000);
    check_near("...and still a gap at level zero", lvl, 0.0, 60.0);

    /* The global amount is the dry/wet over the whole thing. Re-anchor first:
     * the runs above have walked into step 2, which is ON, and measuring there
     * reads the wrong step rather than the wrong gain. */
    set(api, inst, "amount", "0.5");
    g_beats = 0.0;
    run_dc(api, inst, 6000, 10000);                /* back inside step 1, a gap */
    lvl = run_dc(api, inst, 3000, 10000);
    check_near("global amount 0.5 halves the gating", lvl, 5000.0, 120.0);

    /* AND ZERO IS A BYPASS -- the whole point of a dry/wet. */
    set(api, inst, "amount", "0");
    g_beats = 0.0;
    run_dc(api, inst, 6000, 10000);
    lvl = run_dc(api, inst, 3000, 10000);
    check_near("global amount 0 passes everything through", lvl, 10000.0, 60.0);
    api->destroy_instance(inst);

    /* --- v1 -> v2 migration ------------------------------------------------ */
    printf("state v1 -> v2 migration:\n");
    inst = api->create_instance(NULL, NULL);
    /* A blob written before per-step depth existed: the triple ends at the
     * length. Absent depths MUST load as full, or every existing patch is
     * silent while the pattern and the ring both look correct. */
    set(api, inst, "state",
        "{\"sv\":1,\"slot\":0,\"rate\":\"1/16\",\"depth\":1.000,\"mix\":1.000,"
        "\"p0\":\"FFFF:0:16\"}");
    set(api, inst, "cursor", "0");
    check("a v1 blob loads its pattern",
          strcmp(get(api, inst, "pattern"), "FFFF") == 0);
    check("a v1 blob loads at FULL depth, not zero",
          strcmp(get(api, inst, "step_amount"), "1.00") == 0);

    set(api, inst, "attack", "0"); set(api, inst, "decay", "0");
    set(api, inst, "sustain", "1"); set(api, inst, "release", "0");
    set(api, inst, "pattern", "0");            /* all closed */
    g_beats = 0.0;
    run_dc(api, inst, 2000, 10000);
    lvl = run_dc(api, inst, 3000, 10000);
    check_near("a migrated patch still gates", lvl, 0.0, 60.0);
    api->destroy_instance(inst);

    /* v2 round trip carries the depths. */
    inst = api->create_instance(NULL, NULL);
    set_length(api, inst, 16);
    set(api, inst, "cursor", "3");            /* index 3 */
    set(api, inst, "step_amount", "0.25");
    strncpy(saved, get(api, inst, "state"), sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';
    check("state reports v3", strstr(saved, "\"sv\":3") != NULL);
    api->destroy_instance(inst);
    inst = api->create_instance(NULL, NULL);
    set(api, inst, "state", saved);
    set(api, inst, "cursor", "3");
    check("per-step depth survives a round trip",
          strcmp(get(api, inst, "step_amount"), "0.25") == 0);
    set(api, inst, "cursor", "4");
    check("its neighbour is untouched",
          strcmp(get(api, inst, "step_amount"), "1.00") == 0);
    api->destroy_instance(inst);

    /* --- B1: the whole depth array must survive a round trip ------------- */
    printf("state: every step, not just the early ones:\n");
    inst = api->create_instance(NULL, NULL);
    set_length(api, inst, 32);
    /* A DISTINCT value per step, so a truncation cannot hide behind a
     * neighbour's. The old char[40] buffer cut this off around step 15 and the
     * rest came back full -- invisibly, because the pattern still looked right
     * and the earlier test happened to check step 4. */
    for (int i = 0; i < 32; i++) {
        char c[8], v[16];
        snprintf(c, sizeof(c), "%d", i);              /* cursor is the index */
        snprintf(v, sizeof(v), "%.2f", 0.20 + i * 0.02);
        set(api, inst, "cursor", c);
        set(api, inst, "step_amount", v);
    }
    strncpy(saved, get(api, inst, "state"), sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';
    api->destroy_instance(inst);

    inst = api->create_instance(NULL, NULL);
    set(api, inst, "state", saved);
    {
        int wrong = -1;
        for (int i = 0; i < 32; i++) {
            char c[8], want[16];
            snprintf(c, sizeof(c), "%d", i);
            snprintf(want, sizeof(want), "%.2f", 0.20 + i * 0.02);
            set(api, inst, "cursor", c);
            if (strcmp(get(api, inst, "step_amount"), want) != 0) { wrong = i; break; }
        }
        if (wrong >= 0) printf("    first wrong step: %d\n", wrong + 1);
        check("all 32 per-step amounts survive a save/load", wrong < 0);
    }
    check("the emitted blob fits a bus insert's 1024-byte cap",
          strlen(saved) <= 1024);
    api->destroy_instance(inst);

    /* --- B2: the cursor must stay inside the slot it moves to ------------- */
    printf("cursor follows the slot:\n");
    inst = api->create_instance(NULL, NULL);
    set(api, inst, "slot", "1");
    set_length(api, inst, 32);
    set(api, inst, "cursor", "30");
    set(api, inst, "slot", "2");
    set_length(api, inst, 8);
    set(api, inst, "slot", "1");
    set(api, inst, "slot", "2");                  /* back to the short one */
    check("switching to a shorter slot pulls the cursor inside it",
          atoi(get(api, inst, "cursor")) <= 8);
    api->destroy_instance(inst);

    /* --- B4: free-run phase must not grow without bound ------------------- */
    printf("free-run phase:\n");
    inst = api->create_instance(NULL, NULL);
    set_length(api, inst, 16);
    set(api, inst, "pattern", "FFFF");
    set(api, inst, "stopped", "Free");
    g_beats = -1.0;                                /* no transport */
    /* Two minutes of audio. Unwrapped this climbs forever; the int floor in
     * process_block is undefined once it passes INT_MAX, and the fraction goes
     * long before that. */
    for (int i = 0; i < 40000; i++) run_dc(api, inst, 128, 0);
    {
        double ph = atof(get(api, inst, "phase"));
        check("free-run phase stays inside the pattern", ph >= 0.0 && ph < 16.0);
    }
    api->destroy_instance(inst);

    /* --- the v2 -> v3 fold ------------------------------------------------ */
    printf("state v2 -> v3 (mix x depth -> amount):\n");
    inst = api->create_instance(NULL, NULL);
    set(api, inst, "attack", "0");  set(api, inst, "decay", "0");
    set(api, inst, "sustain", "1"); set(api, inst, "release", "0");
    /* A v2 patch with HALF depth and HALF mix. Only the product ever reached
     * the audio, so the faithful migration is 0.25 -- reading either field
     * alone would make this patch gate twice as hard as it was saved. */
    set(api, inst, "state",
        "{\"sv\":2,\"slot\":0,\"rate\":\"1/16\",\"depth\":0.500,\"mix\":0.500,"
        "\"p0\":\"0:0:16\"}");
    check("a v2 blob folds mix x depth into one amount",
          strcmp(get(api, inst, "amount"), "0.25") == 0);
    g_beats = 0.0;
    run_dc(api, inst, 2000, 10000);
    lvl = run_dc(api, inst, 3000, 10000);
    check_near("and it sounds the same as it did under v2", lvl, 7500.0, 120.0);
    api->destroy_instance(inst);

    /* --- gate length ------------------------------------------------------ */
    printf("gate length (sustain has a LEVEL, not a duration):\n");
    inst = api->create_instance(NULL, NULL);
    set(api, inst, "attack", "0");  set(api, inst, "decay", "0");
    set(api, inst, "sustain", "1"); set(api, inst, "release", "0");
    set_length(api, inst, 16); set(api, inst, "amount", "1");
    set(api, inst, "pattern", "FFFF");          /* every step on */
    check("gate length defaults to the whole step",
          strcmp(get(api, inst, "hold"), "1.00") == 0);

    /* A full-length gate is open all the way across a step. */
    g_beats = 0.0;
    lvl = run_dc(api, inst, 5000, 10000);
    check_near("at 100% the step never closes", lvl, 10000.0, 60.0);

    /* Half length: open for the first half, shut for the second. One 1/16
     * step at 120 BPM is 5512 samples, so the halves are 0..2756 and
     * 2756..5512. */
    set(api, inst, "hold", "0.5");
    g_beats = 0.0;
    lvl = run_dc(api, inst, 2600, 10000);
    check_near("the first half of the step is open", lvl, 10000.0, 60.0);
    run_dc(api, inst, 300, 10000);              /* cross the gate edge */
    lvl = run_dc(api, inst, 2400, 10000);
    check_near("the second half is shut", lvl, 0.0, 60.0);

    /* A TIE means hold through, so it must override the shortening or the two
     * controls would contradict each other. */
    set(api, inst, "ties", "FFFF");
    g_beats = 0.0;
    run_dc(api, inst, 3000, 10000);             /* past where it would close */
    lvl = run_dc(api, inst, 2000, 10000);
    check_near("a tied step is not cut short", lvl, 10000.0, 60.0);
    api->destroy_instance(inst);

    /* An old blob has no gate length, and absent must mean the whole step --
     * anything else shortens every gate in every patch that already works. */
    inst = api->create_instance(NULL, NULL);
    set(api, inst, "state",
        "{\"sv\":2,\"slot\":0,\"rate\":\"1/16\",\"mix\":1.000,\"p0\":\"FFFF:0:16\"}");
    check("a blob without a gate length loads at 100%",
          strcmp(get(api, inst, "hold"), "1.00") == 0);
    api->destroy_instance(inst);

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASS",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
