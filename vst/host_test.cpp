/* Offline x86 test of matrix_vst.cpp (test.sh builds it with -DNO_ALSA and ASan/UBSan): a fake
 * host gives the clock (60 BPM at 9600 Hz with 100-frame blocks = exactly one pulse of 96 per
 * quarter per block, so every note can be checked against its pulse), the outgoing MIDI is
 * captured through a test hook, the iPad grid is driven over real HTTP on 127.0.0.1:FORCEHUB_PORT.
 * Covers steps, swing, gate, velocity, probability, ratchets, polymeter, mute/solo by CC, pattern
 * queue at the bar line, program change, NOTE LEARN, stop/locate without hanging notes, skin
 * parameters and their echo to the host, chunk round trip, factory and saved presets, the hub
 * with two instances. Prints PASSED/FAILED. */
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

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
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
typedef std::vector<std::array<int, 3>> Out;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

/* ---- fake host ---- */
static VstTimeInfo g_ti;
static long long g_pulse = 0;
static bool g_play = false;
static std::map<int, float> g_automated;
static intptr_t master(AEffect *, int32_t op, int32_t idx, intptr_t, void *, float v) {
    if (op == 1) return 2400;
    if (op == 7) {
        std::memset(&g_ti, 0, sizeof g_ti);
        g_ti.sampleRate = 9600; g_ti.tempo = 60; g_ti.ppqPos = g_pulse / 96.0;
        g_ti.flags = (g_play ? 2 : 0) | (1 << 9) | (1 << 10);
        return (intptr_t)&g_ti;
    }
    if (op == 0) g_automated[idx] = v;
    return 0;
}
static float bufL[128], bufR[128];
static void block(AEffect *e) {
    float *io[2] = {bufL, bufR};
    e->processReplacing(e, io, io, 100);
    if (g_play) g_pulse++;
}

typedef void (*capture_t)(AEffect *, Out *);
typedef void (*in_t)(AEffect *, int, int, int);
typedef int (*port_t)(AEffect *);
static capture_t t_capture;
static in_t t_in;
static port_t t_port;

/* notes captured with the pulse they went out in */
struct Note { long long pulse; int st, d1, d2; };
static Out g_out;
static std::vector<Note> g_notes;
static void run(AEffect *e, int pulses) {
    for (int i = 0; i < pulses; i++) {
        const long long p = g_pulse;   /* g_out may already hold what an edit sent (a note change ends its note) */
        block(e);
        for (auto &m : g_out) g_notes.push_back({p, m[0], m[1], m[2]});
        g_out.clear();
    }
}
static std::vector<long long> ons(int note, int ch = 0) {
    std::vector<long long> v;
    for (auto &n : g_notes) if (n.st == (0x90 | ch) && n.d1 == note && n.d2 > 0) v.push_back(n.pulse);
    return v;
}
static int count_ons() { int k = 0; for (auto &n : g_notes) if ((n.st & 0xF0) == 0x90 && n.d2) k++; return k; }
static int count_offs() { int k = 0; for (auto &n : g_notes) if ((n.st & 0xF0) == 0x80 || ((n.st & 0xF0) == 0x90 && !n.d2)) k++; return k; }

/* ---- HTTP ---- */
static int hub_port() { return std::atoi(std::getenv("FORCEHUB_PORT")); }
static std::string http_full(const std::string &path_in, int port) {
    std::string pe;
    for (char c : path_in) { if (c == ' ') pe += "%20"; else pe += c; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string r;
    for (int t = 0; t < 20 && connect(fd, (sockaddr *)&a, sizeof a) < 0; t++) { usleep(50000); close(fd); fd = socket(AF_INET, SOCK_STREAM, 0); }
    const std::string req = "GET " + pe + " HTTP/1.1\r\nHost: x\r\n\r\n";
    if (send(fd, req.data(), req.size(), MSG_NOSIGNAL) < 0) { close(fd); return r; }
    char b[8192];
    ssize_t n;
    while ((n = recv(fd, b, sizeof b, 0)) > 0) r.append(b, (size_t)n);
    close(fd);
    return r;
}
static std::string http_get(const std::string &path, int port = 0) {
    const std::string r = http_full(path, port ? port : hub_port());
    const size_t h = r.find("\r\n\r\n");
    return h == std::string::npos ? "" : r.substr(h + 4);
}
static bool has(const std::string &s, const std::string &x) { return s.find(x) != std::string::npos; }

/* ---- parameters ---- */
static int pidx(AEffect *e, const char *name) {
    char b[64];
    for (int i = 0; i < e->numParams; i++) { b[0] = 0; e->dispatcher(e, 8, i, 0, b, 0); if (!std::strcmp(b, name)) return i; }
    return -1;
}
static std::string display(AEffect *e, const char *name) {
    char b[128] = "";
    e->dispatcher(e, 7, pidx(e, name), 0, b, 0);
    return b;
}
static void set_ui(AEffect *e, const char *name, float lo, float hi, float v) { e->setParameter(e, pidx(e, name), (v - lo) / (hi - lo)); }
static void press(AEffect *e, const char *name) { const int i = pidx(e, name); e->setParameter(e, i, 1.0f); e->setParameter(e, i, 0.0f); }

static AEffect *open_plugin(void *lib) {
    typedef AEffect *(*entry_t)(audioMasterCallback);
    AEffect *e = ((entry_t)dlsym(lib, "VSTPluginMain"))(master);
    e->dispatcher(e, 0, 0, 0, nullptr, 0);
    e->dispatcher(e, 10, 0, 0, nullptr, 9600.0f);
    t_capture(e, &g_out);
    return e;
}
static void close_plugin(AEffect *e) { e->dispatcher(e, 1, 0, 0, nullptr, 0); }
static void reset_clock() { g_pulse = 0; g_notes.clear(); }

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: host_test plugin.so\n"); return 2; }
    if (!std::getenv("FORCEHUB_PORT")) setenv("FORCEHUB_PORT", "18090", 1);
    char bank[] = "/tmp/matrix_test_bank_XXXXXX";
    int fd = mkstemp(bank);
    if (fd >= 0) close(fd);
    setenv("MATRIX_PRESETS", bank, 1);
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::printf("dlopen: %s\n", dlerror()); return 2; }
    t_capture = (capture_t)dlsym(lib, "matrix_test_capture");
    t_in = (in_t)dlsym(lib, "matrix_test_in");
    t_port = (port_t)dlsym(lib, "matrix_test_port");
    if (!t_capture || !t_in || !t_port) { std::printf("no test hooks (build with -DNO_ALSA)\n"); return 2; }

    AEffect *e = open_plugin(lib);
    CHECK(e->numPrograms == 32 && e->numInputs == 2 && e->numOutputs == 2, "insert effect with 32 programs");
    CHECK(pidx(e, "Pattern") >= 0 && pidx(e, "Swing") >= 0 && pidx(e, "Clear Track") >= 0, "parameters present");
    {
        char b[64] = "";
        e->dispatcher(e, 29, 1, 0, b, 0);
        CHECK(!std::strcmp(b, "HOUSE (3 PATTERNS)"), "factory preset 2 named: %s", b);
    }

    /* ---- hub and page ---- */
    CHECK(t_port(e) == hub_port(), "first instance on the first port (%d)", t_port(e));
    CHECK(has(http_get("/"), "<title>Force</title>"), "hub at /");
    CHECK(has(http_get("/app"), "<title>Matrix</title>"), "grid page at /app");
    const std::string info = http_full("/info", hub_port());
    CHECK(has(info, "Access-Control-Allow-Origin: *") && has(info, "\"id\":\"matrix\""), "info: %s", info.c_str());
    CHECK(has(info, "STOP"), "info summary shows the transport");
    std::string st = http_get("/state?p=0");
    CHECK(has(st, "\"tracks\":[{\"note\":36,\"ch\":1,\"m\":0,\"s\":0,\"n\":\"Kick\"}"), "state: kit: %.120s", st.c_str());
    CHECK(has(st, "\"presets\":[{\"n\":\"EMPTY KIT 36-51\""), "state: presets");
    CHECK(has(st, "\"len\":[16,16"), "state: lengths");

    /* ---- steps from the grid; one bar at 60 BPM ---- */
    for (int s : {0, 4, 8, 12}) CHECK(has(http_get("/step?p=0&t=0&s=" + std::to_string(s) + "&v=100"), "\"ok\":true"), "set kick step %d", s);
    CHECK(has(http_get("/step?p=0&t=1&s=4"), "\"ok\":true"), "toggle snare step 4 (tap velocity)");
    CHECK(has(http_get("/step?p=0&t=1&s=12&v=127"), "ok"), "snare accent 12");
    CHECK(has(http_get("/step?p=0&t=99&s=0"), "p 0..15"), "bad track refused");
    st = http_get("/state?p=0");
    CHECK(has(st, "\"vel\":[\"64000000640000006400000064000000"), "state: kick velocities in hex");
    reset_clock();
    g_play = true;
    run(e, 96 * 4);                       /* one bar */
    std::vector<long long> k = ons(36);
    CHECK(k.size() == 4 && k[0] == 0 && k[1] == 96 && k[2] == 192 && k[3] == 288, "kick on the quarters: %zu", k.size());
    std::vector<long long> sn = ons(37);
    CHECK(sn.size() == 2 && sn[0] == 96 && sn[1] == 288, "snare on 2 and 4: %zu", sn.size());
    {
        int v12 = -1, gate_off = -1;
        for (auto &n : g_notes) {
            if (n.st == 0x90 && n.d1 == 37 && n.pulse == 288) v12 = n.d2;
            if (n.st == 0x80 && n.d1 == 36 && n.pulse > 0 && gate_off < 0) gate_off = (int)n.pulse;
        }
        CHECK(v12 == 127, "accent velocity %d", v12);
        CHECK(gate_off == 12, "gate 50 %% = 12 pulses: off at %d", gate_off);
    }
    st = http_get("/pos");
    CHECK(has(st, "\"run\":true") && has(st, "\"pos\":[15,15"), "pos: running, last 16th: %s", st.c_str());
    CHECK(has(display(e, "Status"), "BAR 1.4.4"), "status '%s'", display(e, "Status").c_str());
    CHECK(display(e, "Steps") == "x... x... x... x...", "steps text '%s'", display(e, "Steps").c_str());

    /* ---- swing 75: odd 16ths 12 pulses later ---- */
    http_get("/step?p=0&t=3&s=1&v=90");
    http_get("/glob?swing=75");
    reset_clock();
    run(e, 96);
    k = ons(39);
    CHECK(k.size() == 1 && k[0] == 24 + 12, "swung 16th at %lld", k.empty() ? -1 : k[0]);
    http_get("/glob?swing=50");
    run(e, 1);
    g_out.clear();
    {   /* the host heard the web page's swing changes */
        const int sw = pidx(e, "Swing");
        CHECK(g_automated.count(sw) && std::fabs(g_automated[sw] - 0.0f) < 1e-4, "swing reported back to the host");
    }

    /* ---- stop: everything off ---- */
    g_play = false;
    run(e, 2);
    CHECK(count_ons() <= count_offs() + 0, "stop: %d ons, %d offs", count_ons(), count_offs());
    CHECK(has(http_get("/pos"), "\"run\":false"), "stopped");

    /* ---- polymeter, probability, ratchet ---- */
    http_get("/edit?op=clear&p=0&t=0");
    http_get("/edit?op=clear&p=0&t=1");
    http_get("/edit?op=clear&p=0&t=3");
    http_get("/step?p=0&t=2&s=0&v=100");
    http_get("/len?p=0&t=2&len=3");          /* clap every 3rd 16th */
    http_get("/step?p=0&t=4&s=0&v=100&rat=3");
    for (int s = 0; s < 16; s++) http_get("/step?p=0&t=5&s=" + std::to_string(s) + "&v=80&prob=50");
    reset_clock();
    g_play = true;
    run(e, 96 * 4 * 4);                      /* 4 bars */
    k = ons(38);
    CHECK(k.size() == 22 && k[1] == 72 && k[2] == 144, "clap every 3 steps: %zu", k.size());
    k = ons(40);
    CHECK(k.size() == 12 && k[0] == 0 && k[1] == 8 && k[2] == 16 && k[3] == 384, "ratchet x3 per bar: %zu", k.size());
    const size_t pr = ons(41).size();
    CHECK(pr > 12 && pr < 52, "chance 50 %%: %zu of 64", pr);

    /* ---- Control In on CONTROL CH 14: mute, solo, pattern by CC / program change ---- */
    g_notes.clear();
    t_in(e, 0xBD, 3, 127);                   /* CC 3 on ch 14: mute track 3 (clap) */
    t_in(e, 0xB0, 5, 127);                   /* other channel: ignored */
    run(e, 96 * 4);
    CHECK(ons(38).empty() && !ons(40).empty(), "clap muted by CC 3");
    CHECK(has(http_get("/state"), "\"m\":1"), "mute shows on the page");
    g_notes.clear();
    t_in(e, 0xBD, 3, 0);
    t_in(e, 0xBD, 21, 127);                  /* solo track 5 (ratchet) */
    run(e, 96 * 4);
    CHECK(ons(38).empty() && ons(41).empty() && ons(40).size() == 3, "solo track 5 only");
    t_in(e, 0xBD, 21, 0);
    /* pattern 2: queued, starts at the next bar */
    http_get("/step?p=1&t=8&s=0&v=100");
    g_notes.clear();
    while (g_pulse % 384 != 200) run(e, 1);
    CHECK(has(http_get("/pat?play=1"), "ok"), "queue pattern 2");
    CHECK(has(display(e, "Pattern"), "NEXT"), "skin shows the queued pattern: '%s'", display(e, "Pattern").c_str());
    run(e, 184 + 10);
    k = ons(44);
    CHECK(k.size() == 1 && k[0] % 384 == 0, "pattern 2 starts on the bar line");
    CHECK(has(http_get("/pos"), "\"play\":1,\"queued\":-1"), "pattern 2 playing");
    t_in(e, 0xCD, 0, 0);                     /* program change 1 on ch 14: back to pattern 1 */
    CHECK(has(http_get("/pos"), "\"queued\":0"), "program change queues pattern 1");
    t_in(e, 0xBD, 37, 127);                  /* CC 37: previous pattern from the queued one */
    CHECK(has(http_get("/pos"), "\"queued\":15"), "CC 37 previous pattern: %s", http_get("/pos").c_str());
    t_in(e, 0xBD, 36, 127);                  /* CC 36: next from 16 = 1 */
    CHECK(has(http_get("/pos"), "\"queued\":0"), "CC 36 next pattern: %s", http_get("/pos").c_str());
    t_in(e, 0xBD, 36, 127);                  /* again: 2 = the playing one, nothing queued */
    CHECK(has(http_get("/pos"), "\"queued\":-1"), "CC 36 back to the playing pattern: %s", http_get("/pos").c_str());
    http_get("/pat?play=0");

    /* ---- locate: no hanging notes ---- */
    http_get("/len?p=0&t=4&len=16");
    run(e, 3);
    g_pulse = 5000;
    run(e, 50);
    g_play = false;
    run(e, 2);
    CHECK(count_ons() <= count_offs(), "locate + stop: %d ons, %d offs", count_ons(), count_offs());

    /* ---- skin: TRACK tab ---- */
    set_ui(e, "Track", 1, 16, 4);
    CHECK(display(e, "Track") == "4 Hat C", "track 4 '%s'", display(e, "Track").c_str());
    set_ui(e, "Note", 0, 127, 42);
    set_ui(e, "Channel", 1, 16, 10);
    set_ui(e, "Length", 1, 64, 32);
    st = http_get("/state?p=0");
    CHECK(has(st, "{\"note\":42,\"ch\":10,\"m\":0,\"s\":0,\"n\":\"Hat C\"}"), "skin edits the kit");
    CHECK(has(st, "\"len\":[16,16,3,32,"), "skin edits the length");
    CHECK(display(e, "Note") == "42  F#1", "note name '%s'", display(e, "Note").c_str());
    /* NOTE LEARN: next note from anywhere */
    press(e, "Learn Note");
    CHECK(has(display(e, "Track"), "LEARN"), "learn armed");
    t_in(e, 0x92, 50, 90);
    st = http_get("/state");
    CHECK(has(st, "{\"note\":50,\"ch\":3,"), "learned note 50 ch 3");
    CHECK(has(st, "\"learn\":-1"), "learn done");
    http_get("/learn?t=0");
    t_in(e, 0x99, 36, 100);
    CHECK(has(http_get("/state"), "[{\"note\":36,\"ch\":10,"), "web learn: kick on ch 10");
    /* audition */
    g_notes.clear();
    http_get("/hit?t=0");
    run(e, 3);
    k = ons(36, 9);
    CHECK(k.size() == 1, "audition sends the kick");
    run(e, 15);                               /* 120 ms = 1152 samples = 12 blocks */
    CHECK(count_offs() >= 1, "audition note off");

    /* ---- names, copy, rotate, repeat ---- */
    http_get("/pat?p=0&name=Verse%20A");
    http_get("/track?t=1&name=Snare%3B2");
    http_get("/pat?copy=0&to=5");
    st = http_get("/state?p=5");
    CHECK(has(st, "\"pats\":[{\"n\":\"Verse A\",\"u\":1}") && has(st, "{\"n\":\"Verse A\",\"u\":1},{\"n\":\"\",\"u\":0},{\"n\":\"\",\"u\":0}"), "copy keeps the name");
    http_get("/edit?op=rot&p=5&t=2&d=1");
    CHECK(has(http_get("/state?p=5"), "\"vel\":[\"" + std::string(128, '0') + "\",\"" + std::string(128, '0') + "\",\"0064") , "rotate clap");
    http_get("/step?p=5&t=9&s=2&v=99");
    http_get("/edit?op=repeat&p=5&t=9&n=16");
    st = http_get("/state?p=5");
    {
        const size_t a = st.find("\"vel\":[");
        size_t q = a;
        for (int i = 0; i < 9; i++) q = st.find("\",\"", q + 1);
        const std::string row = st.substr(q + 3, 128);
        CHECK(row.substr(4, 2) == "63" && row.substr(36, 2) == "63" && row.substr(100, 2) == "63", "repeat to 64: %s", row.c_str());
    }

    /* ---- chunk round trip ---- */
    void *ck = nullptr;
    const intptr_t n = e->dispatcher(e, 23, 0, 0, &ck, 0);
    const std::string saved((const char *)ck, (size_t)n);
    CHECK(n > 10 && !saved.compare(0, 5, "MTX1;") && saved.size() < 2000, "chunk %zd bytes", (ssize_t)n);
    AEffect *e2 = open_plugin(lib);
    CHECK(t_port(e2) == hub_port() + 1, "second instance on the next port: %d", t_port(e2));
    CHECK(has(http_get("/info", hub_port() + 1), "\"name\":\"Matrix\""), "second instance's info");
    CHECK(e2->dispatcher(e2, 24, 0, (intptr_t)saved.size(), (void *)saved.data(), 0) == 1, "set chunk");
    st = http_get("/state?p=0", hub_port() + 1);
    CHECK(has(st, "Snare;2") && has(st, "\"Verse A\""), "restored names");
    void *ck2 = nullptr;
    const intptr_t n2 = e2->dispatcher(e2, 23, 0, 0, &ck2, 0);
    CHECK(n2 == n && !std::memcmp(ck, ck2, (size_t)n), "chunk identical after round trip\n%s\n%s", saved.c_str(), (const char *)ck2);
    e2->dispatcher(e2, 2, 0, 0, nullptr, 0);   /* the host re-selects the current program: patterns stay */
    CHECK(has(http_get("/state?p=0", hub_port() + 1), "\"Verse A\""), "effSetProgram(current) keeps the project");
    close_plugin(e2);
    CHECK(http_get("/info", hub_port() + 1).empty(), "closed instance's server gone");

    /* ---- presets ---- */
    CHECK(has(http_get("/preset?save=10&name=My%20Kit"), "\"ok\":true"), "save preset 10 from the web");
    {
        char b[64] = "";
        e->dispatcher(e, 29, 9, 0, b, 0);
        CHECK(!std::strcmp(b, "My Kit"), "preset 10 name '%s'", b);
        FILE *f = std::fopen(bank, "r");
        char line[256] = "";
        if (f) { if (!std::fgets(line, sizeof line, f)) line[0] = 0; std::fclose(f); }
        CHECK(!std::strncmp(line, "10\tMy Kit\tsw=", 13), "bank file line '%.40s'", line);
    }
    e->dispatcher(e, 2, 0, 1, nullptr, 0);   /* program 2: HOUSE */
    st = http_get("/state?p=0");
    CHECK(has(st, "\"House A\"") && has(st, "\"House Fill\""), "factory preset HOUSE loaded");
    CHECK(has(st, "\"swing\":56"), "factory swing");
    CHECK(display(e, "Preset") == "2 HOUSE (3 PATTERNS)", "preset display '%s'", display(e, "Preset").c_str());
    set_ui(e, "Preset", 1, 32, 10);
    press(e, "Load");
    CHECK(has(http_get("/state?p=0"), "\"Verse A\""), "LOAD preset 10 brings the saved kit back");
    for (int f = 2; f <= 3; f++) {   /* every factory preset plays without hanging notes */
        e->dispatcher(e, 2, 0, f, nullptr, 0);
        reset_clock(); g_play = true; run(e, 96 * 16); g_play = false; run(e, 2);
        CHECK(count_ons() > 20 && count_ons() <= count_offs(), "factory %d plays: %d ons, %d offs", f + 1, count_ons(), count_offs());
    }

    /* ---- web view off / on ---- */
    set_ui(e, "Web View", 0, 1, 0);
    CHECK(http_get("/info").empty(), "phone view off");
    set_ui(e, "Web View", 0, 1, 1);
    CHECK(has(http_get("/info"), "matrix"), "phone view on again");
    CHECK(has(display(e, "Phone Address"), ":"), "address '%s'", display(e, "Phone Address").c_str());

    /* ---- random stress over HTTP and parameters while playing ---- */
    srand(7);
    g_play = true;
    for (int i = 0; i < 400; i++) {
        const int r = rand() % 6;
        char q[160];
        if (r == 0) std::snprintf(q, sizeof q, "/step?p=%d&t=%d&s=%d&v=%d&prob=%d&rat=%d", rand() % 16, rand() % 16, rand() % 64, rand() % 128, rand() % 101, rand() % 5);
        else if (r == 1) std::snprintf(q, sizeof q, "/len?p=%d&t=%d&len=%d", rand() % 16, rand() % 17 - 1, rand() % 70);
        else if (r == 2) std::snprintf(q, sizeof q, "/pat?play=%d", rand() % 16);
        else if (r == 3) std::snprintf(q, sizeof q, "/track?t=%d&mute=%d&note=%d", rand() % 16, rand() % 2, rand() % 128);
        else if (r == 4) std::snprintf(q, sizeof q, "/edit?op=rot&p=%d&t=%d&d=%d", rand() % 16, rand() % 16, rand() % 9 - 4);
        else std::snprintf(q, sizeof q, "/state?p=%d", rand() % 20);
        http_get(q);
        e->setParameter(e, rand() % e->numParams, (rand() % 1000) / 999.0f);
        run(e, 7);
    }
    g_play = false;
    run(e, 2);
    CHECK(count_ons() <= count_offs(), "stress: %d ons, %d offs", count_ons(), count_offs());

    close_plugin(e);
    unlink(bank);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
