/*
 * lube_gfx.h - showing a Lube program's window on a surface of the host
 * program's own: a phone's screen (mobile/android, mobile/ios), a game
 * engine's texture, a test. The gfx plugin must be inside the program
 * (LUBE_STATIC_PLUGINS, or lube_plugin_add("gfx", lube_plugin_init_gfx)).
 *
 *   static void show(void *user, const uint32_t *pixels, int w, int h) { ... }
 *
 *   LubeGfxHost host = {0};
 *   host.present = show;
 *   lube_gfx_set_host(&host);         // window() now opens on it
 *   ...
 *   LubeGfxEvent e = {LUBE_GFX_TOUCH_DOWN};
 *   e.x = 10; e.y = 20;               // in the window's own pixels
 *   lube_gfx_post(&e);                // from any thread
 *
 * The program draws on its window's pixels (the size it asked for);
 * present() hands them to the host, which scales them to its surface as it
 * likes. lube_gfx_fit says how: as large as fits, centered (and so the
 * host can turn a touch on its surface into the window's pixels).
 */
#ifndef LUBE_GFX_H
#define LUBE_GFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *user;  /* given back to each function */
    /* A frame: width * height pixels, rows top first, each 0xAARRGGBB. Called on the program's thread. */
    void (*present)(void *user, const uint32_t *pixels, int width, int height);
    /* Optional: called when the program asks for events and when it presents (a host that must poll its
     * own events on the program's thread does it here: Android's looper). */
    void (*pump)(void *user);
    /* Optional: the program opened its window (its size and title), or closed it. */
    void (*opened)(void *user, int width, int height, const char *title);
    void (*closed)(void *user);
} LubeGfxHost;

enum {
    LUBE_GFX_KEY_DOWN = 1, LUBE_GFX_KEY_UP, LUBE_GFX_TEXT,
    LUBE_GFX_MOUSE_MOVE, LUBE_GFX_MOUSE_DOWN, LUBE_GFX_MOUSE_UP, LUBE_GFX_WHEEL,
    LUBE_GFX_TOUCH_DOWN, LUBE_GFX_TOUCH_MOVE, LUBE_GFX_TOUCH_UP,
    LUBE_GFX_FOCUS, LUBE_GFX_BLUR, LUBE_GFX_CLOSE,
    /* game controllers: id is which (0 to 7) */
    LUBE_GFX_PAD_CONNECTED, LUBE_GFX_PAD_DISCONNECTED, LUBE_GFX_PAD_DOWN, LUBE_GFX_PAD_UP, LUBE_GFX_PAD_AXIS
};

typedef struct {
    int kind;              /* LUBE_GFX_... */
    const char *key;       /* KEY_DOWN, KEY_UP: a key's name, as the program sees it ("a", "left", "space"...);
                            * PAD_DOWN, PAD_UP: a button's ("a", "start", "up"...: see pad_buttons());
                            * PAD_AXIS: an axis's ("left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger") */
    const char *text;      /* TEXT: UTF-8; PAD_CONNECTED: the controller's name */
    double x, y;           /* MOUSE_..., TOUCH_...: in the window's pixels */
    int button;            /* MOUSE_DOWN, MOUSE_UP: 1 left, 2 right, 3 middle */
    int id;                /* TOUCH_...: which finger; PAD_...: which controller */
    double dx, dy;         /* WHEEL */
    int repeat;            /* KEY_DOWN: 1 if the key is held and repeating */
    double value;          /* PAD_AXIS: a stick's -1 to 1 (right, and down, are more), a trigger's 0 to 1 */
} LubeGfxEvent;

void lube_gfx_set_host(const LubeGfxHost *host);  /* (copied; NULL: windows are the system's again) */
void lube_gfx_post(const LubeGfxEvent *event);    /* (copied; any thread) */
/* Where a window of w by h pixels goes on a surface of sw by sh: its top left and its scale. */
void lube_gfx_fit(int w, int h, int sw, int sh, double *x, double *y, double *scale);

#ifdef __cplusplus
}
#endif

#endif
