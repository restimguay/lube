/*
 * gfx.c - the plugin gfx: 2D graphics, drawn by Lube itself, pixel by pixel.
 *
 *   import gfx.*
 *   let win = window(320, 180, "Game", 3)    a window of 320 by 180 pixels, shown 3 times as large
 *   let ship = load_image("ship.png")        an Image (PNG); image(w, h) a blank one
 *   while win.is_open() {
 *     for e in win.events() { ... }          {kind: "key_down", key: "left"}, mouse, touch, ...
 *     win.clear(rgb(10, 10, 30))
 *     win.draw(ship, x, y, {rotate: 90})     also rect, fill_rect, line, circle, text, ...
 *     win.present()                          shows it, 60 times a second
 *   }
 *
 * Images and windows draw alike: whatever is drawn is the same everywhere,
 * a server can draw images with no screen at all (img.to_png()), and the
 * tests can check every pixel. A window shows its pixels with what the
 * system has, loaded when the first window opens: Win32 (gfx/win32.c),
 * Cocoa (gfx/cocoa.c), X11 (gfx/x11.c); or on a host program's surface
 * (lube_gfx.h: the phone templates in mobile/); or nowhere (LUBE_GFX=none).
 */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#elif !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  /* nanosleep, dlopen under -std=c99 */
#endif
#include "../lube_plugin.h"
#include "../lube_gfx.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if defined(LUBE_NO_DISPLAY)
#define GFX_SYSTEM 0  /* (built without the system's windows: the static Linux binary, the coverage build) */
#elif defined(_WIN32)
#define GFX_SYSTEM 1
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
#define GFX_SYSTEM 2
#elif defined(__linux__) && !defined(__ANDROID__)
#define GFX_SYSTEM 3
#else
#define GFX_SYSTEM 0
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const LubeAPI *lube;
static LubeType *ImageType, *WindowType;

typedef struct {
    int w, h;
    uint32_t *px;  /* w * h, rows top first, 0xAARRGGBB */
} Canvas;

#include "gfx/draw.c"
#include "gfx/font.c"
#include "gfx/png.c"

/* ======================================================================
 * Windows: their events and keys, whatever shows them
 * ==================================================================== */

static const char *const KEY_NAMES[] = {
    "unknown",
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "space", "enter", "escape", "tab", "backspace", "delete", "insert", "home", "end", "page_up", "page_down",
    "left", "right", "up", "down", "shift", "control", "alt", "command", "caps_lock",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    "minus", "equals", "left_bracket", "right_bracket", "semicolon", "quote", "comma", "period", "slash", "backslash", "backquote",
};
#define KEY_COUNT ((int)(sizeof KEY_NAMES / sizeof KEY_NAMES[0]))
enum { K_A = 1, K_0 = 27, K_SPACE = 37, K_ENTER, K_ESCAPE, K_TAB, K_BACKSPACE, K_DELETE, K_INSERT, K_HOME, K_END, K_PAGE_UP,
       K_PAGE_DOWN, K_LEFT, K_RIGHT, K_UP, K_DOWN, K_SHIFT, K_CONTROL, K_ALT, K_COMMAND, K_CAPS_LOCK, K_F1,
       K_MINUS = K_F1 + 12, K_EQUALS, K_LEFT_BRACKET, K_RIGHT_BRACKET, K_SEMICOLON, K_QUOTE, K_COMMA, K_PERIOD, K_SLASH,
       K_BACKSLASH, K_BACKQUOTE };

static int key_named(const char *name) {
    for (int i = 1; i < KEY_COUNT; i++)
        if (!strcmp(KEY_NAMES[i], name)) return i;
    return 0;
}

static const char *const EVENT_NAMES[] = {"", "key_down", "key_up", "text", "mouse_move", "mouse_down", "mouse_up", "wheel",
                                          "touch_down", "touch_move", "touch_up", "focus", "blur", "close"};

typedef struct {
    int kind;            /* LUBE_GFX_... */
    int key, button, id, repeat;
    double x, y, dx, dy;
    char text[16];
} Event;

/* Typed text into e: as much as fits, ending where a character does. */
static void set_text(Event *e, const char *t) {
    size_t n = strlen(t);
    if (n >= sizeof e->text) {
        n = sizeof e->text - 1;
        while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
    }
    memcpy(e->text, t, n);
    e->text[n] = '\0';
}

typedef struct Win Win;
typedef struct {
    const char *(*open)(Win *w, const char *title, int scale);  /* NULL, or why it cannot */
    void (*pump)(Win *w);       /* the system's events into w's */
    void (*present)(Win *w);    /* shows w->c */
    void (*close)(Win *w);
    void (*title)(Win *w, const char *title);
} Backend;

struct Win {
    Canvas c;
    const Backend *be;
    void *sys;                  /* the backend's */
    int open;
    double fps, next;           /* frames a second (0: as fast as it goes), and when the next is due */
    unsigned char held[KEY_COUNT];
    double mx, my;
    int buttons;                /* 1 left, 2 right, 4 middle */
    int primary;                /* the touch that is also the mouse, or -1 */
    Event *q;
    int nq, capq;
};

static Win *active;  /* the window open now (one at a time) */

#define MAX_EVENTS 4096  /* (a program that never asks keeps only the latest) */

static void enqueue(Win *w, const Event *e) {
    if (w->nq == MAX_EVENTS) {
        memmove(w->q, w->q + MAX_EVENTS / 2, sizeof(Event) * (MAX_EVENTS / 2));
        w->nq = MAX_EVENTS / 2;
    }
    if (w->nq == w->capq) {
        w->capq = w->capq ? w->capq * 2 : 64;
        w->q = (Event *)realloc(w->q, sizeof(Event) * (size_t)w->capq);
        if (!w->q) abort();  /* LCOV_EXCL_LINE: out of memory */
    }
    w->q[w->nq++] = *e;
}

static void push_event(Win *w, const Event *e) {
    if (e->kind == LUBE_GFX_TOUCH_DOWN || e->kind == LUBE_GFX_TOUCH_MOVE || e->kind == LUBE_GFX_TOUCH_UP) {
        /* the first finger down also works the mouse (programs written for a mouse work on a phone) */
        int primary = e->kind == LUBE_GFX_TOUCH_DOWN ? w->primary < 0 : e->id == w->primary;
        if (e->kind == LUBE_GFX_TOUCH_DOWN && primary) w->primary = e->id;
        if (e->kind == LUBE_GFX_TOUCH_UP && primary) w->primary = -1;
        enqueue(w, e);
        if (primary) {
            Event m = *e;
            m.kind = e->kind == LUBE_GFX_TOUCH_DOWN ? LUBE_GFX_MOUSE_DOWN : e->kind == LUBE_GFX_TOUCH_UP ? LUBE_GFX_MOUSE_UP : LUBE_GFX_MOUSE_MOVE;
            m.button = 1;
            push_event(w, &m);
        }
        return;
    }
    enqueue(w, e);
    switch (e->kind) {
    case LUBE_GFX_KEY_DOWN: w->held[e->key] = 1; break;
    case LUBE_GFX_KEY_UP: w->held[e->key] = 0; break;
    case LUBE_GFX_MOUSE_MOVE: w->mx = e->x, w->my = e->y; break;
    case LUBE_GFX_MOUSE_DOWN: w->mx = e->x, w->my = e->y, w->buttons |= 1 << (e->button - 1); break;
    case LUBE_GFX_MOUSE_UP: w->mx = e->x, w->my = e->y, w->buttons &= ~(1 << (e->button - 1)); break;
    case LUBE_GFX_BLUR:  /* (keys let go of elsewhere would stay down) */
        memset(w->held, 0, sizeof w->held);
        w->buttons = 0;
        break;
    default: break;
    }
}

static void push_simple(Win *w, int kind, int key, double x, double y) {
    Event e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.key = key;
    e.x = x;
    e.y = y;
    push_event(w, &e);
}

/* The user closed it (the close button, or the host's surface went away). */
static void closed_by_user(Win *w) {
    if (!w->open) return;
    push_simple(w, LUBE_GFX_CLOSE, 0, 0, 0);
    w->be->close(w);
    w->open = 0;
    if (active == w) active = NULL;
}

void lube_gfx_fit(int w, int h, int sw, int sh, double *x, double *y, double *scale) {
    double s = w > 0 && h > 0 ? fmin((double)sw / w, (double)sh / h) : 1;
    if (s >= 1) s = floor(s);  /* (whole pixels where they fit: sharp) */
    *scale = s > 0 ? s : 1;
    *x = floor((sw - w * *scale) / 2);
    *y = floor((sh - h * *scale) / 2);
}

#if GFX_SYSTEM >= 2
/* c fitted into a sw by sh buffer (black around it). */
static void scale_into(const Canvas *c, uint32_t *dst, int sw, int sh) {
    double ox, oy, s;
    lube_gfx_fit(c->w, c->h, sw, sh, &ox, &oy, &s);
    for (int y = 0; y < sh; y++) {
        int sy = (int)floor((y - oy) / s);
        uint32_t *row = dst + (size_t)y * (size_t)sw;
        if (sy < 0 || sy >= c->h) {
            for (int x = 0; x < sw; x++) row[x] = 0xFF000000u;
            continue;
        }
        const uint32_t *src = c->px + (size_t)sy * (size_t)c->w;
        for (int x = 0; x < sw; x++) {
            int sx = (int)floor((x - ox) / s);
            row[x] = sx < 0 || sx >= c->w ? 0xFF000000u : src[sx] | 0xFF000000u;
        }
    }
}
#endif

#if GFX_SYSTEM
/* A point on a surface of sw by sh into the window's pixels. */
static void to_window(const Win *w, double sx, double sy, int sw, int sh, double *x, double *y) {
    double ox, oy, s;
    lube_gfx_fit(w->c.w, w->c.h, sw, sh, &ox, &oy, &s);
    *x = (sx - ox) / s;
    *y = (sy - oy) / s;
}
#endif

static void sleep_for(double seconds) {
#ifdef _WIN32
    Sleep((DWORD)(seconds * 1000));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) && errno == EINTR) continue;  /* (woken early by a signal: the rest) */
#endif
}

/* ---------- nowhere: LUBE_GFX=none (servers, tests) ---------- */

static const char *none_open(Win *w, const char *title, int scale) {
    (void)w, (void)title, (void)scale;
    return NULL;
}
static void none_pump(Win *w) { (void)w; }
static void none_present(Win *w) { (void)w; }
static void none_close(Win *w) { (void)w; }
static void none_title(Win *w, const char *title) { (void)w, (void)title; }
static const Backend NONE_BACKEND = {none_open, none_pump, none_present, none_close, none_title};

/* ---------- a host program's surface (lube_gfx.h) ---------- */

#ifdef _WIN32
static SRWLOCK host_lock = SRWLOCK_INIT;
#define HOST_LOCK() AcquireSRWLockExclusive(&host_lock)
#define HOST_UNLOCK() ReleaseSRWLockExclusive(&host_lock)
#else
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
#define HOST_LOCK() pthread_mutex_lock(&host_lock)
#define HOST_UNLOCK() pthread_mutex_unlock(&host_lock)
#endif
static LubeGfxHost host;
static int have_host;
static Event *posted;  /* what the host posted, for the program's thread */
static int nposted, capposted;

void lube_gfx_set_host(const LubeGfxHost *h) {
    HOST_LOCK();
    have_host = h != NULL;
    if (h) host = *h;
    HOST_UNLOCK();
}

void lube_gfx_post(const LubeGfxEvent *in) {
    if (in->kind < LUBE_GFX_KEY_DOWN || in->kind > LUBE_GFX_CLOSE) return;  /* (none Lube knows) */
    Event e;
    memset(&e, 0, sizeof e);
    e.kind = in->kind;
    e.key = in->key ? key_named(in->key) : 0;
    if ((e.kind == LUBE_GFX_KEY_DOWN || e.kind == LUBE_GFX_KEY_UP) && !e.key) return;  /* (as the systems': keys with names) */
    if (in->text) set_text(&e, in->text);
    e.x = in->x;
    e.y = in->y;
    e.button = in->button >= 1 && in->button <= 3 ? in->button : 1;
    e.id = in->id;
    e.dx = in->dx;
    e.dy = in->dy;
    e.repeat = e.kind == LUBE_GFX_KEY_DOWN && in->repeat;
    HOST_LOCK();
    if (nposted == capposted) {
        capposted = capposted ? capposted * 2 : 64;
        posted = (Event *)realloc(posted, sizeof(Event) * (size_t)capposted);
        if (!posted) abort();  /* LCOV_EXCL_LINE: out of memory */
    }
    posted[nposted++] = e;
    HOST_UNLOCK();
}

static const char *host_open(Win *w, const char *title, int scale) {
    (void)scale;
    HOST_LOCK();
    nposted = 0;  /* (what came before the window is not for it) */
    LubeGfxHost h = host;
    HOST_UNLOCK();
    if (h.opened) h.opened(h.user, w->c.w, w->c.h, title);
    return NULL;
}

static void host_pump(Win *w) {
    HOST_LOCK();
    LubeGfxHost h = host;
    HOST_UNLOCK();
    if (h.pump) h.pump(h.user);
    HOST_LOCK();
    int n = nposted;
    Event *got = n ? (Event *)malloc(sizeof(Event) * (size_t)n) : NULL;
    if (n) memcpy(got, posted, sizeof(Event) * (size_t)n);
    nposted = 0;
    HOST_UNLOCK();
    for (int i = 0; i < n && w->open; i++) {
        if (got[i].kind == LUBE_GFX_CLOSE) closed_by_user(w);
        else push_event(w, &got[i]);
    }
    free(got);
}

static void host_present(Win *w) {
    HOST_LOCK();
    LubeGfxHost h = host;
    HOST_UNLOCK();
    if (h.present) h.present(h.user, w->c.px, w->c.w, w->c.h);
}

static void host_close(Win *w) {
    (void)w;
    HOST_LOCK();
    LubeGfxHost h = host;
    HOST_UNLOCK();
    if (h.closed) h.closed(h.user);
}

static const Backend HOST_BACKEND = {host_open, host_pump, host_present, host_close, none_title};

/* ---------- the system's own ---------- */

#if GFX_SYSTEM == 1
#include "gfx/win32.c"
#elif GFX_SYSTEM == 2
#include "gfx/cocoa.c"
#elif GFX_SYSTEM == 3
#include "gfx/x11.c"
#endif

/* ======================================================================
 * The package: values and arguments
 * ==================================================================== */

typedef struct {
    Win *w;
} WinObj;

static void image_release(void *data) { free(((Canvas *)data)->px); }

static int image_describe(const void *data, char *buf, size_t size) {
    const Canvas *c = (const Canvas *)data;
    return snprintf(buf, size, "Image(%d x %d)", c->w, c->h);
}

static void window_release(void *data) {
    Win *w = ((WinObj *)data)->w;
    if (!w) return;  /* LCOV_EXCL_LINE: (one made but never filled) */
    if (w->open) {
        w->be->close(w);
        if (active == w) active = NULL;
    }
    free(w->c.px);
    free(w->q);
    free(w);
}

static int window_describe(const void *data, char *buf, size_t size) {
    const Win *w = ((const WinObj *)data)->w;
    return snprintf(buf, size, "Window(%d x %d%s)", w->c.w, w->c.h, w->open ? "" : ", closed");
}

/* The pixels v draws on: an Image's, or a Window's. NULL if it is neither. */
static Canvas *canvas_of(LubeValue v) {
    Canvas *c = (Canvas *)lube->foreign(v, ImageType);
    if (c) return c;
    WinObj *o = (WinObj *)lube->foreign(v, WindowType);
    return o ? &o->w->c : NULL;
}

static Win *window_of(LubeValue v) { return ((WinObj *)lube->foreign(v, WindowType))->w; }

static LubeValue new_image(LubeVM *vm, int w, int h, Canvas **out) {
    void *data;
    LubeValue v = lube->new_foreign(vm, ImageType, &data);
    Canvas *c = (Canvas *)data;
    c->w = w;
    c->h = h;
    size_t n = (size_t)w * (size_t)h;
    c->px = (uint32_t *)calloc(n ? n : 1, sizeof(uint32_t));
    if (!c->px) abort();  /* LCOV_EXCL_LINE: out of memory */
    if (out) *out = c;
    return v;
}

/* Arguments from..to-1 as Numbers into out: 0 after raising an error (in *err). A method's argv[0] is
 * its object (not counted when the message says which argument); a function's is its first argument. */
static int args_of(LubeVM *vm, const char *fn, int method, int argc, const LubeValue *argv, int from, int to, double *out,
                   LubeValue *err) {
    for (int i = from; i < to && i < argc; i++) {
        if (lube->kind(argv[i]) != LUBE_NUMBER) {
            *err = lube->error(vm, "%s() expects a Number as argument %d, got %s", fn, method ? i : i + 1, lube->type_name(argv[i]));
            return 0;
        }
        out[i - from] = lube->number(argv[i]);
    }
    return 1;
}

static int color_of(LubeVM *vm, const char *fn, LubeValue v, uint32_t *out, LubeValue *err) {
    double d = lube->kind(v) == LUBE_NUMBER ? lube->number(v) : -1;
    if (d >= 0 && d <= 4294967295.0 && d == floor(d)) {
        *out = (uint32_t)d;
        return 1;
    }
    char num[64];
    if (lube->kind(v) == LUBE_NUMBER) lube->format_number(d, num, sizeof num);
    *err = lube->error(vm, "%s(): a color is a Number from rgb(...), rgba(...) or color(...), not %s", fn,
                       lube->kind(v) == LUBE_NUMBER ? num : lube->a_type(v));
    return 0;
}

/* A whole Number of pixels (rounded down), within what any image can be. */
static int px(double d) { return d < -1e9 ? -1000000000 : d > 1e9 ? 1000000000 : (int)floor(d); }

static int size_ok(LubeVM *vm, const char *fn, double w, double h, LubeValue *err) {
    if (w >= 1 && h >= 1 && w == floor(w) && h == floor(h) && w <= 16384 && h <= 16384 && w * h <= MAX_PIXELS) return 1;
    *err = lube->error(vm, "%s(): a size is whole Numbers of pixels, from 1 to 16384", fn);
    return 0;
}

/* ======================================================================
 * Colors
 * ==================================================================== */

static uint32_t channel(double v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint32_t)(v + 0.5); }

/* rgb(r, g, b) and rgba(r, g, b, a): each 0 to 255. */
FN(f_rgb) {
    double v[4] = {0, 0, 0, 255};
    LubeValue err;
    if (!args_of(vm, argc == 4 ? "rgba" : "rgb", 0, argc, argv, 0, argc, v, &err)) return err;
    return lube->number_value((double)(channel(v[3]) << 24 | channel(v[0]) << 16 | channel(v[1]) << 8 | channel(v[2])));
}

static const struct {
    const char *name;
    uint32_t value;
} NAMED[] = {
    {"black", 0xFF000000u}, {"white", 0xFFFFFFFFu}, {"red", 0xFFFF0000u}, {"green", 0xFF00FF00u}, {"blue", 0xFF0000FFu},
    {"yellow", 0xFFFFFF00u}, {"cyan", 0xFF00FFFFu}, {"magenta", 0xFFFF00FFu}, {"gray", 0xFF808080u},
    {"orange", 0xFFFFA500u}, {"purple", 0xFF800080u}, {"brown", 0xFF8B4513u}, {"pink", 0xFFFFC0CBu},
    {"transparent", 0x00000000u},
};

static int hex_digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* color("orange"), color("#ff8800"), color("#ff880080"), color("#f80"). */
FN(f_color) {
    (void)argc;
    size_t n;
    const char *s = lube->str(argv[0], &n);
    if (!s) return lube->error(vm, "color() expects a str (a name, or \"#rrggbb\"), got %s", lube->type_name(argv[0]));
    for (size_t i = 0; i < sizeof NAMED / sizeof NAMED[0]; i++)
        if (!strcmp(NAMED[i].name, s)) return lube->number_value(NAMED[i].value);
    if (s[0] == '#' && (n == 4 || n == 7 || n == 9)) {
        uint32_t v = 0;
        int ok = 1;
        for (size_t i = 1; i < n; i++) {
            int d = hex_digit(s[i]);
            if (d < 0) ok = 0;
            v = v << 4 | (uint32_t)(d < 0 ? 0 : d);
            if (n == 4) v = v << 4 | (uint32_t)(d < 0 ? 0 : d);  /* (#f80: each digit twice) */
        }
        if (ok) return lube->number_value(n == 9 ? (double)((v & 0xFF) << 24 | v >> 8) : (double)(0xFF000000u | v));
    }
    return lube->error(vm, "color(): '%s' is not a color: a name (\"orange\"), or \"#rgb\", \"#rrggbb\" or \"#rrggbbaa\"", s);
}

/* text_size(text, scale = 1): [width, height] in pixels. */
FN(f_text_size) {
    size_t n;
    const char *s = lube->str(argv[0], &n);
    if (!s) return lube->error(vm, "text_size() expects a str, got %s", lube->type_name(argv[0]));
    double scale = 1;
    LubeValue err;
    if (!args_of(vm, "text_size", 0, argc, argv, 1, 2, &scale, &err)) return err;
    int w, h;
    text_size(s, n, scale < 1 ? 1 : px(scale), &w, &h);
    LubeValue l = lube->new_list(vm);
    lube->list_push(l, lube->number_value(w));
    lube->list_push(l, lube->number_value(h));
    return l;
}

/* keys(): the names keys have. */
FN(f_keys) {
    (void)argc, (void)argv;
    LubeValue l = lube->new_list(vm);
    for (int i = 1; i < KEY_COUNT; i++) lube->list_push(l, lube->new_str(vm, KEY_NAMES[i], strlen(KEY_NAMES[i])));
    return l;
}

/* ======================================================================
 * Images
 * ==================================================================== */

/* image(width, height, color = transparent). */
FN(f_image) {
    double v[2];
    uint32_t fill = 0;
    LubeValue err;
    if (!args_of(vm, "image", 0, argc, argv, 0, 2, v, &err) || !size_ok(vm, "image", v[0], v[1], &err)) return err;
    if (argc > 2 && !color_of(vm, "image", argv[2], &fill, &err)) return err;
    Canvas *c;
    LubeValue img = new_image(vm, (int)v[0], (int)v[1], &c);
    for (size_t i = 0; i < (size_t)c->w * (size_t)c->h; i++) c->px[i] = fill;
    return img;
}

/* A whole file's bytes (malloc'd), or NULL. */
static unsigned char *read_whole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Out o = {0};
    unsigned char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
        for (size_t i = 0; i < got; i++) out_byte(&o, chunk[i]);
    fclose(f);
    *len = o.len;
    return o.data ? o.data : (unsigned char *)calloc(1, 1);
}

/* load_image(path), or load_image(bytes): a PNG. */
FN(f_load_image) {
    (void)argc;
    const unsigned char *p;
    size_t n;
    unsigned char *file = NULL;
    const char *path = lube->str(argv[0], NULL);
    if (lube->kind(argv[0]) == LUBE_BYTES) {
        p = lube->bytes(argv[0], &n);
    } else if (path) {
        file = read_whole(path, &n);
        if (!file) return lube->error(vm, "load_image(): cannot open '%s': %s", path, strerror(errno));
        p = file;
    } else {
        return lube->error(vm, "load_image() expects a path or a PNG's Bytes, got %s", lube->type_name(argv[0]));
    }
    Canvas got;
    const char *why = png_decode(p, n, &got);
    free(file);
    if (why) return path ? lube->error(vm, "load_image(): '%s' is %s", path, why) : lube->error(vm, "load_image(): these bytes are %s", why);
    void *data;
    LubeValue img = lube->new_foreign(vm, ImageType, &data);
    *(Canvas *)data = got;
    return img;
}

/* ======================================================================
 * Drawing (the methods of Image and Window alike)
 * ==================================================================== */

FN(m_width) {
    (void)vm, (void)argc;
    return lube->number_value(canvas_of(argv[0])->w);
}

FN(m_height) {
    (void)vm, (void)argc;
    return lube->number_value(canvas_of(argv[0])->h);
}

/* x.clear(color): a window black, an image clear, unless told. */
FN(m_clear) {
    Canvas *c = canvas_of(argv[0]);
    uint32_t color = lube->foreign(argv[0], WindowType) ? 0xFF000000u : 0;
    LubeValue err;
    if (argc > 1 && !color_of(vm, "clear", argv[1], &color, &err)) return err;
    for (size_t i = 0; i < (size_t)c->w * (size_t)c->h; i++) c->px[i] = color;
    return lube->nothing();
}

/* x.pixel(x, y): its color (transparent outside). */
FN(m_pixel) {
    Canvas *c = canvas_of(argv[0]);
    double v[2];
    LubeValue err;
    if (!args_of(vm, "pixel", 1, argc, argv, 1, 3, v, &err)) return err;
    int x = px(v[0]), y = px(v[1]);
    return lube->number_value(x < 0 || y < 0 || x >= c->w || y >= c->h ? 0 : c->px[(size_t)y * (size_t)c->w + (size_t)x]);
}

/* x.set_pixel(x, y, color): that pixel becomes the color (no blending). */
FN(m_set_pixel) {
    Canvas *c = canvas_of(argv[0]);
    double v[2];
    uint32_t color;
    LubeValue err;
    if (!args_of(vm, "set_pixel", 1, argc, argv, 1, 3, v, &err) || !color_of(vm, "set_pixel", argv[3], &color, &err)) return err;
    int x = px(v[0]), y = px(v[1]);
    if (x >= 0 && y >= 0 && x < c->w && y < c->h) c->px[(size_t)y * (size_t)c->w + (size_t)x] = color;
    return lube->nothing();
}

/* The shapes: n Number arguments, then a color. */
static int shape_args(LubeVM *vm, const char *fn, int argc, const LubeValue *argv, int n, double *v, uint32_t *color, LubeValue *err) {
    return args_of(vm, fn, 1, argc, argv, 1, 1 + n, v, err) && color_of(vm, fn, argv[1 + n], color, err);
}

FN(m_line) {
    double v[4];
    uint32_t color;
    LubeValue err;
    if (!shape_args(vm, "line", argc, argv, 4, v, &color, &err)) return err;
    line(canvas_of(argv[0]), px(v[0]), px(v[1]), px(v[2]), px(v[3]), color);
    return lube->nothing();
}

FN(m_rect) {
    double v[4];
    uint32_t color;
    LubeValue err;
    if (!shape_args(vm, "rect", argc, argv, 4, v, &color, &err)) return err;
    rect(canvas_of(argv[0]), px(v[0]), px(v[1]), px(v[2]), px(v[3]), color);
    return lube->nothing();
}

FN(m_fill_rect) {
    double v[4];
    uint32_t color;
    LubeValue err;
    if (!shape_args(vm, "fill_rect", argc, argv, 4, v, &color, &err)) return err;
    fill_rect(canvas_of(argv[0]), px(v[0]), px(v[1]), px(v[2]), px(v[3]), color);
    return lube->nothing();
}

FN(m_circle) {
    double v[3];
    uint32_t color;
    LubeValue err;
    if (!shape_args(vm, "circle", argc, argv, 3, v, &color, &err)) return err;
    circle(canvas_of(argv[0]), px(v[0]), px(v[1]), px(v[2]), color);
    return lube->nothing();
}

FN(m_fill_circle) {
    double v[3];
    uint32_t color;
    LubeValue err;
    if (!shape_args(vm, "fill_circle", argc, argv, 3, v, &color, &err)) return err;
    fill_circle(canvas_of(argv[0]), px(v[0]), px(v[1]), px(v[2]), color);
    return lube->nothing();
}

FN(m_fill_triangle) {
    double v[6];
    uint32_t color;
    LubeValue err;
    if (!shape_args(vm, "fill_triangle", argc, argv, 6, v, &color, &err)) return err;
    fill_triangle(canvas_of(argv[0]), v[0], v[1], v[2], v[3], v[4], v[5], color);
    return lube->nothing();
}

/* An option's Number (or def if it is not there); 0 after raising an error. */
static int option(LubeVM *vm, LubeValue opts, const char *name, double def, double *out, LubeValue *err) {
    LubeValue v;
    *out = def;
    if (!lube->object_get(vm, opts, name, &v)) return 1;
    if (lube->kind(v) == LUBE_NUMBER) {
        *out = lube->number(v);
        return 1;
    }
    if (lube->kind(v) == LUBE_BOOL) {
        *out = lube->truthy(v);
        return 1;
    }
    *err = lube->error(vm, "draw(): option %s is a Number, not %s", name, lube->a_type(v));
    return 0;
}

/* x.draw(image, x, y, options): options {part: [x, y, w, h], scale, scale_x, scale_y, flip_x, flip_y,
 * rotate (degrees, clockwise, around its middle), alpha (0 to 1)}. */
FN(m_draw) {
    Canvas *dst = canvas_of(argv[0]), *src = canvas_of(argv[1]);
    if (!src) return lube->error(vm, "draw() expects an Image (or a Window) as argument 1, got %s", lube->type_name(argv[1]));
    double v[2];
    LubeValue err;
    if (!args_of(vm, "draw", 1, argc, argv, 2, 4, v, &err)) return err;
    DrawOptions o;
    o.sx = 0, o.sy = 0, o.sw = src->w, o.sh = src->h;
    o.scale_x = o.scale_y = 1;
    o.angle = 0;
    o.alpha = 1;
    if (argc > 4) {
        LubeValue opts = argv[4];
        if (lube->kind(opts) != LUBE_OBJECT)
            return lube->error(vm, "draw(): its options are an object ({scale: 2, rotate: 90...}), not %s", lube->a_type(opts));
        double scale, sx, sy, fx, fy, rot, alpha;
        if (!option(vm, opts, "scale", 1, &scale, &err) || !option(vm, opts, "scale_x", scale, &sx, &err) ||
            !option(vm, opts, "scale_y", scale, &sy, &err) || !option(vm, opts, "flip_x", 0, &fx, &err) ||
            !option(vm, opts, "flip_y", 0, &fy, &err) || !option(vm, opts, "rotate", 0, &rot, &err) ||
            !option(vm, opts, "alpha", 1, &alpha, &err))
            return err;
        LubeValue part;
        if (lube->object_get(vm, opts, "part", &part)) {
            int ok = lube->kind(part) == LUBE_LIST && lube->list_len(part) == 4;
            double p[4] = {0, 0, 0, 0};
            for (size_t i = 0; ok && i < 4; i++) {
                ok = lube->kind(lube->list_get(part, i)) == LUBE_NUMBER;
                p[i] = lube->number(lube->list_get(part, i));
            }
            if (!ok) return lube->error(vm, "draw(): option part is [x, y, width, height], of the image's pixels");
            o.sx = px(p[0]), o.sy = px(p[1]), o.sw = px(p[2]), o.sh = px(p[3]);
        }
        o.scale_x = fx ? -sx : sx;
        o.scale_y = fy ? -sy : sy;
        o.angle = fmod(rot, 360) * M_PI / 180;
        o.alpha = alpha;
    }
    if (o.sw <= 0 || o.sh <= 0) return lube->nothing();
    if (src == dst) {  /* (onto itself: from a copy) */
        Canvas copy = *src;
        copy.px = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)src->w * (size_t)src->h);
        if (!copy.px) abort();  /* LCOV_EXCL_LINE: out of memory */
        memcpy(copy.px, src->px, sizeof(uint32_t) * (size_t)src->w * (size_t)src->h);
        draw_image(dst, &copy, v[0], v[1], &o);
        free(copy.px);
    } else {
        draw_image(dst, src, v[0], v[1], &o);
    }
    return lube->nothing();
}

/* x.text(text, x, y, color = white, scale = 1). */
FN(m_text) {
    size_t n;
    const char *s = lube->str(argv[1], &n);
    if (!s) return lube->error(vm, "text() expects a str as argument 1, got %s", lube->type_name(argv[1]));
    double v[2], scale = 1;
    uint32_t color = 0xFFFFFFFFu;
    LubeValue err;
    if (!args_of(vm, "text", 1, argc, argv, 2, 4, v, &err)) return err;
    if (argc > 4 && !color_of(vm, "text", argv[4], &color, &err)) return err;
    if (!args_of(vm, "text", 1, argc, argv, 5, 6, &scale, &err)) return err;
    draw_text(canvas_of(argv[0]), s, n, px(v[0]), px(v[1]), color, scale < 1 ? 1 : px(scale > 1000 ? 1000 : scale));
    return lube->nothing();
}

/* x.copy(): a new Image of its pixels (of a window: what it has drawn). */
FN(m_copy) {
    (void)argc;
    const Canvas *c = canvas_of(argv[0]);
    Canvas *out;
    LubeValue img = new_image(vm, c->w, c->h, &out);
    memcpy(out->px, c->px, sizeof(uint32_t) * (size_t)c->w * (size_t)c->h);
    return img;
}

/* x.crop(x, y, w, h): a new Image of that part (transparent where it goes past the edge). */
FN(m_crop) {
    const Canvas *c = canvas_of(argv[0]);
    double v[4];
    LubeValue err;
    if (!args_of(vm, "crop", 1, argc, argv, 1, 5, v, &err) || !size_ok(vm, "crop", v[2], v[3], &err)) return err;
    Canvas *out;
    LubeValue img = new_image(vm, (int)v[2], (int)v[3], &out);
    int x0 = px(v[0]), y0 = px(v[1]);
    for (int y = 0; y < out->h; y++)
        for (int x = 0; x < out->w; x++) {
            int sx = x0 + x, sy = y0 + y;
            if (sx >= 0 && sy >= 0 && sx < c->w && sy < c->h)
                out->px[(size_t)y * (size_t)out->w + (size_t)x] = c->px[(size_t)sy * (size_t)c->w + (size_t)sx];
        }
    return img;
}

/* x.to_png(): a PNG of it, as Bytes. */
FN(m_to_png) {
    (void)argc;
    Out o = {0};
    png_encode(canvas_of(argv[0]), &o);
    LubeValue b = lube->new_bytes(vm, o.data, o.len);
    free(o.data);
    return b;
}

/* x.save_png(path). */
FN(m_save_png) {
    (void)argc;
    const char *path = lube->str(argv[1], NULL);
    if (!path) return lube->error(vm, "save_png() expects a str (a path), got %s", lube->type_name(argv[1]));
    Out o = {0};
    png_encode(canvas_of(argv[0]), &o);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(o.data, 1, o.len, f) == o.len;
    if (f && fclose(f)) ok = 0;  /* LCOV_EXCL_LINE: a full disk */
    free(o.data);
    return ok ? lube->nothing() : lube->error(vm, "save_png(): cannot write '%s': %s", path, strerror(errno));
}

/* ======================================================================
 * Windows
 * ==================================================================== */

/* window(width, height, title = "Lube", scale = 1): one at a time. */
FN(f_window) {
    double v[2], scale = 1;
    LubeValue err;
    if (!args_of(vm, "window", 0, argc, argv, 0, 2, v, &err) || !size_ok(vm, "window", v[0], v[1], &err)) return err;
    const char *title = argc > 2 ? lube->str(argv[2], NULL) : "Lube";
    if (!title) return lube->error(vm, "window() expects a str (its title) as argument 3, got %s", lube->type_name(argv[2]));
    if (!args_of(vm, "window", 0, argc, argv, 3, 4, &scale, &err)) return err;
    if (!(scale >= 1 && scale <= 16)) return lube->error(vm, "window(): its scale is from 1 to 16");
    if (active) return lube->error(vm, "window(): there is a window open already (one at a time: close it first)");
    const char *want = getenv("LUBE_GFX");
    const Backend *be = want && !strcmp(want, "none") ? &NONE_BACKEND : have_host ? &HOST_BACKEND : NULL;
#if GFX_SYSTEM == 1
    if (!be) be = &WIN32_BACKEND;
#elif GFX_SYSTEM == 2
    if (!be) be = &COCOA;
#elif GFX_SYSTEM == 3
    if (!be) be = &X11;
#endif
    if (!be)  /* (a lube built without them, and no host: the tests set LUBE_GFX, or are a host) */
        return lube->error(vm, "window(): this lube has no windows (draw on an image(w, h), or set LUBE_GFX=none)");  /* LCOV_EXCL_LINE */
    Win *w = (Win *)calloc(1, sizeof(Win));
    if (!w) abort();  /* LCOV_EXCL_LINE: out of memory */
    w->c.w = (int)v[0];
    w->c.h = (int)v[1];
    w->c.px = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)w->c.w * (size_t)w->c.h);
    if (!w->c.px) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (size_t i = 0; i < (size_t)w->c.w * (size_t)w->c.h; i++) w->c.px[i] = 0xFF000000u;
    w->be = be;
    w->fps = 60;
    w->next = lube->now();
    w->primary = -1;
    const char *why = be->open(w, title, (int)scale);
    if (why) {  /* LCOV_EXCL_START: (only the system's windows can fail to open: not in the tests' builds) */
        free(w->c.px);
        free(w);
        return lube->error(vm, "window(): %s", why);
    }  /* LCOV_EXCL_STOP */
    w->open = 1;
    active = w;
    void *data;
    LubeValue win = lube->new_foreign(vm, WindowType, &data);
    ((WinObj *)data)->w = w;
    return win;
}

FN(m_is_open) {
    (void)vm, (void)argc;
    return lube->boolean(window_of(argv[0])->open);
}

/* win.close(): it closes (again: nothing). */
FN(m_close) {
    (void)vm, (void)argc;
    Win *w = window_of(argv[0]);
    if (w->open) {
        w->be->close(w);
        w->open = 0;
        if (active == w) active = NULL;
    }
    return lube->nothing();
}

static void event_field(LubeVM *vm, LubeValue o, const char *name, double v) { lube->object_set(vm, o, name, lube->number_value(v)); }

/* win.events(): what happened since it last asked: [{kind: "key_down", key: "left", repeat: false}, ...]. */
FN(m_events) {
    (void)argc;
    Win *w = window_of(argv[0]);
    if (w->open) w->be->pump(w);
    LubeValue l = lube->new_list(vm);
    for (int i = 0; i < w->nq; i++) {
        const Event *e = &w->q[i];
        LubeValue o = lube->new_object(vm);
        lube->object_set(vm, o, "kind", lube->new_str(vm, EVENT_NAMES[e->kind], strlen(EVENT_NAMES[e->kind])));
        switch (e->kind) {
        case LUBE_GFX_KEY_DOWN:
        case LUBE_GFX_KEY_UP:
            lube->object_set(vm, o, "key", lube->new_str(vm, KEY_NAMES[e->key], strlen(KEY_NAMES[e->key])));
            if (e->kind == LUBE_GFX_KEY_DOWN) lube->object_set(vm, o, "repeat", lube->boolean(e->repeat));
            break;
        case LUBE_GFX_TEXT: lube->object_set(vm, o, "text", lube->new_str(vm, e->text, strlen(e->text))); break;
        case LUBE_GFX_WHEEL:
            event_field(vm, o, "dx", e->dx);
            event_field(vm, o, "dy", e->dy);
            break;
        case LUBE_GFX_TOUCH_DOWN:
        case LUBE_GFX_TOUCH_MOVE:
        case LUBE_GFX_TOUCH_UP:
        case LUBE_GFX_MOUSE_MOVE:
        case LUBE_GFX_MOUSE_DOWN:
        case LUBE_GFX_MOUSE_UP:
            if (e->kind >= LUBE_GFX_TOUCH_DOWN) event_field(vm, o, "id", e->id);
            event_field(vm, o, "x", e->x);
            event_field(vm, o, "y", e->y);
            if (e->kind == LUBE_GFX_MOUSE_DOWN || e->kind == LUBE_GFX_MOUSE_UP) event_field(vm, o, "button", e->button);
            break;
        default: break;
        }
        lube->list_push(l, o);
    }
    w->nq = 0;
    return l;
}

/* win.present(): shows what it has drawn, then waits for the next frame (60 a second; set_fps). */
FN(m_present) {
    (void)vm, (void)argc;
    Win *w = window_of(argv[0]);
    if (!w->open) return lube->nothing();
    w->be->present(w);
    if (w->fps > 0) {
        w->next += 1 / w->fps;
        double now = lube->now();
        if (w->next > now) sleep_for(w->next - now);
        else if (now - w->next > 0.25) w->next = now;  /* (fallen behind: catch up, don't rush) */
    }
    return lube->nothing();
}

/* win.key_down(name): whether that key is held down now. */
FN(m_key_down) {
    (void)argc;
    const char *name = lube->str(argv[1], NULL);
    int k = name ? key_named(name) : 0;
    if (!k)
        return lube->error(vm, "key_down(): %s%s%s is not a key's name (\"a\", \"left\", \"space\"...: see keys())",
                           name ? "'" : "", name ? name : lube->a_type(argv[1]), name ? "'" : "");
    return lube->boolean(window_of(argv[0])->held[k]);
}

/* win.mouse(): {x, y, left, right, middle}. */
FN(m_mouse) {
    (void)argc;
    const Win *w = window_of(argv[0]);
    LubeValue o = lube->new_object(vm);
    event_field(vm, o, "x", w->mx);
    event_field(vm, o, "y", w->my);
    lube->object_set(vm, o, "left", lube->boolean(w->buttons & 1));
    lube->object_set(vm, o, "right", lube->boolean(w->buttons & 2));
    lube->object_set(vm, o, "middle", lube->boolean(w->buttons & 4));
    return o;
}

FN(m_set_title) {
    (void)argc;
    const char *title = lube->str(argv[1], NULL);
    if (!title) return lube->error(vm, "set_title() expects a str, got %s", lube->type_name(argv[1]));
    Win *w = window_of(argv[0]);
    if (w->open) w->be->title(w, title);
    return lube->nothing();
}

/* win.set_fps(n): present() keeps to n frames a second (0: no waiting). */
FN(m_set_fps) {
    (void)argc;
    double fps = lube->kind(argv[1]) == LUBE_NUMBER ? lube->number(argv[1]) : -1;
    if (!(fps >= 0 && fps <= 1000)) return lube->error(vm, "set_fps(): frames a second, from 0 (no waiting) to 1000");
    Win *w = window_of(argv[0]);
    w->fps = fps;
    w->next = lube->now();
    return lube->nothing();
}

/* ======================================================================
 * The plugin
 * ==================================================================== */

static void drawing_methods(const LubeAPI *api, LubeType *t) {
    api->method(t, "width", m_width, 1, 1);
    api->method(t, "height", m_height, 1, 1);
    api->method(t, "clear", m_clear, 1, 2);
    api->method(t, "pixel", m_pixel, 3, 3);
    api->method(t, "set_pixel", m_set_pixel, 4, 4);
    api->method(t, "line", m_line, 6, 6);
    api->method(t, "rect", m_rect, 6, 6);
    api->method(t, "fill_rect", m_fill_rect, 6, 6);
    api->method(t, "circle", m_circle, 5, 5);
    api->method(t, "fill_circle", m_fill_circle, 5, 5);
    api->method(t, "fill_triangle", m_fill_triangle, 8, 8);
    api->method(t, "draw", m_draw, 4, 5);
    api->method(t, "text", m_text, 4, 6);
    api->method(t, "copy", m_copy, 1, 1);
    api->method(t, "crop", m_crop, 5, 5);
    api->method(t, "to_png", m_to_png, 1, 1);
    api->method(t, "save_png", m_save_png, 2, 2);
}

LUBE_PLUGIN(gfx) {
    lube = api;
    api->function(plugin, "window", f_window, 2, 4);
    api->function(plugin, "image", f_image, 2, 3);
    api->function(plugin, "load_image", f_load_image, 1, 1);
    api->function(plugin, "rgb", f_rgb, 3, 3);
    api->function(plugin, "rgba", f_rgb, 4, 4);
    api->function(plugin, "color", f_color, 1, 1);
    api->function(plugin, "text_size", f_text_size, 1, 2);
    api->function(plugin, "keys", f_keys, 0, 0);
    ImageType = api->type(plugin, "Image", sizeof(Canvas), image_release, image_describe);
    api->constructor(ImageType, f_image, 2, 3);
    drawing_methods(api, ImageType);
    WindowType = api->type(plugin, "Window", sizeof(WinObj), window_release, window_describe);
    api->constructor(WindowType, f_window, 2, 4);
    drawing_methods(api, WindowType);
    api->method(WindowType, "is_open", m_is_open, 1, 1);
    api->method(WindowType, "close", m_close, 1, 1);
    api->method(WindowType, "events", m_events, 1, 1);
    api->method(WindowType, "present", m_present, 1, 1);
    api->method(WindowType, "key_down", m_key_down, 2, 2);
    api->method(WindowType, "mouse", m_mouse, 1, 1);
    api->method(WindowType, "set_title", m_set_title, 2, 2);
    api->method(WindowType, "set_fps", m_set_fps, 2, 2);
    return LUBE_PLUGIN_API;
}
