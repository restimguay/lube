/*
 * main.c - a Lube program as an Android app (a NativeActivity: no Java).
 *
 * It copies the files in assets/ (mobile/game/, folders and all) to the
 * app's own storage, runs main.lube there, and shows its window (gfx) on the
 * screen: as large as fits, in whole pixels where it can, black around it.
 * Touches go to the program as touch events (the first finger as the mouse
 * too), keys of a keyboard as key events, the back button as "escape", game
 * controllers as controllers. What it prints goes to the log (adb logcat -s
 * lube). When the program ends, so does the app.
 *
 * The program runs on android_main's thread; whenever it presents a frame
 * or asks for its events, this thread also takes the system's (the looper).
 * While the app is in the background (no surface), the program waits there.
 */
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lube_embed.h"
#include "lube_gfx.h"

#define TAG "lube"

static struct android_app *app;
static int win_w, win_h;  /* the program's window (0 when none is open) */

/* Java's side, for what the NDK does not say: this thread, joined to the VM when first asked. (Each start of the
 * app is a thread of its own, even in the same process: android_main forgets the last one's.) */
static JNIEnv *env_now;

static JNIEnv *jni(void) {
    if (!env_now && (*app->activity->vm)->AttachCurrentThread(app->activity->vm, &env_now, NULL) != JNI_OK) env_now = NULL;
    return env_now;
}

/* (a Java exception: cleared, and so failed) */
static int jni_failed(JNIEnv *env) {
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionClear(env);
    return 1;
}

static void jni_string(JNIEnv *env, jstring s, char *out, size_t size) {
    out[0] = '\0';
    const char *p = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
    if (!p) return;
    snprintf(out, size, "%s", p);
    (*env)->ReleaseStringUTFChars(env, s, p);
}

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

/* ---- game controllers: each device's number, its buttons and axes ---- */

#define MAX_PADS 8
static struct {
    int used;
    int32_t device;          /* Android's id for it */
    double axes[6];          /* as last posted */
    int hat;                 /* the d-pad as an axis: 1 left, 2 right, 4 up, 8 down */
    int analog_triggers;     /* (then its L2 and R2 keys are left out: the axes say it) */
} pads[MAX_PADS];
static const char *const AXES[] = {"left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger"};

static void post_pad(int kind, int i, const char *key, const char *text, double value) {
    LubeGfxEvent e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.id = i;
    e.key = key;
    e.text = text;
    e.value = value;
    lube_gfx_post(&e);
}

/* What Android calls a device ("Xbox Wireless Controller"), and whether it is a game controller. */
static int device_info(JNIEnv *env, int32_t device, char *name, size_t size) {
    name[0] = '\0';
    if (!env || (*env)->PushLocalFrame(env, 8) != 0) return 0;
    int is_pad = 0;
    jclass c = (*env)->FindClass(env, "android/view/InputDevice");
    jmethodID get = c ? (*env)->GetStaticMethodID(env, c, "getDevice", "(I)Landroid/view/InputDevice;") : NULL;
    jobject d = get ? (*env)->CallStaticObjectMethod(env, c, get, device) : NULL;
    if (!jni_failed(env) && d) {
        jmethodID sources = (*env)->GetMethodID(env, c, "getSources", "()I");
        jmethodID get_name = (*env)->GetMethodID(env, c, "getName", "()Ljava/lang/String;");
        jint s = sources ? (*env)->CallIntMethod(env, d, sources) : 0;
        jstring n = !jni_failed(env) && get_name ? (jstring)(*env)->CallObjectMethod(env, d, get_name) : NULL;
        if (!jni_failed(env)) jni_string(env, n, name, size);
        is_pad = (s & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (s & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
    }
    jni_failed(env);
    (*env)->PopLocalFrame(env, NULL);
    return is_pad;
}

/* The number of a device's controller (a new one: the first free, said to be connected); -1 if there is no room. */
static int pad_of(int32_t device) {
    int i, free_slot = -1;
    for (i = 0; i < MAX_PADS; i++) {
        if (pads[i].used && pads[i].device == device) return i;
        if (!pads[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return -1;
    memset(&pads[free_slot], 0, sizeof pads[free_slot]);
    pads[free_slot].used = 1;
    pads[free_slot].device = device;
    char name[128];
    device_info(jni(), device, name, sizeof name);
    post_pad(LUBE_GFX_PAD_CONNECTED, free_slot, NULL, name, 0);
    return free_slot;
}

/* Every second: controllers plugged in (there before a button is pressed), and those gone. */
static void scan_pads(void) {
    JNIEnv *env = jni();
    if (!env || (*env)->PushLocalFrame(env, 8) != 0) return;
    jclass c = (*env)->FindClass(env, "android/view/InputDevice");
    jmethodID ids_of = c ? (*env)->GetStaticMethodID(env, c, "getDeviceIds", "()[I") : NULL;
    jintArray list = ids_of ? (jintArray)(*env)->CallStaticObjectMethod(env, c, ids_of) : NULL;
    if (jni_failed(env) || !list) {
        (*env)->PopLocalFrame(env, NULL);
        return;
    }
    jsize n = (*env)->GetArrayLength(env, list);
    jint ids[64];
    if (n > 64) n = 64;
    (*env)->GetIntArrayRegion(env, list, 0, n, ids);
    (*env)->PopLocalFrame(env, NULL);
    char name[128];
    for (jsize k = 0; k < n; k++)
        if (device_info(env, ids[k], name, sizeof name)) pad_of(ids[k]);
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i].used) continue;
        int there = 0;
        for (jsize k = 0; k < n; k++) there |= ids[k] == pads[i].device;
        if (!there) {
            pads[i].used = 0;
            post_pad(LUBE_GFX_PAD_DISCONNECTED, i, NULL, NULL, 0);
        }
    }
}

/* A controller's button as Lube names it; NULL for other keys. */
static const char *pad_button(int32_t code) {
    switch (code) {
    case AKEYCODE_BUTTON_A: return "a";
    case AKEYCODE_BUTTON_B: return "b";
    case AKEYCODE_BUTTON_X: return "x";
    case AKEYCODE_BUTTON_Y: return "y";
    case AKEYCODE_BUTTON_L1: return "left_shoulder";
    case AKEYCODE_BUTTON_R1: return "right_shoulder";
    case AKEYCODE_BUTTON_L2: return "left_trigger";
    case AKEYCODE_BUTTON_R2: return "right_trigger";
    case AKEYCODE_BUTTON_SELECT: return "back";
    case AKEYCODE_BUTTON_START: return "start";
    case AKEYCODE_BUTTON_MODE: return "guide";
    case AKEYCODE_BUTTON_THUMBL: return "left_stick";
    case AKEYCODE_BUTTON_THUMBR: return "right_stick";
    case AKEYCODE_DPAD_UP: return "up";
    case AKEYCODE_DPAD_DOWN: return "down";
    case AKEYCODE_DPAD_LEFT: return "left";
    case AKEYCODE_DPAD_RIGHT: return "right";
    default: return NULL;
    }
}

/* A controller's key: 1 if it was one. */
static int pad_key(const AInputEvent *ev) {
    int32_t code = AKeyEvent_getKeyCode(ev), action = AKeyEvent_getAction(ev);
    int from_pad = (AInputEvent_getSource(ev) & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD;
    const char *name = pad_button(code);
    if (!name || (!from_pad && code >= AKEYCODE_DPAD_UP && code <= AKEYCODE_DPAD_RIGHT)) return 0;  /* (a keyboard's arrows) */
    int i = pad_of(AInputEvent_getDeviceId(ev));
    if (i < 0 || (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP)) return 1;
    if ((code == AKEYCODE_BUTTON_L2 || code == AKEYCODE_BUTTON_R2) && pads[i].analog_triggers) return 1;
    post_pad(action == AKEY_EVENT_ACTION_DOWN ? LUBE_GFX_PAD_DOWN : LUBE_GFX_PAD_UP, i, name, NULL, 0);
    return 1;
}

/* A controller's sticks, triggers and d-pad (when it has a d-pad as an axis). */
static void pad_motion(const AInputEvent *ev) {
    int i = pad_of(AInputEvent_getDeviceId(ev));
    if (i < 0) return;
    float lt = fmaxf(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_LTRIGGER, 0), AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_BRAKE, 0));
    float rt = fmaxf(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RTRIGGER, 0), AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_GAS, 0));
    double axes[6] = {AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0), AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0),
                      AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Z, 0), AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RZ, 0),
                      lt, rt};  /* (the right stick: Z and RZ, as Android has it) */
    if (lt > 0 || rt > 0) pads[i].analog_triggers = 1;
    for (int a = 0; a < 6; a++)
        if (axes[a] != pads[i].axes[a]) {
            pads[i].axes[a] = axes[a];
            post_pad(LUBE_GFX_PAD_AXIS, i, AXES[a], NULL, axes[a]);
        }
    float hx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0);
    int hat = (hx < -0.5f) | (hx > 0.5f) << 1 | (hy < -0.5f) << 2 | (hy > 0.5f) << 3;
    static const char *const dirs[] = {"left", "right", "up", "down"};
    for (int b = 0; b < 4; b++)
        if ((hat ^ pads[i].hat) >> b & 1) post_pad(hat >> b & 1 ? LUBE_GFX_PAD_DOWN : LUBE_GFX_PAD_UP, i, dirs[b], NULL, 0);
    pads[i].hat = hat;
}

static int32_t on_input(struct android_app *a, AInputEvent *ev) {
    (void)a;
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION && (AInputEvent_getSource(ev) & AINPUT_SOURCE_CLASS_JOYSTICK)) {
        pad_motion(ev);
        return 1;
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY && pad_key(ev)) return 1;  /* (handled: no "back" made of B) */
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

static double pads_scanned;

static void opened(void *user, int width, int height, const char *title) {
    (void)user, (void)title;
    win_w = width;
    win_h = height;
    memset(pads, 0, sizeof pads);  /* (a new window: every controller said to be there again, at once) */
    pads_scanned = -10;
}

static void closed(void *user) {
    (void)user;
    win_w = win_h = 0;
}

static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void pump(void *user) {
    (void)user;
    if (seconds() - pads_scanned >= 1) {
        pads_scanned = seconds();
        scan_pads();
    }
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

/* The app's own file (its APK): where it is. */
static int apk_path(char *out, size_t size) {
    JNIEnv *env = jni();
    out[0] = '\0';
    if (!env || (*env)->PushLocalFrame(env, 4) != 0) return 0;
    jobject activity = app->activity->clazz;
    jmethodID m = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, activity), "getPackageCodePath", "()Ljava/lang/String;");
    jstring s = !jni_failed(env) && m ? (jstring)(*env)->CallObjectMethod(env, activity, m) : NULL;
    if (!jni_failed(env)) jni_string(env, s, out, size);
    (*env)->PopLocalFrame(env, NULL);
    return out[0] != '\0';
}

/* One file of assets/ into dir, its folders made as it needs. */
static int copy_asset(const char *rel, const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    for (char *slash = path + strlen(dir) + 1; (slash = strchr(slash, '/')) != NULL; slash++) {
        *slash = '\0';
        mkdir(path, 0700);
        *slash = '/';
    }
    AAsset *asset = AAssetManager_open(app->activity->assetManager, rel, AASSET_MODE_STREAMING);
    FILE *f = asset ? fopen(path, "wb") : NULL;
    int ok = f != NULL;
    if (f) {
        char chunk[16384];
        int n;
        while ((n = AAsset_read(asset, chunk, sizeof chunk)) > 0) ok &= fwrite(chunk, 1, (size_t)n, f) == (size_t)n;
        ok &= fclose(f) == 0;
    }
    if (asset) AAsset_close(asset);
    if (!ok) __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot copy %s to %s", rel, path);
    return ok;
}

static uint32_t le16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

/* Every file under assets/ in the APK (a zip: its list at the end) into dir. (Android's asset listing has
 * no folders, and its top has the system's own beside the app's.) */
static int copy_from_apk(const char *apk, const char *dir) {
    FILE *z = fopen(apk, "rb");
    if (!z || fseek(z, 0, SEEK_END) != 0) {
        if (z) fclose(z);
        return 0;
    }
    long size = ftell(z), tail = size < 65557 ? size : 65557;  /* (the zip's end record, and its comment) */
    unsigned char *end = (unsigned char *)malloc((size_t)tail), *list = NULL;
    long at = -1;
    if (end && fseek(z, size - tail, SEEK_SET) == 0 && fread(end, 1, (size_t)tail, z) == (size_t)tail)
        for (long i = tail - 22; i >= 0 && at < 0; i--)
            if (le32(end + i) == 0x06054b50) at = i;
    uint32_t count = at >= 0 ? le16(end + at + 10) : 0, list_size = at >= 0 ? le32(end + at + 12) : 0,
             list_at = at >= 0 ? le32(end + at + 16) : 0;
    free(end);
    int ok = at >= 0 && count != 0xFFFF && list_at != 0xFFFFFFFFu;  /* (not zip64: an APK of 4 GB) */
    if (ok) list = (unsigned char *)malloc(list_size ? list_size : 1);
    ok = ok && list && fseek(z, (long)list_at, SEEK_SET) == 0 && fread(list, 1, list_size, z) == list_size;
    fclose(z);
    for (uint32_t k = 0, p = 0; ok && k < count && p + 46 <= list_size; k++) {
        if (le32(list + p) != 0x02014b50) break;
        uint32_t n = le16(list + p + 28), skip = 46 + n + le16(list + p + 30) + le16(list + p + 32);
        const char *name = (const char *)list + p + 46;
        if (p + 46 + n <= list_size && n > 7 && n < 900 && !memcmp(name, "assets/", 7) && name[n - 1] != '/') {
            char rel[1024];
            memcpy(rel, name + 7, n - 7);
            rel[n - 7] = '\0';
            if (!strstr(rel, "..")) ok = copy_asset(rel, dir);
        }
        p += skip;
    }
    free(list);
    return ok;
}

/* The app's files (assets/: mobile/game, folders and all) into dir: when the app is new or has changed (the APK's
 * time and size, kept in dir/.lube-apk), so that a program's own files there are not written over each time. */
static void copy_assets(const char *dir) {
    char apk[1024], stamp[1100], was[128] = "", now[128];
    struct stat st;
    if (!apk_path(apk, sizeof apk) || stat(apk, &st) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot find the app's own files");
        return;
    }
    snprintf(now, sizeof now, "%lld %lld", (long long)st.st_mtime, (long long)st.st_size);
    snprintf(stamp, sizeof stamp, "%s/.lube-apk", dir);
    FILE *f = fopen(stamp, "r");
    if (f) {
        if (!fgets(was, sizeof was, f)) was[0] = '\0';
        fclose(f);
    }
    if (!strcmp(was, now)) return;  /* (copied already) */
    if (!copy_from_apk(apk, dir)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "cannot copy the app's files from %s", apk);
        return;
    }
    f = fopen(stamp, "w");
    if (f) {
        fputs(now, f);
        fclose(f);
    }
}

void android_main(struct android_app *a) {
    app = a;
    env_now = NULL;  /* (a thread of its own, not joined yet) */
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
    if (env_now) (*a->activity->vm)->DetachCurrentThread(a->activity->vm);
    env_now = NULL;
}
