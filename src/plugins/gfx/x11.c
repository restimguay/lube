/*
 * x11.c (part of gfx.c) - windows on Linux, the BSDs and a Raspberry Pi's
 * desktop, with X11 (Wayland desktops run X programs too).
 *
 * libX11, opened when the first window opens, so that building Lube needs no
 * X headers and a server without X still loads the plugin. Its types, the
 * few used here, are declared here (they have not changed in decades). Each
 * frame is fitted to the window's size and put with XPutImage.
 */
#include <dlfcn.h>

typedef struct XDisplay Display;
typedef struct XVisual Visual;
typedef struct XGCValues *GC;
typedef unsigned long XID;

typedef struct {  /* XKeyEvent, XButtonEvent and XMotionEvent alike */
    int type;
    unsigned long serial;
    int send_event;
    Display *display;
    XID window, root, subwindow;
    unsigned long time;
    int x, y, x_root, y_root;
    unsigned int state, code;  /* (the key's code, or the button) */
    int same_screen;
} XInput;

typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    Display *display;
    XID event, window;
    int x, y, width, height, border_width;
    XID above;
    int override_redirect;
} XConfigure;

typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    Display *display;
    XID window, message_type;
    int format;
    union {
        char b[20];
        short s[10];
        long l[5];
    } data;
} XClientMessage;

typedef union {
    int type;
    XInput input;
    XConfigure configure;
    XClientMessage client;
    long pad[24];
} XEvent;

typedef struct XImage {
    int width, height, xoffset, format;
    char *data;
    int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth, bytes_per_line, bits_per_pixel;
    unsigned long red_mask, green_mask, blue_mask;
    char *obdata;
    struct {
        void *create_image;
        int (*destroy_image)(struct XImage *image);
        void *get_pixel, *put_pixel, *sub_image, *add_pixel;
    } f;
} XImage;

static Display *(*x_open)(const char *name);
static int (*x_screen)(Display *d);
static XID (*x_root)(Display *d, int screen);
static Visual *(*x_visual)(Display *d, int screen);
static int (*x_depth)(Display *d, int screen);
static unsigned long (*x_black)(Display *d, int screen);
static XID (*x_create)(Display *d, XID parent, int x, int y, unsigned w, unsigned h, unsigned border, unsigned long bc, unsigned long bg);
static int (*x_store_name)(Display *d, XID w, const char *name);
static int (*x_change_property)(Display *d, XID w, XID property, XID type, int format, int mode, const unsigned char *data, int n);
static int (*x_select)(Display *d, XID w, long mask);
static XID (*x_atom)(Display *d, const char *name, int only_if_exists);
static int (*x_protocols)(Display *d, XID w, XID *atoms, int n);
static int (*x_map)(Display *d, XID w);
static GC (*x_create_gc)(Display *d, XID drawable, unsigned long mask, void *values);
static int (*x_free_gc)(Display *d, GC gc);
static XImage *(*x_create_image)(Display *d, Visual *v, unsigned depth, int format, int offset, char *data, unsigned w,
                                 unsigned h, int pad, int bytes_per_line);
static int (*x_put_image)(Display *d, XID drawable, GC gc, XImage *image, int sx, int sy, int dx, int dy, unsigned w, unsigned h);
static int (*x_pending)(Display *d);
static int (*x_next_event)(Display *d, XEvent *e);
static int (*x_lookup_string)(XInput *e, char *buf, int bytes, unsigned long *keysym, void *status);
static unsigned long (*x_lookup_keysym)(XInput *e, int index);
static int (*x_flush)(Display *d);
static int (*x_destroy)(Display *d, XID w);
static int (*x_close)(Display *d);
static int (*x_detectable_repeat)(Display *d, int detectable, int *supported);

static int x_symbol(void *lib, const char *name, void *fn, size_t size) {
    void *sym = dlsym(lib, name);
    memcpy(fn, &sym, size);  /* (no ISO C cast from data to code, and no type punning) */
    return sym != NULL;
}
#define XSYM(var, name) x_symbol(lib, (name), &(var), sizeof(var))

static int x_load(void) {
    static int loaded;
    if (loaded) return loaded > 0;
    void *lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    int ok = lib && XSYM(x_open, "XOpenDisplay") && XSYM(x_screen, "XDefaultScreen") && XSYM(x_root, "XRootWindow") &&
             XSYM(x_visual, "XDefaultVisual") && XSYM(x_depth, "XDefaultDepth") && XSYM(x_black, "XBlackPixel") &&
             XSYM(x_create, "XCreateSimpleWindow") && XSYM(x_store_name, "XStoreName") &&
             XSYM(x_change_property, "XChangeProperty") && XSYM(x_select, "XSelectInput") && XSYM(x_atom, "XInternAtom") &&
             XSYM(x_protocols, "XSetWMProtocols") && XSYM(x_map, "XMapWindow") && XSYM(x_create_gc, "XCreateGC") &&
             XSYM(x_free_gc, "XFreeGC") && XSYM(x_create_image, "XCreateImage") && XSYM(x_put_image, "XPutImage") &&
             XSYM(x_pending, "XPending") && XSYM(x_next_event, "XNextEvent") && XSYM(x_lookup_string, "XLookupString") &&
             XSYM(x_lookup_keysym, "XLookupKeysym") && XSYM(x_flush, "XFlush") && XSYM(x_destroy, "XDestroyWindow") &&
             XSYM(x_close, "XCloseDisplay") && XSYM(x_detectable_repeat, "XkbSetDetectableAutoRepeat");
    loaded = ok ? 1 : -1;
    return ok;
}

typedef struct {
    Display *d;
    XID win, wm_delete, net_name, utf8;
    GC gc;
    Visual *visual;
    int depth, sw, sh;
    XImage *image;  /* the frame, fitted to the window */
} X;

/* An X keysym, as a key. */
static int x_key(unsigned long k) {
    if (k >= 'a' && k <= 'z') return K_A + (int)(k - 'a');
    if (k >= 'A' && k <= 'Z') return K_A + (int)(k - 'A');
    if (k >= '0' && k <= '9') return K_0 + (int)(k - '0');
    if (k >= 0xffbe && k <= 0xffc9) return K_F1 + (int)(k - 0xffbe);
    switch (k) {
    case ' ': return K_SPACE;
    case 0xff0d: case 0xff8d: return K_ENTER;
    case 0xff1b: return K_ESCAPE;
    case 0xff09: return K_TAB;
    case 0xff08: return K_BACKSPACE;
    case 0xffff: return K_DELETE;
    case 0xff63: return K_INSERT;
    case 0xff50: return K_HOME;
    case 0xff57: return K_END;
    case 0xff55: return K_PAGE_UP;
    case 0xff56: return K_PAGE_DOWN;
    case 0xff51: return K_LEFT;
    case 0xff52: return K_UP;
    case 0xff53: return K_RIGHT;
    case 0xff54: return K_DOWN;
    case 0xffe1: case 0xffe2: return K_SHIFT;
    case 0xffe3: case 0xffe4: return K_CONTROL;
    case 0xffe9: case 0xffea: case 0xffe7: case 0xffe8: return K_ALT;
    case 0xffeb: case 0xffec: return K_COMMAND;
    case 0xffe5: return K_CAPS_LOCK;
    case '-': return K_MINUS;
    case '=': return K_EQUALS;
    case '[': return K_LEFT_BRACKET;
    case ']': return K_RIGHT_BRACKET;
    case ';': return K_SEMICOLON;
    case '\'': return K_QUOTE;
    case ',': return K_COMMA;
    case '.': return K_PERIOD;
    case '/': return K_SLASH;
    case '\\': return K_BACKSLASH;
    case '`': return K_BACKQUOTE;
    default: return 0;
    }
}

static const char *x11_open(Win *w, const char *title, int scale) {
    if (!x_load()) return "cannot load libX11 (no X here: draw on an image(w, h), or set LUBE_GFX=none)";
    X *s = (X *)calloc(1, sizeof(X));
    if (!s) abort();
    s->d = x_open(NULL);
    if (!s->d) {
        free(s);
        return "cannot reach a display (is DISPLAY set? or draw on an image(w, h), or set LUBE_GFX=none)";
    }
    int screen = x_screen(s->d);
    s->visual = x_visual(s->d, screen);
    s->depth = x_depth(s->d, screen);
    s->sw = w->c.w * scale;
    s->sh = w->c.h * scale;
    XImage *probe = x_create_image(s->d, s->visual, (unsigned)s->depth, 2 /* ZPixmap */, 0, NULL, 1, 1, 32, 0);
    int ok = probe && probe->bits_per_pixel == 32 && probe->red_mask == 0xFF0000 && probe->green_mask == 0xFF00 &&
             probe->blue_mask == 0xFF;
    if (probe) probe->f.destroy_image(probe);
    if (!ok) {
        x_close(s->d);
        free(s);
        return "this display's pixels are not 24-bit color (as nearly every display's are)";
    }
    unsigned long black = x_black(s->d, screen);
    s->win = x_create(s->d, x_root(s->d, screen), 0, 0, (unsigned)s->sw, (unsigned)s->sh, 0, black, black);
    x_select(s->d, s->win, 1L | 2L | 4L | 8L | (1L << 6) | (1L << 15) | (1L << 17) | (1L << 21));
    /* (keys, buttons, motion, exposure, structure, focus) */
    s->wm_delete = x_atom(s->d, "WM_DELETE_WINDOW", 0);
    x_protocols(s->d, s->win, &s->wm_delete, 1);
    s->net_name = x_atom(s->d, "_NET_WM_NAME", 0);
    s->utf8 = x_atom(s->d, "UTF8_STRING", 0);
    x_store_name(s->d, s->win, title);
    x_change_property(s->d, s->win, s->net_name, s->utf8, 8, 0, (const unsigned char *)title, (int)strlen(title));
    x_detectable_repeat(s->d, 1, NULL);  /* (a held key: presses, not press and release over and over) */
    s->gc = x_create_gc(s->d, s->win, 0, NULL);
    x_map(s->d, s->win);
    x_flush(s->d);
    w->sys = s;
    return NULL;
}

static void x11_mouse(Win *w, const XInput *in, int kind, int button) {
    X *s = (X *)w->sys;
    Event e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.button = button;
    to_window(w, in->x, in->y, s->sw, s->sh, &e.x, &e.y);
    push_event(w, &e);
}

static void x11_blit(Win *w);

static void x11_pump(Win *w) {
    X *s = (X *)w->sys;
    while (w->open && x_pending(s->d)) {
        XEvent ev;
        x_next_event(s->d, &ev);
        Event e;
        memset(&e, 0, sizeof e);
        switch (ev.type) {
        case 2:  /* key press */
        case 3: {  /* key release */
            e.kind = ev.type == 2 ? LUBE_GFX_KEY_DOWN : LUBE_GFX_KEY_UP;
            e.key = x_key(x_lookup_keysym(&ev.input, 0));
            e.repeat = ev.type == 2 && e.key && w->held[e.key];
            if (e.key) push_event(w, &e);
            if (ev.type == 2) {
                char buf[16];
                unsigned long sym;
                int n = x_lookup_string(&ev.input, buf, (int)sizeof buf - 1, &sym, NULL);
                unsigned char c = n == 1 ? (unsigned char)buf[0] : 0;
                if (c >= 32 && c != 127) {  /* (Latin-1, as UTF-8) */
                    Event te;
                    memset(&te, 0, sizeof te);
                    te.kind = LUBE_GFX_TEXT;
                    if (c < 128) {
                        te.text[0] = (char)c;
                    } else {
                        te.text[0] = (char)(0xC0 | (c >> 6));
                        te.text[1] = (char)(0x80 | (c & 63));
                    }
                    push_event(w, &te);
                }
            }
            break;
        }
        case 4:  /* button press */
        case 5: {  /* button release */
            unsigned b = ev.input.code;
            if (b >= 4 && b <= 7) {  /* the wheel: a press for each step */
                if (ev.type == 4) {
                    e.kind = LUBE_GFX_WHEEL;
                    e.dy = b == 4 ? 1 : b == 5 ? -1 : 0;
                    e.dx = b == 6 ? -1 : b == 7 ? 1 : 0;
                    push_event(w, &e);
                }
            } else if (b >= 1 && b <= 3) {
                x11_mouse(w, &ev.input, ev.type == 4 ? LUBE_GFX_MOUSE_DOWN : LUBE_GFX_MOUSE_UP, b == 1 ? 1 : b == 3 ? 2 : 3);
            }
            break;
        }
        case 6: x11_mouse(w, &ev.input, LUBE_GFX_MOUSE_MOVE, 0); break;
        case 9: push_simple(w, LUBE_GFX_FOCUS, 0, 0, 0); break;
        case 10: push_simple(w, LUBE_GFX_BLUR, 0, 0, 0); break;
        case 12: x11_blit(w); break;  /* exposed: the last frame again */
        case 22:
            s->sw = ev.configure.width;
            s->sh = ev.configure.height;
            break;
        case 33:
            if ((XID)ev.client.data.l[0] == s->wm_delete) closed_by_user(w);
            break;
        default: break;
        }
    }
}

static void x11_blit(Win *w) {
    X *s = (X *)w->sys;
    if (s->sw < 1 || s->sh < 1) return;
    if (!s->image || s->image->width != s->sw || s->image->height != s->sh) {
        if (s->image) s->image->f.destroy_image(s->image);  /* (its pixels too) */
        char *data = (char *)malloc(sizeof(uint32_t) * (size_t)s->sw * (size_t)s->sh);
        if (!data) abort();
        s->image = x_create_image(s->d, s->visual, (unsigned)s->depth, 2, 0, data, (unsigned)s->sw, (unsigned)s->sh, 32, 0);
        if (!s->image) {
            free(data);
            return;
        }
    }
    scale_into(&w->c, (uint32_t *)(void *)s->image->data, s->sw, s->sh);
    x_put_image(s->d, s->win, s->gc, s->image, 0, 0, 0, 0, (unsigned)s->sw, (unsigned)s->sh);
    x_flush(s->d);
}

static void x11_present(Win *w) {
    x11_pump(w);  /* (a window that is shown but never asked for its events still closes) */
    if (w->open) x11_blit(w);
}

static void x11_close(Win *w) {
    X *s = (X *)w->sys;
    if (s->image) s->image->f.destroy_image(s->image);
    x_free_gc(s->d, s->gc);
    x_destroy(s->d, s->win);
    x_close(s->d);
    free(s);
    w->sys = NULL;
}

static void x11_title(Win *w, const char *title) {
    X *s = (X *)w->sys;
    x_store_name(s->d, s->win, title);
    x_change_property(s->d, s->win, s->net_name, s->utf8, 8, 0, (const unsigned char *)title, (int)strlen(title));
    x_flush(s->d);
}

static const Backend X11 = {x11_open, x11_pump, x11_present, x11_close, x11_title};
