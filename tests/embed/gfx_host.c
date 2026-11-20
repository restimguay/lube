/*
 * gfx_host.c - a program that shows Lube's windows itself (lube_gfx.h), as the
 * phone templates in mobile/ do: it is given each frame's pixels, and gives the
 * Lube program its input (here, made up: a few events each time it is asked).
 * tests/cli.sh builds it with Lube's sources and runs it.
 */
#include "lube_embed.h"
#include "lube_gfx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *SCRIPT =
    "import gfx.*\n"
    "fn show(w) { for e in w.events() { print(' ', e) } }\n"
    "let w = window(8, 6, 'Hosted', 2)\n"
    "w.set_fps(0)\n"
    "w.clear(rgb(10, 20, 30))\n"
    "w.set_pixel(4, 3, color('white'))\n"
    "w.present()\n"
    "print('keys, text, the mouse, the wheel:')\n"
    "show(w)\n"
    "print(w.key_down('left'), w.key_down('a'), w.mouse())\n"
    "print('let go of; fingers (the first also the mouse):')\n"
    "show(w)\n"
    "print(w.key_down('left'), w.mouse())\n"
    "print('held, then away (every key let go of):')\n"
    "show(w)\n"
    "print(w.key_down('right'))\n"
    "print('back, a key no one knows, a long text:')\n"
    "show(w)\n"
    "print('5000 at once:')\n"
    "let many = w.events()\n"
    "print(len(many), many[0].x, many[len(many) - 1].x)\n"
    "print('controllers:')\n"
    "show(w)\n"
    "let p = w.pad(0)\n"
    "print(w.pads(), p.connected, p.name, p.a, p.b, p.start, p.left_x, round(p.left_y * 1000) / 1000, p.right_x, p.right_y,\n"
    "      p.left_trigger, p.right_trigger)\n"
    "print(w.pad(2))\n"
    "print('controllers: let go of, past their ends, and one gone:')\n"
    "show(w)\n"
    "let q = w.pad(0)\n"
    "print(w.pads(), q.a, round(q.left_x * 1000) / 1000, round(q.left_y * 1000) / 1000, q.right_x, q.right_y, q.left_trigger,\n"
    "      q.right_trigger, w.pad(1).connected, w.pad(3).name)\n"
    "print('closed by the host:')\n"
    "show(w)\n"
    "print(w.is_open(), w)\n"
    "w.present()\n"
    "print(w.events())\n"
    "let again = window(2, 2)\n"  /* (another: the host still showing it) */
    "print(again.events())\n"
    "again.close()\n";

static int frames, pumps;

static void opened(void *user, int width, int height, const char *title) {
    printf("[host] opened %dx%d '%s' (%s)\n", width, height, title, (const char *)user);
}

static void present(void *user, const uint32_t *pixels, int width, int height) {
    (void)user;
    printf("[host] frame %d: %dx%d, corner %08x, middle %08x\n", ++frames, width, height, (unsigned)pixels[0],
           (unsigned)pixels[(height / 2) * width + width / 2]);
}

static void closed(void *user) {
    (void)user;
    printf("[host] closed\n");
}

static void post(int kind, const char *key, const char *text, double x, double y, int button, int id) {
    LubeGfxEvent e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.key = key;
    e.text = text;
    e.x = x;
    e.y = y;
    e.button = button;
    e.id = id;
    if (kind == LUBE_GFX_WHEEL) e.dx = x, e.dy = y;
    lube_gfx_post(&e);
}

static void post_pad(int kind, int id, const char *key, const char *text, double value) {
    LubeGfxEvent e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.id = id;
    e.key = key;
    e.text = text;
    e.value = value;
    lube_gfx_post(&e);
}

static void repeating(const char *key) {  /* (held down: again) */
    LubeGfxEvent e;
    memset(&e, 0, sizeof e);
    e.kind = LUBE_GFX_KEY_DOWN;
    e.key = key;
    e.repeat = 1;
    lube_gfx_post(&e);
}

/* What has happened since the program last asked: a little more each time. */
static void pump(void *user) {
    (void)user;
    switch (++pumps) {
    case 1:
        post(LUBE_GFX_KEY_DOWN, "left", NULL, 0, 0, 0, 0);
        post(LUBE_GFX_TEXT, NULL, "\xc3\xa9", 0, 0, 0, 0);
        post(LUBE_GFX_MOUSE_MOVE, NULL, NULL, 3.5, 2, 0, 0);
        post(LUBE_GFX_MOUSE_DOWN, NULL, NULL, 3.5, 2, 2, 0);
        post(LUBE_GFX_MOUSE_DOWN, NULL, NULL, 3.5, 2, 9, 0);  /* (a button past 3: the left) */
        post(LUBE_GFX_WHEEL, NULL, NULL, 0, -1, 0, 0);
        post(0, NULL, NULL, 0, 0, 0, 0);   /* (kinds Lube doesn't know: left out) */
        post(99, NULL, NULL, 0, 0, 0, 0);
        break;
    case 2:
        post(LUBE_GFX_KEY_UP, "left", NULL, 0, 0, 0, 0);
        post(LUBE_GFX_MOUSE_UP, NULL, NULL, 4, 2, 2, 0);
        post(LUBE_GFX_TOUCH_DOWN, NULL, NULL, 1, 1, 0, 7);
        post(LUBE_GFX_TOUCH_DOWN, NULL, NULL, 6, 5, 0, 8);
        post(LUBE_GFX_TOUCH_MOVE, NULL, NULL, 2, 1, 0, 7);
        post(LUBE_GFX_TOUCH_MOVE, NULL, NULL, 6, 4, 0, 8);
        post(LUBE_GFX_TOUCH_UP, NULL, NULL, 6, 4, 0, 8);
        post(LUBE_GFX_TOUCH_UP, NULL, NULL, 2, 1, 0, 7);
        break;
    case 3:
        post(LUBE_GFX_KEY_DOWN, "right", NULL, 0, 0, 0, 0);
        repeating("right");
        post(LUBE_GFX_BLUR, NULL, NULL, 0, 0, 0, 0);  /* (and every key let go of) */
        break;
    case 4:
        post(LUBE_GFX_FOCUS, NULL, NULL, 0, 0, 0, 0);
        post(LUBE_GFX_KEY_DOWN, "no-such-key", NULL, 0, 0, 0, 0);  /* (keys without a name: left out) */
        post(LUBE_GFX_KEY_UP, NULL, NULL, 0, 0, 0, 0);
        post(LUBE_GFX_TEXT, NULL, "abcdefghijklm\xc3\xa9\xc3\xa9", 0, 0, 0, 0);  /* (cut before the é that doesn't fit) */
        break;
    case 5:  /* (more than a program that never asks keeps: the latest) */
        for (int i = 0; i < 5000; i++) post(LUBE_GFX_MOUSE_MOVE, NULL, NULL, i, 0, 0, 0);
        break;
    case 6:  /* game controllers */
        post_pad(LUBE_GFX_PAD_CONNECTED, 0, NULL, "Test Pad", 0);
        post_pad(LUBE_GFX_PAD_DOWN, 0, "a", NULL, 0);
        post_pad(LUBE_GFX_PAD_DOWN, 0, "a", NULL, 0);              /* (down already: nothing) */
        post_pad(LUBE_GFX_PAD_AXIS, 0, "left_y", NULL, 0.6);       /* (from 0.2, its dead middle, to 1: 0.5) */
        post_pad(LUBE_GFX_PAD_AXIS, 0, "right_x", NULL, 0.1);      /* (in the dead middle: 0) */
        post_pad(LUBE_GFX_PAD_AXIS, 0, "right_y", NULL, 0.1);
        post_pad(LUBE_GFX_PAD_AXIS, 0, "left_trigger", NULL, 0.7);  /* (past half way: down) */
        post_pad(LUBE_GFX_PAD_AXIS, 0, "left_trigger", NULL, 0.45); /* (not below 0.4: still down) */
        post_pad(LUBE_GFX_PAD_DOWN, 0, "right_trigger", NULL, 0);  /* (a trigger as a button: all the way) */
        post_pad(LUBE_GFX_PAD_DOWN, 1, "start", NULL, 0);          /* (one not said to be there: it is now) */
        post_pad(LUBE_GFX_PAD_DOWN, 0, "no-such-button", NULL, 0); /* (names Lube doesn't know, and numbers it has no room for: left out) */
        post_pad(LUBE_GFX_PAD_DOWN, 0, NULL, NULL, 0);
        post_pad(LUBE_GFX_PAD_AXIS, 0, "no-such-axis", NULL, 1);
        post_pad(LUBE_GFX_PAD_AXIS, 0, NULL, NULL, 1);
        post_pad(LUBE_GFX_PAD_DOWN, 8, "a", NULL, 0);
        post_pad(LUBE_GFX_PAD_DOWN, -1, "a", NULL, 0);
        break;
    case 7:
        post_pad(LUBE_GFX_PAD_UP, 0, "a", NULL, 0);
        post_pad(LUBE_GFX_PAD_AXIS, 0, "left_trigger", NULL, -3);  /* (below 0: 0, and let go of) */
        post_pad(LUBE_GFX_PAD_UP, 0, "right_trigger", NULL, 0);
        post_pad(LUBE_GFX_PAD_AXIS, 0, "left_x", NULL, -5);        /* (past the end: -1; the stick no more than 1 out) */
        post_pad(LUBE_GFX_PAD_AXIS, 0, "right_x", NULL, 2);
        post_pad(LUBE_GFX_PAD_AXIS, 0, "right_y", NULL, nan(""));  /* (not a number: 0) */
        post_pad(LUBE_GFX_PAD_CONNECTED, 0, NULL, "Again", 0);     /* (there already: nothing) */
        post_pad(LUBE_GFX_PAD_DISCONNECTED, 1, NULL, NULL, 0);
        post_pad(LUBE_GFX_PAD_DISCONNECTED, 1, NULL, NULL, 0);     /* (gone already: nothing) */
        post_pad(LUBE_GFX_PAD_CONNECTED, 2, NULL, NULL, 0);        /* (no name: "controller") */
        post_pad(LUBE_GFX_PAD_CONNECTED, 3, NULL, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\xc3\xa9", 0);  /* (cut before the é) */
        break;
    case 8:
        post(LUBE_GFX_CLOSE, NULL, NULL, 0, 0, 0, 0);
        post(LUBE_GFX_KEY_DOWN, "a", NULL, 0, 0, 0, 0);  /* (after it closed: nothing) */
        break;
    default:
        post(LUBE_GFX_KEY_DOWN, "b", NULL, 0, 0, 0, 0);
        break;
    }
}

int main(void) {
    double x, y, scale;  /* fitting a window into a screen: whole pixels while they fit, in the middle */
    lube_gfx_fit(320, 180, 1920, 1080, &x, &y, &scale);
    printf("fit 320x180 into 1920x1080: at %g,%g, %g times\n", x, y, scale);
    lube_gfx_fit(320, 180, 1000, 1000, &x, &y, &scale);
    printf("fit 320x180 into 1000x1000: at %g,%g, %g times\n", x, y, scale);
    lube_gfx_fit(400, 400, 300, 200, &x, &y, &scale);
    printf("fit 400x400 into 300x200: at %g,%g, %g times\n", x, y, scale);
    lube_gfx_fit(0, 0, 300, 200, &x, &y, &scale);
    printf("fit nothing into 300x200: at %g,%g, %g times\n", x, y, scale);

    LubeGfxHost host;
    memset(&host, 0, sizeof host);
    host.user = (void *)"the host's own";
    host.present = present;
    host.pump = pump;
    host.opened = opened;
    host.closed = closed;
    lube_gfx_set_host(&host);
    post(LUBE_GFX_KEY_DOWN, "z", NULL, 0, 0, 0, 0);  /* (before any window: not for one) */

    LubeVM *vm = lube_open();
    int status = lube_run(vm, SCRIPT, "hosted");
    if (status != LUBE_OK) printf("failed: %s\n", lube_error_message(vm));
    lube_close(vm);

    memset(&host, 0, sizeof host);  /* a host that takes nothing: windows still work */
    lube_gfx_set_host(&host);
    vm = lube_open();
    status = lube_run(vm,
                      "import gfx\n"
                      "let w = gfx.window(3, 3)\n"
                      "w.present()\n"
                      "print(w.events(), w.is_open())\n"
                      "w.close()\n",
                      "bare");
    if (status != LUBE_OK) printf("failed: %s\n", lube_error_message(vm));
    lube_close(vm);
    lube_gfx_set_host(NULL);
    return 0;
}
