/*
 * main.c - a Lube program as an Android app (a NativeActivity: no Java).
 *
 * It copies the files in assets/ (mobile/game/) to the app's own storage,
 * runs main.lube there, and shows its window (gfx) on the screen: as large
 * as fits, in whole pixels where it can, black around it. Touches go to the
 * program as touch events (the first finger as the mouse too), keys of a
 * keyboard as key events, the back button as "escape". What it prints goes
 * to the log (adb logcat -s lube). When the program ends, so does the app.
 *
 * The program runs on android_main's thread; whenever it presents a frame
 * or asks for its events, this thread also takes the system's (the looper).
 * While the app is in the background (no surface), the program waits there.
 */
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lube_embed.h"
#include "lube_gfx.h"

#define TAG "lube"

static struct android_app *app;
static int win_w, win_h;  /* the program's window (0 when none is open) */

/* ---- what the program prints: to the log, a line at a time ---- */

static char line[2][1024];  /* (standard output's, and errors') */
static size_t line_len[2];

static void flush_line(int is_error) {
    line[is_error][line_len[is_error]] = '\0';
    __android_log_write(is_error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, TAG, line[is_error]);
    line_len[is_error] = 0;
}

static void output(const char *text, size_t len, int is_error, void *data) {
    (void)data;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            flush_line(is_error);
            continue;
        }
        if (line_len[is_error] == sizeof line[0] - 1) flush_line(is_error);  /* (a long one: in pieces) */
        line[is_error][line_len[is_error]++] = text[i];
    }
}

/* ---- the system's events: commands, touches, keys ---- */

static void post_simple(int kind) {
    LubeGfxEvent e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    lube_gfx_post(&e);
}

static void post_touch(int kind, const AInputEvent *ev, size_t i) {
    if (!win_w || !app->window) return;
    double ox, oy, s;  /* (the screen's pixels into the window's: as present() fits it) */
    lube_gfx_fit(win_w, win_h, ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window), &ox, &oy, &s);
    LubeGfxEvent e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.id = AMotionEvent_getPointerId(ev, i);
    e.x = (AMotionEvent_getX(ev, i) - ox) / s;
    e.y = (AMotionEvent_getY(ev, i) - oy) / s;
    lube_gfx_post(&e);
}

/* A key's name as Lube has it; NULL for the system's own (volume...). */
static const char *key_name(int32_t code) {
    static const char *const letters[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                                          "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
    static const char *const digits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    static const char *const fkeys[] = {"f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12"};
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) return letters[code - AKEYCODE_A];
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) return digits[code - AKEYCODE_0];
    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12) return fkeys[code - AKEYCODE_F1];
    switch (code) {
    case AKEYCODE_SPACE: return "space";
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: case AKEYCODE_DPAD_CENTER: return "enter";
    case AKEYCODE_BACK: case AKEYCODE_ESCAPE: return "escape";
    case AKEYCODE_TAB: return "tab";
    case AKEYCODE_DEL: return "backspace";
    case AKEYCODE_FORWARD_DEL: return "delete";
    case AKEYCODE_INSERT: return "insert";
    case AKEYCODE_MOVE_HOME: return "home";
    case AKEYCODE_MOVE_END: return "end";
    case AKEYCODE_PAGE_UP: return "page_up";
    case AKEYCODE_PAGE_DOWN: return "page_down";
    case AKEYCODE_DPAD_LEFT: return "left";
    case AKEYCODE_DPAD_RIGHT: return "right";
    case AKEYCODE_DPAD_UP: return "up";
    case AKEYCODE_DPAD_DOWN: return "down";
    case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: return "shift";
    case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT: return "control";
    case AKEYCODE_ALT_LEFT: case AKEYCODE_ALT_RIGHT: return "alt";
    case AKEYCODE_META_LEFT: case AKEYCODE_META_RIGHT: return "command";
    case AKEYCODE_CAPS_LOCK: return "caps_lock";
    case AKEYCODE_MINUS: return "minus";
    case AKEYCODE_EQUALS: return "equals";
    case AKEYCODE_LEFT_BRACKET: return "left_bracket";
    case AKEYCODE_RIGHT_BRACKET: return "right_bracket";
    case AKEYCODE_SEMICOLON: return "semicolon";
    case AKEYCODE_APOSTROPHE: return "quote";
    case AKEYCODE_COMMA: return "comma";
    case AKEYCODE_PERIOD: return "period";
    case AKEYCODE_SLASH: return "slash";
    case AKEYCODE_BACKSLASH: return "backslash";
    case AKEYCODE_GRAVE: return "backquote";
    default: return NULL;
    }
}

static int32_t on_input(struct android_app *a, AInputEvent *ev) {
    (void)a;
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
        int32_t action = AMotionEvent_getAction(ev);
        size_t index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        size_t count = AMotionEvent_getPointerCount(ev);
        switch (action & AMOTION_EVENT_ACTION_MASK) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN: post_touch(LUBE_GFX_TOUCH_DOWN, ev, index); break;
        case AMOTION_EVENT_ACTION_MOVE:
            for (size_t i = 0; i < count; i++) post_touch(LUBE_GFX_TOUCH_MOVE, ev, i);
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP: post_touch(LUBE_GFX_TOUCH_UP, ev, index); break;
        case AMOTION_EVENT_ACTION_CANCEL:  /* (every finger: lifted) */
            for (size_t i = 0; i < count; i++) post_touch(LUBE_GFX_TOUCH_UP, ev, i);
            break;
        default: break;
        }
        return 1;
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) {
        const char *name = key_name(AKeyEvent_getKeyCode(ev));
        int32_t action = AKeyEvent_getAction(ev);
        if (!name) return 0;
        if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) return 1;
        LubeGfxEvent e;
        memset(&e, 0, sizeof e);
        e.kind = action == AKEY_EVENT_ACTION_DOWN ? LUBE_GFX_KEY_DOWN : LUBE_GFX_KEY_UP;
        e.key = name;
        e.repeat = AKeyEvent_getRepeatCount(ev) > 0;
        lube_gfx_post(&e);
        if (e.kind == LUBE_GFX_KEY_DOWN && (name[1] == '\0' || !strcmp(name, "space"))) {
            char typed[2] = {strcmp(name, "space") ? name[0] : ' ', '\0'};  /* (letters, digits, space) */
            if (typed[0] >= 'a' && typed[0] <= 'z' && (AKeyEvent_getMetaState(ev) & AMETA_SHIFT_ON)) typed[0] -= 32;
            memset(&e, 0, sizeof e);
            e.kind = LUBE_GFX_TEXT;
            e.text = typed;
            lube_gfx_post(&e);
        }
        return 1;
    }
    return 0;
}

static void on_command(struct android_app *a, int32_t command) {
    switch (command) {
    case APP_CMD_INIT_WINDOW:  /* the screen's surface: in the window's colors, 4 bytes a pixel */
        ANativeWindow_setBuffersGeometry(a->window, 0, 0, WINDOW_FORMAT_RGBX_8888);
        break;
    case APP_CMD_GAINED_FOCUS: post_simple(LUBE_GFX_FOCUS); break;
    case APP_CMD_LOST_FOCUS: post_simple(LUBE_GFX_BLUR); break;
    case APP_CMD_DESTROY: post_simple(LUBE_GFX_CLOSE); break;
    default: break;
    }
}

/* The system's events: waiting up to wait_ms for the first (-1: as long as it takes). */
static void poll_events(int wait_ms) {
    for (;;) {
        int events;
        struct android_poll_source *source = NULL;
        int got = ALooper_pollOnce(wait_ms, NULL, &events, (void **)&source);
        if (got == ALOOPER_POLL_TIMEOUT || got == ALOOPER_POLL_ERROR) return;
        if (source) source->process(app, source);
        wait_ms = 0;
    }
}

/* ---- the program's window, on the screen ---- */

static void opened(void *user, int width, int height, const char *title) {
    (void)user, (void)title;
    win_w = width;
    win_h = height;
}

static void closed(void *user) {
    (void)user;
    win_w = win_h = 0;
}

static void pump(void *user) {
    (void)user;
    poll_events(0);
    while (!app->window && !app->destroyRequested) poll_events(-1);  /* (in the background: the program waits) */
}

static void present(void *user, const uint32_t *pixels, int width, int height) {
    pump(user);
    ANativeWindow_Buffer buf;
    if (!app->window || ANativeWindow_lock(app->window, &buf, NULL) != 0) return;
    if (buf.format != WINDOW_FORMAT_RGBX_8888 && buf.format != WINDOW_FORMAT_RGBA_8888) {
        ANativeWindow_unlockAndPost(app->window);
        return;
    }
    double ox, oy, s;
    lube_gfx_fit(width, height, buf.width, buf.height, &ox, &oy, &s);
    int *from_x = (int *)malloc(sizeof(int) * (size_t)buf.width);  /* each column's pixel of the window, or -1 */
    if (!from_x) {
        ANativeWindow_unlockAndPost(app->window);
        return;
    }
    for (int x = 0; x < buf.width; x++) {
        int sx = (int)floor((x - ox) / s);
        from_x[x] = sx >= 0 && sx < width ? sx : -1;
    }
    for (int y = 0; y < buf.height; y++) {
        uint32_t *row = (uint32_t *)buf.bits + (size_t)y * (size_t)buf.stride;
        int sy = (int)floor((y - oy) / s);
        const uint32_t *src = sy >= 0 && sy < height ? pixels + (size_t)sy * (size_t)width : NULL;
        for (int x = 0; x < buf.width; x++) {
            uint32_t p = src && from_x[x] >= 0 ? src[from_x[x]] : 0;  /* 0xAARRGGBB, to bytes R, G, B, A */
            row[x] = 0xFF000000u | (p & 0xFF00u) | (p >> 16 & 0xFFu) | (p & 0xFFu) << 16;
        }
    }
    free(from_x);
    ANativeWindow_unlockAndPost(app->window);
}

/* ---- the program's files, and the program ---- */

/* The files at the top of assets/ into dir (each time it starts: the app's own, as built). */
static void copy_assets(const char *dir) {
    AAssetManager *assets = app->activity->assetManager;
    AAssetDir *top = AAssetManager_openDir(assets, "");
    const char *name;
    while ((name = AAssetDir_getNextFileName(top)) != NULL) {
        AAsset *asset = AAssetManager_open(assets, name, AASSET_MODE_STREAMING);
        if (!asset) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            char chunk[16384];
            int n;
            while ((n = AAsset_read(asset, chunk, sizeof chunk)) > 0) fwrite(chunk, 1, (size_t)n, f);
            fclose(f);
        } else {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot write %s", path);
        }
        AAsset_close(asset);
    }
    AAssetDir_close(top);
}

void android_main(struct android_app *a) {
    app = a;
    a->onAppCmd = on_command;
    a->onInputEvent = on_input;
    lube_set_output(output, NULL);
    while (!a->window && !a->destroyRequested) poll_events(-1);  /* (the screen first) */
    if (!a->destroyRequested) {
        const char *dir = a->activity->internalDataPath;
        mkdir(dir, 0700);
        copy_assets(dir);
        if (chdir(dir) != 0) __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot go to %s", dir);
        LubeGfxHost host;
        memset(&host, 0, sizeof host);
        host.present = present;
        host.pump = pump;
        host.opened = opened;
        host.closed = closed;
        lube_gfx_set_host(&host);
        LubeVM *vm = lube_open();
        if (lube_run_script(vm, "main.lube") == LUBE_ERROR) output(lube_error_message(vm), strlen(lube_error_message(vm)), 1, NULL);
        for (int i = 0; i < 2; i++)
            if (line_len[i]) flush_line(i);  /* (the last lines, if they had no end) */
        lube_close(vm);
        lube_gfx_set_host(NULL);
        ANativeActivity_finish(a->activity);
    }
    while (!a->destroyRequested) poll_events(-1);
}
