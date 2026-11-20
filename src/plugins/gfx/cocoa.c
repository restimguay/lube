/*
 * cocoa.c (part of gfx.c) - windows on macOS, with AppKit.
 *
 * Through the Objective-C runtime, from C: no Objective-C to compile and
 * nothing to link, the frameworks opened when the first window opens (a
 * program that only draws images never loads them). Each frame is fitted
 * to the window's size and given to its view's layer as an image.
 */
#include <dlfcn.h>
#include <stdbool.h>

typedef void *id;
typedef void *SEL;
typedef struct {
    double x, y;
} CPoint;
typedef struct {
    double x, y, w, h;
} CRect;

static id (*objc_class)(const char *name);
static SEL (*objc_sel)(const char *name);
static void *objc_send;  /* objc_msgSend: cast to each call's own type */
#if defined(__x86_64__)
static void *objc_send_stret;  /* (on Intel a struct of 32 bytes comes back through memory) */
#endif
static void *(*pool_push)(void);
static void (*pool_pop)(void *pool);
static void *(*cg_rgb)(void);
static void *(*cf_data)(void *allocator, const unsigned char *bytes, long length);
static void *(*cg_provider)(void *data);
static void *(*cg_image)(size_t w, size_t h, size_t bits, size_t pixel_bits, size_t row_bytes, void *space,
                         uint32_t info, void *provider, const double *decode, bool interpolate, int intent);
static void (*cf_release)(void *object);
static id *ca_nearest, *ca_resize, *ns_default_mode;

#define SEND(type) ((type)objc_send)

static id send_id(id o, const char *s) { return SEND(id(*)(id, SEL))(o, objc_sel(s)); }
static void send_void(id o, const char *s) { SEND(void (*)(id, SEL))(o, objc_sel(s)); }
static void send_obj(id o, const char *s, id a) { SEND(void (*)(id, SEL, id))(o, objc_sel(s), a); }
static void send_bool(id o, const char *s, int b) { SEND(void (*)(id, SEL, signed char))(o, objc_sel(s), (signed char)b); }
static int get_bool(id o, const char *s) { return SEND(signed char (*)(id, SEL))(o, objc_sel(s)) != 0; }
static long get_long(id o, const char *s) { return SEND(long (*)(id, SEL))(o, objc_sel(s)); }
static unsigned get_ushort(id o, const char *s) { return SEND(unsigned short (*)(id, SEL))(o, objc_sel(s)); }
static unsigned long get_ulong(id o, const char *s) { return SEND(unsigned long (*)(id, SEL))(o, objc_sel(s)); }
static double get_double(id o, const char *s) { return SEND(double (*)(id, SEL))(o, objc_sel(s)); }
static double get_float(id o, const char *s) { return SEND(float (*)(id, SEL))(o, objc_sel(s)); }
static int responds(id o, const char *s) {
    return SEND(signed char (*)(id, SEL, SEL))(o, objc_sel("respondsToSelector:"), objc_sel(s)) != 0;
}
static CPoint get_point(id o, const char *s) { return SEND(CPoint(*)(id, SEL))(o, objc_sel(s)); }
static CRect get_rect(id o, const char *s) {
#if defined(__x86_64__)
    CRect r;
    ((void (*)(CRect *, id, SEL))objc_send_stret)(&r, o, objc_sel(s));
    return r;
#else
    return SEND(CRect(*)(id, SEL))(o, objc_sel(s));
#endif
}
static id ns_string(const char *utf8) {
    return SEND(id(*)(id, SEL, const char *))(objc_class("NSString"), objc_sel("stringWithUTF8String:"), utf8);
}

static int load_symbol_into(void *lib, const char *name, void *fn, size_t size) {
    void *sym = lib ? dlsym(lib, name) : NULL;
    memcpy(fn, &sym, size);  /* (no ISO C cast from data to code, and no type punning) */
    return sym != NULL;
}
#define SYM(lib, var, name) load_symbol_into((lib), (name), &(var), sizeof(var))

static int cocoa_load(void) {
    static int loaded;
    if (loaded) return loaded > 0;
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
    void *appkit = dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_GLOBAL);
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY | RTLD_GLOBAL);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_GLOBAL);
    void *qc = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_LAZY | RTLD_GLOBAL);
    void *fd = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_LAZY | RTLD_GLOBAL);
    int ok = appkit && SYM(objc, objc_class, "objc_getClass") && SYM(objc, objc_sel, "sel_registerName") &&
             (objc_send = dlsym(objc, "objc_msgSend")) != NULL && SYM(objc, pool_push, "objc_autoreleasePoolPush") &&
             SYM(objc, pool_pop, "objc_autoreleasePoolPop") && SYM(cg, cg_rgb, "CGColorSpaceCreateDeviceRGB") &&
             SYM(cg, cg_provider, "CGDataProviderCreateWithCFData") && SYM(cg, cg_image, "CGImageCreate") &&
             SYM(cf, cf_data, "CFDataCreate") && SYM(cf, cf_release, "CFRelease") &&
             (ca_nearest = (id *)dlsym(qc, "kCAFilterNearest")) != NULL &&
             (ca_resize = (id *)dlsym(qc, "kCAGravityResize")) != NULL &&
             (ns_default_mode = (id *)dlsym(fd, "NSDefaultRunLoopMode")) != NULL;
#if defined(__x86_64__)
    ok = ok && (objc_send_stret = dlsym(objc, "objc_msgSend_stret")) != NULL;
#endif
    loaded = ok ? 1 : -1;
    return ok;
}

typedef struct {
    id app, win, view, layer;
    void *space;
    uint32_t *buf;         /* the frame, fitted to the view */
    int bw, bh;
    int focused;
    unsigned long flags;   /* the modifier keys down */
    int fs_want;           /* fullscreen as last asked for, */
    double fs_since;       /* since when (0: not on its way: the window's own says) */
    int cursor_hidden;
    id pads[MAX_PADS];     /* each controller's GCController, or NULL */
} Cocoa;

#define FULLSCREEN_MASK (1ul << 14)  /* (NSWindowStyleMaskFullScreen) */

/* macOS's key codes (kVK_...), as keys. */
static int mac_key(unsigned code) {
    static const unsigned char letters[] = {
        /* 0x00 */ 'a', 's', 'd', 'f', 'h', 'g', 'z', 'x', 'c', 'v', 0, 'b', 'q', 'w', 'e', 'r',
        /* 0x10 */ 'y', 't', '1', '2', '3', '4', '6', '5', '=', '9', '7', '-', '8', '0', ']', 'o',
        /* 0x20 */ 'u', '[', 'i', 'p', 0, 'l', 'j', '\'', 'k', ';', '\\', ',', '/', 'n', 'm', '.',
    };
    if (code < sizeof letters && letters[code]) {
        int c = letters[code];
        if (c >= 'a' && c <= 'z') return K_A + (c - 'a');
        if (c >= '0' && c <= '9') return K_0 + (c - '0');
        switch (c) {
        case '=': return K_EQUALS;
        case '-': return K_MINUS;
        case ']': return K_RIGHT_BRACKET;
        case '[': return K_LEFT_BRACKET;
        case '\'': return K_QUOTE;
        case ';': return K_SEMICOLON;
        case '\\': return K_BACKSLASH;
        case ',': return K_COMMA;
        case '/': return K_SLASH;
        default: return K_PERIOD;  /* '.' */
        }
    }
    switch (code) {
    case 0x24: return K_ENTER;
    case 0x30: return K_TAB;
    case 0x31: return K_SPACE;
    case 0x32: return K_BACKQUOTE;
    case 0x33: return K_BACKSPACE;
    case 0x35: return K_ESCAPE;
    case 0x37: case 0x36: return K_COMMAND;
    case 0x38: case 0x3C: return K_SHIFT;
    case 0x39: return K_CAPS_LOCK;
    case 0x3A: case 0x3D: return K_ALT;
    case 0x3B: case 0x3E: return K_CONTROL;
    case 0x7A: return K_F1;
    case 0x78: return K_F1 + 1;
    case 0x63: return K_F1 + 2;
    case 0x76: return K_F1 + 3;
    case 0x60: return K_F1 + 4;
    case 0x61: return K_F1 + 5;
    case 0x62: return K_F1 + 6;
    case 0x64: return K_F1 + 7;
    case 0x65: return K_F1 + 8;
    case 0x6D: return K_F1 + 9;
    case 0x67: return K_F1 + 10;
    case 0x6F: return K_F1 + 11;
    case 0x72: return K_INSERT;
    case 0x73: return K_HOME;
    case 0x74: return K_PAGE_UP;
    case 0x75: return K_DELETE;
    case 0x77: return K_END;
    case 0x79: return K_PAGE_DOWN;
    case 0x7B: return K_LEFT;
    case 0x7C: return K_RIGHT;
    case 0x7D: return K_DOWN;
    case 0x7E: return K_UP;
    default: return 0;
    }
}

static const char *cocoa_open(Win *w, const char *title, int scale) {
    if (!pthread_main_np()) return "a window opens on the program's main thread (on macOS), not in an isolate";
    if (!cocoa_load()) return "cannot load AppKit";
    void *pool = pool_push();
    Cocoa *s = (Cocoa *)calloc(1, sizeof(Cocoa));
    if (!s) abort();
    s->app = send_id(objc_class("NSApplication"), "sharedApplication");
    static int launched;
    if (!launched) {  /* (a program from a terminal: a regular app, with a Dock icon, that can have the keyboard) */
        SEND(void (*)(id, SEL, long))(s->app, objc_sel("setActivationPolicy:"), 0);
        send_void(s->app, "finishLaunching");
        launched = 1;
    }
    CRect r = {0, 0, (double)w->c.w * scale, (double)w->c.h * scale};
    s->win = SEND(id(*)(id, SEL, CRect, unsigned long, unsigned long, signed char))(
        send_id(objc_class("NSWindow"), "alloc"), objc_sel("initWithContentRect:styleMask:backing:defer:"), r,
        1 | 2 | 4 | 8 /* titled, closable, miniaturizable, resizable */, 2 /* buffered */, 0);
    send_bool(s->win, "setReleasedWhenClosed:", 0);
    SEND(void (*)(id, SEL, unsigned long))(s->win, objc_sel("setCollectionBehavior:"), 1ul << 7);  /* (can be fullscreen) */
    send_obj(s->win, "setTitle:", ns_string(title));
    send_obj(s->win, "setBackgroundColor:", send_id(objc_class("NSColor"), "blackColor"));
    send_void(s->win, "center");
    s->view = send_id(s->win, "contentView");
    send_bool(s->view, "setWantsLayer:", 1);
    s->layer = send_id(s->view, "layer");
    send_obj(s->layer, "setMagnificationFilter:", *ca_nearest);
    send_obj(s->layer, "setContentsGravity:", *ca_resize);
    send_obj(s->win, "makeKeyAndOrderFront:", NULL);
    send_bool(s->app, "activateIgnoringOtherApps:", 1);
    s->space = cg_rgb();
    w->sys = s;
    pool_pop(pool);
    return NULL;
}

static void cocoa_mouse(Win *w, id ev, int kind, int button) {
    Cocoa *s = (Cocoa *)w->sys;
    CPoint p = get_point(ev, "locationInWindow");
    CRect b = get_rect(s->view, "bounds");
    Event e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.button = button;
    to_window(w, p.x, b.h - p.y, (int)b.w, (int)b.h, &e.x, &e.y);  /* (Cocoa's y goes up) */
    push_event(w, &e);
}

/* ---- game controllers: GameController's, asked for their state each time ---- */

static unsigned gc_pressed(id parent, const char *element) {  /* (some elements are newer than some macOS) */
    id e = parent && responds(parent, element) ? send_id(parent, element) : NULL;
    return e && get_bool(e, "isPressed") ? 1u : 0u;
}

static double gc_value(id parent, const char *element, const char *axis) {
    id e = send_id(parent, element);
    if (e && axis) e = send_id(e, axis);
    return e ? get_float(e, "value") : 0;
}

static void cocoa_pads(Win *w) {
    static int loaded;  /* (GameController: opened when the first window asks) */
    if (!loaded)
        loaded = dlopen("/System/Library/Frameworks/GameController.framework/GameController", RTLD_LAZY | RTLD_GLOBAL) &&
                         objc_class("GCController") ? 1 : -1;
    if (loaded < 0) return;
    Cocoa *s = (Cocoa *)w->sys;
    id list = send_id(objc_class("GCController"), "controllers");
    unsigned long n = list ? get_ulong(list, "count") : 0;
    int seen[MAX_PADS] = {0};
    for (unsigned long k = 0; k < n; k++) {
        id c = SEND(id(*)(id, SEL, unsigned long))(list, objc_sel("objectAtIndex:"), k);
        id g = send_id(c, "extendedGamepad");
        if (!g) continue;  /* (a remote: not a game controller's buttons) */
        int i = 0;
        while (i < MAX_PADS && s->pads[i] != c) i++;
        if (i == MAX_PADS) {  /* a new one: the first free number */
            for (i = 0; i < MAX_PADS && s->pads[i]; i++) continue;
            if (i == MAX_PADS) continue;
            s->pads[i] = c;
            id vendor = send_id(c, "vendorName");
            pad_connect(w, i, vendor ? (const char *)send_id(vendor, "UTF8String") : NULL);
        }
        seen[i] = 1;
        id dpad = send_id(g, "dpad");
        unsigned b = gc_pressed(g, "buttonA") << P_A | gc_pressed(g, "buttonB") << P_B | gc_pressed(g, "buttonX") << P_X |
                     gc_pressed(g, "buttonY") << P_Y | gc_pressed(g, "leftShoulder") << P_LEFT_SHOULDER |
                     gc_pressed(g, "rightShoulder") << P_RIGHT_SHOULDER | gc_pressed(g, "buttonOptions") << P_BACK |
                     gc_pressed(g, "buttonMenu") << P_START | gc_pressed(g, "buttonHome") << P_GUIDE |
                     gc_pressed(g, "leftThumbstickButton") << P_LEFT_STICK | gc_pressed(g, "rightThumbstickButton") << P_RIGHT_STICK |
                     gc_pressed(dpad, "up") << P_UP | gc_pressed(dpad, "down") << P_DOWN | gc_pressed(dpad, "left") << P_LEFT |
                     gc_pressed(dpad, "right") << P_RIGHT;
        double axes[AXIS_COUNT] = {gc_value(g, "leftThumbstick", "xAxis"), -gc_value(g, "leftThumbstick", "yAxis"),  /* (up is more) */
                                   gc_value(g, "rightThumbstick", "xAxis"), -gc_value(g, "rightThumbstick", "yAxis"),
                                   gc_value(g, "leftTrigger", NULL), gc_value(g, "rightTrigger", NULL)};
        pad_state(w, i, b, axes);
    }
    for (int i = 0; i < MAX_PADS; i++)
        if (s->pads[i] && !seen[i]) {
            s->pads[i] = NULL;
            pad_disconnect(w, i);
        }
}

static void cocoa_pump(Win *w) {
    Cocoa *s = (Cocoa *)w->sys;
    void *pool = pool_push();
    id past = send_id(objc_class("NSDate"), "distantPast");
    for (;;) {
        id ev = SEND(id(*)(id, SEL, unsigned long long, id, id, signed char))(
            s->app, objc_sel("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0ULL, past, *ns_default_mode, 1);
        if (!ev) break;
        long type = get_long(ev, "type");
        int pass = 1;  /* (to AppKit too: dragging the window, its buttons) */
        Event e;
        memset(&e, 0, sizeof e);
        switch (type) {
        case 10:  /* key down */
        case 11: {  /* key up */
            pass = 0;  /* (else AppKit beeps: nothing in the window takes keys) */
            e.kind = type == 10 ? LUBE_GFX_KEY_DOWN : LUBE_GFX_KEY_UP;
            e.key = mac_key(get_ushort(ev, "keyCode"));
            e.repeat = type == 10 && get_bool(ev, "isARepeat");
            if (e.key) push_event(w, &e);
            unsigned long mods = get_ulong(ev, "modifierFlags");
            if (type == 10 && !(mods & (1ul << 20))) {  /* the text it types (not with command held) */
                const char *t = (const char *)send_id(send_id(ev, "characters"), "UTF8String");
                if (t && (unsigned char)t[0] >= 32 && t[0] != 127 && !((unsigned char)t[0] == 0xEF && (unsigned char)t[1] >= 0x9C && (unsigned char)t[1] <= 0x9F)) {
                    Event te;  /* (U+F700 to U+F8FF: the arrow and function keys) */
                    memset(&te, 0, sizeof te);
                    te.kind = LUBE_GFX_TEXT;
                    set_text(&te, t);
                    push_event(w, &te);
                }
            }
            break;
        }
        case 12: {  /* a modifier key: down or up, from the flags */
            unsigned code = get_ushort(ev, "keyCode");
            unsigned long flags = get_ulong(ev, "modifierFlags"), mask = 0;
            switch (code) {
            case 0x38: case 0x3C: mask = 1ul << 17; break;
            case 0x3B: case 0x3E: mask = 1ul << 18; break;
            case 0x3A: case 0x3D: mask = 1ul << 19; break;
            case 0x37: case 0x36: mask = 1ul << 20; break;
            case 0x39: mask = 1ul << 16; break;
            default: break;
            }
            if (mask) {
                e.kind = flags & mask ? LUBE_GFX_KEY_DOWN : LUBE_GFX_KEY_UP;
                e.key = mac_key(code);
                push_event(w, &e);
            }
            s->flags = flags;
            break;
        }
        case 1: cocoa_mouse(w, ev, LUBE_GFX_MOUSE_DOWN, 1); break;
        case 2: cocoa_mouse(w, ev, LUBE_GFX_MOUSE_UP, 1); break;
        case 3: cocoa_mouse(w, ev, LUBE_GFX_MOUSE_DOWN, 2); break;
        case 4: cocoa_mouse(w, ev, LUBE_GFX_MOUSE_UP, 2); break;
        case 25: cocoa_mouse(w, ev, LUBE_GFX_MOUSE_DOWN, 3); break;
        case 26: cocoa_mouse(w, ev, LUBE_GFX_MOUSE_UP, 3); break;
        case 5: case 6: case 7: case 27: cocoa_mouse(w, ev, LUBE_GFX_MOUSE_MOVE, 0); break;
        case 22:  /* the wheel (or two fingers on a trackpad) */
            e.kind = LUBE_GFX_WHEEL;
            e.dx = get_double(ev, "scrollingDeltaX");
            e.dy = get_double(ev, "scrollingDeltaY");
            if (get_bool(ev, "hasPreciseScrollingDeltas")) {  /* (a trackpad's are in points: about a tenth of a step) */
                e.dx /= 10;
                e.dy /= 10;
            }
            if (e.dx != 0 || e.dy != 0) push_event(w, &e);
            break;
        default: break;
        }
        if (pass) send_obj(s->app, "sendEvent:", ev);
    }
    send_void(s->app, "updateWindows");
    if (!get_bool(s->win, "isVisible")) {  /* (its close button: it hid) */
        pool_pop(pool);
        closed_by_user(w);
        return;
    }
    int focused = get_bool(s->win, "isKeyWindow");
    if (focused != s->focused) {
        s->focused = focused;
        push_simple(w, focused ? LUBE_GFX_FOCUS : LUBE_GFX_BLUR, 0, 0, 0);
    }
    int fs = (get_ulong(s->win, "styleMask") & FULLSCREEN_MASK) != 0;  /* (the green button changes it too) */
    if (s->fs_since && (fs == s->fs_want || lube->now() - s->fs_since > 3)) s->fs_since = 0;  /* (there, or never coming) */
    if (!s->fs_since) w->fullscreen = fs;
    cocoa_pads(w);
    pool_pop(pool);
}

static void cocoa_fullscreen(Win *w, int on) {
    Cocoa *s = (Cocoa *)w->sys;
    void *pool = pool_push();
    if (((get_ulong(s->win, "styleMask") & FULLSCREEN_MASK) != 0) != on) send_obj(s->win, "toggleFullScreen:", NULL);
    s->fs_want = on;
    s->fs_since = lube->now();  /* (it takes a moment: the screen slides) */
    pool_pop(pool);
}

static void cocoa_cursor(Win *w, int shown) {
    Cocoa *s = (Cocoa *)w->sys;
    if (shown != s->cursor_hidden) return;
    send_void(objc_class("NSCursor"), shown ? "unhide" : "hide");  /* (counted: hidden once, shown once) */
    s->cursor_hidden = !shown;
}

static void cocoa_present(Win *w) {
    Cocoa *s = (Cocoa *)w->sys;
    cocoa_pump(w);  /* (a window that is shown but never asked for its events still answers its buttons) */
    if (!w->open) return;
    void *pool = pool_push();
    CRect b = get_rect(s->view, "bounds");
    int bw = (int)b.w, bh = (int)b.h;
    if (bw > 0 && bh > 0) {
        if (bw != s->bw || bh != s->bh) {
            free(s->buf);
            s->buf = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)bw * (size_t)bh);
            if (!s->buf) abort();
            s->bw = bw;
            s->bh = bh;
        }
        scale_into(&w->c, s->buf, bw, bh);
        void *data = cf_data(NULL, (const unsigned char *)s->buf, (long)bw * bh * 4);
        void *provider = cg_provider(data);
        void *image = cg_image((size_t)bw, (size_t)bh, 8, 32, (size_t)bw * 4, s->space,
                               (2u << 12) | 6u /* 32-bit little-endian, the top byte unused */, provider, NULL, 0, 0);
        send_obj(s->layer, "setContents:", (id)image);
        cf_release(image);
        cf_release(provider);
        cf_release(data);
        send_void(objc_class("CATransaction"), "flush");  /* (shown now: this program does not return to a run loop) */
    }
    pool_pop(pool);
}

static void cocoa_close(Win *w) {
    Cocoa *s = (Cocoa *)w->sys;
    void *pool = pool_push();
    if (s->cursor_hidden) send_void(objc_class("NSCursor"), "unhide");
    send_void(s->win, "close");
    send_void(s->win, "release");
    cf_release(s->space);
    pool_pop(pool);
    free(s->buf);
    free(s);
    w->sys = NULL;
}

static void cocoa_title(Win *w, const char *title) {
    void *pool = pool_push();
    send_obj(((Cocoa *)w->sys)->win, "setTitle:", ns_string(title));
    pool_pop(pool);
}

static const Backend COCOA = {cocoa_open, cocoa_pump, cocoa_present, cocoa_close, cocoa_title, cocoa_fullscreen, cocoa_cursor};
