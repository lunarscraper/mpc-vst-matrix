/* =============================================================================
 * matrix_web.h - the iPad/phone grid of Matrix on the shared Force hub (forcehub.h).
 *
 *   GET /  /info  /app          hub, tile info, the grid page (matrix_page.h) - from forcehub.h
 *   GET /state?p=N              everything the page draws for pattern N (kit, steps, patterns, presets)
 *   GET /pos                    transport only (rev, running, playing/queued pattern, track positions);
 *                               the page polls it ~10x a second and refetches /state when rev changes
 *   GET /step?p&t&s[&v][&prob][&rat]   set one step (v 0 = off)
 *   GET /len?p&t&len            track length 1..64 (t=-1: all tracks)
 *   GET /track?t[&note][&ch][&name][&mute][&solo]   kit (t=-1 with mute/solo: all tracks)
 *   GET /learn?t                NOTE LEARN for track t (-1 cancels)
 *   GET /hit?t                  audition the track's note
 *   GET /pat?play=N | clear=N | copy=A&to=B | p=N&name=X
 *   GET /edit?op=clear|rot|repeat&p&t[&d][&n]
 *   GET /glob?[swing][gate][tv][cc]
 *   GET /notes?map=0|1          note map 36..51 / GM
 *   GET /preset?load=N | save=N[&name=X]   (N 1-based)
 * Every edit answers {"ok":true,"rev":R}. The engine is only touched with the plugin's mutex held.
 * ========================================================================== */
#ifndef MATRIX_WEB_H
#define MATRIX_WEB_H
#include <functional>
#include <mutex>
#include "forcehub.h"
#include "matrix_core.h"

namespace mweb {

using forcehub::js;

struct Hooks {
    mtx::Engine *e = nullptr;
    std::mutex *mu = nullptr;
    std::function<std::string()> presets;            /* JSON array of the 32 preset names (own lock) */
    std::function<int()> preset_cur;
    std::function<bool(int)> preset_load;            /* 0-based; lock themselves */
    std::function<bool(int, const std::string &)> preset_save;
    std::function<void(int)> hit;                    /* audition track t */
    std::function<void()> changed;                   /* after an edit: tell the host / redraw */
};

static inline void hex2(std::string &o, int v) {
    static const char H[] = "0123456789abcdef";
    o += H[(v >> 4) & 15];
    o += H[v & 15];
}

/* caller holds the mutex */
static inline std::string state_json(const mtx::Engine &e, int p) {
    using namespace mtx;
    p = clampi(p, 0, NPAT - 1);
    const Pattern &P = e.pat[p];
    std::string j;
    char b[200];
    std::snprintf(b, sizeof b, "{\"rev\":%u,\"p\":%d,\"play\":%d,\"queued\":%d,\"run\":%s,\"step\":%lld,\"swing\":%d,\"gate\":%d,"
                  "\"tv\":%d,\"cc\":%d,\"learn\":%d,\"sel\":%d,\"tracks\":[",
                  e.rev, p, e.play, e.queued, e.running ? "true" : "false", e.step, e.swing, e.gate, e.tap_vel, e.ctl_ch, e.learn, e.sel);
    j = b;
    for (int t = 0; t < NTRK; t++) {
        std::snprintf(b, sizeof b, "%s{\"note\":%d,\"ch\":%d,\"m\":%d,\"s\":%d,\"n\":", t ? "," : "", e.trk[t].note, e.trk[t].ch + 1,
                      e.trk[t].mute, e.trk[t].solo);
        j += b;
        j += js(e.trk[t].name) + "}";
    }
    j += "],\"pats\":[";
    for (int q = 0; q < NPAT; q++) {
        j += q ? "," : "";
        j += "{\"n\":" + js(e.pat[q].name) + (e.pat[q].used() ? ",\"u\":1}" : ",\"u\":0}");
    }
    j += "],\"len\":[";
    for (int t = 0; t < NTRK; t++) { std::snprintf(b, sizeof b, "%s%d", t ? "," : "", P.len[t]); j += b; }
    std::string vel = "],\"vel\":[", prob = "],\"prob\":[", rat = "],\"rat\":[";
    for (int t = 0; t < NTRK; t++) {
        const char *sep = t ? "," : "";
        vel += sep; prob += sep; rat += sep;
        vel += '"'; prob += '"'; rat += '"';
        for (int i = 0; i < NSTEP; i++) {
            hex2(vel, P.s[t][i].vel);
            hex2(prob, P.s[t][i].prob);
            rat += (char)('0' + P.s[t][i].rat);
        }
        vel += '"'; prob += '"'; rat += '"';
    }
    j += vel + prob + rat + "]}";
    return j;
}
static inline std::string pos_json(const mtx::Engine &e) {
    char b[320];
    int k = std::snprintf(b, sizeof b, "{\"rev\":%u,\"run\":%s,\"play\":%d,\"queued\":%d,\"step\":%lld,\"pos\":[", e.rev,
                          e.running ? "true" : "false", e.play, e.queued, e.step);
    for (int t = 0; t < mtx::NTRK && k < (int)sizeof b - 8; t++) k += std::snprintf(b + k, sizeof b - k, "%s%d", t ? "," : "", e.pos[t]);
    std::snprintf(b + k, sizeof b - k, "]}");
    return b;
}

static inline int route(const Hooks &h, int c, const forcehub::Request &r) {
    using namespace forcehub;
    mtx::Engine &e = *h.e;
    auto done = [&](bool ok_) {
        char b[48];
        uint32_t rv;
        { std::lock_guard<std::mutex> lk(*h.mu); rv = e.rev; }
        std::snprintf(b, sizeof b, "{\"ok\":%s,\"rev\":%u}", ok_ ? "true" : "false", rv);
        if (ok_ && h.changed) h.changed();
        ok_json(c, b);
        return DONE;
    };
    const int p = r.num("p", -1), t = r.num("t", -1), s = r.num("s", -1);
    if (r.route == "/state") {
        std::string j;
        { std::lock_guard<std::mutex> lk(*h.mu); j = state_json(e, p < 0 ? e.play : p); }
        j.pop_back();   /* add the presets (their own lock) */
        char b[48];
        std::snprintf(b, sizeof b, ",\"preset\":%d,\"presets\":", h.preset_cur ? h.preset_cur() + 1 : 1);
        j += b + (h.presets ? h.presets() : std::string("[]")) + "}";
        ok_json(c, j);
        return DONE;
    }
    if (r.route == "/pos") {
        std::string j;
        { std::lock_guard<std::mutex> lk(*h.mu); j = pos_json(e); }
        ok_json(c, j);
        return DONE;
    }
    if (r.route == "/step") {
        {
            std::lock_guard<std::mutex> lk(*h.mu);
            if (!e.ok(p, t, s)) { bad(c, "p 0..15, t 0..15, s 0..63"); return DONE; }
            if (!r.has("v") && !r.has("prob") && !r.has("rat")) e.toggle(p, t, s);
            else e.set_step(p, t, s, r.has("v") ? r.num("v") : -1, r.has("prob") ? r.num("prob") : -1, r.has("rat") ? r.num("rat") : -1);
        }
        return done(true);
    }
    if (r.route == "/len") {
        {
            std::lock_guard<std::mutex> lk(*h.mu);
            if (p < 0 || p >= mtx::NPAT) { bad(c, "p 0..15"); return DONE; }
            if (t < 0) e.set_len_all(p, r.num("len", 16)); else e.set_len(p, t, r.num("len", 16));
        }
        return done(true);
    }
    if (r.route == "/track") {
        {
            std::lock_guard<std::mutex> lk(*h.mu);
            if (t < -1 || t >= mtx::NTRK) { bad(c, "t 0..15"); return DONE; }
            for (int k = (t < 0 ? 0 : t); k <= (t < 0 ? mtx::NTRK - 1 : t); k++) {
                if (r.has("mute")) e.set_mute(k, r.num("mute"));
                if (r.has("solo")) e.set_solo(k, r.num("solo"));
            }
            if (t >= 0) {
                if (r.has("note")) e.set_note(t, r.num("note"));
                if (r.has("ch")) e.set_ch(t, r.num("ch") - 1);
                if (r.has("name")) e.set_track_name(t, r.arg("name"));
                e.sel = t;
            }
        }
        return done(true);
    }
    if (r.route == "/learn") {
        { std::lock_guard<std::mutex> lk(*h.mu); e.learn = (t >= 0 && t < mtx::NTRK) ? t : -1; if (t >= 0 && t < mtx::NTRK) e.sel = t; e.touch(); }
        return done(true);
    }
    if (r.route == "/hit") {
        if (t < 0 || t >= mtx::NTRK) { bad(c, "t 0..15"); return DONE; }
        if (h.hit) h.hit(t);
        return done(true);
    }
    if (r.route == "/pat") {
        {
            std::lock_guard<std::mutex> lk(*h.mu);
            if (r.has("play")) e.select_play(r.num("play"));
            else if (r.has("clear")) { const int q = r.num("clear"); if (q == e.play) e.all_off(); e.clear_pattern(q); }
            else if (r.has("copy")) e.copy_pattern(r.num("copy"), r.num("to", -1));
            else if (r.has("name")) e.set_pattern_name(p, r.arg("name"));
            else { bad(c, "play, clear, copy or name"); return DONE; }
        }
        return done(true);
    }
    if (r.route == "/edit") {
        {
            std::lock_guard<std::mutex> lk(*h.mu);
            const std::string op = r.arg("op");
            if (!e.ok(p, t)) { bad(c, "p, t"); return DONE; }
            if (op == "clear") e.clear_track(p, t);
            else if (op == "rot") e.rotate(p, t, r.num("d", 1));
            else if (op == "repeat") e.fill_repeat(p, t, r.num("n", 16));
            else { bad(c, "op clear|rot|repeat"); return DONE; }
        }
        return done(true);
    }
    if (r.route == "/glob") {
        {
            std::lock_guard<std::mutex> lk(*h.mu);
            if (r.has("swing")) e.swing = mtx::clampi(r.num("swing"), 50, 75);
            if (r.has("gate")) e.gate = mtx::clampi(r.num("gate"), 5, 95);
            if (r.has("tv")) e.tap_vel = mtx::clampi(r.num("tv"), 1, 127);
            if (r.has("cc")) e.ctl_ch = mtx::clampi(r.num("cc"), 0, 16);
            e.touch();
        }
        return done(true);
    }
    if (r.route == "/notes") {
        { std::lock_guard<std::mutex> lk(*h.mu); e.note_map(r.num("map")); }
        return done(true);
    }
    if (r.route == "/preset") {
        bool ok_ = false;
        if (r.has("load") && h.preset_load) ok_ = h.preset_load(r.num("load") - 1);
        else if (r.has("save") && h.preset_save) ok_ = h.preset_save(r.num("save") - 1, r.arg("name"));
        else { bad(c, "load or save"); return DONE; }
        return done(ok_);
    }
    return NOT_MINE;
}

}   // namespace mweb
#endif
