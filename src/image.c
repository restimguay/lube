/*
 * image.c - compiled programs saved as bytes and loaded back.
 *
 * lube --build --small packs each file of the program compiled (its
 * functions' bytecode and everything the VM reads beside it), so that the
 * executable needs no lexer, parser, resolver or compiler: it loads these
 * instead (compile_source in interp.c). The sources go along still, for
 * the lines error messages show.
 *
 * An image is a list of entries, one per file and compile mode:
 *
 *   image:  u32 count, then each entry: u32 path length, path, u32 mode,
 *           u32 length, the entry
 *   entry:  u32 string count, each a u32 length and its bytes; the
 *           file's package (a string); u32 object count, a kind byte for
 *           each, then each one's fields (below); object 0 is the file's
 *           top-level function
 *
 * Objects are functions, `on` blocks and lists of them, which point at each
 * other (a block's function, the blocks watching a variable): such a
 * pointer is written as the object's number + 1 (0: none), a string as its
 * number + 1. Every number (u32 above) is a varint: 7 bits a byte, low
 * first, of the number zigzagged as an int32 (so -1 takes one byte); a
 * function's line numbers go as runs. Run-time caches start empty.
 */
#include "lube.h"

#include <stdlib.h>
#include <string.h>

enum { IMG_FUNC = 1, IMG_ON = 2, IMG_ONLIST = 3 };

/* ======================================================================
 * Writing (lube --build --small)
 * ==================================================================== */
#ifndef LUBE_RUNTIME

typedef struct {
    StrBuf *out;
    ObjMap *strings;  /* string -> its number */
    StrBuf strs;      /* the strings, written first */
    int nstrings;
    void **objs;      /* the objects, numbered as met */
    int *kinds, nobjs, capobjs;
} Writer;

static void w_u32(StrBuf *b, uint32_t n) {  /* (zigzag, then 7 bits a byte) */
    uint32_t z = (n << 1) ^ (uint32_t)-(int32_t)(n >> 31);
    while (z >= 0x80) {
        sb_putc(b, (char)(z | 0x80));
        z >>= 7;
    }
    sb_putc(b, (char)z);
}

static void w_word(StrBuf *b, uint32_t n) {  /* (an instruction: 7 bits a byte, no zigzag) */
    while (n >= 0x80) {
        sb_putc(b, (char)(n | 0x80));
        n >>= 7;
    }
    sb_putc(b, (char)n);
}

static void w_u64(StrBuf *b, uint64_t n) {
    w_u32(b, (uint32_t)n);
    w_u32(b, (uint32_t)(n >> 32));
}

static void w_str(Writer *w, ObjString *s) {
    if (!s) {
        w_u32(w->out, 0);
        return;
    }
    Value n;
    if (!map_get(w->strings, OBJ_VAL(s), &n)) {
        n = NUM_VAL(w->nstrings++);
        map_set(w->strings, retain(OBJ_VAL(s)), n);
        w_u32(&w->strs, (uint32_t)s->length);
        sb_putn(&w->strs, s->chars, s->length);
    }
    w_u32(w->out, (uint32_t)n.as.number + 1);
}

static void w_obj(Writer *w, void *p, int kind) {
    if (!p) {
        w_u32(w->out, 0);
        return;
    }
    int i = 0;
    while (i < w->nobjs && w->objs[i] != p) i++;
    if (i == w->nobjs) {  /* (new: written when its turn comes) */
        if (w->nobjs == w->capobjs) {
            w->capobjs = w->capobjs ? w->capobjs * 2 : 64;
            w->objs = xrealloc(w->objs, sizeof(void *) * (size_t)w->capobjs);
            w->kinds = xrealloc(w->kinds, sizeof(int) * (size_t)w->capobjs);
        }
        w->objs[w->nobjs] = p;
        w->kinds[w->nobjs++] = kind;
    }
    w_u32(w->out, (uint32_t)i + 1);
}

static void w_var(Writer *w, const VarRef *v) {
    w_str(w, v->name);
    w_u32(w->out, (uint32_t)v->kind);
    w_u32(w->out, (uint32_t)v->index);
    w_u32(w->out, (uint32_t)v->changeable);
    w_u32(w->out, (uint32_t)v->origin);
    w_obj(w, v->watch, IMG_ONLIST);
}

static void w_type(Writer *w, const TypeAnn *t) {
    w_u32(w->out, t->set | (uint32_t)t->d.kind << 8 | (uint32_t)t->d.a << 16 | (uint32_t)t->d.b << 24);
    w_u32(w->out, t->d.any);
    w_str(w, t->cls);
}

static void w_code(Writer *w, const Code *c) {
    StrBuf *o = w->out;
    w_u32(o, (uint32_t)c->ncode);
    for (int i = 0; i < c->ncode; i++) w_word(o, c->code[i]);
    for (int i = 0; i < c->ncode;) {  /* lines: (line, how many words) runs */
        int run = 1;
        while (i + run < c->ncode && c->lines[i + run] == c->lines[i]) run++;
        w_u32(o, (uint32_t)c->lines[i]);
        w_u32(o, (uint32_t)run);
        i += run;
    }
    w_u32(o, (uint32_t)c->nconsts);
    for (int i = 0; i < c->nconsts; i++) {
        Value v = c->consts[i];
        w_u32(o, (uint32_t)v.type | (uint32_t)v.ikind << 8);
        if (v.type == VAL_OBJ) w_str(w, AS_STRING(v));  /* (constants are strings, Numbers, Bools, sized ints) */
        else w_u64(o, v.type == VAL_BOOL ? (uint64_t)v.as.boolean : v.as.u);
    }
    w_u32(o, (uint32_t)c->nprotos);
    for (int i = 0; i < c->nprotos; i++) w_obj(w, c->protos[i], IMG_FUNC);
    w_u32(o, (uint32_t)c->nplaces);
    for (int i = 0; i < c->nplaces; i++) {
        w_var(w, &c->places[i].var);
        w_u32(o, (uint32_t)c->places[i].nkeys);
        for (int k = 0; k < c->places[i].nkeys; k++) w_u32(o, c->places[i].is_field[k]);
    }
    w_u32(o, (uint32_t)c->ncalls);
    for (int i = 0; i < c->ncalls; i++) {
        w_u32(o, (uint32_t)c->calls[i].nargs);
        w_u32(o, (uint32_t)c->calls[i].refs);
        for (int k = 0; k < c->calls[i].nargs; k++) w_u32(o, (uint32_t)c->calls[i].place[k]);
    }
    w_u32(o, (uint32_t)c->nmethods);
    for (int i = 0; i < c->nmethods; i++) {
        w_str(w, c->methods[i].name);
        w_u32(o, (uint32_t)c->methods[i].recv_place);
        w_var(w, &c->methods[i].fallback);
    }
    w_u32(o, (uint32_t)c->nsupers);
    for (int i = 0; i < c->nsupers; i++) {
        w_str(w, c->supers[i].name);
        w_var(w, &c->supers[i].self);
        w_u32(o, (uint32_t)c->supers[i].nargs);
    }
    w_u32(o, (uint32_t)c->nassigns);
    for (int i = 0; i < c->nassigns; i++) {
        w_u32(o, (uint32_t)c->assigns[i].place);
        w_u32(o, (uint32_t)c->assigns[i].op);
        w_u32(o, (uint32_t)c->assigns[i].global);
        w_u32(o, (uint32_t)c->assigns[i].kconst);
    }
    w_u32(o, (uint32_t)c->nclasses);
    for (int i = 0; i < c->nclasses; i++) {
        const ClassDesc *cd = &c->classes[i];
        w_str(w, cd->name);
        w_str(w, cd->key);
        w_var(w, &cd->parent);
        w_u32(o, (uint32_t)cd->nfields);
        for (int k = 0; k < cd->nfields; k++) w_str(w, cd->fields[k]);
        w_u32(o, (uint32_t)cd->nmethods);
        for (int k = 0; k < cd->nmethods; k++) w_obj(w, cd->methods[k], IMG_FUNC);
        w_obj(w, cd->ctor, IMG_FUNC);
        w_u32(o, (uint32_t)cd->hot);
    }
    w_u32(o, (uint32_t)c->nons);
    for (int i = 0; i < c->nons; i++) w_obj(w, c->ons[i], IMG_ON);
    w_u32(o, (uint32_t)c->nlwatch);
    for (int i = 0; i < c->nlwatch; i++) {
        w_u32(o, (uint32_t)c->lwatch[i].slot);
        w_obj(w, c->lwatch[i].list, IMG_ONLIST);
    }
    w_u32(o, (uint32_t)c->nhandlers);
    for (int i = 0; i < c->nhandlers; i++) {
        const Handler *h = &c->handlers[i];
        w_u32(o, (uint32_t)h->start);
        w_u32(o, (uint32_t)h->end);
        w_u32(o, (uint32_t)h->target);
        w_u32(o, (uint32_t)h->sp);
        w_u32(o, (uint32_t)h->catch_slot);
        w_u32(o, (uint32_t)h->clear_from);
    }
    w_u32(o, (uint32_t)c->maxstack);  /* (the debugger's names of locals stay behind) */
}

static void w_func(Writer *w, const FuncDecl *d) {
    StrBuf *o = w->out;
    w_str(w, d->name);
    w_u32(o, (uint32_t)d->nparams);
    for (int i = 0; i < d->nparams; i++) {
        w_str(w, d->params[i]);
        w_u32(o, (uint32_t)d->modes[i] | (uint32_t)(d->has_default ? d->has_default[i] : 0) << 8);
    }
    w_u32(o, d->types != NULL);
    for (int i = 0; d->types && i < d->nparams; i++) w_type(w, &d->types[i]);
    w_type(w, &d->ret);
    w_u32(o, (uint32_t)d->strict | (uint32_t)d->is_arrow << 1 | (uint32_t)d->is_method << 2 | (uint32_t)d->is_ctor << 3 |
                 (uint32_t)d->has_ref << 4 | (uint32_t)d->plain << 5 | (uint32_t)d->plain_method << 6 |
                 (uint32_t)d->plain_ctor << 7 | (uint32_t)d->is_component << 8);
    w_u32(o, (uint32_t)d->comp_slot);
    w_u32(o, (uint32_t)d->nslots);
    w_u32(o, (uint32_t)d->line);
    w_str(w, d->key);
    w_u32(o, (uint32_t)d->ncaptures);
    for (int i = 0; i < d->ncaptures; i++) {
        w_u32(o, (uint32_t)d->captures[i].kind | (uint32_t)d->captures[i].index << 8);
        w_str(w, d->capnames ? d->capnames[i] : NULL);
    }
    w_obj(w, d->on_block, IMG_ON);
    w_code(w, &d->code);
}

static void w_on(Writer *w, const OnDesc *od) {
    StrBuf *o = w->out;
    w_u32(o, (uint32_t)od->count);
    for (int i = 0; i < od->count; i++) {
        w_str(w, od->items[i].name);
        w_var(w, &od->items[i].root);
        w_u32(o, (uint32_t)od->items[i].npath);
        for (int k = 0; k < od->items[i].npath; k++) w_str(w, od->items[i].path[k]);
    }
    w_obj(w, od->func, IMG_FUNC);
    w_u32(o, (uint32_t)od->frame | (uint32_t)od->kind << 1);
    w_u32(o, (uint32_t)od->token);
    w_u32(o, (uint32_t)od->param_slot[0]);
    w_u32(o, (uint32_t)od->param_slot[1]);
    w_u32(o, (uint32_t)od->first);
    w_u32(o, (uint32_t)od->nslots);
    w_u32(o, (uint32_t)od->nuses);
    for (int i = 0; i < od->nuses; i++) {
        w_str(w, od->uses[i].name);
        w_u32(o, (uint32_t)od->uses[i].slot);
    }
}

/* Appends to image an entry: the compiled file `path` (its top-level
 * function d, compiled in mode). */
void lube_image_add(StrBuf *image, const char *path, int mode, FuncDecl *d) {
    Writer w;
    StrBuf objs;
    memset(&w, 0, sizeof w);
    sb_init(&objs);
    w.out = &objs;
    w.strings = new_map();
    StrBuf pkg;  /* the file's package */
    sb_init(&pkg);
    w.out = &pkg;
    w_obj(&w, d, IMG_FUNC);  /* (object 0: numbered first) */
    pkg.len = 0;
    w_str(&w, d->src ? d->src->package : NULL);
    w.out = &objs;
    for (int i = 0; i < w.nobjs; i++) {  /* (writing one may number more) */
        if (w.kinds[i] == IMG_FUNC) w_func(&w, w.objs[i]);
        else if (w.kinds[i] == IMG_ON) w_on(&w, w.objs[i]);
        else {
            OnList *l = w.objs[i];
            w_u32(&objs, (uint32_t)l->n);
            for (int k = 0; k < l->n; k++) w_obj(&w, l->ons[k], IMG_ON);
        }
    }
    StrBuf entry;
    sb_init(&entry);
    w_u32(&entry, (uint32_t)w.nstrings);
    if (w.strs.len) sb_putn(&entry, w.strs.data, w.strs.len);
    sb_putn(&entry, pkg.data, pkg.len);
    w_u32(&entry, (uint32_t)w.nobjs);
    for (int i = 0; i < w.nobjs; i++) sb_putc(&entry, (char)w.kinds[i]);
    sb_putn(&entry, objs.data, objs.len);
    sb_free(&pkg);
    if (!image->len) sb_putn(image, "\0\0\0\0", 4);  /* (the count, kept up to date: 4 bytes) */
    uint32_t count = (uint32_t)(unsigned char)image->data[0] | (uint32_t)(unsigned char)image->data[1] << 8 |
                     (uint32_t)(unsigned char)image->data[2] << 16 | (uint32_t)(unsigned char)image->data[3] << 24;
    count++;
    for (int i = 0; i < 4; i++) image->data[i] = (char)((count >> (8 * i)) & 0xff);
    size_t plen = strlen(path);
    w_u32(image, (uint32_t)plen);
    sb_putn(image, path, plen);
    w_u32(image, (uint32_t)mode);
    w_u32(image, (uint32_t)entry.len);
    sb_putn(image, entry.data, entry.len);
    sb_free(&entry);
    sb_free(&objs);
    sb_free(&w.strs);
    release(OBJ_VAL(w.strings));
    free(w.objs);
    free(w.kinds);
}

#endif

/* ======================================================================
 * Reading
 * ==================================================================== */

/* The image this executable carries (read only, for every isolate's thread). */
static const unsigned char *image_data;
static size_t image_size;

void lube_image_set(const unsigned char *data, size_t size) {
    image_data = data;
    image_size = size;
}

typedef struct {
    Interp *I;
    Arena *arena;
    const unsigned char *p, *end;
    ObjString **strs;
    int nstrs;
    void **objs;
    int *kinds, nobjs;
    int bad;
    Source *src;
} Reader;

static uint32_t r_u32(Reader *r) {
    uint32_t z = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (r->p >= r->end) {  /* LCOV_EXCL_START: a damaged image */
            r->bad = 1;
            return 0;
        }  /* LCOV_EXCL_STOP */
        unsigned char b = *r->p++;
        z |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
    }
    return (z >> 1) ^ (uint32_t)-(int32_t)(z & 1);
}

static uint32_t r_word(Reader *r) {
    uint32_t n = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (r->p >= r->end) {  /* LCOV_EXCL_START: a damaged image */
            r->bad = 1;
            return 0;
        }  /* LCOV_EXCL_STOP */
        unsigned char b = *r->p++;
        n |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
    }
    return n;
}

static uint64_t r_u64(Reader *r) {
    uint64_t lo = r_u32(r);
    return lo | (uint64_t)r_u32(r) << 32;
}

static int r_int(Reader *r) { return (int)(int32_t)r_u32(r); }

static ObjString *r_str(Reader *r) {
    uint32_t i = r_u32(r);
    if (i > (uint32_t)r->nstrs) r->bad = 1;  /* LCOV_EXCL_LINE: a damaged image */
    return i && i <= (uint32_t)r->nstrs ? r->strs[i - 1] : NULL;
}

static void *r_obj(Reader *r) {
    uint32_t i = r_u32(r);
    if (i > (uint32_t)r->nobjs) r->bad = 1;  /* LCOV_EXCL_LINE: a damaged image */
    return i && i <= (uint32_t)r->nobjs ? r->objs[i - 1] : NULL;
}

/* n things of `size` bytes from the image's arena (zeroed). */
static void *r_alloc(Reader *r, size_t n, size_t size) {
    size_t total = n * size;
    if (total == 0) total = 1;
    void *p = arena_alloc(r->arena, total);
    memset(p, 0, total);
    return p;
}

static void r_var(Reader *r, VarRef *v) {
    memset(v, 0, sizeof *v);
    v->name = r_str(r);
    v->kind = (VarKind)r_u32(r);
    v->index = r_int(r);
    v->changeable = r_int(r);
    v->origin = (Origin)r_u32(r);
    v->watch = r_obj(r);
}

static void r_type(Reader *r, TypeAnn *t) {
    uint32_t a = r_u32(r);
    memset(t, 0, sizeof *t);
    t->set = (unsigned char)(a & 0xff);
    t->d.kind = (unsigned char)(a >> 8);
    t->d.a = (unsigned char)(a >> 16);
    t->d.b = (unsigned char)(a >> 24);
    t->d.any = (unsigned char)r_u32(r);
    t->cls = r_str(r);
}

static void r_code(Reader *r, Code *c) {
    c->ncode = r_int(r);
    if (c->ncode < 0 || (size_t)c->ncode > (size_t)(r->end - r->p)) {  /* LCOV_EXCL_START: a damaged image */
        r->bad = 1;
        return;
    }  /* LCOV_EXCL_STOP */
    c->code = r_alloc(r, (size_t)c->ncode, sizeof(Instr));
    c->lines = r_alloc(r, (size_t)c->ncode, sizeof(int));
    for (int i = 0; i < c->ncode; i++) c->code[i] = r_word(r);
    for (int i = 0; i < c->ncode && !r->bad;) {
        int line = r_int(r), run = r_int(r);
        if (run <= 0 || run > c->ncode - i) {  /* LCOV_EXCL_START: a damaged image */
            r->bad = 1;
            return;
        }  /* LCOV_EXCL_STOP */
        while (run--) c->lines[i++] = line;
    }
#define COUNT(field, n)                                                            \
    do {                                                                           \
        n = r_int(r);                                                              \
        if (n < 0 || (size_t)n > (size_t)(r->end - r->p)) { /* LCOV_EXCL_LINE */   \
            r->bad = 1;                                     /* LCOV_EXCL_LINE */   \
            return;                                         /* LCOV_EXCL_LINE */   \
        }                                                   /* LCOV_EXCL_LINE */   \
        field = r_alloc(r, (size_t)n, sizeof *field);                              \
    } while (0)
    COUNT(c->consts, c->nconsts);
    for (int i = 0; i < c->nconsts; i++) {
        uint32_t t = r_u32(r);
        Value v;
        memset(&v, 0, sizeof v);
        v.type = (ValueType)(t & 0xff);
        v.ikind = (unsigned char)(t >> 8);
        if (v.type == VAL_OBJ) {
            ObjString *s = r_str(r);
            v = s ? OBJ_VAL(s) : NIL_VAL;
        } else {
            uint64_t u = r_u64(r);
            if (v.type == VAL_BOOL) v.as.boolean = (int)u;
            else v.as.u = u;
        }
        c->consts[i] = v;
    }
    c->gcache = r_alloc(r, (size_t)c->nconsts, sizeof(int));
    for (int i = 0; i < c->nconsts; i++) c->gcache[i] = -1;
    COUNT(c->protos, c->nprotos);
    for (int i = 0; i < c->nprotos; i++) c->protos[i] = r_obj(r);
    COUNT(c->places, c->nplaces);
    for (int i = 0; i < c->nplaces; i++) {
        r_var(r, &c->places[i].var);
        c->places[i].nkeys = r_int(r);
        if (c->places[i].nkeys < 0 || c->places[i].nkeys > MAX_PATH) {  /* LCOV_EXCL_START: a damaged image */
            r->bad = 1;
            return;
        }  /* LCOV_EXCL_STOP */
        for (int k = 0; k < c->places[i].nkeys; k++) c->places[i].is_field[k] = (unsigned char)r_u32(r);
    }
    COUNT(c->calls, c->ncalls);
    for (int i = 0; i < c->ncalls; i++) {
        int n;
        COUNT(c->calls[i].place, n);
        c->calls[i].nargs = n;
        c->calls[i].refs = r_int(r);
        for (int k = 0; k < n; k++) c->calls[i].place[k] = r_int(r);
    }
    COUNT(c->methods, c->nmethods);
    for (int i = 0; i < c->nmethods; i++) {
        c->methods[i].name = r_str(r);
        c->methods[i].recv_place = r_int(r);
        r_var(r, &c->methods[i].fallback);
        c->methods[i].cache_method = NIL_VAL;
    }
    COUNT(c->supers, c->nsupers);
    for (int i = 0; i < c->nsupers; i++) {
        c->supers[i].name = r_str(r);
        r_var(r, &c->supers[i].self);
        c->supers[i].nargs = r_int(r);
    }
    COUNT(c->assigns, c->nassigns);
    for (int i = 0; i < c->nassigns; i++) {
        c->assigns[i].place = r_int(r);
        c->assigns[i].op = (TokenType)r_u32(r);
        c->assigns[i].global = r_int(r);
        c->assigns[i].kconst = r_int(r);
    }
    COUNT(c->classes, c->nclasses);
    for (int i = 0; i < c->nclasses; i++) {
        ClassDesc *cd = &c->classes[i];
        cd->name = r_str(r);
        cd->key = r_str(r);
        r_var(r, &cd->parent);
        COUNT(cd->fields, cd->nfields);
        for (int k = 0; k < cd->nfields; k++) cd->fields[k] = r_str(r);
        COUNT(cd->methods, cd->nmethods);
        for (int k = 0; k < cd->nmethods; k++) cd->methods[k] = r_obj(r);
        cd->ctor = r_obj(r);
        cd->hot = r_int(r);
    }
    COUNT(c->ons, c->nons);
    for (int i = 0; i < c->nons; i++) c->ons[i] = r_obj(r);
    COUNT(c->lwatch, c->nlwatch);
    for (int i = 0; i < c->nlwatch; i++) {
        c->lwatch[i].slot = r_int(r);
        c->lwatch[i].list = r_obj(r);
    }
    COUNT(c->handlers, c->nhandlers);
    for (int i = 0; i < c->nhandlers; i++) {
        Handler *h = &c->handlers[i];
        h->start = r_int(r);
        h->end = r_int(r);
        h->target = r_int(r);
        h->sp = r_int(r);
        h->catch_slot = r_int(r);
        h->clear_from = r_int(r);
    }
    c->maxstack = r_int(r);
#undef COUNT
}

static void r_func(Reader *r, FuncDecl *d) {
    d->name = r_str(r);
    d->nparams = r_int(r);
    if (d->nparams < 0 || (size_t)d->nparams > (size_t)(r->end - r->p)) {  /* LCOV_EXCL_START: a damaged image */
        r->bad = 1;
        return;
    }  /* LCOV_EXCL_STOP */
    d->params = r_alloc(r, (size_t)d->nparams, sizeof(ObjString *));
    d->modes = r_alloc(r, (size_t)d->nparams, 1);
    d->has_default = r_alloc(r, (size_t)d->nparams, 1);
    d->defaults = r_alloc(r, (size_t)d->nparams, sizeof(Node *));  /* (none: the code computes them) */
    for (int i = 0; i < d->nparams; i++) {
        d->params[i] = r_str(r);
        uint32_t m = r_u32(r);
        d->modes[i] = (unsigned char)(m & 0xff);
        d->has_default[i] = (unsigned char)(m >> 8);
    }
    if (r_u32(r)) {
        d->types = r_alloc(r, (size_t)d->nparams, sizeof(TypeAnn));
        for (int i = 0; i < d->nparams; i++) r_type(r, &d->types[i]);
    }
    r_type(r, &d->ret);
    uint32_t f = r_u32(r);
    d->strict = f & 1;
    d->is_arrow = (f >> 1) & 1;
    d->is_method = (f >> 2) & 1;
    d->is_ctor = (f >> 3) & 1;
    d->has_ref = (unsigned char)((f >> 4) & 1);
    d->plain = (unsigned char)((f >> 5) & 1);
    d->plain_method = (unsigned char)((f >> 6) & 1);
    d->plain_ctor = (unsigned char)((f >> 7) & 1);
    d->is_component = (unsigned char)((f >> 8) & 1);
    d->comp_slot = r_int(r);
    d->nslots = r_int(r);
    d->line = r_int(r);
    d->key = r_str(r);
    d->ncaptures = r_int(r);
    if (d->ncaptures < 0 || (size_t)d->ncaptures > (size_t)(r->end - r->p)) {  /* LCOV_EXCL_START: a damaged image */
        r->bad = 1;
        return;
    }  /* LCOV_EXCL_STOP */
    d->captures = r_alloc(r, (size_t)d->ncaptures, sizeof(CaptureSrc));
    d->capnames = r_alloc(r, (size_t)d->ncaptures, sizeof(ObjString *));
    for (int i = 0; i < d->ncaptures; i++) {
        uint32_t c = r_u32(r);
        d->captures[i].kind = (VarKind)(c & 0xff);
        d->captures[i].index = (int)(c >> 8);
        d->capnames[i] = r_str(r);
    }
    d->on_block = r_obj(r);
    d->src = r->src;
    r_code(r, &d->code);
}

static void r_on(Reader *r, OnDesc *od) {
    od->count = r_int(r);
    if (od->count < 0 || (size_t)od->count > (size_t)(r->end - r->p)) {  /* LCOV_EXCL_START: a damaged image */
        r->bad = 1;
        return;
    }  /* LCOV_EXCL_STOP */
    od->items = r_alloc(r, (size_t)od->count, sizeof(OnItem));
    for (int i = 0; i < od->count; i++) {
        od->items[i].name = r_str(r);
        r_var(r, &od->items[i].root);
        od->items[i].npath = r_int(r);
        if (od->items[i].npath < 0 || (size_t)od->items[i].npath > (size_t)(r->end - r->p)) {  /* LCOV_EXCL_START */
            r->bad = 1;
            return;
        }  /* LCOV_EXCL_STOP */
        od->items[i].path = r_alloc(r, (size_t)od->items[i].npath, sizeof(ObjString *));
        for (int k = 0; k < od->items[i].npath; k++) od->items[i].path[k] = r_str(r);
    }
    od->func = r_obj(r);
    od->frame = r_int(r);
    od->kind = od->frame >> 1;
    od->frame &= 1;
    od->token = r_int(r);
    od->param_slot[0] = r_int(r);
    od->param_slot[1] = r_int(r);
    od->first = r_int(r);
    od->nslots = r_int(r);
    od->nuses = od->capuses = r_int(r);
    if (od->nuses < 0 || (size_t)od->nuses > (size_t)(r->end - r->p)) {  /* LCOV_EXCL_START: a damaged image */
        r->bad = 1;
        return;
    }  /* LCOV_EXCL_STOP */
    od->uses = r_alloc(r, (size_t)od->nuses, sizeof(OnUse));
    for (int i = 0; i < od->nuses; i++) {
        od->uses[i].name = r_str(r);
        od->uses[i].slot = r_int(r);
    }
}

/* Loads entry e (len bytes) into I: its top-level function, or NULL. */
static FuncDecl *read_entry(Interp *I, const unsigned char *e, size_t len, Source *src) {
    Reader r;
    memset(&r, 0, sizeof r);
    r.I = I;
    r.p = e;
    r.end = e + len;
    r.src = src;
    r.arena = arena_new();
    lube_keep_arena(I, r.arena);  /* (its functions last as long as the program) */
    r.nstrs = r_int(&r);
    if (r.nstrs < 0 || (size_t)r.nstrs > len) r.bad = 1, r.nstrs = 0;  /* LCOV_EXCL_LINE: a damaged image */
    r.strs = xmalloc(sizeof(ObjString *) * (size_t)(r.nstrs + 1));
    for (int i = 0; !r.bad && i < r.nstrs; i++) {
        uint32_t n = r_u32(&r);
        if (n > (size_t)(r.end - r.p)) {  /* LCOV_EXCL_START: a damaged image */
            r.bad = 1;
            break;
        }  /* LCOV_EXCL_STOP */
        r.strs[i] = lube_intern(I, (const char *)r.p, n);
        r.p += n;
    }
    src->package = r_str(&r);
    r.nobjs = r.bad ? 0 : r_int(&r);
    if (r.nobjs <= 0 || (size_t)r.nobjs > (size_t)(r.end - r.p)) r.nobjs = 0, r.bad = 1;  /* LCOV_EXCL_LINE: a damaged image */
    r.objs = xmalloc(sizeof(void *) * (size_t)(r.nobjs + 1));
    r.kinds = xmalloc(sizeof(int) * (size_t)(r.nobjs + 1));
    for (int i = 0; i < r.nobjs; i++) {  /* every object first (they point at each other), then their fields */
        int k = *r.p++;
        r.kinds[i] = k;
        r.objs[i] = k == IMG_FUNC ? r_alloc(&r, 1, sizeof(FuncDecl)) : k == IMG_ON ? r_alloc(&r, 1, sizeof(OnDesc))
                                                                                    : r_alloc(&r, 1, sizeof(OnList));
        if (k != IMG_FUNC && k != IMG_ON && k != IMG_ONLIST) r.bad = 1;  /* LCOV_EXCL_LINE: a damaged image */
    }
    for (int i = 0; !r.bad && i < r.nobjs; i++) {
        if (r.kinds[i] == IMG_FUNC) {
            r_func(&r, r.objs[i]);
        } else if (r.kinds[i] == IMG_ON) {
            r_on(&r, r.objs[i]);
        } else {
            OnList *l = r.objs[i];
            l->n = l->cap = r_int(&r);
            if (l->n < 0 || (size_t)l->n > (size_t)(r.end - r.p)) {  /* LCOV_EXCL_START: a damaged image */
                r.bad = 1;
                break;
            }  /* LCOV_EXCL_STOP */
            l->ons = r_alloc(&r, (size_t)l->n, sizeof(OnDesc *));
            for (int k = 0; k < l->n; k++) l->ons[k] = r_obj(&r);
        }
    }
    FuncDecl *d = !r.bad && r.kinds[0] == IMG_FUNC ? r.objs[0] : NULL;
    free(r.strs);
    free(r.objs);
    free(r.kinds);
    return d;
}

/* The compiled file src (in mode) from this executable's image, or NULL
 * when it has none. */
FuncDecl *lube_image_load(Interp *I, Source *src, int mode) {
    const unsigned char *p = image_data, *end = image_data + image_size;
    Reader r;
    memset(&r, 0, sizeof r);
    r.p = p;
    r.end = end;
    uint32_t count = 0;
    if (image_size >= 4) {  /* (the count: 4 bytes, the rest varints) */
        count = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        r.p += 4;
    }
    size_t plen = strlen(src->path);
    for (uint32_t i = 0; i < count && !r.bad; i++) {
        uint32_t n = r_u32(&r);
        if (n > (size_t)(r.end - r.p)) break;  /* LCOV_EXCL_LINE: a damaged image */
        const unsigned char *path = r.p;
        r.p += n;
        uint32_t m = r_u32(&r), len = r_u32(&r);
        if (len > (size_t)(r.end - r.p)) break;  /* LCOV_EXCL_LINE: a damaged image */
        if (n == plen && !memcmp(path, src->path, n) && m == (uint32_t)mode) return read_entry(I, r.p, len, src);
        r.p += len;
    }
    return NULL;  /* LCOV_EXCL_LINE: not there (a damaged executable) */
}
