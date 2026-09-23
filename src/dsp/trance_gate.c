/*
 * trance_gate.c -- the SCHWUNG shell. The engine is trance_gate_core.c.
 *
 * What lives here is everything that only means something inside Schwung: the
 * `chain_params` contract the knob grid reads, the `ui_hierarchy` refusal that
 * routes the editor to ui_chain.js, the audio_fx_api_v2 vtable, and the
 * transport callbacks. What lives in the core is the gate itself.
 *
 * The split exists because the same engine now runs in a VST3/AU plugin, and
 * a second copy of the envelope would drift from this one within a month --
 * with the symptom "it sounds different in Live", which is the hardest kind
 * of bug to chase. One engine, two shells.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "audio_fx_api_v2.h"
#include "trance_gate_core.h"

static const host_api_v1_t *g_host = NULL;

/* The two calls that were compiled into the engine and are now a struct it is
 * handed. A plugin has no `g_host`; it fills these from its own transport. */
static void tg_read_transport(tg_transport_t *t) {
    t->bpm = 120.0f;
    t->running = 0;
    t->beats = -1.0;
    if (g_host && g_host->get_bpm) {
        float b = g_host->get_bpm();
        if (b > 1.0f && b < 1000.0f) t->bpm = b;
    }
    if (g_host && g_host->get_beat_position) {
        double beats = g_host->get_beat_position();
        if (beats >= 0.0) { t->running = 1; t->beats = beats; }
    }
}

static void *v2_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    /* Move's mailbox is 44100 and always has been; the core takes it as a
     * parameter only so a DAW can say otherwise. */
    return tg_core_create(44100.0);
}

static void v2_destroy_instance(void *instance) {
    tg_core_destroy((tg_core_t *)instance);
}

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    tg_transport_t t;
    tg_read_transport(&t);
    tg_core_process_i16((tg_core_t *)instance, audio_inout, frames, &t);
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    tg_core_set_param((tg_core_t *)instance, key, val);
}

/*
 * The core serves every parameter it owns and answers -1 for the rest, which
 * is how the two keys below stay here: they describe how SCHWUNG should draw
 * this module and mean nothing to a plugin.
 */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    tg_core_t *in = (tg_core_t *)instance;
    if (!in || !key) return -1;


    /* Refusing ui_hierarchy is what routes the component editor to our
     * ui_chain.js, which is the only route to host_pad_block and therefore to
     * pad editing. enterComponentEdit() tries the hierarchy editor first and
     * returns; only the null fallback reaches loadModuleUi. */
    /*
     * SERVED-EMPTY, NOT REFUSED.
     *
     * Returning -1 reads as "the read did not complete", and the component
     * entry gate then HOLDS for HOLD_UNSERVED_READ_LIMIT attempts before
     * falling back (component_load_gate.mjs) -- a delay on every entry, for a
     * question we can answer instantly. "" means "I have none", which is the
     * truth: the hierarchy is supplied by ui_chain.js to its own controller.
     *
     * Both spellings are falsy, so getComponentHierarchy still returns null
     * and ui_chain.js still loads. Only the wait goes away.
     */
    if (strcmp(key, "ui_hierarchy") == 0) { if (buf_len > 0) buf[0] = '\0'; return 0; }

    if (strcmp(key, "chain_params") == 0) {
        /* THIS copy is the one the UI reads -- the chain host asks the plugin
         * first and only falls back to re-parsing module.json, which drops
         * `viz` entirely. Declared here rather than in module.json for a
         * second reason: module.json is read by a minimal parser with an 8 KB
         * budget and this does not comfortably fit.
         *
         * The four envelope keys are CONTIGUOUS on purpose: a viz group whose
         * members do not land on one row of the 2x4 page is dropped whole,
         * silently. */
        static const char *params =
        "["
        /*
         * INDEX-WIRED AND SAYING SO, like `length` and `cursor` below.
         *
         * Every one of the three is a numeral enum, where a value is
         * ambiguous by construction: "1" is both option 1 by name and option 0
         * by number. The host's learner guesses NAME first for such a value,
         * and a guess is not something to build on -- it is right here only by
         * luck and wrong on `length`, which is how a 16-step pattern came to
         * read 15 and write 17.
         *
         * A declaration is honoured ahead of the learner and never learned
         * over, on every host version, which is what makes this correct on
         * released Schwung as well as on a patched one. This param used to be
         * name-wired: it displayed right and WROTE WRONG on a stock host,
         * because the learner latched NAME off the numeral and the write came
         * back as the option one past the one picked.
         */
        "{\"key\":\"slot\",\"name\":\"Slot\",\"type\":\"enum\","
          "\"wire_format\":\"index\","
          "\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\"],\"default\":\"0\"},"
        /* Knob 3: this step's amount -- an accent, scaled by the global
         * Amount on the settings page. Same quantity, two scopes, which is why
         * they share a name, a unit and a range. */
        "{\"key\":\"step_amount\",\"name\":\"Step Amount\",\"short_name\":\"Step\",\"type\":\"float\",\"min\":0,\"max\":1,"
          "\"default\":1,\"step\":0.01,\"unit\":\"%\"},"
        /* Same reasoning as `cursor`: 32 values at one detent each is a flick
         * from end to end. */
        "{\"key\":\"length\",\"name\":\"Len\",\"type\":\"enum\","
          "\"wire_format\":\"index\",\"default\":\"15\","
          "\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\",\"13\",\"14\",\"15\",\"16\",\"17\",\"18\",\"19\",\"20\",\"21\",\"22\",\"23\",\"24\",\"25\",\"26\",\"27\",\"28\",\"29\",\"30\",\"31\",\"32\",\"33\",\"34\",\"35\",\"36\",\"37\",\"38\",\"39\",\"40\",\"41\",\"42\",\"43\",\"44\",\"45\",\"46\",\"47\",\"48\",\"49\",\"50\",\"51\",\"52\",\"53\",\"54\",\"55\",\"56\",\"57\",\"58\",\"59\",\"60\",\"61\",\"62\",\"63\",\"64\",\"65\",\"66\",\"67\",\"68\",\"69\",\"70\",\"71\",\"72\",\"73\",\"74\",\"75\",\"76\",\"77\",\"78\",\"79\",\"80\",\"81\",\"82\",\"83\",\"84\",\"85\",\"86\",\"87\",\"88\",\"89\",\"90\",\"91\",\"92\",\"93\",\"94\",\"95\",\"96\",\"97\",\"98\",\"99\",\"100\",\"101\",\"102\",\"103\",\"104\",\"105\",\"106\",\"107\",\"108\",\"109\",\"110\",\"111\",\"112\",\"113\",\"114\",\"115\",\"116\",\"117\",\"118\",\"119\",\"120\",\"121\",\"122\",\"123\",\"124\",\"125\",\"126\",\"127\",\"128\"]},"
        /* A PLAIN ENUM, NOT type "rate".
         *
         * `rate` is expanded into its option list by buildRateParamMeta in
         * shadow_ui.js -- HOST code. This module builds its own controller out
         * of the shared param_pages library, which has never heard of the type,
         * so the meta arrived as {type:"rate"} with no options, no min and no
         * max: undiscretised, and the knob could not step through note values
         * at all. Declaring the options here is what makes the knob discrete,
         * and it costs nothing else -- the labels ARE the wire value either
         * way, so the same C table answers both. */
        "{\"key\":\"rate\",\"name\":\"Rate\",\"type\":\"enum\",\"options\":"
          "[\"1/1T\",\"1/2\",\"1/2T\",\"1/4\",\"1/4T\",\"1/8\",\"1/8T\","
           "\"1/16\",\"1/16T\",\"1/32\",\"1/32T\",\"1/64\",\"1/128\"],"
          "\"default\":\"1/16\"},"
        /*
         * Knob 1: which step the ring's marker sits on.
         *
         * AN ENUM, NOT AN INT, AND THE REASON IS THE FEEL.
         * knob_engine gives an int ONE detent per value unless its range is
         * 2..16 (NARROW_RANGE_MAX), where it gives four. A step cursor is a
         * discrete identity exactly like the pad index that constant was
         * raised for -- "these numbers move crazy fast on a single detent" --
         * but 1..32 falls outside the band, so it flew past the whole ring on
         * one flick. knobStep divides an ENUM's delta by ENUM_DELTA_DIV (4)
         * whatever its length, which is the feel we want and the house
         * constant rather than a number invented here.
         *
         * The options are numerals, so a value is ambiguous between a name
         * and an index -- and the host's THREE enum resolvers do not agree
         * about which to prefer. Speaking the index is what they all read the
         * same way; see the note in set_param.
         */
        "{\"key\":\"cursor\",\"name\":\"Step\",\"type\":\"enum\","
          "\"wire_format\":\"index\",\"default\":\"0\","
          "\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\",\"13\",\"14\",\"15\",\"16\",\"17\",\"18\",\"19\",\"20\",\"21\",\"22\",\"23\",\"24\",\"25\",\"26\",\"27\",\"28\",\"29\",\"30\",\"31\",\"32\",\"33\",\"34\",\"35\",\"36\",\"37\",\"38\",\"39\",\"40\",\"41\",\"42\",\"43\",\"44\",\"45\",\"46\",\"47\",\"48\",\"49\",\"50\",\"51\",\"52\",\"53\",\"54\",\"55\",\"56\",\"57\",\"58\",\"59\",\"60\",\"61\",\"62\",\"63\",\"64\",\"65\",\"66\",\"67\",\"68\",\"69\",\"70\",\"71\",\"72\",\"73\",\"74\",\"75\",\"76\",\"77\",\"78\",\"79\",\"80\",\"81\",\"82\",\"83\",\"84\",\"85\",\"86\",\"87\",\"88\",\"89\",\"90\",\"91\",\"92\",\"93\",\"94\",\"95\",\"96\",\"97\",\"98\",\"99\",\"100\",\"101\",\"102\",\"103\",\"104\",\"105\",\"106\",\"107\",\"108\",\"109\",\"110\",\"111\",\"112\",\"113\",\"114\",\"115\",\"116\",\"117\",\"118\",\"119\",\"120\",\"121\",\"122\",\"123\",\"124\",\"125\",\"126\",\"127\",\"128\"]},"
        /* Knob 2: what the step under the cursor IS. Three states, not a
         * switch plus a tie switch -- see set_param. */
        "{\"key\":\"step\",\"name\":\"Gate\",\"short_name\":\"Gate\",\"type\":\"enum\","
          "\"options\":[\"Off\",\"On\",\"Tie\"],\"default\":\"Off\"},"
        /* Adjacent ON steps hold as one gate rather than re-articulating. Audible
         * below Sustain 100%; at 100% the attack already ramps from a fully
         * open gate and there is nothing to hear. */
        /* THE KEY STAYS `legato`. It is in every saved patch, it is the
         * plugin's automation parameter id, and Live's saved automation
         * references that id -- so only the NAME moves. "Join" is what fits a
         * cell; the long form is what the plugin has room for. */
        "{\"key\":\"legato\",\"name\":\"Join Neighbors\",\"short_name\":\"Join\","
          "\"type\":\"enum\","
          "\"options\":[\"Off\",\"On\"],\"wire_format\":\"index\",\"default\":\"0\"},"
        /* What Att/Dec/Rel MEAN: milliseconds, or a share of the step. In %
         * the envelope keeps its shape at every rate instead of being cut off
         * at the fast ones -- see stage_samples in the engine. */
        "{\"key\":\"time_mode\",\"name\":\"Env Time\",\"short_name\":\"Time\","
          "\"type\":\"enum\","
          "\"options\":[\"ms\",\"% Step\"],\"wire_format\":\"index\",\"default\":\"0\"},"
        /* The PATH each stage takes between its endpoints -- the stage still
         * starts and ends where it did and still takes as long. See
         * env_shape in the engine. */
        "{\"key\":\"curve\",\"name\":\"Env Curve\",\"short_name\":\"Curve\","
          "\"type\":\"enum\","
          "\"options\":[\"Linear\",\"Exponential\",\"S-Curve\"],"
          "\"wire_format\":\"index\",\"default\":\"0\"},"
        "{\"key\":\"attack\",\"name\":\"Att\",\"type\":\"float\",\"min\":0,\"max\":500,"
          "\"default\":2,\"step\":1,\"unit\":\"ms\","
          "\"viz\":{\"group\":\"adsr\",\"role\":\"attack\",\"kind\":\"envelope\"}},"
        "{\"key\":\"decay\",\"name\":\"Dec\",\"type\":\"float\",\"min\":0,\"max\":500,"
          "\"default\":20,\"step\":1,\"unit\":\"ms\","
          "\"viz\":{\"group\":\"adsr\",\"role\":\"decay\"}},"
        "{\"key\":\"sustain\",\"name\":\"Sus\",\"type\":\"float\",\"min\":0,\"max\":1,"
          "\"default\":1,\"step\":0.01,\"unit\":\"%\","
          "\"viz\":{\"group\":\"adsr\",\"role\":\"sustain\"}},"
        "{\"key\":\"release\",\"name\":\"Rel\",\"type\":\"float\",\"min\":0,\"max\":500,"
          "\"default\":20,\"step\":1,\"unit\":\"ms\","
          "\"viz\":{\"group\":\"adsr\",\"role\":\"release\"}},"
        /*
         * THE SAME WORD, AND THE SCOPE IN FRONT OF IT.
         *
         * These are one quantity at two scopes, so they share the word -- but
         * naming them identically made them indistinguishable on the grid, and
         * the per-step one was turned to zero expecting the whole effect to
         * bypass. It only silenced one step, which is what it is for. "Step"
         * and "All" say which you are holding; the shared "Amount" still says
         * they are the same idea.
         *
         * Shown 0-100%: `unit: "%"` with max 1 is what makes the formatter
         * scale it. Zero here is a true bypass -- see the gain in
         * process_block.
         */
        /* THE GATE'S WIDTH -- how much of the step it stays open for, as a
         * share of the step so it follows the Rate. Sustain is a LEVEL and
         * has no duration; this is the duration.
         *
         * It was called "Gate", which collided with two other things wearing
         * that name here: the per-step on/off/tie control above, and the ring
         * page below. The KEY stays `hold` -- it is in every saved patch and
         * is the plugin's automation parameter id. */
        "{\"key\":\"hold\",\"name\":\"Width\",\"short_name\":\"Width\",\"type\":\"float\","
          "\"min\":0.05,\"max\":1,\"default\":1,\"step\":0.01,\"unit\":\"%\"},"
        "{\"key\":\"amount\",\"name\":\"All Amount\",\"short_name\":\"All\",\"type\":\"float\",\"min\":0,\"max\":1,"
          "\"default\":1,\"step\":0.01,\"unit\":\"%\"},"
        "{\"key\":\"pattern\",\"name\":\"Pat\",\"type\":\"string\",\"access\":\"read\"},"
        "{\"key\":\"ties\",\"name\":\"Ties\",\"type\":\"string\",\"access\":\"read\"},"
        "{\"key\":\"phase\",\"name\":\"Phase\",\"type\":\"string\",\"access\":\"read\","
          "\"live\":true},"
        /* The compound readout the animated page reads. It earns no cell --
         * nothing lists it in `knobs` -- so it needs no hiding. */
        "{\"key\":\"ui\",\"name\":\"UI\",\"type\":\"string\",\"access\":\"read\"},"
        /* THE RING. `as_page` makes this a page in the level's jog rotation
         * rather than a cell you dive into, carrying that level's own eight
         * knobs -- so the encoders work with no input code and the host draws
         * its own header, bank bar and footer AROUND the body. That last part
         * is what makes it impossible for the picture to collide with the
         * text: the drawer is handed a frame, not the screen. */
        "{\"key\":\"gate\",\"name\":\"Gate\",\"type\":\"canvas\",\"as_page\":true,"
          /* READ WITHOUT A KNOB. The rotation fetches the page's keys plus
           * extra_keys and nothing else, so a value the drawer reads and the
           * page does not declare is simply absent and its label renders
           * EMPTY -- no error. `rate` is not one of the ring's eight, so it
           * has to be here. */
          "\"show_value\":false,"
          /* NO `page_knobs` HERE, ON PURPOSE.
           *
           * A canvas page takes the level's FIRST EIGHT knobs, so the ring's
           * layout is carried by the ORDER of `knobs` in ui_chain.js rather
           * than by a declaration -- which means this build needs nothing of
           * the host and runs on released Schwung. `page_knobs` would give a
           * tidier grid behind the ring (Length and Rate as cells rather than
           * a page of their own) and exists only on the beta track, which
           * waits on a host that has it. */
          "\"extra_keys\":[\"ui\",\"rate\"]}"
        "]";
        int len = (int)strlen(params);
        if (len >= buf_len) return -1;
        memcpy(buf, params, len + 1);
        return len;
    }

    return tg_core_get_param(in, key, buf, buf_len);
}

/* ---------------------------------------------------------------- init -- */

static audio_fx_api_v2_t g_fx_api_v2;

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version      = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance  = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block    = v2_process_block;
    g_fx_api_v2.set_param        = v2_set_param;
    g_fx_api_v2.get_param        = v2_get_param;
    return &g_fx_api_v2;
}

/*
 * A FREE SYMBOL, not a vtable field: the chain host dlsym's this name rather
 * than reading it off audio_fx_api_v2_t.
 */
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    tg_core_on_midi((tg_core_t *)instance, msg, len);
}
