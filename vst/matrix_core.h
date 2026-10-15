/* =============================================================================
 * matrix_core.h - the engine of Matrix: a 16 x 64 drum step sequencer after the MIDIbox SEQ's
 * BLM 16x16 (grid of tracks x steps on the iPad/phone, the sequencer itself in the plugin).
 * No VST, no ALSA, no sockets: matrix_vst.cpp feeds it the host clock and sends what it emits;
 * host_test.cpp drives it directly.
 *
 *   KIT       16 tracks, shared by all patterns: name, note, MIDI channel, mute, solo
 *   PATTERNS  16 patterns; per pattern and track a length of 1..64 steps (polymeter) and 64 steps
 *             of velocity (0 = off), probability (1..100 %) and ratchet (1..4 hits in the step)
 *   PLAY      one pattern plays; a new one is queued and starts at the next bar line (16 steps)
 *   CLOCK     absolute pulse index at 96 per quarter (PpqClock in matrix_vst.cpp, from
 *             mpc-vst-drumgen): step = pulse / 24 = 1/16, track position = step % its length, so
 *             every track stays on the MPC grid. Swing delays odd 16ths up to 12 pulses (75 %).
 *
 * The state is one text line (project chunk, presets): only steps that differ from "off" are
 * written, so an ordinary drum pattern takes a few hundred bytes.
 * ========================================================================== */
#ifndef MATRIX_CORE_H
#define MATRIX_CORE_H
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace mtx {

constexpr int NTRK = 16, NSTEP = 64, NPAT = 16;
constexpr int RES = 96, PPS = RES / 4;     /* pulses per quarter, per 16th */
constexpr int QMAX = 512;

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

struct Step { uint8_t vel = 0, prob = 100, rat = 1; };
struct Track { uint8_t note = 36, ch = 0, mute = 0, solo = 0; char name[16] = ""; };
struct Pattern {
    Step s[NTRK][NSTEP];
    uint8_t len[NTRK];
    char name[16] = "";
    Pattern() { std::fill(len, len + NTRK, (uint8_t)16); }
    bool used() const {
        for (int t = 0; t < NTRK; t++) for (int i = 0; i < NSTEP; i++) if (s[t][i].vel) return true;
        return false;
    }
};

static const char *const DEFAULT_NAMES[NTRK] = {
    "Kick", "Snare", "Clap", "Hat C", "Hat O", "Tom L", "Tom M", "Tom H",
    "Ride", "Crash", "Rim", "Shaker", "Perc 1", "Perc 2", "Perc 3", "Perc 4"};

/* the note maps for NOTES (TRACK tab / web): Force drum program pads A01.. = 36.., GM drums */
enum { NOTES_CHROM = 0, NOTES_GM = 1 };
static const uint8_t GM_NOTES[NTRK] = {36, 38, 39, 42, 46, 41, 45, 48, 51, 49, 37, 70, 56, 75, 62, 63};

/* percent-encoding for names inside the state text */
static inline std::string esc(const char *s) {
    std::string o;
    char b[4];
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c <= ' ' || c == ';' || c == '=' || c == ',' || c == '%' || c == '|' || c >= 127) { std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
        else o += (char)c;
    }
    return o;
}
static inline void unesc(const std::string &in, char *out, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < in.size() && k + 1 < n; i++) {
        if (in[i] == '%' && i + 2 < in.size()) { out[k++] = (char)std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else out[k++] = in[i];
    }
    out[k] = 0;
}
static inline void set_name(char *dst, size_t n, const std::string &s) {
    size_t k = 0;
    for (size_t i = 0; i < s.size() && k + 1 < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c != 127) dst[k++] = (char)c;
    }
    dst[k] = 0;
}

struct Engine {
    Track trk[NTRK];
    Pattern pat[NPAT];
    int play = 0, queued = -1;
    int swing = 50, gate = 50, tap_vel = 100;     /* %, %, velocity of a new step */
    int ctl_ch = 14;                               /* 0 = OFF, else 1..16 */
    int web = 1;
    int sel = 0;                                   /* track of the TRACK tab */
    int edit = 0;                                  /* pattern the skin's track settings (LENGTH) edit */
    int learn = -1;                                /* track waiting for a note (NOTE LEARN), -1 = none */

    /* runtime (not saved) */
    bool running = false;
    long long step = -1;                           /* last fired 16th, absolute */
    int pos[NTRK];                                 /* position each track played last, -1 = none */
    uint32_t rev = 1;                              /* bumps on every change the web page must refetch */
    uint32_t rng = 0x9e3779b9u;
    long notes = 0;
    struct Ev { long long at; uint8_t st, d1, d2; };
    Ev q[QMAX];
    int qn = 0;
    long long off_at[NTRK];
    uint8_t off_note[NTRK], off_ch[NTRK];
    void (*out)(void *, int, int, int) = nullptr;  /* status, data1, data2 */
    void *out_ctx = nullptr;

    Engine() { reset(); }
    void reset() {
        for (int t = 0; t < NTRK; t++) {
            trk[t] = Track();
            trk[t].note = (uint8_t)(36 + t);
            set_name(trk[t].name, sizeof trk[t].name, DEFAULT_NAMES[t]);
        }
        for (int p = 0; p < NPAT; p++) pat[p] = Pattern();
        play = 0; queued = -1; swing = 50; gate = 50; tap_vel = 100; ctl_ch = 14; web = 1; sel = 0; edit = 0; learn = -1;
        for (int t = 0; t < NTRK; t++) { pos[t] = -1; off_at[t] = -1; }
        qn = 0;
        rev++;
    }
    void touch() { rev++; }

    /* ---- output ---- */
    void emit(int st, int d1, int d2) { if (out) out(out_ctx, st, d1, d2); }
    uint32_t xr() { uint32_t x = rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rng = x ? x : 1; }
    void note_off_track(int t) {
        if (off_at[t] < 0) return;
        emit(0x80 | off_ch[t], off_note[t], 0);
        off_at[t] = -1;
    }
    void q_push(long long at, int st, int d1, int d2) {
        if (qn >= QMAX) return;
        q[qn++] = {at, (uint8_t)st, (uint8_t)d1, (uint8_t)d2};
    }
    void q_run(long long idx) {   /* due note-offs first, then note-ons */
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < qn;) {
                const bool off = (q[i].st & 0xF0) == 0x80;
                if (q[i].at <= idx && off == (pass == 0)) {
                    emit(q[i].st, q[i].d1, q[i].d2);
                    if (!off) notes++;
                    q[i] = q[--qn];
                } else i++;
            }
    }
    void all_off() {
        for (int t = 0; t < NTRK; t++) note_off_track(t);
        for (int i = 0; i < qn; i++) if ((q[i].st & 0xF0) == 0x80) emit(q[i].st, q[i].d1, 0);
        qn = 0;
    }
    bool any_solo() const { for (int t = 0; t < NTRK; t++) if (trk[t].solo) return true; return false; }
    bool audible(int t) const { return !trk[t].mute && (!any_solo() || trk[t].solo); }

    /* ---- transport ---- */
    void start() { running = true; step = -1; }
    void stop() {
        all_off();
        running = false;
        step = -1;
        for (int t = 0; t < NTRK; t++) pos[t] = -1;
        if (queued >= 0) { play = queued; queued = -1; }
        touch();
    }
    /* the clock jumped (loop, locate): nothing may hang */
    void jumped() { all_off(); }

    void fire_step(long long st, long long pulse) {
        if (st % 16 == 0 && queued >= 0) { play = queued; queued = -1; touch(); }
        const Pattern &P = pat[play];
        const int g = std::max(1, gate * PPS / 100);
        for (int t = 0; t < NTRK; t++) {
            const int len = clampi(P.len[t], 1, NSTEP);
            const int i = (int)(st % len);
            pos[t] = i;
            const Step &s = P.s[t][i];
            if (!s.vel || !audible(t)) continue;
            if (s.prob < 100 && (int)(xr() % 100) >= s.prob) continue;
            const int ch = trk[t].ch & 15, note = trk[t].note & 127;
            note_off_track(t);
            const int r = clampi(s.rat, 1, 4);
            if (r > 1) {                      /* ratchet: r hits spread over the step, through the queue */
                const int sp = PPS / r, rg = std::max(1, std::min(g, sp) * 3 / 4);
                for (int k = 0; k < r; k++) {
                    q_push(pulse + (long long)k * sp, 0x90 | ch, note, s.vel);
                    q_push(pulse + (long long)k * sp + rg, 0x80 | ch, note, 0);
                }
                q_run(pulse);
            } else {
                emit(0x90 | ch, note, s.vel);
                notes++;
                off_note[t] = (uint8_t)note; off_ch[t] = (uint8_t)ch; off_at[t] = pulse + g;
            }
        }
        step = st;
    }
    void run_pulse(long long idx) {
        for (int t = 0; t < NTRK; t++) if (off_at[t] >= 0 && idx >= off_at[t]) note_off_track(t);
        q_run(idx);
        if (idx < 0) return;                                   /* pre-roll before bar 1 */
        const int sw = (int)((swing - 50) * PPS / 50.0 + 0.5); /* 0..12 pulses */
        if (idx % PPS == 0 && (idx / PPS) % 2 == 0) fire_step(idx / PPS, idx);
        const long long o = idx - sw;
        if (o >= 0 && o % PPS == 0 && (o / PPS) % 2 == 1) fire_step(o / PPS, idx);
        q_run(idx);
    }

    /* ---- edits (all bump rev) ---- */
    bool ok(int p, int t, int s = 0) const { return p >= 0 && p < NPAT && t >= 0 && t < NTRK && s >= 0 && s < NSTEP; }
    void set_step(int p, int t, int s, int vel, int prob = -1, int rat = -1) {
        if (!ok(p, t, s)) return;
        Step &x = pat[p].s[t][s];
        if (vel >= 0) x.vel = (uint8_t)clampi(vel, 0, 127);
        if (prob >= 0) x.prob = (uint8_t)clampi(prob, 1, 100);
        if (rat >= 0) x.rat = (uint8_t)clampi(rat, 1, 4);
        touch();
    }
    void toggle(int p, int t, int s) { if (ok(p, t, s)) set_step(p, t, s, pat[p].s[t][s].vel ? 0 : tap_vel); }
    void set_len(int p, int t, int len) { if (ok(p, t)) { pat[p].len[t] = (uint8_t)clampi(len, 1, NSTEP); touch(); } }
    void set_len_all(int p, int len) { for (int t = 0; t < NTRK; t++) set_len(p, t, len); }
    void clear_track(int p, int t) { if (ok(p, t)) { for (Step &x : pat[p].s[t]) x = Step(); touch(); } }
    void clear_pattern(int p) { if (p >= 0 && p < NPAT) { pat[p] = Pattern(); touch(); } }
    void copy_pattern(int from, int to) { if (from >= 0 && from < NPAT && to >= 0 && to < NPAT && from != to) { pat[to] = pat[from]; touch(); } }
    /* rotate the track's used length by d steps (+1 = later) */
    void rotate(int p, int t, int d) {
        if (!ok(p, t)) return;
        const int len = clampi(pat[p].len[t], 1, NSTEP);
        Step tmp[NSTEP];
        for (int i = 0; i < len; i++) tmp[((i + d) % len + len) % len] = pat[p].s[t][i];
        for (int i = 0; i < len; i++) pat[p].s[t][i] = tmp[i];
        touch();
    }
    /* copy steps 0..from_len-1 over the rest of the track up to 64 (doubling a 16-step beat) */
    void fill_repeat(int p, int t, int from_len) {
        if (!ok(p, t) || from_len < 1 || from_len >= NSTEP) return;
        for (int i = from_len; i < NSTEP; i++) pat[p].s[t][i] = pat[p].s[t][i % from_len];
        touch();
    }
    void set_mute(int t, int v) { if (t >= 0 && t < NTRK && trk[t].mute != (v != 0)) { trk[t].mute = v != 0; touch(); } }
    void set_solo(int t, int v) { if (t >= 0 && t < NTRK && trk[t].solo != (v != 0)) { trk[t].solo = v != 0; touch(); } }
    void set_note(int t, int n) { if (t >= 0 && t < NTRK) { if (trk[t].note != n) note_off_track(t); trk[t].note = (uint8_t)clampi(n, 0, 127); touch(); } }
    void set_ch(int t, int c) { if (t >= 0 && t < NTRK) { if (trk[t].ch != c) note_off_track(t); trk[t].ch = (uint8_t)clampi(c, 0, 15); touch(); } }
    void set_track_name(int t, const std::string &n) { if (t >= 0 && t < NTRK) { set_name(trk[t].name, sizeof trk[t].name, n); touch(); } }
    void set_pattern_name(int p, const std::string &n) { if (p >= 0 && p < NPAT) { set_name(pat[p].name, sizeof pat[p].name, n); touch(); } }
    void note_map(int which) {
        for (int t = 0; t < NTRK; t++) set_note(t, which == NOTES_GM ? GM_NOTES[t] : 36 + t);
    }
    /* PATTERN: while playing it is queued for the next bar, stopped it switches at once */
    void select_play(int p) {
        p = clampi(p, 0, NPAT - 1);
        if (!running) { play = p; queued = -1; }
        else if (p == play) queued = -1;
        else queued = p;
        touch();
    }
    /* an incoming note while NOTE LEARN waits */
    bool learn_note(int ch, int note) {
        if (learn < 0 || learn >= NTRK) return false;
        set_note(learn, note);
        set_ch(learn, ch);
        learn = -1;
        touch();
        return true;
    }
    /* one bar.beat.16th text of the transport */
    void where(char *b, size_t n) const {
        if (!running || step < 0) std::snprintf(b, n, "STOP  |  PATTERN %d%s", play + 1, queued >= 0 ? " > NEXT" : "");
        else std::snprintf(b, n, "BAR %lld.%lld.%lld  |  PATTERN %d%s", step / 16 + 1, (step % 16) / 4 + 1, step % 4 + 1, play + 1,
                           queued >= 0 ? " > NEXT" : "");
        if (queued >= 0) {
            const size_t k = std::strlen(b);
            std::snprintf(b + k, n - k, " %d", queued + 1);
        }
    }
    /* the first 16 steps of track t of pattern p as text, x = on, . = off, | every 4 */
    void steps_text(int p, int t, char *b, size_t n) const {
        size_t k = 0;
        const int len = clampi(pat[p].len[t], 1, NSTEP);
        for (int i = 0; i < 16 && k + 2 < n; i++) {
            if (i && i % 4 == 0) b[k++] = ' ';
            b[k++] = i >= len ? ' ' : !pat[p].s[t][i].vel ? '.' : pat[p].s[t][i].vel >= 120 ? 'X' : 'x';
        }
        b[k] = 0;
        if (len != 16 && k + 8 < n) std::snprintf(b + k, n - k, "  /%d", len);
    }

    /* ---- state text ----
     * "sw=50;gt=50;tv=100;cc=14;wb=1;pl=0;sel=0;ed=0;"
     * "t<i>=note,ch,mute,solo,name;"            per track
     * "p<i>=len*16 (comma)|name;"               per used or named pattern
     * "s<p>.<t>=i:vel[:prob[:rat]],...;"        steps that are not "off, 100 %, 1 hit" */
    std::string state() const {
        std::string o;
        char b[160];
        std::snprintf(b, sizeof b, "sw=%d;gt=%d;tv=%d;cc=%d;wb=%d;pl=%d;sel=%d;ed=%d;", swing, gate, tap_vel, ctl_ch, web, play, sel, edit);
        o = b;
        for (int t = 0; t < NTRK; t++) {
            std::snprintf(b, sizeof b, "t%d=%d,%d,%d,%d,", t, trk[t].note, trk[t].ch, trk[t].mute, trk[t].solo);
            o += b + esc(trk[t].name) + ";";
        }
        for (int p = 0; p < NPAT; p++) {
            const Pattern &P = pat[p];
            bool lens = false;
            for (int t = 0; t < NTRK; t++) lens |= P.len[t] != 16;
            bool steps = false;
            for (int t = 0; t < NTRK; t++) {
                std::string line;
                for (int i = 0; i < NSTEP; i++) {
                    const Step &s = P.s[t][i];
                    if (!s.vel && s.prob == 100 && s.rat == 1) continue;
                    if (s.prob == 100 && s.rat == 1) std::snprintf(b, sizeof b, "%d:%d,", i, s.vel);
                    else if (s.rat == 1) std::snprintf(b, sizeof b, "%d:%d:%d,", i, s.vel, s.prob);
                    else std::snprintf(b, sizeof b, "%d:%d:%d:%d,", i, s.vel, s.prob, s.rat);
                    line += b;
                }
                if (line.empty()) continue;
                steps = true;
                line.pop_back();
                std::snprintf(b, sizeof b, "s%d.%d=", p, t);
                o += b + line + ";";
            }
            if (!lens && !P.name[0] && !steps) continue;
            std::snprintf(b, sizeof b, "p%d=", p);
            o += b;
            for (int t = 0; t < NTRK; t++) { std::snprintf(b, sizeof b, t ? ",%d" : "%d", P.len[t]); o += b; }
            o += "|" + esc(P.name) + ";";
        }
        return o;
    }
    void apply(const std::string &st) {
        reset();
        size_t a = 0;
        while (a < st.size()) {
            size_t e = st.find(';', a);
            if (e == std::string::npos) e = st.size();
            const std::string item = st.substr(a, e - a);
            a = e + 1;
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
            const int iv = std::atoi(v.c_str());
            if (k == "sw") swing = clampi(iv, 50, 75);
            else if (k == "gt") gate = clampi(iv, 5, 95);
            else if (k == "tv") tap_vel = clampi(iv, 1, 127);
            else if (k == "cc") ctl_ch = clampi(iv, 0, 16);
            else if (k == "wb") web = iv ? 1 : 0;
            else if (k == "pl") play = clampi(iv, 0, NPAT - 1);
            else if (k == "sel") sel = clampi(iv, 0, NTRK - 1);
            else if (k == "ed") edit = clampi(iv, 0, NPAT - 1);
            else if (k.size() < 2 || k[1] < '0' || k[1] > '9') continue;   /* e.g. the plugin's "prog=" */
            else if (k[0] == 't') {
                const int t = std::atoi(k.c_str() + 1);
                if (t < 0 || t >= NTRK) continue;
                int n = 36, c = 0, m = 0, so = 0, used = 0;
                std::sscanf(v.c_str(), "%d,%d,%d,%d,%n", &n, &c, &m, &so, &used);
                trk[t].note = (uint8_t)clampi(n, 0, 127); trk[t].ch = (uint8_t)clampi(c, 0, 15);
                trk[t].mute = m != 0; trk[t].solo = so != 0;
                if (used > 0) unesc(v.substr((size_t)used), trk[t].name, sizeof trk[t].name);
            } else if (k[0] == 'p') {
                const int p = std::atoi(k.c_str() + 1);
                if (p < 0 || p >= NPAT) continue;
                const size_t bar = v.find('|');
                const std::string lens = v.substr(0, bar);
                size_t q = 0;
                for (int t = 0; t < NTRK && q <= lens.size(); t++) {
                    pat[p].len[t] = (uint8_t)clampi(std::atoi(lens.c_str() + q), 1, NSTEP);
                    const size_t c = lens.find(',', q);
                    if (c == std::string::npos) break;
                    q = c + 1;
                }
                if (bar != std::string::npos) unesc(v.substr(bar + 1), pat[p].name, sizeof pat[p].name);
            } else if (k[0] == 's') {
                int p = -1, t = -1;
                if (std::sscanf(k.c_str() + 1, "%d.%d", &p, &t) != 2 || !ok(p, t)) continue;
                size_t q = 0;
                while (q < v.size()) {
                    size_t c = v.find(',', q);
                    if (c == std::string::npos) c = v.size();
                    int i = -1, vel = 0, prob = 100, rat = 1;
                    if (std::sscanf(v.c_str() + q, "%d:%d:%d:%d", &i, &vel, &prob, &rat) >= 2 && i >= 0 && i < NSTEP) {
                        Step &s = pat[p].s[t][i];
                        s.vel = (uint8_t)clampi(vel, 0, 127); s.prob = (uint8_t)clampi(prob, 1, 100); s.rat = (uint8_t)clampi(rat, 1, 4);
                    }
                    q = c + 1;
                }
            }
        }
        touch();
    }
};

/* ---- factory patterns (presets), written with the engine itself ---- */
enum { K = 0, SN = 1, CL = 2, HC = 3, HO = 4, TL = 5, TM = 6, TH = 7, RD = 8, CR = 9, RM = 10, SH = 11 };
static inline void put(Engine &e, int p, int t, const char *grid, int vel = 100) {
    /* grid: one char per 16th, x = vel, X = 127, g = ghost 40, r = ratchet 2, ? = 50 %, '.' off; spaces ignored */
    int i = 0;
    for (const char *c = grid; *c && i < NSTEP; c++) {
        if (*c == ' ' || *c == '|') continue;
        Step &s = e.pat[p].s[t][i++];
        if (*c == 'x') s.vel = (uint8_t)vel;
        else if (*c == 'X') s.vel = 127;
        else if (*c == 'g') s.vel = 40;
        else if (*c == 'r') { s.vel = (uint8_t)vel; s.rat = 2; }
        else if (*c == 'R') { s.vel = (uint8_t)vel; s.rat = 3; }
        else if (*c == '?') { s.vel = (uint8_t)vel; s.prob = 50; }
    }
}
static inline std::string factory_state(int which) {
    Engine e;
    switch (which) {
    case 1:   /* HOUSE: pattern 1 basic, 2 with open hats and claps, 3 fill */
        e.set_pattern_name(0, "House A");
        put(e, 0, K, "x... x... x... x...");
        put(e, 0, CL, ".... x... .... x...");
        put(e, 0, HC, "..x. ..x. ..x. ..x.", 90);
        put(e, 0, SH, "gxgx gxgx gxgx gxgx", 60);
        e.set_pattern_name(1, "House B");
        put(e, 1, K, "x... x... x... x...");
        put(e, 1, CL, ".... x... .... x..g");
        put(e, 1, HC, "x.x. x.x. x.x. x.x.", 70);
        put(e, 1, HO, "..x. ..x. ..x. ..x.", 90);
        put(e, 1, RD, "x... x... x... x...", 60);
        e.set_pattern_name(2, "House Fill");
        put(e, 2, K, "x... x... x... x.x.");
        put(e, 2, CL, ".... x... .... xrRR");
        put(e, 2, HC, "..x. ..x. ..x. ....", 90);
        put(e, 2, CR, "X...............");
        e.swing = 56;
        break;
    case 2:   /* BREAKBEAT */
        e.set_pattern_name(0, "Break A");
        put(e, 0, K, "x.x. .... ..x. ....");
        put(e, 0, SN, ".... x..g .g.. x..g");
        put(e, 0, HC, "x.x. x.x. x.x. x.x?", 80);
        e.set_pattern_name(1, "Break B");
        put(e, 1, K, "x.x. .... ..xx ..x.");
        put(e, 1, SN, ".... x..g .g.. x.rx");
        put(e, 1, HC, "x.x. x.x. x.x. x.x.", 80);
        put(e, 1, HO, ".... .... .... ...x");
        e.swing = 58;
        break;
    case 3:   /* TECHNO 64: kick 16, hats 12 (polymeter), rim 64 with variations */
        e.set_pattern_name(0, "Techno 64");
        put(e, 0, K, "X... x... x... x...");
        put(e, 0, HO, "..x. ..x. ..x. ..x.", 90);
        put(e, 0, HC, "xgx. xgxg x.xg", 70);
        e.set_len(0, HC, 12);
        put(e, 0, CL, ".... x... .... x...", 90);
        put(e, 0, RM, "...x ..x. .x.. ...x ...x ..x. .x.. .x.? ...x ..x. .x.. ...x ...x .xx. .x.x xx?x", 80);
        e.set_len(0, RM, 64);
        put(e, 0, TL, ".... .... .... .... .... .... .... .... .... .... .... .... .... .... ..x. x.x.", 90);
        e.set_len(0, TL, 64);
        break;
    default: break;   /* 0: EMPTY */
    }
    return e.state();
}
static const char *const FACTORY_NAMES[] = {"EMPTY KIT 36-51", "HOUSE (3 PATTERNS)", "BREAKBEAT", "TECHNO 64 POLY"};
constexpr int NFACTORY = 4;

}   // namespace mtx
#endif
