/*
 * evdev.c (part of gfx.c, with x11.c) - game controllers on Linux: the
 * kernel's own (/dev/input/event*), which every Linux has. A device is a
 * controller when the kernel names its buttons as a game controller's
 * (BTN_SOUTH...: Xbox, PlayStation, Switch and most others). One plugged in
 * later is noticed (inotify on /dev/input), one unplugged when it can no
 * longer be read.
 *
 * Its left and top buttons: the kernel's drivers differ. Sony's and
 * Nintendo's name them by where they are (BTN_WEST, BTN_NORTH); the others
 * (Xbox's, Steam's, most) report the left as BTN_X, which is BTN_NORTH's
 * number. Lube names them by where they are either way: x left, y top.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    int fd;              /* -1: none */
    char path[64];
    int positional;      /* BTN_NORTH is the top button (else BTN_X, the same number, is the left) */
    int min[ABS_HAT0Y + 1], max[ABS_HAT0Y + 1];
    int analog_triggers; /* ABS_Z and ABS_RZ (else BTN_TL2 and BTN_TR2: all or nothing) */
} EvPad;

typedef struct {
    EvPad pads[MAX_PADS];
    int notify;          /* inotify on /dev/input, or -1 (then: a look every 2 seconds) */
    int scan;
    double scanned;
} Evdev;

#define BITS_LONG (8 * sizeof(unsigned long))
static int has_bit(const unsigned long *bits, int n) { return (int)(bits[n / BITS_LONG] >> (n % BITS_LONG) & 1); }

static int ev_button(const EvPad *p, int code) {
    switch (code) {
    case BTN_SOUTH: return P_A;
    case BTN_EAST: return P_B;
    case BTN_NORTH: return p->positional ? P_Y : P_X;
    case BTN_WEST: return p->positional ? P_X : P_Y;
    case BTN_TL: return P_LEFT_SHOULDER;
    case BTN_TR: return P_RIGHT_SHOULDER;
    case BTN_TL2: return P_LEFT_TRIGGER;
    case BTN_TR2: return P_RIGHT_TRIGGER;
    case BTN_SELECT: return P_BACK;
    case BTN_START: return P_START;
    case BTN_MODE: return P_GUIDE;
    case BTN_THUMBL: return P_LEFT_STICK;
    case BTN_THUMBR: return P_RIGHT_STICK;
    case BTN_DPAD_UP: return P_UP;
    case BTN_DPAD_DOWN: return P_DOWN;
    case BTN_DPAD_LEFT: return P_LEFT;
    case BTN_DPAD_RIGHT: return P_RIGHT;
    default: return 0;
    }
}

static void ev_key(Win *w, int i, const EvPad *p, int code, int down) {
    int b = ev_button(p, code);
    if (b == P_LEFT_TRIGGER || b == P_RIGHT_TRIGGER) {
        if (!p->analog_triggers) pad_axis(w, i, b == P_LEFT_TRIGGER ? A_LEFT_TRIGGER : A_RIGHT_TRIGGER, down);
    } else if (b) {
        pad_button(w, i, b, down);
    }
}

static void ev_abs(Win *w, int i, const EvPad *p, int code, int v) {
    if (code > ABS_HAT0Y) return;
    double span = p->max[code] - p->min[code], t = span > 0 ? (v - p->min[code]) / span : 0;  /* 0 to 1 */
    switch (code) {
    case ABS_X: pad_axis(w, i, A_LEFT_X, 2 * t - 1); break;
    case ABS_Y: pad_axis(w, i, A_LEFT_Y, 2 * t - 1); break;
    case ABS_RX: pad_axis(w, i, A_RIGHT_X, 2 * t - 1); break;
    case ABS_RY: pad_axis(w, i, A_RIGHT_Y, 2 * t - 1); break;
    case ABS_Z: pad_axis(w, i, A_LEFT_TRIGGER, t); break;
    case ABS_RZ: pad_axis(w, i, A_RIGHT_TRIGGER, t); break;
    case ABS_HAT0X:  /* the d-pad, as an axis: -1, 0 or 1 */
        pad_button(w, i, P_LEFT, v < 0);
        pad_button(w, i, P_RIGHT, v > 0);
        break;
    case ABS_HAT0Y:
        pad_button(w, i, P_UP, v < 0);
        pad_button(w, i, P_DOWN, v > 0);
        break;
    default: break;
    }
}

/* path, if it is a controller not open yet: opened, as the first free number. */
static void ev_try(Evdev *v, Win *w, const char *path) {
    int i, free_slot = -1;
    for (i = 0; i < MAX_PADS; i++) {
        if (v->pads[i].fd >= 0 && !strcmp(v->pads[i].path, path)) return;
        if (v->pads[i].fd < 0 && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;  /* (not ours to read: a keyboard, say) */
    unsigned long keys[KEY_MAX / BITS_LONG + 1], abs[ABS_MAX / BITS_LONG + 1], held[KEY_MAX / BITS_LONG + 1];
    memset(keys, 0, sizeof keys);
    memset(abs, 0, sizeof abs);
    memset(held, 0, sizeof held);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 || !has_bit(keys, BTN_SOUTH)) {
        close(fd);
        return;
    }
    i = free_slot;
    EvPad *p = &v->pads[i];
    memset(p, 0, sizeof *p);
    p->fd = fd;
    snprintf(p->path, sizeof p->path, "%s", path);
    struct input_id id;
    p->positional = ioctl(fd, EVIOCGID, &id) >= 0 && (id.vendor == 0x054c || id.vendor == 0x057e);  /* (Sony, Nintendo) */
    char name[48];
    memset(name, 0, sizeof name);
    if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0) name[0] = '\0';
    pad_connect(w, i, name);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    p->analog_triggers = has_bit(abs, ABS_Z) && has_bit(abs, ABS_RZ);
    for (int code = 0; code <= ABS_HAT0Y; code++) {  /* each axis's range, and where it is now */
        struct input_absinfo a;
        if (!has_bit(abs, code) || ioctl(fd, EVIOCGABS(code), &a) < 0) continue;
        p->min[code] = a.minimum;
        p->max[code] = a.maximum;
        ev_abs(w, i, p, code, a.value);
    }
    if (ioctl(fd, EVIOCGKEY(sizeof held), held) >= 0)  /* (buttons held already) */
        for (int code = BTN_SOUTH; code <= BTN_THUMBR; code++)
            if (has_bit(held, code)) ev_key(w, i, p, code, 1);
}

static void ev_scan(Evdev *v, Win *w) {
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[64];
        snprintf(path, sizeof path, "/dev/input/%.40s", e->d_name);
        ev_try(v, w, path);
    }
    closedir(d);
}

static void ev_open(Evdev *v) {
    for (int i = 0; i < MAX_PADS; i++) v->pads[i].fd = -1;
    v->notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (v->notify >= 0 && inotify_add_watch(v->notify, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        close(v->notify);
        v->notify = -1;
    }
    v->scan = 1;
}

static void ev_poll(Evdev *v, Win *w) {
    char buf[4096];
    if (v->notify >= 0) {
        while (read(v->notify, buf, sizeof buf) > 0) v->scan = 1;  /* (a device came, or became readable) */
    } else if (lube->now() - v->scanned > 2) {
        v->scan = 1;
    }
    if (v->scan) {
        v->scan = 0;
        v->scanned = lube->now();
        ev_scan(v, w);
    }
    for (int i = 0; i < MAX_PADS; i++) {
        EvPad *p = &v->pads[i];
        if (p->fd < 0) continue;
        struct input_event in[64];
        ssize_t n;
        while ((n = read(p->fd, in, sizeof in)) > 0)
            for (size_t k = 0; k < (size_t)n / sizeof in[0]; k++) {
                if (in[k].type == EV_KEY) ev_key(w, i, p, in[k].code, in[k].value != 0);
                if (in[k].type == EV_ABS) ev_abs(w, i, p, in[k].code, in[k].value);
            }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {  /* unplugged (ENODEV) */
            close(p->fd);
            p->fd = -1;
            pad_disconnect(w, i);
        }
    }
}

static void ev_close(Evdev *v) {
    for (int i = 0; i < MAX_PADS; i++)
        if (v->pads[i].fd >= 0) close(v->pads[i].fd);
    if (v->notify >= 0) close(v->notify);
}
