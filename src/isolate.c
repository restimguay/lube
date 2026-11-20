/*
 * isolate.c - Lube on several cores: isolates, and channels between them.
 *
 * An isolate is a whole interpreter (heap, globals, VM) running on its own
 * thread. Isolates share no Lube values at all, so reference counts stay
 * plain integers and each isolate runs at full speed. They talk through
 * channels: send() copies a value out of the sender's heap into a message,
 * receive() rebuilds it in the receiver's. Channels and isolate handles are
 * the only things shared; each isolate holding one has a small wrapper
 * object (ObjChannel, ObjIsolate).
 *
 *   spawn(f, args...)   runs the top-level function f in a new isolate
 *   iso.join()          waits for it: f's result, or f's error raised here
 *   channel(n?)         a queue (with room for n, or unbounded)
 *   ch.send(v) / ch.receive() / ch.try_receive(default) / ch.close()
 *   ch.take(n)          n values, as a list;  for v in ch { }  until closed
 *   cores()             how many cores this machine has
 *   parallel_map(xs, f) map with f on every core
 *
 * The program ends when the main isolate and every isolate started have
 * finished.
 *
 * A new isolate runs the program's declarations (functions, classes,
 * imports, global variables, `on` blocks), then calls f.
 *
 * One lock (the "world") guards every channel and handle. A waiting isolate
 * is counted as blocked; any change that could let one go on (a send, a
 * receive making room, a close, an isolate ending) resets that count and
 * wakes them all to look again. So when an isolate is about to wait while
 * every other one is already waiting, with nothing changed since they
 * looked, nothing can ever wake it: that is a deadlock, and it gets an
 * error instead of hanging.
 */
#include "lube.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE(name) static Value name(Interp *I, int argc, Value *argv)

/* ---------- the world: what every isolate shares ---------- */

static LubeLock *world;     /* made by the main isolate before any other exists */
static int running = 1;     /* isolates alive: the main one and every one not yet ended */
static int blocked;         /* isolates waiting, with nothing changed since they looked */
static unsigned long epoch; /* counts changes */
static int leaked;          /* an isolate's leak check failed (LUBE_CHECK_LEAKS builds) */
static int doomed;          /* the program has ended with every isolate left waiting */

static void world_init(void) {
    if (!world) world = lock_new();
}

/* Something a waiter may want happened (world lock held). */
static void changed(void) {
    epoch++;
    blocked = 0;
    lock_notify(world);
}

/* Waits until something changes (world lock held). Returns 0 instead when
 * every other isolate is waiting too: then nothing can ever wake this one. */
static int await_change(void) {
    if (doomed || blocked + 1 >= running) return 0;
    blocked++;
    unsigned long seen = epoch;
    while (epoch == seen) lock_wait(world);
    return 1;
}

/* await_change for an isolate (world lock held): 0 after a deadlock, with
 * the lock released and the error raised. */
static int wait_change(Interp *I) {
    if (await_change()) return 1;
    lock_release(world);
    lube_error(I, "deadlock: every isolate is waiting (for a channel that nothing will send to or close, or for a join)");
    return 0;
}

/* ---------- messages: values copied between heaps ---------- */

typedef struct Message {
    struct Message *next;
    StrBuf data;
    struct Channel **channels;  /* channels inside the value: each holds a reference */
    int nchannels;
} Message;

typedef struct Channel {
    Message *head, *tail;
    size_t count, capacity;     /* capacity 0: no limit */
    int closed;
    int refs;                   /* wrappers (in any isolate) and messages holding it */
} Channel;

enum { M_NUM, M_TRUE, M_FALSE, M_INT, M_STR, M_STROBJ, M_LIST, M_MAP, M_INSTANCE, M_RANGE, M_CHANNEL, M_BYTES };

static void put_u32(StrBuf *sb, size_t n) {
    uint32_t v = (uint32_t)n;
    sb_putn(sb, (const char *)&v, 4);
}

static void put_text(StrBuf *sb, ObjString *s) {
    put_u32(sb, s->length);
    sb_putn(sb, s->chars, s->length);
}

static void message_free(Message *m) {
    for (int i = 0; i < m->nchannels; i++)
        if (m->channels[i]) channel_release(m->channels[i]);
    free(m->channels);
    sb_free(&m->data);
    free(m);
}

#define MAX_SEND_DEPTH 10000  /* nesting a message may have (receiving is just as deep) */

/* Copies v into the message. 0, or -1 after raising an error. */
static int encode(Interp *I, Message *m, Value v, int depth) {
    StrBuf *sb = &m->data;
    if (depth > MAX_SEND_DEPTH) {
        lube_error(I, "this value is nested too deeply to send to another isolate (at most %d levels)", MAX_SEND_DEPTH);
        return -1;
    }
    if (IS_NUM(v)) {
        sb_putc(sb, M_NUM);
        sb_putn(sb, (const char *)&v.as.number, 8);
    } else if (IS_BOOL(v)) {
        sb_putc(sb, v.as.boolean ? M_TRUE : M_FALSE);
    } else if (IS_INT(v)) {
        sb_putc(sb, M_INT);
        sb_putc(sb, (char)v.ikind);
        sb_putn(sb, (const char *)&v.as.u, 8);
    } else if (IS_STRING(v) || IS_STROBJ(v)) {
        sb_putc(sb, IS_STRING(v) ? M_STR : M_STROBJ);
        put_text(sb, text_of(v));
    } else if (IS_LIST(v)) {
        ObjList *l = AS_LIST(v);
        sb_putc(sb, M_LIST);
        sb_putc(sb, (char)l->elem);
        put_u32(sb, l->count);
        for (size_t i = 0; i < l->count; i++)
            if (encode(I, m, l->items[i], depth + 1) < 0) return -1;
    } else if (IS_MAP(v)) {
        ObjMap *mp = AS_MAP(v);
        if (mp->klass) {  /* an instance: the receiver makes it from its own copy of the class */
            sb_putc(sb, M_INSTANCE);
            put_text(sb, mp->klass->key);
        } else {
            sb_putc(sb, M_MAP);
            sb_putc(sb, (char)mp->dict);
            sb_putc(sb, (char)mp->ktype);
            sb_putc(sb, (char)mp->vtype);
        }
        put_u32(sb, mp->count);
        for (size_t i = 0; i < mp->used; i++) {
            if (!mp->entries[i].live) continue;
            if (encode(I, m, mp->entries[i].key, depth + 1) < 0 || encode(I, m, mp->entries[i].value, depth + 1) < 0)
                return -1;
        }
    } else if (IS_RANGE(v)) {
        ObjRange *r = AS_RANGE(v);
        sb_putc(sb, M_RANGE);
        sb_putn(sb, (const char *)&r->start, 8);
        sb_putn(sb, (const char *)&r->end, 8);
        sb_putn(sb, (const char *)&r->step, 8);
        sb_putc(sb, (char)r->inclusive);
    } else if (IS_BYTES(v)) {
        sb_putc(sb, M_BYTES);
        put_u32(sb, AS_BYTES(v)->len);
        if (AS_BYTES(v)->len) sb_putn(sb, (const char *)AS_BYTES(v)->data, AS_BYTES(v)->len);
    } else if (IS_CHANNEL(v)) {  /* a channel goes along as itself */
        Channel *ch = AS_CHANNEL(v)->ch;
        lock_acquire(world);
        ch->refs++;
        lock_release(world);
        m->channels = xrealloc(m->channels, sizeof(Channel *) * (size_t)(m->nchannels + 1));
        m->channels[m->nchannels] = ch;
        sb_putc(sb, M_CHANNEL);
        put_u32(sb, (size_t)m->nchannels++);
    } else {
        lube_error(I, "%s cannot be sent to another isolate: only data can (numbers, text, bytes, lists, maps, "
                      "instances and channels)", with_article(type_name(v)));
        return -1;
    }
    return 0;
}

static Message *new_message(void) {
    Message *m = xmalloc(sizeof(Message));
    m->next = NULL;
    sb_init(&m->data);
    m->channels = NULL;
    m->nchannels = 0;
    return m;
}

static Value wrap_channel(Channel *ch) {
    ObjChannel *o = (ObjChannel *)alloc_object(sizeof(ObjChannel), OBJ_CHANNEL);
    o->ch = ch;
    return OBJ_VAL(o);
}

typedef struct {
    Interp *I;
    Message *m;
    size_t pos;
    int take;                   /* take over the message's channel references (else add our own) */
} Reader;

static uint32_t get_u32(Reader *r) {
    uint32_t v;
    memcpy(&v, r->m->data.data + r->pos, 4);
    r->pos += 4;
    return v;
}

static ObjString *get_text(Reader *r) {
    uint32_t n = get_u32(r);
    ObjString *s = new_string(r->m->data.data + r->pos, n);
    r->pos += n;
    return s;
}

/* Rebuilds the next value of a message in this isolate's heap. */
static Value decode(Reader *r) {
    Interp *I = r->I;  /* (no deeper than sending allowed) */
    int tag = (unsigned char)r->m->data.data[r->pos++];
    double d;
    switch (tag) {
    case M_NUM:
        memcpy(&d, r->m->data.data + r->pos, 8);
        r->pos += 8;
        return NUM_VAL(d);
    case M_TRUE:
    case M_FALSE:
        return BOOL_VAL(tag == M_TRUE);
    case M_INT: {
        int kind = (unsigned char)r->m->data.data[r->pos++];
        uint64_t u;
        memcpy(&u, r->m->data.data + r->pos, 8);
        r->pos += 8;
        return IK_SIGNED(kind) ? SINT_VAL(kind, (int64_t)u) : UINT_VAL(kind, u);
    }
    case M_STR:
        return OBJ_VAL(get_text(r));
    case M_BYTES: {
        uint32_t n = get_u32(r);
        ObjBytes *b = new_bytes((size_t)n);
        if (n) memcpy(b->data, r->m->data.data + r->pos, n);
        b->len = n;
        r->pos += n;
        return OBJ_VAL(b);
    }
    case M_STROBJ:
        return OBJ_VAL(new_strobj(get_text(r)));
    case M_LIST: {
        ObjList *l = new_list();
        l->elem = (unsigned char)r->m->data.data[r->pos++];
        uint32_t n = get_u32(r);
        list_reserve(l, n);
        for (uint32_t i = 0; i < n; i++) {
            Value item = decode(r);
            if (IS_ERR(item)) {
                release(OBJ_VAL(l));
                return item;
            }
            l->items[l->count++] = item;
        }
        return OBJ_VAL(l);
    }
    case M_MAP:
    case M_INSTANCE: {
        ObjMap *m = new_map();
        if (tag == M_INSTANCE) {
            ObjString *name = get_text(r);
            Value *c = map_find(I->globals, OBJ_VAL(name));
            if (!c || !IS_CLASS(*c)) {
                Value e = lube_error(I, "cannot receive %s here: this isolate has no class %s",
                                     with_article(lube_shown(name)), lube_shown(name));
                release(OBJ_VAL(name));
                release(OBJ_VAL(m));
                return e;
            }
            release(OBJ_VAL(name));
            m->klass = AS_CLASS(*c);
        } else {
            m->dict = (unsigned char)r->m->data.data[r->pos++];
            m->ktype = (unsigned char)r->m->data.data[r->pos++];
            m->vtype = (unsigned char)r->m->data.data[r->pos++];
        }
        uint32_t n = get_u32(r);
        for (uint32_t i = 0; i < n; i++) {
            Value k = decode(r);
            Value val = IS_ERR(k) ? ERR_VAL : decode(r);
            if (IS_ERR(val)) {
                release(k);
                release(OBJ_VAL(m));
                return ERR_VAL;
            }
            map_set(m, k, val);
        }
        return OBJ_VAL(m);
    }
    case M_RANGE: {
        double a, b, s;
        memcpy(&a, r->m->data.data + r->pos, 8);
        memcpy(&b, r->m->data.data + r->pos + 8, 8);
        memcpy(&s, r->m->data.data + r->pos + 16, 8);
        int inclusive = r->m->data.data[r->pos + 24];
        r->pos += 25;
        return OBJ_VAL(new_range(a, b, s, inclusive));
    }
    default:  /* M_CHANNEL */
        break;
    }
    uint32_t i = get_u32(r);
    Channel *ch = r->m->channels[i];
    if (r->take) {
        r->m->channels[i] = NULL;  /* the wrapper owns this reference now */
    } else {
        lock_acquire(world);
        ch->refs++;
        lock_release(world);
    }
    return wrap_channel(ch);
}

static Value decode_message(Interp *I, Message *m, int take) {
    Reader r;
    r.I = I;
    r.m = m;
    r.pos = 0;
    r.take = take;
    return decode(&r);
}

/* ---------- channels ---------- */

void channel_release(Channel *ch) {
    lock_acquire(world);
    int last = --ch->refs == 0;
    lock_release(world);
    if (!last) return;
    while (ch->head) {  /* messages nobody received (they may hold other channels) */
        Message *m = ch->head;
        ch->head = m->next;
        message_free(m);
    }
    free(ch);
}

static Channel *channel_arg(Interp *I, const char *fn, Value v) {
    if (IS_CHANNEL(v)) return AS_CHANNEL(v)->ch;
    lube_error(I, "%s() needs a Channel, not %s", fn, a_type(v));
    return NULL;
}

/* Takes the next message off ch, waiting for one. 1 = got it (in *out),
 * 0 = the channel is closed and empty, -1 = deadlock (error raised). */
static int channel_get(Interp *I, Channel *ch, Message **out) {
    lock_acquire(world);
    for (;;) {
        if (ch->head) {
            Message *m = ch->head;
            ch->head = m->next;
            if (!ch->head) ch->tail = NULL;
            ch->count--;
            changed();  /* there is room for a waiting sender */
            lock_release(world);
            *out = m;
            return 1;
        }
        if (ch->closed) {
            lock_release(world);
            return 0;
        }
        if (!wait_change(I)) return -1;
    }
}

NATIVE(n_channel) {
    size_t capacity = 0;
    if (argc > 0) {
        if (!IS_NUMERIC(argv[0]) || numeric_value(argv[0]) < 1 || numeric_value(argv[0]) != floor(numeric_value(argv[0])))
            return lube_error(I, "a channel's room must be a whole number of at least 1, not %s", a_type(argv[0]));
        capacity = numeric_value(argv[0]) > 1e9 ? 1000000000u : (size_t)numeric_value(argv[0]);
    }
    world_init();
    Channel *ch = xmalloc(sizeof(Channel));
    ch->head = ch->tail = NULL;
    ch->count = 0;
    ch->capacity = capacity;
    ch->closed = 0;
    ch->refs = 1;
    return wrap_channel(ch);
}

NATIVE(n_send) {
    (void)argc;
    Channel *ch = channel_arg(I, "send", argv[0]);
    if (!ch) return ERR_VAL;
    Message *m = new_message();
    if (encode(I, m, argv[1], 0) < 0) {
        message_free(m);
        return ERR_VAL;
    }
    lock_acquire(world);
    for (;;) {
        if (ch->closed) {
            lock_release(world);
            message_free(m);
            return lube_error(I, "cannot send: the channel is closed");
        }
        if (!ch->capacity || ch->count < ch->capacity) break;
        if (!wait_change(I)) {  /* full, and nothing will ever receive */
            message_free(m);
            return ERR_VAL;
        }
    }
    if (ch->tail) ch->tail->next = m;
    else ch->head = m;
    ch->tail = m;
    ch->count++;
    changed();
    lock_release(world);
    return NIL_VAL;
}

static Value receive_one(Interp *I, Channel *ch) {
    Message *m;
    int got = channel_get(I, ch, &m);
    if (got < 0) return ERR_VAL;
    if (!got) return lube_error(I, "cannot receive: the channel is closed and empty");
    Value v = decode_message(I, m, 1);
    message_free(m);
    return v;
}

NATIVE(n_receive) {
    (void)argc;
    Channel *ch = channel_arg(I, "receive", argv[0]);
    return ch ? receive_one(I, ch) : ERR_VAL;
}

NATIVE(n_try_receive) {  /* a value if one is waiting, else the default: never waits */
    (void)argc;
    Channel *ch = channel_arg(I, "try_receive", argv[0]);
    if (!ch) return ERR_VAL;
    lock_acquire(world);
    Message *m = ch->head;
    if (m) {
        ch->head = m->next;
        if (!ch->head) ch->tail = NULL;
        ch->count--;
        changed();
    }
    lock_release(world);
    if (!m) return retain(argv[1]);
    Value v = decode_message(I, m, 1);
    message_free(m);
    return v;
}

NATIVE(n_close) {
    (void)argc;
    Channel *ch = channel_arg(I, "close", argv[0]);
    if (!ch) return ERR_VAL;
    lock_acquire(world);
    ch->closed = 1;
    changed();
    lock_release(world);
    return NIL_VAL;
}

NATIVE(n_take) {  /* n values, waiting for each */
    (void)argc;
    Channel *ch = channel_arg(I, "take", argv[0]);
    if (!ch) return ERR_VAL;
    if (!IS_NUMERIC(argv[1]) || numeric_value(argv[1]) < 0 || numeric_value(argv[1]) != floor(numeric_value(argv[1])))
        return lube_error(I, "take() needs a whole number of values to take, not %s", a_type(argv[1]));
    ObjList *out = new_list();
    for (double i = 0; i < numeric_value(argv[1]); i++) {
        Value v = receive_one(I, ch);
        if (IS_ERR(v)) {
            release(OBJ_VAL(out));
            return v;
        }
        list_push(out, v);
    }
    return OBJ_VAL(out);
}

/* for v in ch: the next value; 0 when the channel is closed and empty. */
int channel_next(Interp *I, Value v, Value *out) {
    Message *m;
    int got = channel_get(I, AS_CHANNEL(v)->ch, &m);
    if (got <= 0) return got;
    *out = decode_message(I, m, 1);
    message_free(m);
    return IS_ERR(*out) ? -1 : 1;
}

/* ---------- isolates ---------- */

struct ProgFile {               /* a source the program ran, kept for new isolates */
    char *path;
    char *text;
    size_t len;
};

typedef struct {                /* what a new isolate loads: shared by the isolates started together */
    int refs;
    struct ProgFile *files;     /* the top-level sources, in order */
    int nfiles;
    struct ProgFile *bundle;    /* a built program's modules */
    int nbundle;
    char **args;
    int nargs;
    char *root;                 /* the program's folder (packages are found from it) */
} Program;

typedef struct Isolate {
    int refs;                   /* handles to it, and its own thread while it runs (world lock) */
    int done;
    Message *args;              /* the function's arguments */
    Message *result;            /* its result (NULL when it returned nothing) */
    char *error;                /* or the error it ended with */
    int error_seen;             /* a join reported the error */
    char *fname;
    int map;                    /* parallel_map: map fname over the list in args */
    Program *program;
} Isolate;

static char *copy_text(const char *s, size_t n) { return xstrndup(s, n); }

/* A function's name for messages: work for my.pkg::work. */
static const char *shown(const char *key) {
    const char *p = strstr(key, "::");
    return p ? p + 2 : key;
}

void lube_note_program(Interp *I, const char *path, const char *text, size_t len) {
    if (I->nprogram == I->cap_program) {
        I->cap_program = I->cap_program ? I->cap_program * 2 : 4;
        I->program = xrealloc(I->program, sizeof(struct ProgFile) * (size_t)I->cap_program);
    }
    struct ProgFile *f = &I->program[I->nprogram++];
    f->path = copy_text(path, strlen(path));
    f->text = copy_text(text, len);
    f->len = len;
}

void lube_free_program(Interp *I) {
    for (int i = 0; i < I->nprogram; i++) {
        free(I->program[i].path);
        free(I->program[i].text);
    }
    free(I->program);
}

/* What a new isolate needs to load this program: the sources as last run
 * (a hot reload's version of a file), a built program's modules, and args. */
static Program *snapshot(Interp *I) {
    Program *p = xmalloc(sizeof(Program));
    p->refs = 0;
    p->nfiles = I->nprogram;
    p->files = xmalloc(sizeof(struct ProgFile) * (size_t)(I->nprogram ? I->nprogram : 1));
    for (int i = 0; i < I->nprogram; i++) {
        struct ProgFile *f = &I->program[i];
        Value now;
        ObjString *path = new_string(f->path, strlen(f->path));
        int reloaded = map_get(I->watch, OBJ_VAL(path), &now);
        release(OBJ_VAL(path));
        p->files[i].path = copy_text(f->path, strlen(f->path));
        p->files[i].len = reloaded ? AS_STRING(now)->length : f->len;
        p->files[i].text = copy_text(reloaded ? AS_STRING(now)->chars : f->text, p->files[i].len);
    }
    p->nbundle = 0;
    p->bundle = NULL;
    if (I->bundle) {
        p->bundle = xmalloc(sizeof(struct ProgFile) * (size_t)(I->bundle->count ? I->bundle->count : 1));
        for (size_t i = 0; i < I->bundle->used; i++) {
            MapEntry *e = &I->bundle->entries[i];
            if (!e->live) continue;
            struct ProgFile *f = &p->bundle[p->nbundle++];
            f->path = copy_text(AS_STRING(e->key)->chars, AS_STRING(e->key)->length);
            f->len = AS_STRING(e->value)->length;
            f->text = copy_text(AS_STRING(e->value)->chars, f->len);
        }
    }
    Value args;
    p->root = I->root ? copy_text(I->root, strlen(I->root)) : NULL;
    p->nargs = 0;
    p->args = NULL;
    if (map_get(I->globals, OBJ_VAL(lube_intern(I, "args", 4)), &args) && IS_LIST(args)) {
        ObjList *l = AS_LIST(args);
        p->args = xmalloc(sizeof(char *) * (l->count ? l->count : 1));
        for (size_t i = 0; i < l->count; i++)
            if (IS_TEXT(l->items[i])) p->args[p->nargs++] = copy_text(text_of(l->items[i])->chars, text_of(l->items[i])->length);
    }
    return p;
}

static void free_files(struct ProgFile *f, int n) {
    for (int i = 0; i < n; i++) {
        free(f[i].path);
        free(f[i].text);
    }
    free(f);
}

static void program_release(Program *p) {
    lock_acquire(world);
    int last = --p->refs == 0;
    lock_release(world);
    if (!last) return;
    free_files(p->files, p->nfiles);
    free_files(p->bundle, p->nbundle);
    for (int i = 0; i < p->nargs; i++) free(p->args[i]);
    free(p->args);
    free(p->root);
    free(p);
}

void isolate_release(Isolate *iso) {
    lock_acquire(world);
    int last = --iso->refs == 0;
    lock_release(world);
    if (!last) return;
    if (iso->error && !iso->error_seen)  /* nobody joined it to hear about this */
        lube_print(1, "lube: an isolate running %s() failed: %s\n", shown(iso->fname), iso->error);
    if (iso->args) message_free(iso->args);
    if (iso->result) message_free(iso->result);
    free(iso->error);
    free(iso->fname);
    free(iso);
}

/* The error a run left behind, as text (and gone from the isolate). */
static char *take_error(Interp *I) {
    ObjString *s = value_to_string(I->error_val);
    char *text = copy_text(s->chars, s->length);
    release(OBJ_VAL(s));
    release(I->error_val);
    I->error_val = NIL_VAL;
    return text;
}

/* f(args), or for parallel_map: [f(x) for each x of the list]. */
static Value run_entry(Interp *I, Isolate *iso) {
    Value f;
    if (!map_get(I->globals, OBJ_VAL(lube_intern(I, iso->fname, strlen(iso->fname))), &f) || !IS_FUNCTION(f))
        return lube_error(I, "%s() is not a function in the new isolate", iso->fname); /* LCOV_EXCL_LINE */
    f = retain(f);
    Value args = decode_message(I, iso->args, 0), r;
    if (IS_ERR(args)) {
        r = args;
    } else if (iso->map) {  /* a slice of parallel_map's list */
        ObjList *in = AS_LIST(args), *out = new_list();
        r = OBJ_VAL(out);
        for (size_t i = 0; i < in->count; i++) {
            Value v = lube_call(I, f, 1, &in->items[i]);
            if (!IS_ERR(v) && IS_NIL(v)) v = lube_error(I, "the function given to parallel_map() did not return a value");
            if (IS_ERR(v)) {
                release(r);
                r = v;
                break;
            }
            list_push(out, v);
        }
    } else {
        ObjList *in = AS_LIST(args);
        r = lube_call(I, f, (int)in->count, in->items);
    }
    release(args);
    release(f);
    return r;
}

static void isolate_main(void *arg) {
    Isolate *iso = arg;
    Program *p = iso->program;
    char marker;
    Interp *I = lube_new(&marker);
    I->isolate = iso;
    if (p->root) I->root = copy_text(p->root, strlen(p->root));
    ObjList *args = new_list();
    for (int i = 0; i < p->nargs; i++) list_push(args, OBJ_VAL(new_string(p->args[i], strlen(p->args[i]))));
    lube_define(I, "args", OBJ_VAL(args));
    if (p->nbundle) {
        I->bundle = new_map();
        for (int i = 0; i < p->nbundle; i++)
            map_set(I->bundle, OBJ_VAL(new_string(p->bundle[i].path, strlen(p->bundle[i].path))),
                    OBJ_VAL(new_string(p->bundle[i].text, p->bundle[i].len)));
    }
    char *error = NULL;
    Message *result = NULL;
    I->starting = 1;  /* the program's declarations: functions, classes, imports, globals */
    for (int i = 0; i < p->nfiles && !error; i++)
        if (lube_run_declarations(I, p->files[i].path, p->files[i].text, p->files[i].len) != 0)
            error = IS_NIL(I->error_val) ? copy_text("the program has errors", 22) : take_error(I);
    I->starting = 0;
    program_release(p);
    if (!error) {
        Value r = run_entry(I, iso);
        if (IS_ERR(r)) {
            error = take_error(I);
        } else if (!IS_NIL(r)) {
            result = new_message();
            if (encode(I, result, r, 0) < 0) {  /* the result cannot go back: say so */
                message_free(result);
                result = NULL;
                error = take_error(I);
            }
            release(r);
        }
    }
    size_t alive = lube_free(I);
    lube_pool_drain();  /* (its thread ends: the main one's goes with the process) */
#ifdef LUBE_CHECK_LEAKS
    if (alive) lube_print(1, "lube: leak check failed in an isolate: %lu objects still alive\n", (unsigned long)alive);
#endif
    lock_acquire(world);
    int orphan = iso->refs == 1 && error;  /* nobody holds a handle to join it: report the error now */
    if (orphan) iso->error_seen = 1;
    lock_release(world);
    if (orphan) lube_print(1, "lube: an isolate running %s() failed: %s\n", shown(iso->fname), error);
    lock_acquire(world);
    if (alive) leaked = 1;
    iso->result = result;
    iso->error = error;
    iso->done = 1;
    running--;
    changed();
    lock_release(world);
    isolate_release(iso);  /* the thread's reference */
}

/* At the end of the program: waits for every isolate to finish. One still
 * waiting for something that can never happen gets a deadlock error (and
 * ends), so this never hangs on a forgotten close(). */
void lube_wait_isolates(void) {
    if (!world) return;
    lock_acquire(world);
    while (running > 1) {
        if (!doomed && blocked + 1 >= running) {  /* every one left is waiting: wake them to a deadlock error */
            doomed = 1;
            changed();
        }
        blocked++;  /* the main isolate waits too */
        unsigned long seen = epoch;
        while (epoch == seen) lock_wait(world);
    }
    lock_release(world);
}

int lube_isolates_leaked(void) {
    if (!world) return 0;
    lock_acquire(world);
    int r = leaked;
    lock_release(world);
    return r;
}

/* A function spawn() can start: declared with fn at the top level (the new
 * isolate finds it by name), and taking no & argument. */
static FuncDecl *top_level_fn(Interp *I, const char *who, Value f) {
    Value g;
    FuncDecl *fd = IS_FUNCTION(f) ? AS_FUNCTION(f)->decl : NULL;
    ObjString *key = fd ? (fd->key ? fd->key : fd->name) : NULL;  /* the global the new isolate finds it in */
    if (!key || !map_get(I->globals, OBJ_VAL(key), &g) || !IS_FUNCTION(g) || g.as.obj != f.as.obj) {
        lube_error(I, "%s() needs a function declared with fn at the top of a file (the new isolate finds it by name), "
                      "not %s", who, IS_FUNCTION(f) ? "a nested function or lambda" : a_type(f));
        return NULL;
    }
    FuncDecl *d = AS_FUNCTION(f)->decl;
    for (int i = 0; i < d->nparams; i++) {
        if (d->modes[i] == PARAM_REF) {
            lube_error(I, "%s() cannot run %s() in an isolate: it changes its argument '%s' with &", who, d->name->chars,
                       d->params[i]->chars);
            return NULL;
        }
    }
    return d;
}

static Value start_isolate(Interp *I, FuncDecl *d, Message *args, int map, Program *p) {
    Isolate *iso = xmalloc(sizeof(Isolate));
    iso->refs = 2;  /* the handle and the thread */
    iso->done = 0;
    iso->args = args;
    iso->result = NULL;
    iso->error = NULL;
    iso->error_seen = 0;
    ObjString *key = d->key ? d->key : d->name;
    iso->fname = copy_text(key->chars, key->length);
    iso->map = map;
    iso->program = p;
    lock_acquire(world);
    p->refs++;
    running++;
    lock_release(world);
    if (!thread_start(isolate_main, iso, LUBE_STACK_SIZE)) {  /* LCOV_EXCL_START: the system has no threads left */
        lock_acquire(world);
        running--;
        p->refs--;
        lock_release(world);
        iso->refs = 1;
        isolate_release(iso);
        return lube_error(I, "cannot start an isolate: the system has no more threads");
    }  /* LCOV_EXCL_STOP */
    ObjIsolate *o = (ObjIsolate *)alloc_object(sizeof(ObjIsolate), OBJ_ISOLATE);
    o->iso = iso;
    return OBJ_VAL(o);
}

NATIVE(n_spawn) {
    if (I->starting)
        return lube_error(I, "spawn() cannot run while an isolate is starting (a global's value would start isolates "
                             "again in every new one)");
    FuncDecl *d = top_level_fn(I, "spawn", argv[0]);
    if (!d) return ERR_VAL;
    int n = argc - 1, required = d->nparams;
    while (required > 0 && d->has_default[required - 1]) required--;
    if (n < required || n > d->nparams) {
        if (required == d->nparams)
            return lube_error(I, "%s() takes %d argument%s but got %d", d->name->chars, required, required == 1 ? "" : "s", n);
        return lube_error(I, "%s() takes %d to %d arguments but got %d", d->name->chars, required, d->nparams, n);
    }
    Message *m = new_message();  /* the arguments, as one list */
    ObjList *list = new_list();
    for (int i = 1; i < argc; i++) list_push(list, retain(argv[i]));
    int bad = encode(I, m, OBJ_VAL(list), 0) < 0;
    release(OBJ_VAL(list));
    if (bad) {
        message_free(m);
        return ERR_VAL;
    }
    world_init();
    Program *p = snapshot(I);
    Value r = start_isolate(I, d, m, 0, p);
    return r;
}

/* Waits for an isolate to end; 0, or -1 after a deadlock error. */
static int await_isolate(Interp *I, Isolate *iso) {
    lock_acquire(world);
    while (!iso->done)
        if (!wait_change(I)) return -1;
    lock_release(world);
    return 0;
}

/* An ended isolate's result (rebuilt here), or its error raised here. */
static Value isolate_result(Interp *I, Isolate *iso) {
    if (iso->error) {
        lock_acquire(world);
        iso->error_seen = 1;
        lock_release(world);
        return lube_error(I, "%s() failed in its isolate: %s", shown(iso->fname), iso->error);
    }
    return iso->result ? decode_message(I, iso->result, 0) : NIL_VAL;
}

/* iso.join() (builtins.c's join() comes here for an Isolate). */
Value lube_join_isolate(Interp *I, Value v) {
    Isolate *iso = AS_ISOLATE(v)->iso;
    if (await_isolate(I, iso) < 0) return ERR_VAL;
    return isolate_result(I, iso);
}

NATIVE(n_cores) {
    (void)argc;
    (void)argv;
    return NUM_VAL(lube_cores());
}

/* parallel_map(xs, f): the list in one slice per core, each mapped with f in
 * an isolate of its own; the results in order. */
NATIVE(n_parallel_map) {
    (void)argc;
    if (!IS_LIST(argv[0])) return lube_error(I, "parallel_map() needs a List, not %s", a_type(argv[0]));
    if (I->starting) return lube_error(I, "parallel_map() cannot run while an isolate is starting");
    FuncDecl *d = top_level_fn(I, "parallel_map", argv[1]);
    if (!d) return ERR_VAL;
    ObjList *xs = AS_LIST(argv[0]);
    size_t n = (size_t)lube_cores();
    if (n > xs->count) n = xs->count;
    ObjList *out = new_list();
    if (n == 0) return OBJ_VAL(out);
    Value *handles = xmalloc(sizeof(Value) * n);
    size_t started = 0;
    Value err = NIL_VAL;
    world_init();
    Program *p = snapshot(I);
    p->refs++;  /* ours, while starting them */
    for (size_t k = 0; k < n && IS_NIL(err); k++) {
        size_t from = xs->count * k / n, to = xs->count * (k + 1) / n;
        ObjList *slice = new_list();
        for (size_t i = from; i < to; i++) list_push(slice, retain(xs->items[i]));
        Message *m = new_message();
        int bad = encode(I, m, OBJ_VAL(slice), 0) < 0;
        release(OBJ_VAL(slice));
        if (bad) {
            message_free(m);
            err = ERR_VAL;
            break;
        }
        Value h = start_isolate(I, d, m, 1, p);
        handles[started++] = h;
    }
    program_release(p);
    for (size_t k = 0; k < started; k++) {  /* every one is waited for, even after an error */
        Isolate *iso = AS_ISOLATE(handles[k])->iso;
        if (await_isolate(I, iso) < 0 || !IS_NIL(err)) {
            if (IS_NIL(err)) err = ERR_VAL;
            release(handles[k]);
            continue;
        }
        Value part = isolate_result(I, iso);
        if (IS_ERR(part)) {
            err = ERR_VAL;
        } else {
            ObjList *l = AS_LIST(part);
            for (size_t i = 0; i < l->count; i++) list_push(out, retain(l->items[i]));
            release(part);
        }
        release(handles[k]);
    }
    free(handles);
    if (IS_ERR(err)) {
        release(OBJ_VAL(out));
        return ERR_VAL;
    }
    return OBJ_VAL(out);
}

static const struct {
    const char *name;
    NativeFn fn;
    int min, max;
} ISOLATES[] = {
    LUBE_BUILTIN(channel, n_channel, 0, 1)
    LUBE_BUILTIN(Channel, n_channel, 0, 1)
    LUBE_BUILTIN(send, n_send, 2, 2)
    LUBE_BUILTIN(receive, n_receive, 1, 1)
    LUBE_BUILTIN(try_receive, n_try_receive, 2, 2)
    LUBE_BUILTIN(close, n_close, 1, 1)
    LUBE_BUILTIN(take, n_take, 2, 2)
    LUBE_BUILTIN(spawn, n_spawn, 1, -1)
    LUBE_BUILTIN(cores, n_cores, 0, 0)
    LUBE_BUILTIN(parallel_map, n_parallel_map, 2, 2)
    {NULL, NULL, 0, 0}  /* (the end) */
};

void register_isolates(Interp *I) {
    for (size_t i = 0; ISOLATES[i].name; i++)
        lube_define(I, ISOLATES[i].name,
                    OBJ_VAL(new_native(ISOLATES[i].name, ISOLATES[i].fn, ISOLATES[i].min, ISOLATES[i].max, 0)));
}
