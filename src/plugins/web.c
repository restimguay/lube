/*
 * web.c - the plugin web: pages made of components, served over HTTP, safe
 * by default.
 *
 *   import web.*
 *   fn Counter() component {
 *     var n = 0
 *     fn add() { n += 1 }
 *     return div(p("count: {n}"), button({onclick: add}, "+1"))
 *   }
 *   let app = App()                       // or App({host:, dev:, trust_proxy:, policy: {...}})
 *   app.page("/", fn(req) => Counter(), {title: "Counter"})
 *   app.get("/hello/:name", fn(req) => "hello {req.params.name}")
 *   app.static("/assets", "public")
 *   app.serve(8080)                       // or: app.start(port), then app.step(wait) in a loop
 *
 * A page is HTML from the server. Its handlers stay on the server: the page's
 * script (/_lube/web.js) sends each event over a WebSocket, the handler runs,
 * and the page gets the view again. One thread does it all, in step(): no
 * Lube code ever runs on another thread.
 *
 * The policy, on from the start (only App({policy: {...}}) loosens a rule, by
 * name; an unknown name is an error, so a typo cannot weaken one):
 *   - requests are read strictly (web/http.c), within limits: 16 KB of head,
 *     100 headers, an 8 KB URL, a 1 MB body; and in time: 10 s for the head,
 *     30 s for the body, 5 s idle between requests (slow clients cannot hold
 *     connections: "slowloris"); 512 connections, 64 from one address;
 *   - every answer has a CSP (scripts and styles from this site only, no
 *     inline code, no frames of it elsewhere), nosniff, Referrer-Policy,
 *     COOP, CORP and X-Frame-Options; a handler cannot change those;
 *   - HTML is escaped (web/html.c);
 *   - static files come from their folder alone: no "..", no files or folders
 *     whose names start with ".", no symbolic links, no listings;
 *   - a page's WebSocket must come from the page's own site (Origin), with
 *     the page's secret and the session's cookie (HttpOnly, SameSite=Strict);
 *     its handler ids are secret too, and its events are limited (20 a
 *     second, bursts of 50, 64 KB each);
 *   - a handler's error answers "500", without the error, unless dev is on;
 *   - the server listens on 127.0.0.1 unless told a host; it does not speak
 *     TLS: put it behind a proxy that does, with trust_proxy.
 */
#ifdef _WIN32
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600  /* WSAPoll, inet_ntop: Windows Vista and later */
#endif
#define _CRT_RAND_S  /* rand_s: the system's random numbers */
#elif !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  /* getaddrinfo, poll, realpath, O_NOFOLLOW under -std=c99 */
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE  /* arc4random_buf */
#endif
#include "../lube_plugin.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Sock;
typedef WSAPOLLFD PollFd;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#define sys_poll(fds, n, ms) WSAPoll((fds), (ULONG)(n), (ms))
#define net_errno() WSAGetLastError()
#define E_AGAIN(e) ((e) == WSAEWOULDBLOCK)
#define E_INTR(e) ((e) == WSAEINTR)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int Sock;
typedef struct pollfd PollFd;
#define BAD_SOCK (-1)
#define close_sock close
#define sys_poll(fds, n, ms) poll((fds), (nfds_t)(n), (ms))
#define net_errno() errno
#define E_AGAIN(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define E_INTR(e) ((e) == EINTR)  /* LCOV_EXCL_LINE: (a signal, while it reads or sends) */
#endif
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)

static const LubeAPI *lube;

#include "web/util.c"
#include "web/html.c"
#include "web/http.c"

/* ======================================================================
 * The policy
 * ==================================================================== */

typedef struct {
    double max_header, max_headers, max_url, max_body;
    double header_timeout, body_timeout, keep_alive, ws_idle, page_wait;
    double max_connections, max_per_ip, max_pages;
    double max_message, events_per_second, event_burst;
    int inline_styles, secure_cookies;
    char *csp;               /* NULL: the default */
    char **origins;          /* other sites a page's WebSocket, or a POST, may come from ("https://example.com") */
    int norigins;
    char **hosts;            /* the names it answers to, besides localhost and addresses ("*": any) */
    int nhosts;
} Policy;

static const Policy DEFAULT_POLICY = {
    16384, 100, 8192, 1 << 20,
    10, 30, 5, 120, 60,
    512, 64, 1000,
    65536, 20, 50,
    0, 0, NULL, NULL, 0, NULL, 0,
};

static const char DEFAULT_CSP[] =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; "
    "object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";

/* The policy's Numbers, by name, for App({policy: {...}}). */
static const struct {
    const char *name;
    size_t at;
    int whole;  /* a count (bytes, connections): a whole Number */
} LIMITS[] = {
#define LIM(f, w) {#f, offsetof(Policy, f), w}
    LIM(max_header, 1), LIM(max_headers, 1), LIM(max_url, 1), LIM(max_body, 1),
    LIM(header_timeout, 0), LIM(body_timeout, 0), LIM(keep_alive, 0), LIM(ws_idle, 0), LIM(page_wait, 0),
    LIM(max_connections, 1), LIM(max_per_ip, 1), LIM(max_pages, 1),
    LIM(max_message, 1), LIM(events_per_second, 0), LIM(event_burst, 1),
#undef LIM
};

/* ======================================================================
 * An app: its routes, its connections and its live pages
 * ==================================================================== */

enum { R_FN, R_PAGE, R_STATIC };

typedef struct {
    int kind;
    int methods;    /* M_GET... */
    char **segs;    /* the pattern's segments: "users", ":id", "*rest" */
    int nsegs;
    LubeValue fn;   /* R_FN, R_PAGE */
    char *dir;      /* R_STATIC: the folder, its real path */
    char *title;    /* R_PAGE */
    char **styles, **scripts;
    int nstyles, nscripts;
} Route;

struct Conn;

typedef struct {
    char token[33];       /* the page's secret: its WebSocket names it */
    char cookie[33];      /* the session it was made for */
    uint64_t key[2];      /* its handler ids' key */
    LubeValue root;       /* what its route gave: a component, or a view */
    int version;          /* of the view the browser has */
    Buf html;             /* that view */
    LubeValue *handlers;  /* its handlers, by number (ids from key) */
    int nh;
    double made;
    struct Conn *ws;      /* its WebSocket, once it has one */
} Page;

enum { C_HEAD, C_BODY, C_WS, C_CLOSING, C_DEAD };

typedef struct Conn {
    Sock fd;
    char ip[64];
    int state;
    Buf in, out;
    double deadline;       /* C_HEAD, C_BODY: when it must have come; C_WS: when it has been quiet too long */
    double sent_at;        /* when its answer last made progress */
    int idle;              /* C_HEAD: between requests */
    int eof;               /* it closed its side */
    double ping_at;        /* C_WS */
    size_t head_len;       /* C_BODY: the head's bytes (and its empty line) */
    Req req;
    int keep;              /* keep the connection after this request */
    FILE *file;            /* a static file being sent */
    long long file_left;
    Page *page;            /* C_WS */
    Buf msg;               /* C_WS: a message in parts */
    int msg_open;
    double tokens, refilled;
} Conn;

typedef struct {
    Policy pol;
    char host[64];
    int dev, trust_proxy;
    Buf sec;               /* the policy's headers, as they go out */
    Route *routes;
    int nroutes;
    Sock listen;
    int port;
    int started, busy, stopping;
    Conn **conns;
    int nconns;
    Page **pages;
    int npages;
} App;

static LubeType *AppType;
static const char *const RESERVED = "_lube";

static int isalnum_c(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

static void route_free(Route *r) {
    for (int i = 0; i < r->nsegs; i++) free(r->segs[i]);
    for (int i = 0; i < r->nstyles; i++) free(r->styles[i]);
    for (int i = 0; i < r->nscripts; i++) free(r->scripts[i]);
    free(r->segs);
    free(r->styles);
    free(r->scripts);
    free(r->dir);
    free(r->title);
    if (r->kind != R_STATIC) lube->release(r->fn);
}

static void page_free(Page *p) {
    lube->release(p->root);
    render_drop_handlers(p->handlers, p->nh);
    buf_free(&p->html);
    if (p->ws) p->ws->page = NULL;
    free(p);
}

static void conn_free(Conn *c) {
    if (c->fd != BAD_SOCK) close_sock(c->fd);
    if (c->file) fclose(c->file);
    if (c->page) c->page->ws = NULL;
    buf_free(&c->in);
    buf_free(&c->out);
    buf_free(&c->msg);
    req_free(&c->req);
    free(c);
}

static void app_release(void *data) {
    App *a = (App *)data;
    for (int i = 0; i < a->nroutes; i++) route_free(&a->routes[i]);
    for (int i = 0; i < a->nconns; i++) conn_free(a->conns[i]);
    for (int i = 0; i < a->npages; i++) page_free(a->pages[i]);
    for (int i = 0; i < a->pol.norigins; i++) free(a->pol.origins[i]);
    for (int i = 0; i < a->pol.nhosts; i++) free(a->pol.hosts[i]);
    free(a->pol.origins);
    free(a->pol.hosts);
    free(a->pol.csp);
    free(a->routes);
    free(a->conns);
    free(a->pages);
    buf_free(&a->sec);
    if (a->started) close_sock(a->listen);
}

static int app_describe(const void *data, char *buf, size_t size) {
    const App *a = (const App *)data;
    if (!a->started) return snprintf(buf, size, "App(not started)");
    return snprintf(buf, size, strchr(a->host, ':') ? "App([%s]:%d)" : "App(%s:%d)", a->host, a->port);
}

static void net_start(void) {
#ifdef _WIN32
    static int started;
    if (!started) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        started = 1;
    }
#endif
}

/* ---- App(options) ---- */

static LubeValue option(LubeVM *vm, LubeValue obj, const char *key, int kind, const char *what, int *has) {
    LubeValue v = lube->nothing();
    *has = lube->object_get(vm, obj, key, &v);
    if (!*has) return lube->nothing();
    if (lube->kind(v) != kind) {
        *has = -1;
        return lube->error(vm, "App(): %s is %s, not %s", key, what, lube->a_type(v));
    }
    return v;
}

/* Every key of obj is one of names: NULL, or the error. */
static int unknown_key(LubeVM *vm, LubeValue obj, const char *const *names, const char *where, LubeValue *err) {
    LubeValue keys = lube->object_keys(vm, obj);
    size_t n = lube->list_len(keys);
    for (size_t i = 0; i < n; i++) {
        const char *k = lube->str(lube->list_get(keys, i), NULL);
        int known = 0;
        for (int j = 0; names[j] && !known; j++) known = !strcmp(names[j], k);
        if (!known) {
            *err = lube->error(vm, "%s has no option '%s'", where, k);
            lube->release(keys);
            return 1;
        }
    }
    lube->release(keys);
    return 0;
}

static LubeValue read_policy(LubeVM *vm, App *a, LubeValue pol) {
    static const char *names[32];
    if (!names[0]) {
        int n = 0;
        for (size_t i = 0; i < sizeof LIMITS / sizeof LIMITS[0]; i++) names[n++] = LIMITS[i].name;
        names[n++] = "csp";
        names[n++] = "inline_styles";
        names[n++] = "secure_cookies";
        names[n++] = "origins";
        names[n++] = "hosts";
        names[n] = NULL;
    }
    LubeValue err = lube->nothing();
    if (unknown_key(vm, pol, names, "App()'s policy", &err)) return err;
    for (size_t i = 0; i < sizeof LIMITS / sizeof LIMITS[0]; i++) {
        LubeValue v = lube->nothing();
        if (!lube->object_get(vm, pol, LIMITS[i].name, &v)) continue;
        double x = lube->number(v);
        if (lube->kind(v) != LUBE_NUMBER || !(x > 0) || (LIMITS[i].whole && (x != floor(x) || x > 1e15))) {
            char num[64];
            lube->format_number(x, num, sizeof num);
            return lube->error(vm, "App(): policy %s is a%s Number above 0, not %s", LIMITS[i].name,
                               LIMITS[i].whole ? " whole" : "", lube->kind(v) == LUBE_NUMBER ? num : lube->a_type(v));
        }
        *(double *)((char *)&a->pol + LIMITS[i].at) = x;
    }
    LubeValue v = lube->nothing();
    if (lube->object_get(vm, pol, "csp", &v)) {
        size_t n;
        const char *s = lube->str(v, &n);
        if (!s) return lube->error(vm, "App(): policy csp is text (a Content-Security-Policy), not %s", lube->a_type(v));
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return lube->error(vm, "App(): policy csp has a control character");
        a->pol.csp = copy_str(s, n);
    }
    const char *flags[] = {"inline_styles", "secure_cookies"};
    int *at[] = {&a->pol.inline_styles, &a->pol.secure_cookies};
    for (int i = 0; i < 2; i++)
        if (lube->object_get(vm, pol, flags[i], &v)) {
            if (lube->kind(v) != LUBE_BOOL) return lube->error(vm, "App(): policy %s is a Bool, not %s", flags[i], lube->a_type(v));
            *at[i] = lube->truthy(v);
        }
    if (lube->object_get(vm, pol, "origins", &v)) {
        if (lube->kind(v) != LUBE_LIST) return lube->error(vm, "App(): policy origins is a List of sites, not %s", lube->a_type(v));
        size_t n = lube->list_len(v);
        for (size_t i = 0; i < n; i++) {
            size_t len;
            const char *s = lube->str(lube->list_get(v, i), &len);
            if (!s || (strncmp(s, "http://", 7) && strncmp(s, "https://", 8)) || strchr(s + 8, '/') || len > 255)
                return lube->error(vm, "App(): policy origins has sites, as \"https://example.com\" (no path)");
            a->pol.origins = (char **)realloc(a->pol.origins, sizeof(char *) * (n ? n : 1));
            a->pol.origins[a->pol.norigins++] = copy_str(s, len);
        }
    }
    if (lube->object_get(vm, pol, "hosts", &v)) {
        if (lube->kind(v) != LUBE_LIST) return lube->error(vm, "App(): policy hosts is a List of names, not %s", lube->a_type(v));
        size_t n = lube->list_len(v);
        for (size_t i = 0; i < n; i++) {
            size_t len;
            const char *s = lube->str(lube->list_get(v, i), &len);
            int ok = s && len && len < 254;
            for (size_t k = 0; ok && k < len; k++) ok = isalnum_c(s[k]) || s[k] == '.' || s[k] == '-' || (len == 1 && s[k] == '*');
            if (!ok) return lube->error(vm, "App(): policy hosts has names, as \"example.com\" (no port), or \"*\" for any");
            a->pol.hosts = (char **)realloc(a->pol.hosts, sizeof(char *) * n);
            a->pol.hosts[a->pol.nhosts] = copy_str(s, len);
            for (char *c = a->pol.hosts[a->pol.nhosts]; *c; c++) *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
            a->pol.nhosts++;
        }
    }
    return lube->nothing();
}

FN(f_app) {
    void *data;
    static const char *const names[] = {"host", "dev", "trust_proxy", "policy", NULL};
    if (argc && lube->kind(argv[0]) != LUBE_OBJECT)
        return lube->error(vm, "App() takes its options as an object, not %s", lube->a_type(argv[0]));
    LubeValue opts = argc ? argv[0] : lube->nothing();
    LubeValue err = lube->nothing();
    if (argc && unknown_key(vm, opts, names, "App()", &err)) return err;
    char entropy[3];
    if (!random_hex(entropy, 1)) return lube->error(vm, "App(): the system has no random numbers to make secrets with");  /* LCOV_EXCL_LINE */
    LubeValue app = lube->new_foreign(vm, AppType, &data);
    App *a = (App *)data;
    a->pol = DEFAULT_POLICY;
    a->listen = BAD_SOCK;
    strcpy(a->host, "127.0.0.1");
    int has;
    LubeValue v = lube->nothing();
    if (argc) {
        v = option(vm, opts, "host", LUBE_STR, "text (where to listen: \"0.0.0.0\" is every network)", &has);
        if (has < 0) goto failed;
        if (has) {
            size_t n;
            const char *s = lube->str(v, &n);
            if (!n || n >= sizeof a->host) {
                v = lube->error(vm, "App(): host is an address or a name, as \"0.0.0.0\"");
                goto failed;
            }
            memcpy(a->host, s, n + 1);
        }
        v = option(vm, opts, "dev", LUBE_BOOL, "a Bool", &has);
        if (has < 0) goto failed;
        a->dev = has && lube->truthy(v);
        v = option(vm, opts, "trust_proxy", LUBE_BOOL, "a Bool", &has);
        if (has < 0) goto failed;
        a->trust_proxy = has && lube->truthy(v);
        v = option(vm, opts, "policy", LUBE_OBJECT, "an object (the rules to loosen, by name)", &has);
        if (has < 0) goto failed;
        if (has) {
            v = read_policy(vm, a, v);
            if (lube->failed(v)) goto failed;
        }
    }
    buf_fmt(&a->sec, "Content-Security-Policy: %s\r\n", a->pol.csp ? a->pol.csp : DEFAULT_CSP);
    if (a->pol.inline_styles && !a->pol.csp) {
        a->sec.n = 0;
        buf_str(&a->sec, "Content-Security-Policy: ");
        const char *at = strstr(DEFAULT_CSP, "style-src 'self'") + strlen("style-src 'self'");
        buf_add(&a->sec, DEFAULT_CSP, (size_t)(at - DEFAULT_CSP));
        buf_str(&a->sec, " 'unsafe-inline'");
        buf_str(&a->sec, at);
        buf_str(&a->sec, "\r\n");
    }
    buf_str(&a->sec, "X-Content-Type-Options: nosniff\r\n"
                     "Referrer-Policy: same-origin\r\n"
                     "Cross-Origin-Opener-Policy: same-origin\r\n"
                     "Cross-Origin-Resource-Policy: same-origin\r\n"
                     "X-Frame-Options: DENY\r\n");
    return app;
failed:
    lube->release(app);
    return v;
}

static App *app_of(LubeValue v) { return (App *)lube->foreign(v, AppType); }

/* ---- routes ---- */

/* A pattern's segments: 0, or an error raised (into *err). */
static int pattern_segs(LubeVM *vm, const char *fn, LubeValue pat, Route *r, LubeValue *err) {
    const char *s = lube->str(pat, NULL);
    if (!s || s[0] != '/') {
        *err = s ? lube->error(vm, "%s: a path starts with /, as \"/users/:id\": not \"%s\"", fn, s)
                 : lube->error(vm, "%s needs a path first, as \"/users/:id\", not %s", fn, lube->a_type(pat));
        return 1;
    }
    for (const char *p = s + 1; *p;) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        char *seg = copy_str(start, n);
        const char *why = NULL;
        if (!n && *p) why = "has an empty part (\"//\")";
        else if (!r->nsegs && !strcmp(seg, RESERVED)) why = "is the page script's own (/_lube)";
        else if (!strcmp(seg, ".") || !strcmp(seg, "..")) why = "has . or ..";
        else if ((seg[0] == ':' || seg[0] == '*') && !good_name(seg + 1, 1)) why = "names a part with no name";
        else if (seg[0] == '*' && *p) why = "has *name before its end";
        else if (strpbrk(seg, "?#%\\ ")) why = "has ?, #, %, \\ or a space";
        if (why) {
            free(seg);
            *err = lube->error(vm, "%s: the path \"%s\" %s", fn, s, why);
            return 1;
        }
        r->segs = (char **)realloc(r->segs, sizeof(char *) * (size_t)(r->nsegs + 1));
        r->segs[r->nsegs++] = seg;
        if (*p) p++;
        if (!*p && p[-1] == '/' && n) {  /* a last '/': an empty last part */
            r->segs = (char **)realloc(r->segs, sizeof(char *) * (size_t)(r->nsegs + 1));
            r->segs[r->nsegs++] = copy_str("", 0);
        }
    }
    return 0;
}

static LubeValue add_route(LubeVM *vm, const char *fn, App *a, Route *r) {
    if (a->busy) {
        route_free(r);
        return lube->error(vm, "%s: an app's routes are set before it serves, not while it does", fn);
    }
    a->routes = (Route *)realloc(a->routes, sizeof(Route) * (size_t)(a->nroutes + 1));
    a->routes[a->nroutes++] = *r;
    return lube->nothing();
}

static LubeValue fn_route(LubeVM *vm, int methods, const char *fn, int argc, const LubeValue *argv) {
    App *a = app_of(argv[0]);
    Route r;
    memset(&r, 0, sizeof r);
    r.kind = R_FN;
    r.methods = methods;
    LubeValue err = lube->nothing();
    if (pattern_segs(vm, fn, argv[1], &r, &err)) {
        route_free(&r);  /* (r.fn is nothing yet) */
        return err;
    }
    if (!is_function(argv[2])) {
        route_free(&r);
        return lube->error(vm, "%s needs a function, fn(req), after its path, not %s", fn, lube->a_type(argv[2]));
    }
    r.fn = lube->retain(argv[2]);
    return add_route(vm, fn, a, &r);
}

FN(m_get) { return fn_route(vm, M_GET | M_HEAD, "get()", argc, argv); }
FN(m_post) { return fn_route(vm, M_POST, "post()", argc, argv); }
FN(m_put) { return fn_route(vm, M_PUT, "put()", argc, argv); }
FN(m_patch) { return fn_route(vm, M_PATCH, "patch()", argc, argv); }
FN(m_delete) { return fn_route(vm, M_DELETE, "delete()", argc, argv); }

/* A page's stylesheets or scripts: paths on this site. */
static int site_paths(LubeVM *vm, LubeValue opts, const char *key, char ***out, int *n, LubeValue *err) {
    LubeValue v = lube->nothing();
    if (!lube->object_get(vm, opts, key, &v)) return 0;
    size_t len = lube->kind(v) == LUBE_LIST ? lube->list_len(v) : 0;
    for (size_t i = 0; i < len || lube->kind(v) != LUBE_LIST; i++) {
        const char *s = lube->kind(v) == LUBE_LIST ? lube->str(lube->list_get(v, i), NULL) : NULL;
        if (!s || s[0] != '/' || s[1] == '/' || strpbrk(s, "\"<>\\ ")) {
            *err = lube->error(vm, "page(): %s is a List of paths on this site, as \"/assets/app.%s\"", key,
                               key[1] == 't' ? "css" : "js");
            return 1;
        }
        *out = (char **)realloc(*out, sizeof(char *) * (size_t)(*n + 1));
        (*out)[(*n)++] = copy_str(s, strlen(s));
    }
    return 0;
}

/* app.page(path, fn(req), options?): a page: what fn gives (a component, or a view) as HTML, live. */
FN(m_page) {
    App *a = app_of(argv[0]);
    Route r;
    memset(&r, 0, sizeof r);
    r.kind = R_PAGE;
    r.methods = M_GET | M_HEAD;
    LubeValue err = lube->nothing();
    r.fn = lube->nothing();
    if (pattern_segs(vm, "page()", argv[1], &r, &err)) goto failed;
    if (!is_function(argv[2])) {
        err = lube->error(vm, "page() needs a function, fn(req), after its path, not %s", lube->a_type(argv[2]));
        goto failed;
    }
    if (argc > 3) {
        static const char *const names[] = {"title", "styles", "scripts", NULL};
        LubeValue t = lube->nothing();
        if (lube->kind(argv[3]) != LUBE_OBJECT) {
            err = lube->error(vm, "page() takes its options as an object, not %s", lube->a_type(argv[3]));
            goto failed;
        }
        if (unknown_key(vm, argv[3], names, "page()", &err)) goto failed;
        if (lube->object_get(vm, argv[3], "title", &t)) {
            size_t n;
            const char *s = lube->str(t, &n);
            if (!s) {
                err = lube->error(vm, "page(): title is text, not %s", lube->a_type(t));
                goto failed;
            }
            r.title = copy_str(s, n);
        }
        if (site_paths(vm, argv[3], "styles", &r.styles, &r.nstyles, &err) ||
            site_paths(vm, argv[3], "scripts", &r.scripts, &r.nscripts, &err))
            goto failed;
    }
    r.fn = lube->retain(argv[2]);
    return add_route(vm, "page()", a, &r);
failed:
    route_free(&r);
    return err;
}

/* app.static(prefix, folder): the files in folder, at prefix/... */
FN(m_static) {
    App *a = app_of(argv[0]);
    Route r;
    memset(&r, 0, sizeof r);
    r.kind = R_STATIC;
    r.methods = M_GET | M_HEAD;
    LubeValue err = lube->nothing();
    if (pattern_segs(vm, "static()", argv[1], &r, &err)) goto failed;
    for (int i = 0; i < r.nsegs; i++)
        if (r.segs[i][0] == ':' || r.segs[i][0] == '*') {
            err = lube->error(vm, "static(): its path is a plain one, with no :name or *name");
            goto failed;
        }
    if (r.nsegs && !r.segs[r.nsegs - 1][0]) free(r.segs[--r.nsegs]);  /* "/assets/" is "/assets" */
    const char *dir = lube->str(argv[2], NULL);
    if (!dir) {
        err = lube->error(vm, "static() needs a folder (text) after its path, not %s", lube->a_type(argv[2]));
        goto failed;
    }
#ifdef _WIN32
    wchar_t wdir[MAX_PATH], full[MAX_PATH];
    char real[MAX_PATH * 3];
    DWORD attrs;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dir, -1, wdir, MAX_PATH) || !_wfullpath(full, wdir, MAX_PATH) ||
        (attrs = GetFileAttributesW(full)) == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        !WideCharToMultiByte(CP_UTF8, 0, full, -1, real, sizeof real, NULL, NULL)) {
#else
    char real[PATH_MAX];
    struct stat st;
    if (!realpath(dir, real) || stat(real, &st) || !S_ISDIR(st.st_mode)) {
#endif
        err = lube->error(vm, "static(): there is no folder \"%s\"", dir);
        goto failed;
    }
    r.dir = copy_str(real, strlen(real));
    r.fn = lube->nothing();
    return add_route(vm, "static()", a, &r);
failed:
    r.kind = R_STATIC;
    route_free(&r);
    return err;
}

/* ======================================================================
 * Listening
 * ==================================================================== */

static void set_nonblocking(Sock s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void socket_options(Sock s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

static LubeValue app_listen(LubeVM *vm, App *a, const char *fn, int argc, const LubeValue *argv) {
    if (a->started) return lube->error(vm, "%s: the app is serving already, at port %d", fn, a->port);
    double port = argc > 1 ? lube->number(argv[1]) : 8080;
    if (argc > 1 && (lube->kind(argv[1]) != LUBE_NUMBER || port != floor(port) || port < 0 || port > 65535))
    {
        char num[64];
        lube->format_number(port, num, sizeof num);
        return lube->error(vm, "%s takes a port, a whole Number from 0 to 65535 (0: any free one), not %s", fn,
                           lube->kind(argv[1]) == LUBE_NUMBER ? num : lube->a_type(argv[1]));
    }
    net_start();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    char service[8];
    snprintf(service, sizeof service, "%d", (int)port);
    if (getaddrinfo(a->host, service, &hints, &res) || !res)
        return lube->error(vm, "%s: there is no address \"%s\" to listen on", fn, a->host);
    Sock s = socket(res->ai_family, SOCK_STREAM, 0);
    int on = 1;
    if (s != BAD_SOCK) {
#ifndef _WIN32
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);  /* (a restart can take the port again) */
#else
        (void)on;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on);
#endif
    }
    if (s == BAD_SOCK || bind(s, res->ai_addr, (int)res->ai_addrlen) || listen(s, 128)) {
        if (s != BAD_SOCK) close_sock(s);
        freeaddrinfo(res);
        return lube->error(vm, "%s: cannot listen on %s port %d (is it in use?)", fn, a->host, (int)port);
    }
    freeaddrinfo(res);
    struct sockaddr_storage at;
    socklen_t len = sizeof at;
    getsockname(s, (struct sockaddr *)&at, &len);
    a->port = ntohs(at.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&at)->sin6_port : ((struct sockaddr_in *)&at)->sin_port);
    set_nonblocking(s);
    a->listen = s;
    a->started = 1;
    return lube->number_value(a->port);
}

/* app.start(port?): listens (port 0: any free one), and gives the port. */
FN(m_start) { return app_listen(vm, app_of(argv[0]), "start()", argc, argv); }

FN(m_port) {
    App *a = app_of(argv[0]);
    return lube->number_value(a->started ? a->port : 0);
}

FN(m_pages) { return lube->number_value(app_of(argv[0])->npages); }

/* ======================================================================
 * Answering
 * ==================================================================== */

static void log_line(const char *format, ...) {
    va_list ap;
    fflush(stdout);
    va_start(ap, format);
    fputs("web: ", stderr);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

/* An answer's head, into c->out, for a body of n bytes. extra: more header lines ("Name: value\r\n" each), or
 * NULL. */
static void respond_head(App *a, Conn *c, int status, const char *type, long long n, const char *extra) {
    buf_fmt(&c->out, "HTTP/1.1 %d %s\r\n", status, status_text(status));
    buf_add(&c->out, a->sec.p, a->sec.n);
    if (type) buf_fmt(&c->out, "Content-Type: %s\r\n", type);
    if (status != 204 && status != 304 && status != 101) buf_fmt(&c->out, "Content-Length: %lld\r\n", n);
    if (status != 101) buf_str(&c->out, c->keep ? (c->req.http10 ? "Connection: keep-alive\r\n" : "") : "Connection: close\r\n");
    if (extra) buf_str(&c->out, extra);
    buf_str(&c->out, "\r\n");
}

/* A whole answer (its body left out for HEAD). */
static void respond(App *a, Conn *c, int status, const char *type, const char *body, size_t n, const char *extra) {
    respond_head(a, c, status, type, (long long)n, extra);
    if (c->req.method != M_HEAD) buf_add(&c->out, body, n);
}

/* An answer that is its status alone ("404 Not Found"), and the end of the connection if it is an error that
 * leaves the request unread. */
static void respond_status(App *a, Conn *c, int status, const char *extra) {
    char body[96];
    int n = snprintf(body, sizeof body, "%d %s\n", status, status_text(status));
    respond(a, c, status, "text/plain; charset=utf-8", body, (size_t)n, extra);
}

static void fail_request(App *a, Conn *c, int status) {
    c->keep = 0;
    respond_status(a, c, status, NULL);
    c->state = C_CLOSING;
}

/* ---- the request, for Lube ---- */

static void set_str(LubeVM *vm, LubeValue obj, const char *key, const char *s, size_t n) {
    lube->object_set(vm, obj, key, lube->new_str(vm, s, n));
}

/* k=v&k=v into obj: 0 if an escape is not well formed. */
static int parse_pairs(LubeVM *vm, const char *s, size_t n, LubeValue obj) {
    Buf k = {0}, v = {0};
    int ok = 1;
    for (size_t i = 0; i < n && ok;) {
        size_t start = i;
        while (i < n && s[i] != '&') i++;
        const char *eq = (const char *)memchr(s + start, '=', i - start);
        size_t klen = eq ? (size_t)(eq - (s + start)) : i - start;
        if (i > start) {
            ok = url_decode(s + start, klen, 1, &k) &&
                 url_decode(eq ? eq + 1 : "", eq ? (size_t)(s + i - eq - 1) : 0, 1, &v);
            if (ok && k.n) set_str(vm, obj, k.p, v.p ? v.p : "", v.n);
        }
        i++;
    }
    buf_free(&k);
    buf_free(&v);
    return ok;
}

/* The address the request came from: the connection's, or with trust_proxy, the last the proxy saw
 * (X-Forwarded-For's last). */
static void client_ip(App *a, Conn *c, char *out, size_t size) {
    const char *xff = a->trust_proxy ? req_header(&c->req, "x-forwarded-for") : NULL;
    if (!xff) {
        snprintf(out, size, "%s", c->ip);
        return;
    }
    const char *last = strrchr(xff, ',');
    last = last ? last + 1 : xff;
    while (*last == ' ') last++;
    snprintf(out, size, "%s", last);
}

/* Whether the request came over HTTPS: only a trusted proxy can say. */
static int is_https(App *a, Conn *c) {
    const char *proto = a->trust_proxy ? req_header(&c->req, "x-forwarded-proto") : NULL;
    return proto && !strcmp(proto, "https");
}

/* The session cookie's value, if it has a well-formed one (32 hex digits). */
static int session_cookie(const Req *r, char out[33]) {
    const char *all = req_header(r, "cookie");
    for (const char *p = all; p && *p;) {
        while (*p == ' ' || *p == ';') p++;
        const char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 13 + 32 && !strncmp(p, "lube_session=", 13)) {
            int ok = 1;
            for (int i = 0; i < 32; i++) ok &= hex_digit(p[13 + i]) >= 0 && !(p[13 + i] >= 'A' && p[13 + i] <= 'F');
            if (ok) {
                memcpy(out, p + 13, 32);
                out[32] = 0;
                return 1;
            }
        }
        p += n;
    }
    return 0;
}

static LubeValue request_value(LubeVM *vm, App *a, Conn *c, LubeValue params, const char *body, size_t n) {
    Req *r = &c->req;
    LubeValue req = lube->new_object(vm);
    set_str(vm, req, "method", r->method_name, strlen(r->method_name));
    set_str(vm, req, "path", r->path, strlen(r->path));
    lube->object_set(vm, req, "params", params);
    LubeValue query = lube->new_object(vm);
    parse_pairs(vm, r->query, strlen(r->query), query);  /* (checked when the request came) */
    lube->object_set(vm, req, "query", query);
    LubeValue headers = lube->new_object(vm);
    for (int i = 0; i < r->nh; i++) {
        LubeValue had;
        if (lube->object_get(vm, headers, r->h[i].name, &had)) {  /* more than one: joined */
            size_t hn;
            const char *hs = lube->str(had, &hn);
            Buf j = {0};
            buf_add(&j, hs, hn);
            buf_str(&j, strcmp(r->h[i].name, "cookie") ? ", " : "; ");
            buf_str(&j, r->h[i].value);
            set_str(vm, headers, r->h[i].name, j.p, j.n);
            buf_free(&j);
        } else {
            set_str(vm, headers, r->h[i].name, r->h[i].value, strlen(r->h[i].value));
        }
    }
    lube->object_set(vm, req, "headers", headers);
    lube->object_set(vm, req, "body", lube->new_bytes(vm, body, n));
    int text = utf8_ok(body, n);
    set_str(vm, req, "text", text ? body : "", text ? n : 0);
    LubeValue form = lube->new_object(vm);
    const char *type = req_header(r, "content-type");
    if (type && !strncmp(type, "application/x-www-form-urlencoded", 33) && text) parse_pairs(vm, body, n, form);
    lube->object_set(vm, req, "form", form);
    char ip[64];
    client_ip(a, c, ip, sizeof ip);
    set_str(vm, req, "ip", ip, strlen(ip));
    return req;
}

/* ---- matching ---- */

/* Whether route r takes the request's path; its :names and *name go into params (if not NULL). */
static int route_match(const Route *r, const Req *req, LubeVM *vm, LubeValue params) {
    if (r->kind == R_STATIC) {
        if (req->nsegs < r->nsegs + 1) return 0;
        for (int i = 0; i < r->nsegs; i++)
            if (strcmp(r->segs[i], req->segs[i])) return 0;
        return 1;
    }
    int i = 0;
    for (; i < r->nsegs; i++) {
        const char *s = r->segs[i];
        if (s[0] == '*') {
            if (vm) {
                Buf rest = {0};
                for (int k = i; k < req->nsegs; k++) {
                    if (k > i) buf_add(&rest, "/", 1);
                    buf_str(&rest, req->segs[k]);
                }
                set_str(vm, params, s + 1, rest.p ? rest.p : "", rest.n);
                buf_free(&rest);
            }
            return 1;
        }
        if (i >= req->nsegs) return 0;
        if (s[0] == ':') {
            if (!req->segs[i][0]) return 0;
            if (vm) set_str(vm, params, s + 1, req->segs[i], strlen(req->segs[i]));
        } else if (strcmp(s, req->segs[i])) {
            return 0;
        }
    }
    return i == req->nsegs;
}

/* ---- what a route's function gives ---- */

static const char *const OWN_HEADERS[] = {"content-length", "transfer-encoding", "connection", "content-security-policy",
                                          "x-content-type-options", "referrer-policy", "cross-origin-opener-policy",
                                          "cross-origin-resource-policy", "x-frame-options", NULL};

/* Whether every key of obj is one of names: NULL, or the first that is not (copied into out). */
static const char *unknown_name(LubeVM *vm, LubeValue obj, const char *const *names, char *out, size_t size) {
    LubeValue keys = lube->object_keys(vm, obj);
    const char *bad = NULL;
    for (size_t i = 0; i < lube->list_len(keys) && !bad; i++) {
        const char *k = lube->str(lube->list_get(keys, i), NULL);
        int known = 0;
        for (int j = 0; names[j] && !known; j++) known = !strcmp(names[j], k);
        if (!known) bad = (snprintf(out, size, "%s", k), out);
    }
    lube->release(keys);
    return bad;
}

enum { OK, WHY, RAISED };  /* how an answer went: fine; wrong (why says how); Lube code failed (its error raised) */

/* A body: text, Bytes or a view. Into out, with its type. */
static int body_of(LubeVM *vm, App *a, LubeValue v, Buf *out, const char **type, char *why, size_t size) {
    size_t n;
    const char *s;
    const unsigned char *b;
    if ((s = lube->str(v, &n)) != NULL) {
        buf_add(out, s, n);
        *type = "text/plain; charset=utf-8";
    } else if ((b = lube->bytes(v, &n)) != NULL) {
        buf_add(out, b, n);
        *type = "application/octet-stream";
    } else if (is_component(v)) {
        snprintf(why, size, "a component is a page: serve it with app.page()");
        return WHY;
    } else {
        Render r;
        memset(&r, 0, sizeof r);
        r.vm = vm;
        r.inline_styles = a->pol.inline_styles;
        buf_str(&r.out, "<!doctype html>\n");
        render(&r, v, 0);
        render_drop_handlers(r.handlers, r.nh);
        if (r.failed) {
            buf_free(&r.out);
            snprintf(why, size, "%s", r.why);
            return r.failed == 2 ? RAISED : WHY;
        }
        buf_add(out, r.out.p, r.out.n);
        buf_free(&r.out);
        *type = "text/html; charset=utf-8";
    }
    return OK;
}

/* A header an answer gives: NULL, or why it may not. */
static const char *header_wrong(const char *name, const char *val) {
    char low[64];
    size_t k = 0;
    int ok = *name && strlen(name) < sizeof low;
    for (const char *p = name; ok && *p; p++) ok = tchar(*p), low[k++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    low[k] = 0;
    if (!ok) return "has a name that is not a header's";
    if (!strcmp(low, "content-type")) return "is the answer's type: give it as type:";
    if (in_list(OWN_HEADERS, low)) return "is the server's own (its policy sets it)";
    if (!val) return "has a value that is not text";
    for (const char *p = val; *p; p++)
        if (((unsigned char)*p < 0x20 && *p != '\t') || *p == 0x7f) return "has a line break or a control character in it";
    return NULL;
}

/* What fn(req) gave, as the answer: text, Bytes, a view, nothing (204), or {status:, body:, type:, headers:}. */
static int answer(LubeVM *vm, App *a, Conn *c, LubeValue v, char *why, size_t size) {
    Buf body = {0}, extra = {0};
    const char *type = NULL;
    int status = 200, how = OK;
    char typebuf[256], bad[64];
    if (lube->kind(v) == LUBE_NOTHING) {
        status = 204;
    } else if (lube->kind(v) == LUBE_OBJECT) {
        static const char *const names[] = {"status", "body", "type", "headers", NULL};
        LubeValue x = lube->nothing();
        if (unknown_name(vm, v, names, bad, sizeof bad)) {
            snprintf(why, size, "an answer has no '%s' (it has status, body, type and headers)", bad);
            return WHY;
        }
        if (lube->object_get(vm, v, "status", &x)) {
            double s = lube->number(x);
            if (lube->kind(x) != LUBE_NUMBER || s != floor(s) || s < 200 || s > 599) {
                snprintf(why, size, "an answer's status is a whole Number from 200 to 599");
                return WHY;
            }
            status = (int)s;
        }
        if (lube->object_get(vm, v, "body", &x) && (how = body_of(vm, a, x, &body, &type, why, size)) != OK) goto done;
        if (lube->object_get(vm, v, "type", &x)) {
            const char *t = lube->str(x, NULL);
            int ok = t && strlen(t) < sizeof typebuf;
            for (const char *p = t ? t : ""; ok && *p; p++) ok = (unsigned char)*p >= 0x20 && *p != 0x7f;
            if (!ok) {
                snprintf(why, size, "an answer's type is text, as \"application/json\"");
                how = WHY;
                goto done;
            }
            strcpy(typebuf, t);
            type = typebuf;
        }
        if (lube->object_get(vm, v, "headers", &x)) {
            if (lube->kind(x) != LUBE_OBJECT) {
                snprintf(why, size, "an answer's headers are an object, of names and text");
                how = WHY;
                goto done;
            }
            LubeValue keys = lube->object_keys(vm, x);
            for (size_t i = 0; how == OK && i < lube->list_len(keys); i++) {
                const char *name = lube->str(lube->list_get(keys, i), NULL);
                LubeValue hv = lube->nothing();
                lube->object_get(vm, x, name, &hv);
                const char *val = lube->str(hv, NULL), *wrong = header_wrong(name, val);
                if (wrong) {
                    snprintf(why, size, "an answer's header '%s' %s", name, wrong);
                    how = WHY;
                } else {
                    buf_fmt(&extra, "%s: %s\r\n", name, val);
                }
            }
            lube->release(keys);
            if (how != OK) goto done;
        }
        if (!type && body.n) type = "application/octet-stream";
    } else if ((how = body_of(vm, a, v, &body, &type, why, size)) != OK) {
        goto done;
    }
    if (status == 204 || status == 304) body.n = 0, type = NULL;
    buf_str(&extra, "Cache-Control: no-store\r\n");
    buf_add(&extra, "", 1);
    respond(a, c, status, type, body.p ? body.p : "", body.n, extra.p);
done:
    buf_free(&body);
    buf_free(&extra);
    return how;
}

/* Something went wrong answering: it is logged, and the answer is a 500 (with what went wrong, if dev is on). */
static void server_error_text(App *a, Conn *c, const char *text) {
    log_line("%s %s: %s", c->req.method_name, c->req.path, text);
    if (!a->dev) {
        respond_status(a, c, 500, NULL);
        return;
    }
    char body[1100];
    int n = snprintf(body, sizeof body, "500 Internal Server Error\n%s\n", text);
    respond(a, c, 500, "text/plain; charset=utf-8", body, (size_t)n, NULL);
}

/* An error in Lube code, caught: likewise. */
static void server_error(LubeVM *vm, App *a, Conn *c) {
    char text[1024];
    lube->take_error(vm, text, sizeof text);
    server_error_text(a, c, text);
}

/* ======================================================================
 * Static files
 * ==================================================================== */

static const char *const TYPES[] = {
    "html", "text/html; charset=utf-8", "htm", "text/html; charset=utf-8", "css", "text/css; charset=utf-8",
    "js", "text/javascript; charset=utf-8", "mjs", "text/javascript; charset=utf-8", "json", "application/json",
    "txt", "text/plain; charset=utf-8", "md", "text/markdown; charset=utf-8", "csv", "text/csv; charset=utf-8",
    "xml", "application/xml", "svg", "image/svg+xml", "png", "image/png", "jpg", "image/jpeg", "jpeg", "image/jpeg",
    "gif", "image/gif", "webp", "image/webp", "avif", "image/avif", "ico", "image/x-icon", "woff", "font/woff",
    "woff2", "font/woff2", "ttf", "font/ttf", "otf", "font/otf", "wasm", "application/wasm", "pdf", "application/pdf",
    "mp3", "audio/mpeg", "ogg", "audio/ogg", "wav", "audio/wav", "mp4", "video/mp4", "webm", "video/webm",
    "zip", "application/zip", NULL};

static const char *file_type(const char *name) {
    const char *dot = strrchr(name, '.');
    char ext[8];
    size_t n = dot ? strlen(dot + 1) : 0;
    if (!dot || n >= sizeof ext) return "application/octet-stream";
    for (size_t i = 0; i <= n; i++) ext[i] = (char)(dot[1 + i] >= 'A' && dot[1 + i] <= 'Z' ? dot[1 + i] + 32 : dot[1 + i]);
    for (int i = 0; TYPES[i]; i += 2)
        if (!strcmp(TYPES[i], ext)) return TYPES[i + 1];
    return "application/octet-stream";
}

/* The file at the request's path under route r's folder, opened: NULL if there is none it may give. Each part of
 * the path must be a plain name (none starting with '.', none with ':' or '\\'), and each part a real folder or,
 * the last, a real file: not a symbolic link. */
static FILE *static_file(const Route *r, const Req *req, long long *size) {
    Buf path = {0};
    buf_str(&path, r->dir);
    FILE *f = NULL;
    for (int i = r->nsegs; i < req->nsegs; i++) {
        const char *s = req->segs[i];
        if (!s[0] || s[0] == '.' || strpbrk(s, "/\\:")) goto done;
        buf_add(&path, "/", 1);
        buf_str(&path, s);
        buf_add(&path, "", 1);
        path.n--;
        int last = i == req->nsegs - 1;
#ifdef _WIN32
        wchar_t w[MAX_PATH];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.p, -1, w, MAX_PATH)) goto done;
        DWORD attrs = GetFileAttributesW(w);
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ||
            ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) == last)
            goto done;
        if (last) {
            f = _wfopen(w, L"rb");
            if (f) {
                _fseeki64(f, 0, SEEK_END);
                *size = _ftelli64(f);
                _fseeki64(f, 0, SEEK_SET);
            }
        }
#else
        struct stat st;
        if (lstat(path.p, &st) || S_ISLNK(st.st_mode) || (last ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode))) goto done;
        if (last) {
            int fd = open(path.p, O_RDONLY | O_NOFOLLOW);
            if (fd < 0) goto done;  /* LCOV_EXCL_LINE: it was there a moment ago */
            if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {  /* LCOV_EXCL_START: likewise */
                close(fd);
                goto done;
            }  /* LCOV_EXCL_STOP */
            f = fdopen(fd, "rb");
            *size = (long long)st.st_size;
        }
#endif
    }
done:
    buf_free(&path);
    return f;
}

/* The file, if the folder has it (0 if not). */
static int serve_static(App *a, Conn *c, const Route *r) {
    long long size = 0;
    FILE *f = static_file(r, &c->req, &size);
    if (!f) return 0;
    int head = c->req.method == M_HEAD;
    respond_head(a, c, 200, file_type(c->req.segs[c->req.nsegs - 1]), size, "Cache-Control: no-cache\r\n");
    if (head || !size) {
        fclose(f);
        return 1;
    }
    c->file = f;
    c->file_left = size;
    return 1;
}

/* More of the file being sent, when what went before has gone. */
static void pump(Conn *c) {
    if (!c->file || c->out.n >= 65536) return;
    char chunk[65536];
    size_t want = c->file_left < (long long)sizeof chunk ? (size_t)c->file_left : sizeof chunk;
    size_t got = fread(chunk, 1, want, c->file);
    buf_add(&c->out, chunk, got);
    c->file_left -= (long long)got;
    if (!got || !c->file_left) {  /* (a file that shrank while it was sent: the connection ends short) */
        if (c->file_left) c->keep = 0, c->state = C_CLOSING;  /* LCOV_EXCL_LINE */
        fclose(c->file);
        c->file = NULL;
    }
}

/* ======================================================================
 * Pages
 * ==================================================================== */

/* The page script: it sends the page's events to its handlers, and shows the view it gets back. */
static const char CLIENT_JS[] =
    "(function(){\n"
    "var root=document.getElementById('lube');\n"
    "if(!root||!root.getAttribute('data-page'))return;\n"
    "var version=+root.getAttribute('data-version');\n"
    "var ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/_lube/ws?page='+root.getAttribute('data-page'));\n"
    "var own=['focus','blur','mouseenter','mouseleave'];\n"
    "['click','dblclick','input','change','submit','keydown','keyup','focus','blur','mouseenter','mouseleave'].forEach(function(e){\n"
    "  var alone=own.indexOf(e)>=0;\n"
    "  root.addEventListener(e,function(ev){\n"
    "    var t=ev.target;\n"
    "    if(!alone)t=t.closest?t.closest('[data-on-'+e+']'):null;\n"
    "    if(!t||!t.getAttribute||!t.getAttribute('data-on-'+e)||!root.contains(t))return;\n"
    "    var m={v:version,h:t.getAttribute('data-on-'+e)};\n"
    "    if(e==='submit'){ev.preventDefault();m.a={};new FormData(t).forEach(function(v,k){if(typeof v==='string')m.a[k]=v;});}\n"
    "    else if(e==='input'||e==='change')m.a=t.type==='checkbox'?t.checked:t.value;\n"
    "    else if(e==='keydown'||e==='keyup')m.a=ev.key;\n"
    "    else if(e==='click'&&t.tagName==='A')ev.preventDefault();\n"
    "    if(ws.readyState===1)ws.send(JSON.stringify(m));\n"
    "  },alone);\n"
    "});\n"
    "ws.onmessage=function(m){var d=JSON.parse(m.data);\n"
    "  if(typeof d.html==='string'){version=d.v;root.innerHTML=d.html;}\n"
    "  else if(d.error)console.error('lube:',d.error);};\n"
    "ws.onclose=function(){root.setAttribute('data-closed','');};\n"
    "})();\n";

static Page *find_page(App *a, const char *token) {
    for (int i = 0; i < a->npages; i++)
        if (!strcmp(a->pages[i]->token, token)) return a->pages[i];
    return NULL;
}

/* The page leaves: its component's `on leave` blocks run (an error there is logged), and it is gone. */
static void drop_page(LubeVM *vm, App *a, Page *p) {
    for (int i = 0; i < a->npages; i++)
        if (a->pages[i] == p) {
            a->pages[i] = a->pages[--a->npages];
            break;
        }
    if (is_component(p->root)) {
        LubeValue r = lube->call_method(vm, p->root, "leave", 0, NULL);
        if (lube->failed(r)) {
            char text[1024];
            lube->take_error(vm, text, sizeof text);
            log_line("a page's leave: %s", text);
        } else {
            lube->release(r);
        }
    }
    page_free(p);
}

/* The page's view, again, into p->html and its handlers: 0, or the error (taken: its text in text). */
static int page_render(LubeVM *vm, App *a, Page *p, char *text, size_t size) {
    Render r;
    memset(&r, 0, sizeof r);
    r.vm = vm;
    r.inline_styles = a->pol.inline_styles;
    r.key = p->key;
    render(&r, p->root, 0);
    if (r.failed) {
        render_drop_handlers(r.handlers, r.nh);
        buf_free(&r.out);
        render_error(&r, text, size);
        return 1;
    }
    render_drop_handlers(p->handlers, p->nh);
    p->handlers = r.handlers;
    p->nh = r.nh;
    buf_free(&p->html);
    p->html = r.out;
    return 0;
}

/* Pages whose WebSocket never came. */
static void expire_pages(LubeVM *vm, App *a, double now) {
    for (int i = a->npages - 1; i >= 0; i--)
        if (i < a->npages && !a->pages[i]->ws && now - a->pages[i]->made > a->pol.page_wait) drop_page(vm, a, a->pages[i]);
}

static void serve_page(LubeVM *vm, App *a, Conn *c, const Route *rt, LubeValue params) {
    if (a->npages >= a->pol.max_pages) expire_pages(vm, a, lube->now());
    if (a->npages >= a->pol.max_pages) {
        lube->release(params);
        log_line("%s %s: too many live pages (policy max_pages: %g)", c->req.method_name, c->req.path, a->pol.max_pages);
        respond_status(a, c, 503, "Retry-After: 5\r\n");
        return;
    }
    LubeValue req = request_value(vm, a, c, params, "", 0);
    LubeValue root = lube->call(vm, rt->fn, 1, &req);
    lube->release(req);
    if (lube->failed(root)) {
        server_error(vm, a, c);
        return;
    }
    Page *p = (Page *)calloc(1, sizeof(Page));
    if (!p) abort();  /* LCOV_EXCL_LINE */
    p->root = root;
    p->version = 1;
    p->made = lube->now();
    char cookie[33];
    int had = session_cookie(&c->req, cookie);
    if (!random_hex(p->token, 16) || (!had && !random_hex(cookie, 16)) || !system_random(p->key, sizeof p->key))
        abort();  /* LCOV_EXCL_LINE: random numbers worked a moment ago (App) */
    memcpy(p->cookie, cookie, sizeof cookie);
    char text[1024];
    if (page_render(vm, a, p, text, sizeof text)) {
        a->pages = (Page **)realloc(a->pages, sizeof(Page *) * (size_t)(a->npages + 1));
        a->pages[a->npages++] = p;
        drop_page(vm, a, p);
        server_error_text(a, c, text);
        return;
    }
    int live = p->nh > 0 || is_component(root);
    Buf doc = {0};
    buf_str(&doc, "<!doctype html>\n<html><head><meta charset=\"utf-8\">"
                  "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
    if (rt->title) {
        buf_str(&doc, "<title>");
        buf_escape(&doc, rt->title, strlen(rt->title));
        buf_str(&doc, "</title>");
    }
    for (int i = 0; i < rt->nstyles; i++) {
        buf_str(&doc, "<link rel=\"stylesheet\" href=\"");
        buf_escape(&doc, rt->styles[i], strlen(rt->styles[i]));
        buf_str(&doc, "\">");
    }
    for (int i = 0; i < rt->nscripts; i++) {
        buf_str(&doc, "<script src=\"");
        buf_escape(&doc, rt->scripts[i], strlen(rt->scripts[i]));
        buf_str(&doc, "\" defer></script>");
    }
    if (live) buf_str(&doc, "<script src=\"/_lube/web.js\" defer></script>");
    buf_str(&doc, "</head><body>");
    if (live) buf_fmt(&doc, "<div id=\"lube\" data-page=\"%s\" data-version=\"1\">", p->token);
    else buf_str(&doc, "<div id=\"lube\">");
    buf_add(&doc, p->html.p, p->html.n);
    buf_str(&doc, "</div></body></html>\n");
    Buf extra = {0};
    buf_str(&extra, "Cache-Control: no-store\r\n");
    if (live && !had)
        buf_fmt(&extra, "Set-Cookie: lube_session=%s; Path=/; HttpOnly; SameSite=Strict%s\r\n", cookie,
                a->pol.secure_cookies || is_https(a, c) ? "; Secure" : "");
    buf_add(&extra, "", 1);
    respond(a, c, 200, "text/html; charset=utf-8", doc.p, doc.n, extra.p);
    buf_free(&doc);
    buf_free(&extra);
    if (!live) {
        page_free(p);
        return;
    }
    a->pages = (Page **)realloc(a->pages, sizeof(Page *) * (size_t)(a->npages + 1));
    a->pages[a->npages++] = p;
    if (c->req.method == M_HEAD) drop_page(vm, a, p);  /* (no one will see it: its component leaves now) */
}

/* ======================================================================
 * WebSockets (RFC 6455): a page's events come in, its views go out
 * ==================================================================== */

static void ws_frame(Conn *c, int opcode, const char *data, size_t n) {
    unsigned char h[10];
    size_t k = 0;
    h[k++] = (unsigned char)(0x80 | opcode);
    if (n < 126) {
        h[k++] = (unsigned char)n;
    } else if (n < 65536) {
        h[k++] = 126;
        h[k++] = (unsigned char)(n >> 8);
        h[k++] = (unsigned char)n;
    } else {
        h[k++] = 127;
        for (int i = 7; i >= 0; i--) h[k++] = (unsigned char)((uint64_t)n >> (8 * i));
    }
    buf_add(&c->out, h, k);
    buf_add(&c->out, data, n);
}

/* Closes the WebSocket, with a status (1000: done; 1002: not the protocol; 1007: not UTF-8; 1008: against the
 * policy; 1009: too big). */
static void ws_close(Conn *c, int code) {
    char b[2] = {(char)(code >> 8), (char)(code & 255)};
    c->msg.n = 0;
    ws_frame(c, 8, b, 2);
    c->state = C_CLOSING;
}

static void ws_send(Conn *c, const char *json, size_t n) { ws_frame(c, 1, json, n); }

static void ws_view(Page *p) {
    Buf m = {0};
    buf_fmt(&m, "{\"v\":%d,\"html\":", p->version);
    buf_json(&m, p->html.p ? p->html.p : "", p->html.n);
    buf_add(&m, "}", 1);
    ws_send(p->ws, m.p, m.n);
    buf_free(&m);
}

static void ws_error(App *a, Conn *c, const char *text) {
    Buf m = {0};
    buf_str(&m, "{\"error\":");
    const char *say = a->dev ? text : "the server could not handle that event";
    buf_json(&m, say, strlen(say));
    buf_add(&m, "}", 1);
    ws_send(c, m.p, m.n);
    buf_free(&m);
}

/* An event: {"v": the view's version, "h": a handler's id, "a": what the event gives (if it gives something)}. */
static void ws_event(LubeVM *vm, App *a, Conn *c, const char *msg, size_t n) {
    Page *p = c->page;
    Json j = {msg, msg + n, vm, 0};
    LubeValue ev;
    if (!json_value(&j, &ev)) {
        ws_close(c, 1008);
        return;
    }
    json_space(&j);
    LubeValue v = lube->nothing(), h = lube->nothing(), arg = lube->nothing();
    static const char *const names[] = {"v", "h", "a", NULL};
    LubeValue err = lube->nothing();
    int ok = j.p == j.end && lube->kind(ev) == LUBE_OBJECT && !unknown_key(vm, ev, names, "an event", &err) &&
             lube->object_get(vm, ev, "v", &v) && lube->kind(v) == LUBE_NUMBER && lube->object_get(vm, ev, "h", &h) &&
             lube->str(h, NULL);
    if (!ok) {
        if (lube->failed(err) && lube->kind(ev) == LUBE_OBJECT) {
            char text[256];
            lube->take_error(vm, text, sizeof text);
        }
        lube->release(ev);
        ws_close(c, 1008);
        return;
    }
    if (lube->number(v) != p->version) {  /* it saw an older view: it gets the one there is */
        lube->release(ev);
        ws_view(p);
        return;
    }
    const char *id = lube->str(h, NULL);
    int k = -1;
    for (int i = 0; i < p->nh && k < 0; i++) {
        char want[17];
        handler_id(p->key, i, want);
        if (!strcmp(want, id)) k = i;
    }
    if (k < 0) {  /* no handler has that id: not a page's script */
        lube->release(ev);
        ws_close(c, 1008);
        return;
    }
    int has_arg = lube->object_get(vm, ev, "a", &arg);
    LubeValue handler = lube->retain(p->handlers[k]);
    LubeValue r = lube->call(vm, handler, has_arg, has_arg ? &arg : NULL);
    lube->release(handler);
    lube->release(ev);
    char text[1024];
    if (lube->failed(r)) {
        lube->take_error(vm, text, sizeof text);
        log_line("an event: %s", text);
        if (c->state == C_WS) ws_error(a, c, text);
    } else {
        lube->release(r);
    }
    if (!c->page) return;  /* (the handler stopped the app) */
    Buf before = p->html;
    p->html.p = NULL;
    p->html.n = p->html.cap = 0;
    if (page_render(vm, a, p, text, sizeof text)) {
        p->html = before;
        log_line("a view: %s", text);
        ws_error(a, c, text);
        return;
    }
    int same = before.n == p->html.n && (!before.n || !memcmp(before.p, p->html.p, before.n));
    buf_free(&before);
    if (same) return;
    p->version++;
    ws_view(p);
}

/* The frames that have come, in c->in. */
static void ws_read(LubeVM *vm, App *a, Conn *c, double now) {
    while (c->state == C_WS && c->in.n >= 2) {
        const unsigned char *b = (const unsigned char *)c->in.p;
        int fin = b[0] & 0x80, op = b[0] & 15;
        size_t k = 2;
        uint64_t len = b[1] & 127;
        if ((b[0] & 0x70) || !(b[1] & 0x80)) {  /* an extension we did not agree to, or a frame not masked */
            ws_close(c, 1002);
            return;
        }
        if (len == 126) {
            if (c->in.n < 4) return;
            len = (uint64_t)b[2] << 8 | b[3];
            k = 4;
        } else if (len == 127) {
            if (c->in.n < 10) return;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | b[2 + i];
            k = 10;
        }
        if (op >= 8 && (!fin || len > 125)) {
            ws_close(c, 1002);
            return;
        }
        if (len > (uint64_t)a->pol.max_message || c->msg.n + len > (uint64_t)a->pol.max_message) {
            ws_close(c, 1009);
            return;
        }
        if (c->in.n < k + 4 + len) return;  /* the rest has not come */
        unsigned char *data = (unsigned char *)c->in.p + k + 4;
        for (uint64_t i = 0; i < len; i++) data[i] ^= b[k + (i & 3)];
        c->deadline = now + a->pol.ws_idle;
        c->ping_at = now + a->pol.ws_idle / 4;
        size_t whole = k + 4 + (size_t)len;
        if (op == 8) {  /* close: answered, and done */
            ws_frame(c, 8, (const char *)data, len >= 2 ? 2 : 0);
            c->state = C_CLOSING;
            return;
        }
        if (op == 9) ws_frame(c, 10, (const char *)data, (size_t)len);
        if (op == 1 || op == 0) {
            if ((op == 1) == c->msg_open) {  /* a new message in the middle of one, or a part of none */
                ws_close(c, 1002);
                return;
            }
            c->msg_open = !fin;
            buf_add(&c->msg, data, (size_t)len);
        } else if (op != 9 && op != 10) {  /* binary, or an opcode with no meaning */
            ws_close(c, op == 2 ? 1003 : 1002);
            return;
        }
        buf_drop(&c->in, whole);
        if ((op == 1 || op == 0) && fin) {
            if (!utf8_ok(c->msg.p, c->msg.n)) {
                ws_close(c, 1007);
                return;
            }
            c->tokens = fmin(a->pol.event_burst, c->tokens + (now - c->refilled) * a->pol.events_per_second);
            c->refilled = now;
            if (c->tokens < 1) {
                log_line("a page sent events too fast (policy events_per_second: %g)", a->pol.events_per_second);
                ws_close(c, 1008);
                return;
            }
            c->tokens -= 1;
            Buf m = c->msg;
            c->msg.p = NULL;
            c->msg.n = c->msg.cap = 0;
            ws_event(vm, a, c, m.p ? m.p : "", m.n);
            buf_free(&m);
        }
    }
}

/* Whether the request comes from the page's own site, or one the policy names. */
static int origin_ok(App *a, Conn *c) {
    const char *origin = req_header(&c->req, "origin");
    const char *host = req_header(&c->req, "host");
    if (!origin || !host) return 0;
    for (int i = 0; i < a->pol.norigins; i++)
        if (!strcmp(origin, a->pol.origins[i])) return 1;
    const char *scheme = is_https(a, c) ? "https://" : "http://";
    size_t sn = strlen(scheme);
    if (strncmp(origin, scheme, sn)) return 0;
    const char *o = origin + sn;
    if (strlen(o) != strlen(host)) return 0;
    for (size_t i = 0; o[i]; i++)
        if ((o[i] | 32) != (host[i] | 32)) return 0;
    return 1;
}

static int same_secret(const char *x, const char *y) {  /* (in the same time, whatever they have in common) */
    unsigned diff = 0;
    for (int i = 0; i < 32; i++) diff |= (unsigned)(x[i] ^ y[i]);
    return !diff;
}

/* GET /_lube/ws?page=...: the page's WebSocket. */
static void ws_upgrade(LubeVM *vm, App *a, Conn *c, double now) {
    Req *r = &c->req;
    const char *up = req_header(r, "upgrade"), *key = req_header(r, "sec-websocket-key");
    const char *version = req_header(r, "sec-websocket-version");
    int twice = header_count(r, "origin") > 1 || header_count(r, "sec-websocket-key") > 1 ||
                header_count(r, "sec-websocket-version") > 1 || header_count(r, "upgrade") > 1;
    if (r->method != M_GET || r->http10 || !up || !has_token(up, "websocket") || !r->upgrade || r->length || twice) {
        fail_request(a, c, 400);
        return;
    }
    if (!version || strcmp(version, "13")) {
        c->keep = 0;
        respond_status(a, c, 426, "Sec-WebSocket-Version: 13\r\n");
        c->state = C_CLOSING;
        return;
    }
    int key_ok = key && strlen(key) == 24 && !strcmp(key + 22, "==");
    for (int i = 0; key_ok && i < 22; i++) key_ok = isalnum_c(key[i]) || key[i] == '+' || key[i] == '/';
    LubeValue q = lube->new_object(vm);
    parse_pairs(vm, r->query, strlen(r->query), q);
    LubeValue t = lube->nothing();
    const char *token = lube->object_get(vm, q, "page", &t) ? lube->str(t, NULL) : NULL;
    Page *p = token ? find_page(a, token) : NULL;
    char cookie[33];
    int ok = key_ok && origin_ok(a, c) && p && !p->ws && session_cookie(r, cookie) && same_secret(cookie, p->cookie);
    lube->release(q);
    if (!key_ok) {
        fail_request(a, c, 400);
        return;
    }
    if (!ok) {
        log_line("a WebSocket was refused: %s", !origin_ok(a, c) ? "it came from another site (Origin)"
                                                  : !p ? "no such page" : p->ws ? "the page has one" : "not the page's session");
        fail_request(a, c, 403);
        return;
    }
    char joined[64];
    unsigned char hash[20];
    char accept[32];
    snprintf(joined, sizeof joined, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    sha1(joined, strlen(joined), hash);
    base64(hash, 20, accept);
    char extra[160];
    snprintf(extra, sizeof extra, "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n", accept);
    respond(a, c, 101, NULL, "", 0, extra);
    c->state = C_WS;
    c->page = p;
    p->ws = c;
    c->deadline = now + a->pol.ws_idle;
    c->ping_at = now + a->pol.ws_idle / 4;
    c->tokens = a->pol.event_burst;
    c->refilled = now;
}

/* ======================================================================
 * Serving: one request at a time on each connection, every connection at once
 * ==================================================================== */

#define CLOSE_WAIT 5.0  /* seconds a closing connection has to take its last answer */

static void allow_header(int methods, char *out, size_t size) {
    size_t n = (size_t)snprintf(out, size, "Allow: ");
    for (int i = 0; METHOD_NAMES[i]; i++)
        if (methods & (1 << i)) n += (size_t)snprintf(out + n, size - n, "%s%s", n > 7 ? ", " : "", METHOD_NAMES[i]);
    snprintf(out + n, size - n, "\r\n");
}

/* Whether the request's Host is one this server answers to: an address (127.0.0.1, [::1], 10.0.0.5), localhost,
 * or a name of policy hosts. A page of another site whose name was made to lead here (DNS rebinding) names
 * that site: it gets nothing. */
static int host_ok(App *a, const char *host) {
    if (!host) return 1;  /* (HTTP/1.0: browsers send one) */
    char name[256];
    size_t n = 0;
    if (host[0] == '[') {  /* an IPv6 address */
        const char *end = strchr(host, ']');
        return end && (end[1] == 0 || end[1] == ':');
    }
    while (host[n] && host[n] != ':' && n < sizeof name - 1) name[n] = (char)(host[n] >= 'A' && host[n] <= 'Z' ? host[n] + 32 : host[n]), n++;
    name[n] = 0;
    int dots = 0, digits = 1;
    for (size_t i = 0; i < n; i++) dots += name[i] == '.', digits &= (name[i] >= '0' && name[i] <= '9') || name[i] == '.';
    if ((digits && dots == 3) || !strcmp(name, "localhost")) return 1;
    for (int i = 0; i < a->pol.nhosts; i++)
        if (!strcmp(a->pol.hosts[i], "*") || !strcmp(a->pol.hosts[i], name)) return 1;
    return 0;
}

static int origin_ok(App *a, Conn *c);

/* Whether a request that changes things (POST, PUT, PATCH, DELETE) comes from a page of another site: a form or a
 * script there, sending what this site's user never meant to (cross-site request forgery). Browsers say where a
 * request comes from (Origin, Sec-Fetch-Site); a client that is not a browser says nothing, and is let be. */
static int cross_site(App *a, Conn *c) {
    if (c->req.method & (M_GET | M_HEAD | M_OPTIONS)) return 0;
    const char *site = req_header(&c->req, "sec-fetch-site");
    if (site && !strcmp(site, "cross-site") && !req_header(&c->req, "origin")) return 1;
    return req_header(&c->req, "origin") && !origin_ok(a, c);
}

/* A request, read whole (its body at body): its answer, into c->out. */
static void dispatch(LubeVM *vm, App *a, Conn *c, const char *body, double now) {
    Req *r = &c->req;
    c->keep = r->http10 ? r->keep_alive && !r->close : !r->close;
    if (!host_ok(a, req_header(r, "host"))) {
        log_line("%s %s: refused: the host \"%s\" is not this server's (App({policy: {hosts: [...]}}) names more)",
                 r->method_name, r->path, req_header(r, "host"));
        respond_status(a, c, 421, NULL);
        return;
    }
    if (cross_site(a, c)) {
        log_line("%s %s: refused: it came from another site (App({policy: {origins: [...]}}) names those it may)",
                 r->method_name, r->path);
        respond_status(a, c, 403, NULL);
        return;
    }
    if (r->nsegs >= 1 && !strcmp(r->segs[0], RESERVED)) {
        if (r->nsegs == 2 && !strcmp(r->segs[1], "web.js") && (r->method & (M_GET | M_HEAD)))
            respond(a, c, 200, "text/javascript; charset=utf-8", CLIENT_JS, sizeof CLIENT_JS - 1, "Cache-Control: no-cache\r\n");
        else if (r->nsegs == 2 && !strcmp(r->segs[1], "ws"))
            ws_upgrade(vm, a, c, now);
        else
            respond_status(a, c, 404, NULL);
        return;
    }
    int allowed = 0;
    for (int i = 0; i < a->nroutes; i++) {
        const Route *rt = &a->routes[i];
        if (!route_match(rt, r, NULL, lube->nothing())) continue;
        if (!(rt->methods & r->method)) {
            allowed |= rt->methods;
            continue;
        }
        if (rt->kind == R_STATIC) {
            if (serve_static(a, c, rt)) return;
            continue;  /* no such file: the routes after it may have the path */
        }
        LubeValue params = lube->new_object(vm);
        route_match(rt, r, vm, params);
        if (rt->kind == R_PAGE) {
            serve_page(vm, a, c, rt, params);
            return;
        }
        LubeValue req = request_value(vm, a, c, params, body, (size_t)r->length);
        LubeValue v = lube->call(vm, rt->fn, 1, &req);
        lube->release(req);
        char why[512];
        int how = lube->failed(v) ? RAISED : answer(vm, a, c, v, why, sizeof why);
        if (!lube->failed(v)) lube->release(v);
        if (how == RAISED) server_error(vm, a, c);
        else if (how == WHY) server_error_text(a, c, why);
        return;
    }
    if (!allowed) {
        respond_status(a, c, 404, NULL);
        return;
    }
    char allow[96];
    allow_header(allowed, allow, sizeof allow);
    if (r->method == M_OPTIONS) respond(a, c, 204, NULL, "", 0, allow);
    else respond_status(a, c, 405, allow);
}

/* "\r\n\r\n" in s[0..n), or NULL. */
static char *head_end(char *s, size_t n) {
    for (size_t i = 0; i + 3 < n; i++)
        if (s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' && s[i + 3] == '\n') return s + i;
    return NULL;
}

/* What has come on c: its requests, answered, one at a time (the next waits until the answer has gone). */
static void conn_process(LubeVM *vm, App *a, Conn *c, double now) {
    if (c->state == C_WS) {
        ws_read(vm, a, c, now);
        return;
    }
    while ((c->state == C_HEAD || c->state == C_BODY) && !c->out.n && !c->file) {
        if (c->state == C_HEAD) {
            if (!c->in.n) return;
            char *end = head_end(c->in.p, c->in.n);
            if (!end) {
                if (head_broken(c->in.p, c->in.n)) fail_request(a, c, 400);
                else if (c->in.n > a->pol.max_header) fail_request(a, c, 431);
                return;
            }
            size_t h = (size_t)(end - c->in.p);
            if (h + 4 > a->pol.max_header) {
                fail_request(a, c, 431);
                return;
            }
            req_free(&c->req);
            char *head = copy_str(c->in.p, h);
            Limits lim = {(size_t)a->pol.max_url, (int)a->pol.max_headers, (long long)a->pol.max_body};
            int status = parse_head(head, h, &lim, &c->req);
            free(head);
            if (status) {
                fail_request(a, c, status);
                return;
            }
            c->head_len = h + 4;
            if (c->req.length) {
                c->state = C_BODY;
                c->deadline = now + a->pol.body_timeout;
                if (c->req.expect_continue && c->in.n < c->head_len + (size_t)c->req.length) {
                    buf_str(&c->out, "HTTP/1.1 100 Continue\r\n\r\n");
                    return;
                }
            }
        }
        if (c->in.n < c->head_len + (size_t)c->req.length) return;
        dispatch(vm, a, c, c->in.p + c->head_len, now);
        buf_drop(&c->in, c->head_len + (size_t)c->req.length);
        c->sent_at = now;
        if (c->state == C_WS) {
            ws_read(vm, a, c, now);
            return;
        }
        if (c->state == C_CLOSING) return;
        if (!c->keep) {
            c->state = C_CLOSING;
            return;
        }
        c->state = C_HEAD;
        c->idle = 1;
        c->deadline = now + a->pol.keep_alive;
    }
}

/* Reads what has come (1 MB at most at a time, so that one connection cannot keep the rest waiting). */
static void conn_read(App *a, Conn *c, double now) {
    size_t total = 0;
    while (total < (1 << 20)) {
        char tmp[65536];
        long n = (long)recv(c->fd, tmp, (int)sizeof tmp, 0);
        if (n > 0) {
            if (c->state == C_HEAD && c->idle) {  /* the next request begins: it has the time for a head */
                c->idle = 0;
                c->deadline = now + a->pol.header_timeout;
            }
            if (c->state != C_CLOSING) buf_add(&c->in, tmp, (size_t)n);
            total += (size_t)n;
            continue;
        }
        int e = net_errno();
        if (n < 0 && E_AGAIN(e)) return;
        if (n < 0 && E_INTR(e)) continue;  /* LCOV_EXCL_LINE */
        c->eof = 1;  /* closed (or broken) on its side */
        return;
    }
}

static void conn_flush(Conn *c, double now) {
    pump(c);
    while (c->out.n) {
        long n = (long)send(c->fd, c->out.p, (int)c->out.n, SEND_FLAGS);
        if (n > 0) {
            buf_drop(&c->out, (size_t)n);
            c->sent_at = now;
            pump(c);
            continue;
        }
        int e = net_errno();
        if (n < 0 && E_AGAIN(e)) return;
        if (n < 0 && E_INTR(e)) continue;  /* LCOV_EXCL_LINE */
        c->state = C_DEAD;
        return;
    }
}

static void add_conn(App *a, Sock s, const char *ip, double now) {
    Conn *c = (Conn *)calloc(1, sizeof(Conn));
    if (!c) abort();  /* LCOV_EXCL_LINE */
    c->fd = s;
    snprintf(c->ip, sizeof c->ip, "%s", ip);
    c->state = C_HEAD;
    c->deadline = now + a->pol.header_timeout;
    c->sent_at = now;
    a->conns = (Conn **)realloc(a->conns, sizeof(Conn *) * (size_t)(a->nconns + 1));
    a->conns[a->nconns++] = c;
}

static void accept_all(App *a, double now) {
    for (;;) {
        struct sockaddr_storage sa;
        socklen_t len = sizeof sa;
        Sock s = accept(a->listen, (struct sockaddr *)&sa, &len);
        if (s == BAD_SOCK) return;
        char ip[64] = "?";
        if (sa.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&sa)->sin6_addr, ip, sizeof ip);
        else inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr, ip, sizeof ip);
        int from_ip = 0;
        for (int i = 0; i < a->nconns; i++) from_ip += !strcmp(a->conns[i]->ip, ip);
        if (a->nconns >= a->pol.max_connections || from_ip >= a->pol.max_per_ip) {
            close_sock(s);  /* (too many: shed, at once) */
            continue;
        }
        set_nonblocking(s);
        socket_options(s);
        add_conn(a, s, ip, now);
    }
}

/* Connections whose time is up. */
static void conn_timers(App *a, Conn *c, double now) {
    if (c->state == C_DEAD) return;
    if ((c->out.n || c->file) && now - c->sent_at > a->pol.header_timeout) {  /* it takes no answers: dropped */
        c->state = C_DEAD;
        return;
    }
    if (c->state == C_CLOSING) {
        if (!c->out.n && !c->file) c->state = C_DEAD;
        return;
    }
    if (c->state == C_WS) {
        if (now >= c->deadline) {
            c->state = C_DEAD;
        } else if (now >= c->ping_at) {
            ws_frame(c, 9, "", 0);
            c->ping_at = now + a->pol.ws_idle / 4;
        }
        return;
    }
    if (c->eof && !c->out.n && !c->file) {
        c->state = C_DEAD;
        return;
    }
    if (now < c->deadline || c->out.n || c->file) return;
    if (c->idle || (c->state == C_HEAD && !c->in.n && c->req.method)) {  /* quiet between requests: closed */
        c->state = C_DEAD;
        return;
    }
    fail_request(a, c, 408);  /* too slow to send its request */
    c->sent_at = now;
}

static double next_time(App *a, double now, double until) {
    double t = until;
    for (int i = 0; i < a->nconns; i++) {
        const Conn *c = a->conns[i];
        if (c->out.n || c->file) t = fmin(t, c->sent_at + a->pol.header_timeout);
        if (c->state == C_HEAD || c->state == C_BODY || c->state == C_WS) t = fmin(t, c->deadline);
        if (c->state == C_WS) t = fmin(t, c->ping_at);
    }
    for (int i = 0; i < a->npages; i++)
        if (!a->pages[i]->ws) t = fmin(t, a->pages[i]->made + a->pol.page_wait + 0.001);
    return t < now ? now : t;
}

/* Everything closed: its pages leave, its connections end. */
static void app_close(LubeVM *vm, App *a) {
    while (a->npages) drop_page(vm, a, a->pages[a->npages - 1]);
    for (int i = 0; i < a->nconns; i++) conn_free(a->conns[i]);
    a->nconns = 0;
    close_sock(a->listen);
    a->listen = BAD_SOCK;
    a->started = a->stopping = 0;
}

static LubeValue app_step(LubeVM *vm, App *a, double wait) {
    double now = lube->now();
    int n = 1 + a->nconns;
    PollFd *fds = (PollFd *)calloc((size_t)n, sizeof(PollFd));
    if (!fds) abort();  /* LCOV_EXCL_LINE */
    fds[0].fd = a->listen;
    fds[0].events = POLLIN;
    double room = a->pol.max_header + a->pol.max_body + 65536;  /* what may wait unread */
    for (int i = 0; i < a->nconns; i++) {
        Conn *c = a->conns[i];
        fds[1 + i].fd = c->fd;
        if (!c->eof && c->in.n < room) fds[1 + i].events |= POLLIN;
        if (c->out.n || c->file) fds[1 + i].events |= POLLOUT;
    }
    double until = next_time(a, now, wait == INFINITY ? now + 3600 : now + wait);
    double ms = ceil((until - now) * 1000);
    sys_poll(fds, n, (int)(ms > INT_MAX ? INT_MAX : ms));
    now = lube->now();
    int nconns = a->nconns;
    if (fds[0].revents) accept_all(a, now);
    for (int i = 0; i < nconns && !a->stopping; i++) {
        Conn *c = a->conns[i];
        if (c->state == C_DEAD) continue;
        if (fds[1 + i].revents & (POLLIN | POLLHUP | POLLERR)) conn_read(a, c, now);
        conn_process(vm, a, c, now);
        if (c->state == C_WS && c->eof) c->state = C_DEAD;
        if (c->state != C_DEAD) conn_flush(c, now);
        if (c->state != C_DEAD) conn_process(vm, a, c, now);  /* (its next request, now that the answer has gone) */
        if (c->state != C_DEAD) conn_flush(c, now);
    }
    for (int i = 0; i < a->nconns; i++) conn_timers(a, a->conns[i], now);
    for (int i = a->nconns - 1; i >= 0; i--) {
        Conn *c = a->conns[i];
        if (c->state != C_DEAD) continue;
        a->conns[i] = a->conns[--a->nconns];
        Page *p = c->page;
        conn_free(c);
        if (p) drop_page(vm, a, p);
    }
    expire_pages(vm, a, now);
    free(fds);
    if (a->stopping) app_close(vm, a);
    return lube->nothing();
}

static LubeValue step_checked(LubeVM *vm, App *a, const char *fn, double wait) {
    if (!a->started) return lube->error(vm, "%s: the app has not started (app.start(port) first)", fn);
    if (a->busy) return lube->error(vm, "%s: the app is serving already (a handler cannot serve)", fn);
    a->busy = 1;
    LubeValue r = app_step(vm, a, wait);
    a->busy = 0;
    return r;
}

static double wait_arg(int argc, const LubeValue *argv, int i, double otherwise) {
    if (argc <= i) return otherwise;
    double w = lube->number(argv[i]);
    return lube->kind(argv[i]) == LUBE_NUMBER && w > 0 ? w : 0;
}

/* app.step(wait?): serves what has come, waiting up to wait seconds (0: none) for something to. */
FN(m_step) {
    if (argc > 1 && lube->kind(argv[1]) != LUBE_NUMBER)
        return lube->error(vm, "step() takes the seconds to wait, not %s", lube->a_type(argv[1]));
    return step_checked(vm, app_of(argv[0]), "step()", wait_arg(argc, argv, 1, 0));
}

/* app.serve(port?): listens (unless app.start() did), and serves until app.stop(). */
FN(m_serve) {
    App *a = app_of(argv[0]);
    if (a->busy) return lube->error(vm, "serve(): the app is serving already (a handler cannot serve)");
    LubeValue r = lube->nothing();
    if (!a->started) {  /* (started: it serves where it is) */
        r = app_listen(vm, a, "serve()", argc, argv);
        if (lube->failed(r)) return r;
        log_line("serving at http://%s%s%s:%d/", strchr(a->host, ':') ? "[" : "", a->host, strchr(a->host, ':') ? "]" : "", a->port);
    }
    while (a->started) {
        r = step_checked(vm, a, "serve()", INFINITY);
        if (lube->failed(r)) return r;  /* LCOV_EXCL_LINE: step() fails only when serve() would not start */
    }
    return lube->nothing();
}

/* app.stop(): its pages leave, its connections end, and it stops listening (now, or once the step ends). */
FN(m_stop) {
    App *a = app_of(argv[0]);
    if (!a->started) return lube->nothing();
    if (a->busy) a->stopping = 1;
    else app_close(vm, a);
    return lube->nothing();
}

LUBE_PLUGIN(web) {
    lube = api;
    if (api->version < 3) return -1;  /* LCOV_EXCL_LINE: it needs take_error */
    NodeType = api->type(plugin, "Node", sizeof(Node), node_release, node_describe);
    AppType = api->type(plugin, "App", sizeof(App), app_release, app_describe);
    api->constructor(AppType, f_app, 0, 1);
    api->method(AppType, "get", m_get, 3, 3);
    api->method(AppType, "post", m_post, 3, 3);
    api->method(AppType, "put", m_put, 3, 3);
    api->method(AppType, "patch", m_patch, 3, 3);
    api->method(AppType, "delete", m_delete, 3, 3);
    api->method(AppType, "page", m_page, 3, 4);
    api->method(AppType, "static", m_static, 3, 3);
    api->method(AppType, "start", m_start, 1, 2);
    api->method(AppType, "step", m_step, 1, 2);
    api->method(AppType, "serve", m_serve, 1, 2);
    api->method(AppType, "stop", m_stop, 1, 1);
    api->method(AppType, "port", m_port, 1, 1);
    api->method(AppType, "pages", m_pages, 1, 1);
    declare_tags(plugin);
    api->function(plugin, "html", f_html, 1, 1);
    return LUBE_PLUGIN_API;
}
