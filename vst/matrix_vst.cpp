/* =============================================================================
 * matrix_vst.cpp - Matrix: a 16-track x 64-step drum sequencer for the Akai Force (MPC OS),
 * after the MIDIbox SEQ's BLM 16x16. The sequencer runs in the plugin, on the host clock; the
 * 16 x 16 grid is a web page on the iPad/phone (shared Force hub, forcehub.h + matrix_web.h).
 * Hand-written VST2 shell (after mpc-vst-drumgen / mpc-vst-ccmap), insert effect: audio goes
 * through untouched, no instrument slot is used.
 *
 *   ENGINE   matrix_core.h: kit of 16 tracks (note, channel, mute, solo), 16 patterns, per track a
 *            length of 1..64 steps, per step velocity, probability and ratchet; pattern changes
 *            land on the next bar line
 *   CLOCK    host ppqPos at 96 pulses per quarter by absolute pulse index (PpqClock, from
 *            mpc-vst-euclidier / -drumgen): steps sit on the MPC grid, loops and locates re-anchor
 *   OUTPUT   MPC OS ignores VST MIDI out, so notes go to the plugin's own ALSA seq port
 *            "Matrix" / "MIDI Out": choose it as the MIDI input of a drum track
 *   CONTROL  "Control In" port (every hardware input) + MIDI sent to the track, on CONTROL CH
 *            (default 14): CC 1-16 mute track 1-16 (>= 64 = muted), CC 17-32 solo, CC 33 pattern
 *            (value 0..15), CC 34 swing, CC 35 gate, CC 36 / 37 next / previous pattern (>= 64),
 *            Program Change 0..15 = pattern 1..16. NOTE LEARN takes the next note from anywhere.
 *   PRESETS  32 slots (whole kit + 16 patterns), one text file next to the plugin folder, also
 *            the plugin's VST programs; the first four are factory content until saved over.
 *   PHONE    hub page on the first free port 8000..8009, grid at /app (PHONE VIEW, default ON).
 * Each instance has its own engine, ALSA client and web port. Diagnostics: /tmp/matrix_vst.log.
 * ========================================================================== */
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NO_ALSA
#include <alsa/asoundlib.h>
#endif

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "matrix_core.h"
#include "matrix_web.h"
#include "matrix_page.h"   /* page/index.html, embedded by gen_page.py */

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
    effGetProgramName = 5, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetProgramNameIndexed = 29,
    effGetPlugCategory = 35, effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5 };

#define BUILD_ID "matrix-0.1.0"
static FILE *g_log;
static std::mutex g_log_lock;
#define LOG(...) do { std::lock_guard<std::mutex> lg_(g_log_lock); if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

using namespace mtx;

static double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---------------------------------------------------------------------------
 * PpqClock (from mpc-vst-drumgen): pulse N IS ppq N/RES. Feeds (sent, last-in-this-block]; an
 * overlap feeds nothing twice, a small hole is caught up, a real jump (loop, locate, restart)
 * re-anchors. The step position is a pure function of the pulse index.
 * ------------------------------------------------------------------------- */
struct PpqClock {
    long long sent = 0;
    bool resync = true;
    long pulses = 0, gaps = 0, dups = 0, jumps = 0;
};
static bool ppq_clock_block(PpqClock *c, double ppq, double tempo, double sr, int frames,
                            long long *from, long long *to, bool *jumped) {
    const long long TOL = RES / 2;
    const double end = ppq + frames * (tempo / 60.0) / sr;
    const long long first = (long long)std::ceil(ppq * RES - 1e-6);
    const long long last = (long long)std::ceil(end * RES - 1e-6) - 1;
    *jumped = false;
    if (c->resync) { c->sent = first - 1; c->resync = false; }
    else if (first > c->sent + 1 + TOL || last < c->sent - TOL) { *jumped = true; c->jumps++; c->sent = first - 1; }
    if (first > c->sent + 1) c->gaps += first - (c->sent + 1);
    else if (first <= c->sent) c->dups += (last < c->sent ? last : c->sent) - first + 1;
    if (last <= c->sent) return false;
    *from = c->sent + 1; *to = last;
    c->pulses += last - c->sent;
    c->sent = last;
    return true;
}

/* ---- the parameters, by key (module.json) ------------------------------------------ */
enum {
    PATTERN, SWING, GATE, TAP_VEL, STATUS, STEPS, TRACK, NOTE, CHANNEL, LENGTH, MUTE, SOLO,
    LEARN, CLEAR, CTL_CH, WEB, WEB_ADDR, PRESET, LOAD, SAVE, NKEYS
};
static const char *const KEYS[NKEYS] = {
    "pattern", "swing", "gate", "tap_vel", "status", "steps", "track", "note", "channel", "length", "mute", "solo",
    "learn", "clear", "ctl_ch", "web", "web_addr", "preset", "load", "save",
};
static int IDX[NKEYS];
static int KEY_OF[NPARAMS];   /* PARAMS[] position -> key enum, -1 = not ours (popup flags) */
static bool is_trigger(int k) { return k == LEARN || k == CLEAR || k == LOAD || k == SAVE; }
static bool is_readout(int k) { return k == STATUS || k == STEPS || k == WEB_ADDR; }

#define NSLOTS 32

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::mutex mx;                       /* guards e (audio thread, web thread, dispatcher) */
    Engine e;
    PpqClock clk;
    bool was_playing = false;
    float sr = 44100.0f;
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    bool down[NPARAMS] = {false};
    int sent[NPARAMS];                   /* ui value the host last heard of, per param */
    uint32_t reported_rev = 0;
    std::atomic<bool> redisplay{false};
    double t_display = 0;
    std::atomic<int> cur{0};             /* selected preset slot, 0-based */
    std::vector<uint8_t> chunk;
    forcehub::Server *web = nullptr;
    mweb::Hooks hooks;
    /* MIDI to the track (VST events), handled in processReplacing */
    uint8_t inq[64][3];
    int in_n = 0;
    /* audition from the web page */
    std::atomic<uint32_t> hits{0};
    int aud_left[NTRK];
    uint8_t aud_note[NTRK], aud_ch[NTRK];
    /* diagnostics */
    long blocks = 0, no_ti = 0, ctl_in = 0, ctl_used = 0;
    double t_last_log = 0, last_ppq = 0, last_tempo = 0;
#ifndef NO_ALSA
    snd_seq_t *seq = nullptr;
    int port = -1, ctl_port = -1;
#else
    std::vector<std::array<int, 3>> *test_out = nullptr;
#endif
};

/* ---- helpers --------------------------------------------------------------- */
static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
/* the value of key k in the units of module.json; caller holds w->mx */
static int ui_value(Plugin *w, int k) {
    const Engine &e = w->e;
    const int t = e.sel;
    switch (k) {
    case PATTERN: return (e.queued >= 0 ? e.queued : e.play) + 1;
    case SWING: return e.swing;
    case GATE: return e.gate;
    case TAP_VEL: return e.tap_vel;
    case TRACK: return t + 1;
    case NOTE: return e.trk[t].note;
    case CHANNEL: return e.trk[t].ch + 1;
    case LENGTH: return e.pat[e.play].len[t];
    case MUTE: return e.trk[t].mute;
    case SOLO: return e.trk[t].solo;
    case CTL_CH: return e.ctl_ch;
    case WEB: return e.web;
    case PRESET: return w->cur.load() + 1;
    default: return 0;
    }
}
static bool reportable(int i) { const int k = KEY_OF[i]; return k >= 0 && !is_trigger(k) && !is_readout(k); }

/* ---------------------------------------------------------------------------
 * Preset bank (pattern of mpc-vst-ccmap / -rattler): NSLOTS presets shared by every instance and
 * every project, one text file ("index<TAB>name<TAB>state" per saved slot) next to the plugin's
 * folder. They are also the VST programs. Slots 1-4 are factory content until SAVE overwrites them.
 * A preset holds the kit and all 16 patterns; CONTROL CH and PHONE VIEW stay as they are.
 * ------------------------------------------------------------------------- */
static std::mutex g_bank_lock;
static bool g_bank_loaded;
static std::string g_slot_chunk[NSLOTS], g_slot_name[NSLOTS], g_bank_path;

static std::string so_dir() {
    std::string dir;
    if (FILE *f = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            char *p = std::strstr(line, "/matrix.so");
            char *first = std::strchr(line, '/');
            if (!p || !first || first > p) continue;
            dir.assign(first, (size_t)(p - first));
            break;
        }
        std::fclose(f);
    }
    return dir;
}
static void bank_load_locked() {
    if (g_bank_loaded) return;
    g_bank_loaded = true;
    g_bank_path.clear();
    if (const char *env = std::getenv("MATRIX_PRESETS")) g_bank_path = env;
    else {
        std::string dir = so_dir(), cand[2];
        if (dir.empty()) dir = "/tmp";
        const size_t cut = dir.rfind('/');
        cand[0] = (cut != std::string::npos && cut > 0 ? dir.substr(0, cut) : dir) + "/matrix_presets.txt";
        cand[1] = dir + "/matrix_presets.txt";
        for (const std::string &c : cand)
            if (FILE *f = std::fopen(c.c_str(), "r")) { std::fclose(f); g_bank_path = c; break; }
        for (int i = 0; i < 2 && g_bank_path.empty(); i++)
            if (FILE *f = std::fopen(cand[i].c_str(), "a")) { std::fclose(f); g_bank_path = cand[i]; }
    }
    if (g_bank_path.empty()) return;
    if (FILE *f = std::fopen(g_bank_path.c_str(), "r")) {
        char *line = nullptr;
        size_t cap = 0;
        while (getline(&line, &cap, f) > 0) {
            line[std::strcspn(line, "\r\n")] = 0;
            char *t1 = std::strchr(line, '\t');
            char *t2 = t1 ? std::strchr(t1 + 1, '\t') : nullptr;
            const int idx = std::atoi(line);
            if (!t2 || idx < 1 || idx > NSLOTS) continue;
            *t1 = *t2 = 0;
            g_slot_name[idx - 1] = t1 + 1;
            g_slot_chunk[idx - 1] = t2 + 1;
        }
        std::free(line);
        std::fclose(f);
    }
    LOG("[matrix_vst] preset bank %s\n", g_bank_path.c_str());
}
static bool bank_write_locked() {   /* via a temp file so a power cut can't leave half a bank */
    if (g_bank_path.empty()) return false;
    const std::string tmp = g_bank_path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (int i = 0; i < NSLOTS; i++)
        if (!g_slot_chunk[i].empty())
            std::fprintf(f, "%d\t%s\t%s\n", i + 1, g_slot_name[i].c_str(), g_slot_chunk[i].c_str());
    return std::fclose(f) == 0 && std::rename(tmp.c_str(), g_bank_path.c_str()) == 0;
}
static std::string slot_title_locked(int i) {
    char buf[40];
    if (g_slot_chunk[i].empty()) {
        if (i < NFACTORY) return FACTORY_NAMES[i];
        std::snprintf(buf, sizeof buf, "%02d (empty)", i + 1);
        return buf;
    }
    if (!g_slot_name[i].empty()) return g_slot_name[i];
    std::snprintf(buf, sizeof buf, "MATRIX %02d", i + 1);
    return buf;
}
static std::string slot_title(int i) { std::lock_guard<std::mutex> lk(g_bank_lock); return slot_title_locked(i); }
static std::string slot_state(int n) {
    std::lock_guard<std::mutex> lk(g_bank_lock);
    if (!g_slot_chunk[n].empty()) return g_slot_chunk[n];
    if (n < NFACTORY) return factory_state(n);
    return "";
}
/* apply a state text; keep_setup: CONTROL CH and PHONE VIEW stay; the transport keeps running.
 * Caller holds w->mx. */
static void load_state(Plugin *w, const std::string &s, bool keep_setup) {
    Engine &e = w->e;
    e.all_off();
    const bool run = e.running;
    const long long st = e.step;
    const int cc = e.ctl_ch, wb = e.web;
    e.apply(s);
    e.running = run;
    e.step = st;
    if (keep_setup) { e.ctl_ch = cc; e.web = wb; }
}
static void web_apply(Plugin *w);
static bool slot_load(Plugin *w, int n) {
    n = clampi(n, 0, NSLOTS - 1);
    const std::string c = slot_state(n);
    if (c.empty()) return false;      /* an empty preset leaves the patterns alone */
    w->cur.store(n);
    { std::lock_guard<std::mutex> lk(w->mx); load_state(w, c, true); }
    w->redisplay.store(true);
    LOG("[matrix_vst] preset %d loaded\n", n + 1);
    return true;
}
static bool slot_save(Plugin *w, int n, const std::string &name) {
    n = clampi(n, 0, NSLOTS - 1);
    std::string c;
    { std::lock_guard<std::mutex> lk(w->mx); c = w->e.state(); }
    w->cur.store(n);
    std::lock_guard<std::mutex> lk(g_bank_lock);
    g_slot_chunk[n] = c;
    if (!name.empty()) {
        std::string nm = name;
        for (char &ch : nm) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        g_slot_name[n] = nm.substr(0, 23);
    }
    const bool ok = bank_write_locked();
    LOG("[matrix_vst] preset %d saved (%zu bytes)%s\n", n + 1, c.size(), ok ? "" : " - FILE NOT WRITTEN");
    return ok;
}
static std::string presets_json() {
    std::lock_guard<std::mutex> lk(g_bank_lock);
    std::string j = "[";
    for (int i = 0; i < NSLOTS; i++) {
        j += i ? "," : "";
        j += "{\"n\":" + forcehub::js(slot_title_locked(i)) + (g_slot_chunk[i].empty() && i >= NFACTORY ? ",\"u\":0}" : ",\"u\":1}");
    }
    return j + "]";
}

/* ---- MIDI out / Control In (ALSA) ------------------------------------------------ */
static void handle_in(Plugin *w, int st, int d1, int d2);
#ifndef NO_ALSA
static void ctl_subscribe(Plugin *w, int client, int port) {
    if (client == snd_seq_client_id(w->seq) || client == SND_SEQ_CLIENT_SYSTEM) return;
    snd_seq_port_info_t *pi;
    snd_seq_port_info_alloca(&pi);
    if (snd_seq_get_any_port_info(w->seq, client, port, pi) < 0) return;
    const unsigned cap = snd_seq_port_info_get_capability(pi), type = snd_seq_port_info_get_type(pi);
    if ((cap & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) return;
    if (cap & SND_SEQ_PORT_CAP_NO_EXPORT) return;
    if (!(type & SND_SEQ_PORT_TYPE_HARDWARE)) return;   /* not other plugins' virtual ports */
    const int rc = snd_seq_connect_from(w->seq, w->ctl_port, client, port);
    LOG("[matrix_vst] control in <- %d:%d '%s'%s\n", client, port, snd_seq_port_info_get_name(pi),
        rc < 0 ? " (already connected or refused)" : "");
}
static void ctl_subscribe_all(Plugin *w) {
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(w->seq, ci) >= 0) {
        const int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(w->seq, pi) >= 0) ctl_subscribe(w, c, snd_seq_port_info_get_port(pi));
    }
}
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        w->seq = nullptr; LOG("[matrix_vst] snd_seq_open failed\n"); return;
    }
    snd_seq_set_client_pool_output(w->seq, 2048);
    snd_seq_set_client_name(w->seq, PLUG_NAME);
    w->port = snd_seq_create_simple_port(w->seq, "MIDI Out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                         SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[matrix_vst] ALSA client '" PLUG_NAME "' (%d) port %d\n", snd_seq_client_id(w->seq), w->port);
    w->ctl_port = snd_seq_create_simple_port(w->seq, "Control In", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (w->ctl_port < 0) { LOG("[matrix_vst] no Control In port\n"); return; }
    snd_seq_connect_from(w->seq, w->ctl_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    ctl_subscribe_all(w);
}
static void midi_out(Plugin *w, int st, int d1, int d2) {
    if (!w->seq || w->port < 0) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, w->port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    const int type = st & 0xF0, ch = st & 0x0F;
    if (type == 0x90 && d2) snd_seq_ev_set_noteon(&ev, ch, d1, d2);
    else if (type == 0x80 || type == 0x90) snd_seq_ev_set_noteoff(&ev, ch, d1, 0);
    else if (type == 0xB0) snd_seq_ev_set_controller(&ev, ch, d1, d2);
    else return;
    snd_seq_event_output_direct(w->seq, &ev);
}
static void alsa_poll(Plugin *w) {   /* audio thread, w->mx held */
    if (!w->seq || w->ctl_port < 0) return;
    snd_seq_event_t *ev = nullptr;
    for (int guard = 0; guard < 256 && snd_seq_event_input_pending(w->seq, 1) > 0; guard++) {
        if (snd_seq_event_input(w->seq, &ev) < 0 || !ev) break;
        switch (ev->type) {
        case SND_SEQ_EVENT_CONTROLLER: handle_in(w, 0xB0 | (ev->data.control.channel & 15), (int)ev->data.control.param, ev->data.control.value); break;
        case SND_SEQ_EVENT_PGMCHANGE: handle_in(w, 0xC0 | (ev->data.control.channel & 15), ev->data.control.value, 0); break;
        case SND_SEQ_EVENT_NOTEON: handle_in(w, 0x90 | (ev->data.note.channel & 15), ev->data.note.note, ev->data.note.velocity); break;
        case SND_SEQ_EVENT_PORT_START: ctl_subscribe(w, ev->data.addr.client, ev->data.addr.port); break;
        default: break;
        }
    }
}
static void alsa_close(Plugin *w) { if (w->seq) snd_seq_close(w->seq); w->seq = nullptr; }
#else
static void alsa_open(Plugin *) {}
static void midi_out(Plugin *w, int st, int d1, int d2) {
    if (w->test_out) w->test_out->push_back({st, d1, d2});
    if (std::getenv("MATRIX_TRACE")) std::printf("MIDI %02x %02x %02x\n", st, d1, d2);
}
static void alsa_poll(Plugin *) {}
static void alsa_close(Plugin *) {}
#endif
static void engine_out(void *ctx, int st, int d1, int d2) { midi_out((Plugin *)ctx, st, d1, d2); }

/* ---- control by MIDI (see header); w->mx held ---------------------------------------- */
static void handle_in(Plugin *w, int st, int d1, int d2) {
    Engine &e = w->e;
    const int type = st & 0xF0, ch = st & 15;
    w->ctl_in++;
    if (type == 0x90 && d2 > 0 && e.learn >= 0) {
        const int t = e.learn;
        e.learn_note(ch, d1);
        w->ctl_used++;
        LOG("[matrix_vst] track %d learned note %d ch %d\n", t + 1, d1, ch + 1);
        return;
    }
    if (e.ctl_ch == 0 || ch != e.ctl_ch - 1) return;
    if (type == 0xC0) { if (d1 < NPAT) { e.select_play(d1); w->ctl_used++; } return; }
    if (type != 0xB0) return;
    const int v = clampi(d2, 0, 127);
    if (d1 >= 1 && d1 <= 16) e.set_mute(d1 - 1, v >= 64);
    else if (d1 >= 17 && d1 <= 32) e.set_solo(d1 - 17, v >= 64);
    else if (d1 == 33) { if (v < NPAT) e.select_play(v); }
    else if (d1 == 34) { e.swing = 50 + (v * 25 + 63) / 127; e.touch(); }
    else if (d1 == 35) { e.gate = 5 + (v * 90 + 63) / 127; e.touch(); }
    else if (d1 == 36 || d1 == 37) {
        if (v >= 64) e.select_play(((e.queued >= 0 ? e.queued : e.play) + (d1 == 36 ? 1 : NPAT - 1)) % NPAT);
    } else return;
    w->ctl_used++;
}

/* ---- phone view (forcehub.h + matrix_web.h) ------------------------------------------ */
static void web_apply(Plugin *w) {
    bool want;
    { std::lock_guard<std::mutex> lk(w->mx); want = w->e.web != 0; }
    if (want && !w->web) {
        mweb::Hooks &h = w->hooks;
        h.e = &w->e;
        h.mu = &w->mx;
        h.presets = [] { return presets_json(); };
        h.preset_cur = [w] { return w->cur.load(); };
        h.preset_load = [w](int n) { return slot_load(w, n); };
        h.preset_save = [w](int n, const std::string &nm) { return slot_save(w, n, nm); };
        h.hit = [w](int t) { w->hits.fetch_or(1u << t); };
        h.changed = [w] { w->redisplay.store(true); };
        w->web = new forcehub::Server();
        w->web->id = "matrix";
        w->web->name = "Matrix";
        w->web->color = "#3ccf8e";
        w->web->app = MATRIX_PAGE;
        w->web->summary = [w] {
            std::lock_guard<std::mutex> lk(w->mx);
            char b[96];
            w->e.where(b, sizeof b);
            return std::string(b);
        };
        Plugin *pw = w;
        w->web->route = [pw](int c, const forcehub::Request &r) { return mweb::route(pw->hooks, c, r); };
        if (!w->web->start()) {
            LOG("[matrix_vst] phone view: no free port %d..%d\n", forcehub::Server::base_port(), forcehub::Server::base_port() + 9);
            delete w->web; w->web = nullptr;
        } else LOG("[matrix_vst] phone view on http://%s (hub %d)\n", w->web->address().c_str(), HUB_VERSION);
    } else if (!want && w->web) {
        w->web->stop(); delete w->web; w->web = nullptr;
        LOG("[matrix_vst] phone view off\n");
    }
    w->redisplay.store(true);
}

/* ---- audio: pass through, run the clock, report changes ------------------------------ */
static void processReplacing(AEffect *ef, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)ef->object;
    for (int ch = 0; ch < 2; ch++) {   /* the host may hand us in == out */
        if (in && in[ch]) { if (out[ch] != in[ch]) std::memmove(out[ch], in[ch], (size_t)n * sizeof(float)); }
        else std::memset(out[ch], 0, (size_t)n * sizeof(float));
    }
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstTempoValid | kVstPpqPosValid, 0, 0);
    if (!ti) w->no_ti++;
    const bool playing = ti && (ti->flags & kVstTransportPlaying);
    float val[NPARAMS];
    bool report[NPARAMS] = {false}, any = false;
    {
        std::lock_guard<std::mutex> lk(w->mx);
        Engine &e = w->e;
        for (int i = 0; i < w->in_n; i++) handle_in(w, w->inq[i][0], w->inq[i][1], w->inq[i][2]);
        w->in_n = 0;
        alsa_poll(w);
        if (playing && !w->was_playing) { w->clk.resync = true; e.start(); e.touch(); }
        if (!playing && w->was_playing) e.stop();
        w->was_playing = playing;
        if (playing && (ti->flags & kVstPpqPosValid) && ti->tempo > 0) {
            const double sr = ti->sampleRate > 0 ? ti->sampleRate : (double)w->sr;
            long long from = 0, to = -1;
            bool jumped = false;
            if (ppq_clock_block(&w->clk, ti->ppqPos, ti->tempo, sr, n, &from, &to, &jumped)) {
                if (jumped) { e.jumped(); LOG("[matrix_vst] ppq jump to %.4f\n", ti->ppqPos); }
                for (long long idx = from; idx <= to; idx++) e.run_pulse(idx);
            }
            w->last_ppq = ti->ppqPos; w->last_tempo = ti->tempo;
        }
        /* audition (web page): note on now, off ~120 ms later */
        uint32_t h = w->hits.exchange(0);
        for (int t = 0; t < NTRK; t++) {
            if (w->aud_left[t] > 0 && (w->aud_left[t] -= n) <= 0) { midi_out(w, 0x80 | w->aud_ch[t], w->aud_note[t], 0); w->aud_left[t] = 0; }
            if (h & (1u << t)) {
                if (w->aud_left[t] > 0) midi_out(w, 0x80 | w->aud_ch[t], w->aud_note[t], 0);
                w->aud_note[t] = e.trk[t].note; w->aud_ch[t] = e.trk[t].ch;
                midi_out(w, 0x90 | w->aud_ch[t], w->aud_note[t], e.tap_vel);
                w->aud_left[t] = std::max(1, (int)(w->sr * 0.12f));
            }
        }
        /* our own changes (web page, Control In, pattern landing) -> host */
        if (e.rev != w->reported_rev) {
            w->reported_rev = e.rev;
            for (int i = 0; i < NPARAMS; i++) {
                if (!reportable(i)) continue;
                const int u = ui_value(w, KEY_OF[i]);
                if (u != w->sent[i]) { w->sent[i] = u; report[i] = true; val[i] = ui_to_norm(&PARAMS[i], u); }
            }
            any = true;
        }
    }
    for (int i = 0; i < NPARAMS; i++) {
        if (report[i]) w->master(&w->fx, audioMasterAutomate, i, 0, 0, val[i]);
        if (w->release[i]) { w->release[i] = 0; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    }
    const double t1 = now_ms();
    if (w->redisplay.exchange(false)) any = true;
    if (playing && t1 - w->t_display > 250.0) any = true;    /* STATUS follows the bar */
    if (any) { w->t_display = t1; w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f); }
    w->blocks++;
    if (t1 - w->t_last_log > 5000.0) {
        if (w->t_last_log > 0)
            LOG("[matrix_vst] %ld blocks, playing %d, ppq %.3f, bpm %.2f | clock: %ld pulses, %ld caught up, %ld overlapped,"
                " %ld jumps | notes %ld | no timeinfo %ld | midi in %ld, used %ld | web %ld requests\n",
                w->blocks, (int)w->was_playing, w->last_ppq, w->last_tempo, w->clk.pulses, w->clk.gaps, w->clk.dups,
                w->clk.jumps, w->e.notes, w->no_ti, w->ctl_in, w->ctl_used, w->web ? w->web->requests.load() : 0L);
        w->blocks = w->no_ti = w->ctl_in = w->ctl_used = 0;
        w->e.notes = 0;
        w->clk.pulses = w->clk.gaps = w->clk.dups = w->clk.jumps = 0;
        w->t_last_log = t1;
    }
}

/* ---- parameters ------------------------------------------------------------------ */
static void setParameter(AEffect *ef, int32_t i, float n) {
    Plugin *w = (Plugin *)ef->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    const int key = KEY_OF[i];
    if (key < 0 || is_readout(key)) return;
    if (key == PRESET) {
        const int v = (int)std::lround(clamp01(n) * (NSLOTS - 1));
        if (v != w->cur.load()) { w->cur.store(v); w->redisplay.store(true); }
        w->sent[i] = v + 1;
        return;
    }
    if (is_trigger(key)) {
        const bool dn = n > 0.5f;
        const bool rising = dn && !w->down[i];   /* an echo of our own automate must not fire again */
        w->down[i] = dn;
        if (!rising) return;
        if (key == LOAD) { slot_load(w, w->cur.load()); web_apply(w); }
        else if (key == SAVE) slot_save(w, w->cur.load(), "");
        else {
            std::lock_guard<std::mutex> lk(w->mx);
            Engine &e = w->e;
            if (key == LEARN) e.learn = e.learn == e.sel ? -1 : e.sel;   /* pressed again: cancel */
            else { e.note_off_track(e.sel); e.clear_track(e.play, e.sel); }
            e.touch();
        }
        w->release[i] = 1;
        w->redisplay.store(true);
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {   /* a Q-Link nudge lands between options -> step one option */
        const float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            std::lock_guard<std::mutex> lk(w->mx);
            const int c = ui_value(w, key);
            n = (float)clampi(c + (pos > c ? 1 : -1), 0, p->nopts - 1) / (p->nopts - 1);
            nudge = true;
        }
    }
    const int u = norm_to_ui(p, n);
    {
        std::lock_guard<std::mutex> lk(w->mx);
        Engine &e = w->e;
        const int t = e.sel;
        switch (key) {
        case PATTERN: if (u - 1 != (e.queued >= 0 ? e.queued : e.play)) e.select_play(u - 1); break;
        case SWING: e.swing = clampi(u, 50, 75); break;
        case GATE: e.gate = clampi(u, 5, 95); break;
        case TAP_VEL: e.tap_vel = clampi(u, 1, 127); break;
        case TRACK: e.sel = clampi(u - 1, 0, NTRK - 1); break;
        case NOTE: if (e.trk[t].note != u) e.set_note(t, u); break;
        case CHANNEL: if (e.trk[t].ch != u - 1) e.set_ch(t, u - 1); break;
        case LENGTH: if (e.pat[e.play].len[t] != u) e.set_len(e.play, t, u); break;
        case MUTE: e.set_mute(t, u); break;
        case SOLO: e.set_solo(t, u); break;
        case CTL_CH: e.ctl_ch = clampi(u, 0, 16); break;
        case WEB: e.web = u ? 1 : 0; break;
        default: break;
        }
        e.touch();
        w->sent[i] = u;                     /* the host knows this one */
        if (key == TRACK)                   /* the kit fields now show another track */
            for (int k : {NOTE, CHANNEL, LENGTH, MUTE, SOLO}) if (IDX[k] >= 0) w->sent[IDX[k]] = -9999;
    }
    if (key == WEB) web_apply(w);
    w->redisplay.store(true);
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *ef, int32_t i) {
    Plugin *w = (Plugin *)ef->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    const int key = KEY_OF[i];
    if (key < 0 || is_trigger(key) || is_readout(key)) return 0.0f;
    if (key == PRESET) return (float)w->cur.load() / (NSLOTS - 1);
    std::lock_guard<std::mutex> lk(w->mx);
    return ui_to_norm(&PARAMS[i], ui_value(w, key));
}
static void note_name(char *b, size_t n, int note) {
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::snprintf(b, n, "%d  %s%d", note, N[note % 12], note / 12 - 2);   /* 60 = C3, as on the MPC */
}
static void display(Plugin *w, int idx, char *buf, size_t n) {
    const param_t *pp = &PARAMS[idx];
    const int key = KEY_OF[idx];
    buf[0] = 0;
    if (popup_is(idx)) { std::snprintf(buf, n, "%s", pp->opts[norm_to_ui(pp, w->open[idx])]); return; }
    if (key < 0 || is_trigger(key)) return;
    if (key == PRESET) { std::snprintf(buf, n, "%d %s", w->cur.load() + 1, slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str()); return; }
    if (key == WEB_ADDR) { std::snprintf(buf, n, "%s", w->web ? w->web->address().c_str() : "OFF"); return; }
    std::lock_guard<std::mutex> lk(w->mx);
    const Engine &e = w->e;
    const int t = e.sel;
    switch (key) {
    case STATUS: e.where(buf, n); return;
    case STEPS: e.steps_text(e.play, t, buf, n); return;
    case PATTERN: {
        const int p = e.queued >= 0 ? e.queued : e.play;
        std::snprintf(buf, n, "%d%s%s%s", p + 1, e.pat[p].name[0] ? " " : "", e.pat[p].name, e.queued >= 0 ? " (NEXT)" : "");
        return;
    }
    case TRACK: std::snprintf(buf, n, "%d %s%s", t + 1, e.trk[t].name, e.learn == t ? " (LEARN)" : ""); return;
    case NOTE: note_name(buf, n, e.trk[t].note); return;
    case SWING: case GATE: std::snprintf(buf, n, "%d %%", ui_value(w, key)); return;
    default: break;
    }
    const int u = ui_value(w, key);
    if (pp->nopts) std::snprintf(buf, n, "%s", pp->opts[clampi(u, 0, pp->nopts - 1)]);
    else std::snprintf(buf, n, "%d", u);
}

/* project chunk: "MTX1;" + the engine's state + "prog=<preset>;" */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string t = "MTX1;";
    {
        std::lock_guard<std::mutex> lk(w->mx);
        t += w->e.state();
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "prog=%d;", w->cur.load());
    t += buf;
    w->chunk.assign(t.begin(), t.end());
    w->chunk.push_back(0);
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    if (!data || len < 5) return 0;
    std::string t((const char *)data, (size_t)len);
    t.resize(std::strlen(t.c_str()));   /* stop at a NUL the host may have included */
    if (t.compare(0, 5, "MTX1;")) return 0;
    t = t.substr(5);
    {
        std::lock_guard<std::mutex> lk(w->mx);
        load_state(w, t, false);
        const size_t a = t.find("prog=");
        if (a != std::string::npos) w->cur.store(clampi(std::atoi(t.c_str() + a + 5), 0, NSLOTS - 1));
    }
    web_apply(w);
    LOG("[matrix_vst] project restored (%zu bytes)\n", t.size());
    return 1;
}

static intptr_t dispatcher(AEffect *ef, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)ef->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: {
        if (w->web) { w->web->stop(); delete w->web; w->web = nullptr; }
        {
            std::lock_guard<std::mutex> lk(w->mx);
            w->e.all_off();
            for (int t = 0; t < NTRK; t++) if (w->aud_left[t] > 0) midi_out(w, 0x80 | w->aud_ch[t], w->aud_note[t], 0);
        }
        alsa_close(w);
        LOG("[matrix_vst] closed\n");
        delete w;
        return 1;
    }
    case effSetProgram:
        /* the host also re-selects the current program after restoring a project: that must not
         * overwrite the project's patterns, so only a different slot loads */
        if (v >= 0 && v < NSLOTS && v != w->cur.load()) { slot_load(w, (int)v); web_apply(w); }
        return 0;
    case effGetProgram: return w->cur.load();
    case effGetProgramName: if (p) copy_str(p, slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str(), 24); return 0;
    case effGetProgramNameIndexed:
        if (idx < 0 || idx >= NSLOTS || !p) return 0;
        copy_str(p, slot_title(idx).c_str(), 24);
        return 1;
    case effSetProgramName: {   /* only a saved preset has a line in the file to carry the name */
        const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
        std::lock_guard<std::mutex> lk(g_bank_lock);
        if (!p || g_slot_chunk[n].empty()) return 0;
        std::string nm((const char *)p);
        for (char &c : nm) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        g_slot_name[n] = nm.substr(0, 23);
        bank_write_locked();
        return 0;
    }
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && reportable(idx);
    case effGetParamName: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        char buf[96];
        display(w, idx, buf, sizeof buf);
        copy_str(p, buf, 48);
        return 1;
    }
    case effSetSampleRate: if (o > 0) w->sr = o; return 1;
    case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: {   /* MIDI sent to the track: control + NOTE LEARN, handled in processReplacing */
        VstEvents *ev = (VstEvents *)p;
        std::lock_guard<std::mutex> lk(w->mx);
        for (int i = 0; ev && i < ev->numEvents && w->in_n < 64; i++) {
            if (ev->events[i]->type != 1) continue;
            std::memcpy(w->inq[w->in_n++], ((VstMidiEvent *)ev->events[i])->midiData, 3);
        }
        return 1;
    }
    case effCanDo: {
        const char *s = (const char *)p;
        return s && (!std::strcmp(s, "receiveVstTimeInfo") || !std::strcmp(s, "receiveVstEvents") ||
                     !std::strcmp(s, "receiveVstMidiEvent")) ? 1 : -1;
    }
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

#ifdef NO_ALSA
/* offline test hooks (host_test.cpp) */
extern "C" __attribute__((visibility("default"))) void matrix_test_capture(AEffect *ef, std::vector<std::array<int, 3>> *v) {
    ((Plugin *)ef->object)->test_out = v;
}
extern "C" __attribute__((visibility("default"))) void matrix_test_in(AEffect *ef, int st, int d1, int d2) {
    Plugin *w = (Plugin *)ef->object;
    std::lock_guard<std::mutex> lk(w->mx);
    handle_in(w, st, d1, d2);
}
extern "C" __attribute__((visibility("default"))) int matrix_test_port(AEffect *ef) {
    Plugin *w = (Plugin *)ef->object;
    return w->web ? w->web->port : -1;
}
#endif

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!g_log) g_log = std::fopen("/tmp/matrix_vst.log", "a");
        for (int i = 0; i < NPARAMS; i++) KEY_OF[i] = -1;
        for (int k = 0; k < NKEYS; k++) { IDX[k] = param_index(KEYS[k]); if (IDX[k] >= 0) KEY_OF[IDX[k]] = k; }
    });
    { std::lock_guard<std::mutex> lk(g_bank_lock); bank_load_locked(); }
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) w->sent[i] = -9999;
    for (int t = 0; t < NTRK; t++) { w->aud_left[t] = 0; w->aud_note[t] = 0; w->aud_ch[t] = 0; }
    w->e.out = engine_out;
    w->e.out_ctx = w;
    w->e.rng ^= (uint32_t)time(nullptr) ^ (uint32_t)(uintptr_t)w;
    alsa_open(w);
    web_apply(w);

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = NSLOTS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[matrix_vst] " PLUG_NAME " up (" BUILD_ID "), %d params\n", NPARAMS);
    return e;
}
