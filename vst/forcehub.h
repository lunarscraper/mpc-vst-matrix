/* =============================================================================
 * forcehub.h - the shared web hub of the lunarscraper Force plugins (CC Map, Matrix, ...).
 *
 * Every plugin that has a phone/iPad view runs one of these servers. It takes the first free port
 * of 8000..8009 (FORCEHUB_PORT moves the range, offline tests), so any number of plugins can run
 * side by side, and every one of them answers the same three things:
 *
 *   GET /        the hub: one tile per plugin found on 8000..8009 of the same Force (the page asks
 *                each port for /info; no other process, file or service is involved)
 *   GET /info    {"id","name","color","port","app","summary"} with Access-Control-Allow-Origin: *
 *                so the hub on any port may read it
 *   GET /app     this plugin's own page
 *
 * Everything else goes to the plugin's `route` callback (state JSON, edits, downloads). One thread
 * answers the requests one at a time (poll() with a short timeout so the plugin can close it); a
 * route may keep the socket and hand it to a thread of its own (return KEPT; see `stopping`).
 * Plain POSIX sockets, no dependencies. The same file is copied into every plugin repo: keep the
 * copies identical (HUB_VERSION).
 * ========================================================================== */
#ifndef FORCEHUB_H
#define FORCEHUB_H
#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#define HUB_VERSION 1

namespace forcehub {

/* JSON string escaping */
static inline std::string js(const char *s) {
    std::string o = "\"";
    char b[8];
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 32) { std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}
static inline std::string js(const std::string &s) { return js(s.c_str()); }

/* one GET: route "/state", query "a=1&b=2" (percent-decoded by arg()) */
struct Request {
    std::string route, query;
    std::string arg(const char *key) const {
        const std::string k = std::string(key) + "=";
        size_t p = 0;
        while ((p = query.find(k, p)) != std::string::npos) {
            if (p == 0 || query[p - 1] == '&') {
                const size_t e = query.find('&', p);
                const std::string v = query.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
                std::string o;
                for (size_t i = 0; i < v.size(); i++) {
                    if (v[i] == '%' && i + 2 < v.size()) { o += (char)std::strtol(v.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
                    else if (v[i] == '+') o += ' ';
                    else o += v[i];
                }
                return o;
            }
            p += k.size();
        }
        return "";
    }
    bool has(const char *key) const { return query.find(std::string(key) + "=") != std::string::npos; }
    int num(const char *key, int dflt = 0) const { const std::string v = arg(key); return v.empty() ? dflt : std::atoi(v.c_str()); }
};

static inline void send_all(int c, const char *d, size_t n) {
    size_t off = 0;
    while (off < n) {
        const ssize_t k = send(c, d + off, n - off, MSG_NOSIGNAL);
        if (k <= 0) return;
        off += (size_t)k;
    }
}
static inline void send_all(int c, const std::string &s) { send_all(c, s.data(), s.size()); }
static inline void reply(int c, const char *status, const char *type, const std::string &body, bool cors = false) {
    char h[320];
    std::snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n%s"
                  "Connection: close\r\n\r\n", status, type, body.size(), cors ? "Access-Control-Allow-Origin: *\r\n" : "");
    send_all(c, h, std::strlen(h));
    send_all(c, body);
}
static inline void ok_json(int c, const std::string &j) { reply(c, "200 OK", "application/json", j); }
static inline void bad(int c, const char *why) { reply(c, "400 Bad Request", "text/plain", std::string(why) + "\n"); }

enum { NOT_MINE = 0, DONE = 1, KEPT = 2 };

/* the hub page: asks 8000..8009 (from the port it was loaded from: base = port - port % 10) */
inline const char HUB_PAGE[] = R"HUBPAGE(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<title>Force</title>
<style>
:root{--bg:#141518;--panel:#1f2126;--line:#2e3138;--ink:#f0ece4;--dim:#9a968e}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.35 -apple-system,system-ui,sans-serif;
 padding:max(16px,env(safe-area-inset-top)) 16px 24px}
h1{font-size:20px;margin:4px 0 2px;font-weight:650;letter-spacing:.02em}
.sub{color:var(--dim);font-size:13px;margin-bottom:18px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px}
a.tile{display:block;text-decoration:none;color:var(--ink);background:var(--panel);border:1px solid var(--line);
 border-radius:14px;padding:16px 14px 14px;min-height:108px;position:relative;overflow:hidden}
a.tile:active{transform:scale(.98)}
.bar{position:absolute;left:0;top:0;right:0;height:5px}
.nm{font-size:19px;font-weight:650;margin-top:6px}
.sm{color:var(--dim);font-size:13px;margin-top:6px;min-height:2.6em}
.pt{position:absolute;right:12px;top:12px;color:var(--dim);font-size:12px}
.none{color:var(--dim);padding:24px 4px}
</style></head><body>
<h1>Force</h1>
<div class="sub" id="sub">suche Plugins …</div>
<div class="grid" id="grid"></div>
<script>
const host = location.hostname, me = +location.port || 80, base = me - me % 10;
const ports = Array.from({length: 10}, (_, i) => base + i);
const found = new Map();
async function probe(p) {
  const ac = new AbortController(), t = setTimeout(() => ac.abort(), 900);
  try {
    const r = await fetch('http://' + host + ':' + p + '/info', {cache: 'no-store', signal: ac.signal});
    const j = await r.json();
    found.set(p, j);
  } catch (e) { found.delete(p); }
  clearTimeout(t);
}
function esc(s) { return String(s).replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c])); }
function draw() {
  const list = [...found.entries()].sort((a, b) => a[0] - b[0]);
  const count = {};
  for (const [, j] of list) count[j.id] = (count[j.id] || 0) + 1;
  const g = document.getElementById('grid');
  g.innerHTML = list.length ? list.map(([p, j]) =>
    '<a class="tile" href="http://' + host + ':' + p + '/app">' +
    '<div class="bar" style="background:' + esc(j.color) + '"></div>' +
    '<div class="pt">:' + p + '</div>' +
    '<div class="nm">' + esc(j.name) + (count[j.id] > 1 ? ' <span style="color:var(--dim)">' + (p - base + 1) + '</span>' : '') + '</div>' +
    '<div class="sm">' + esc(j.summary || '') + '</div></a>').join('')
    : '<div class="none">Kein Plugin gefunden. Läuft auf der Force ein Plugin mit eingeschaltetem PHONE VIEW?</div>';
  document.getElementById('sub').textContent = host + ' · ' + list.length + (list.length === 1 ? ' Plugin' : ' Plugins');
}
async function scan() { await Promise.all(ports.map(probe)); draw(); }
scan();
setInterval(scan, 2500);
</script></body></html>)HUBPAGE";

struct Server {
    /* set before start() */
    std::string id = "plugin", name = "Plugin", color = "#ff8a2a";
    const char *app = "";                                        /* the plugin's own page (GET /app) */
    std::function<std::string()> summary;                        /* one line for its tile, may be empty */
    std::function<int(int, const Request &)> route;              /* NOT_MINE / DONE / KEPT */

    std::atomic<int> downloads{0};                               /* for routes that keep the socket */
    volatile bool stopping = false;
    int port = 0;
    std::atomic<long> requests{0};

    int fd = -1;
    std::atomic<bool> running{false};
    std::thread thr;
    std::mutex addr_mu;
    std::string addr_s = "-";
    std::string address() { std::lock_guard<std::mutex> lk(addr_mu); return addr_s; }

    static int base_port() { const char *p = std::getenv("FORCEHUB_PORT"); return p && *p ? std::atoi(p) : 8000; }

    /* the first free port of base..base+count-1 */
    bool start(int count = 10) {
        const int base = base_port();
        for (int p = base; p < base + count; p++) {
            const int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) return false;
            int one = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);   /* TIME_WAIT only; never shares a live port */
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons((uint16_t)p);
            if (bind(s, (sockaddr *)&a, sizeof a) == 0 && listen(s, 8) == 0) {
                fd = s;
                port = p;
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                refresh_addr();
                stopping = false;
                running = true;
                thr = std::thread([this] { loop(); });
                return true;
            }
            close(s);
        }
        return false;
    }
    void stop() {
        stopping = true;
        if (running.exchange(false) && thr.joinable()) thr.join();
        for (int i = 0; i < 500 && downloads.load() > 0; i++) usleep(10000);
        if (fd >= 0) close(fd);
        fd = -1;
    }
    /* the address to type into the phone: the first IPv4 that isn't loopback (wlan0 on the Force) */
    void refresh_addr() {
        ifaddrs *ifs = nullptr;
        char ip[INET_ADDRSTRLEN] = "";
        if (getifaddrs(&ifs) == 0) {
            for (ifaddrs *i = ifs; i; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
                inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
                if (!std::strncmp(i->ifa_name, "wl", 2)) break;   /* prefer the WLAN */
            }
            freeifaddrs(ifs);
        }
        char b[64];
        if (ip[0]) std::snprintf(b, sizeof b, "%s:%d", ip, port);
        else std::snprintf(b, sizeof b, "NO NETWORK (:%d)", port);
        std::lock_guard<std::mutex> lk(addr_mu);
        addr_s = b;
    }
    std::string info() {
        char b[48];
        std::snprintf(b, sizeof b, ",\"port\":%d,\"hub\":%d,\"app\":\"/app\"", port, HUB_VERSION);
        return "{\"id\":" + js(id) + ",\"name\":" + js(name) + ",\"color\":" + js(color) + b +
               ",\"summary\":" + js(summary ? summary() : std::string()) + "}";
    }
    /* true: a route kept the socket */
    bool handle(int c) {
        char buf[4096];
        int got = 0;
        pollfd pf{c, POLLIN, 0};
        while (got < (int)sizeof buf - 1 && poll(&pf, 1, 300) > 0) {
            const ssize_t n = recv(c, buf + got, sizeof buf - 1 - got, 0);
            if (n <= 0) break;
            got += (int)n;
            buf[got] = 0;
            if (std::strstr(buf, "\r\n\r\n")) break;
        }
        buf[got] = 0;
        if (std::strncmp(buf, "GET ", 4)) { reply(c, "405 Method Not Allowed", "text/plain", "GET only\n"); return false; }
        const char *sp = std::strchr(buf + 4, ' ');
        const std::string path(buf + 4, sp ? (size_t)(sp - buf - 4) : std::strlen(buf + 4));
        const size_t qm = path.find('?');
        Request r;
        r.route = path.substr(0, qm);
        r.query = qm == std::string::npos ? "" : path.substr(qm + 1);
        requests++;
        if (r.route == "/" || r.route == "/index.html") { reply(c, "200 OK", "text/html; charset=utf-8", HUB_PAGE); return false; }
        if (r.route == "/info") { reply(c, "200 OK", "application/json", info(), true); return false; }
        if (r.route == "/app") { reply(c, "200 OK", "text/html; charset=utf-8", app); return false; }
        const int h = route ? route(c, r) : NOT_MINE;
        if (h == KEPT) return true;
        if (h == NOT_MINE) reply(c, "404 Not Found", "text/plain", "not found\n");
        return false;
    }
    void loop() {
        int tick = 0;
        while (running) {
            pollfd pf{fd, POLLIN, 0};
            if (poll(&pf, 1, 200) > 0) {
                const int c = accept(fd, nullptr, nullptr);
                if (c >= 0 && !handle(c)) close(c);
            }
            if (++tick % 50 == 0) refresh_addr();   /* about every 10 s: the WLAN may come up later */
        }
    }
};



}   // namespace forcehub
#endif
