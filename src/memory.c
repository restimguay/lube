/*
 * memory.c - object allocation and freeing. There is no garbage collector.
 *
 * Every object counts its owners (retain/release in lube.h). Because values
 * have value semantics and closures capture copies, an object can never end
 * up owning itself, so a count of zero always means "unreachable" and the
 * object is freed right away, together with anything only it owned.
 *
 * lube_live_objects counts objects currently alive. Built with
 * -DLUBE_CHECK_LEAKS, the interpreter verifies at exit that it is zero.
 */
#include "lube.h"

#include <stdlib.h>
#include <string.h>

LUBE_THREAD_LOCAL size_t lube_live_objects = 0;
size_t lube_max_items = LUBE_DEFAULT_MAX_ITEMS;
size_t lube_max_bytes = LUBE_DEFAULT_MAX_BYTES;

/* Small objects (up to POOL_CLASSES * 16 bytes) come from free lists of
 * this thread, one per size in steps of 16 bytes, and go back to them:
 * quicker than malloc and free, for the many small lists and strings
 * programs make. (Built with LUBE_CHECK_LEAKS, every object has a malloc
 * of its own instead, for AddressSanitizer to watch.) */
#ifdef LUBE_CHECK_LEAKS
#define POOL_CLASSES 0
#else
#define POOL_CLASSES 8
#endif
static LUBE_THREAD_LOCAL void *pool[POOL_CLASSES + 1];  /* each free block holds the next */
static LUBE_THREAD_LOCAL void *slabs;  /* the memory they come from, 32 KB at a time (each holds the next first) */
#define SLAB 32768

static void *pool_get(size_t size, unsigned char *cls) {
    size_t c = (size + 15) / 16;
    if (c == 0 || c > POOL_CLASSES) {
        *cls = 0;
        return xmalloc(size);
    }
    *cls = (unsigned char)c;
    void *p = pool[c];
    if (!p) {  /* a new slab, cut into blocks of this size (its first 16 bytes: the list of slabs) */
        char *slab = xmalloc(SLAB);
        *(void **)slab = slabs;
        slabs = slab;
        size_t size_c = c * 16, n = (SLAB - 16) / size_c;
        for (size_t i = n; i > 1; i--) {
            void *b = slab + 16 + (i - 1) * size_c;
            *(void **)b = pool[c];
            pool[c] = b;
        }
        return slab + 16;
    }
    pool[c] = *(void **)p;
    return p;
}

static void pool_put(void *p, unsigned char cls) {
    if (!cls) {
        free(p);
        return;
    }
    *(void **)p = pool[cls];
    pool[cls] = p;
}

/* Memory for a list's items (size bytes): small arrays come from the free
 * lists too. lube_block_free is given the same size back. */
void *lube_block_alloc(size_t size) {
    unsigned char cls;
    return pool_get(size, &cls);
}

void lube_block_free(void *p, size_t size) {
    size_t c = (size + 15) / 16;
    pool_put(p, c > POOL_CLASSES ? 0 : (unsigned char)c);
}

void *lube_block_realloc(void *p, size_t old, size_t size) {
    if (old > POOL_CLASSES * 16 && size > POOL_CLASSES * 16) return xrealloc(p, size);
    void *n = lube_block_alloc(size);
    if (old) memcpy(n, p, old < size ? old : size);
    lube_block_free(p, old);
    return n;
}

/* Frees this thread's slabs (when its interpreter ends: none of its
 * objects is left, and none went to another thread: isolates send copies). */
void lube_pool_drain(void) {
    for (int c = 1; c <= POOL_CLASSES; c++) pool[c] = NULL;
    while (slabs) {
        void *next = *(void **)slabs;
        free(slabs);
        slabs = next;
    }
}

Obj *alloc_object(size_t size, ObjType type) {
    unsigned char cls;
    Obj *o = pool_get(size, &cls);
    o->rc = 1;
    o->type = type;
    o->pool = cls;
    lube_live_objects++;
    return o;
}

/* Object o (of old_size bytes) made size bytes, perhaps moved. */
Obj *realloc_object(Obj *o, size_t old_size, size_t size) {
    if (!o->pool) return xrealloc(o, size);  /* (malloc's stays malloc's) */
    unsigned char cls;
    Obj *n = pool_get(size, &cls);
    memcpy(n, o, old_size < size ? old_size : size);
    pool_put(o, o->pool);
    n->pool = cls;
    return n;
}

/* A growable stack of objects waiting to be freed. Freeing works through
 * this list instead of recursing, so even a list nested a million levels
 * deep cannot overflow the C stack. */
typedef struct {
    Obj **items;
    size_t count, cap;
    Obj *small[64];
} FreeList;

static void push_dead(FreeList *fl, Obj *o) {
    if (fl->count == fl->cap) {
        size_t cap = fl->cap * 2;
        if (fl->items == fl->small) {
            fl->items = xmalloc(cap * sizeof(Obj *));
            for (size_t i = 0; i < fl->count; i++) fl->items[i] = fl->small[i];
        } else {
            fl->items = xrealloc(fl->items, cap * sizeof(Obj *));
        }
        fl->cap = cap;
    }
    fl->items[fl->count++] = o;
}

static void drop(FreeList *fl, Value v) {
    if (v.type == VAL_OBJ && --v.as.obj->rc == 0) push_dead(fl, v.as.obj);
}

void obj_destroy(Obj *first) {
    FreeList fl;
    fl.items = fl.small;
    fl.count = 0;
    fl.cap = sizeof fl.small / sizeof fl.small[0];
    push_dead(&fl, first);

    while (fl.count > 0) {
        Obj *o = fl.items[--fl.count];
        if (o->type == OBJ_LIST) {
            ObjList *l = (ObjList *)o;
            for (size_t i = 0; i < l->count; i++) drop(&fl, l->items[i]);
            lube_block_free(l->items, l->capacity * sizeof(Value));
        } else if (o->type == OBJ_MAP) {
            ObjMap *m = (ObjMap *)o;
            for (size_t i = 0; i < m->used; i++) {
                if (!m->entries[i].live) continue;
                drop(&fl, m->entries[i].key);
                drop(&fl, m->entries[i].value);
            }
            free(m->entries);
            free(m->index);
        } else if (o->type == OBJ_FUNCTION) {
            ObjFunction *f = (ObjFunction *)o;
            for (int i = 0; i < f->ncaptures; i++) drop(&fl, f->captures[i]);
        } else if (o->type == OBJ_STROBJ) {
            drop(&fl, OBJ_VAL(((ObjStrObj *)o)->text));
        } else if (o->type == OBJ_CLASS) {  /* only at shutdown: classes live until then */
            ObjClass *c = (ObjClass *)o;
            for (int i = 0; i < c->nfields; i++) drop(&fl, c->field_defaults[i]);
            drop(&fl, OBJ_VAL(c->methods));
            drop(&fl, c->constructor);
            free(c->field_names);
            free(c->field_defaults);
            free(c->field_types);
        } else if (o->type == OBJ_BYTES) {
            ObjBytes *b = (ObjBytes *)o;
            lube_block_free(b->data, b->cap);
        } else if (o->type == OBJ_FOREIGN) {  /* a plugin's: it lets go of what it holds (a file closes) */
            lube_foreign_free((ObjForeign *)o);
        } else if (o->type == OBJ_CHANNEL) {  /* shared between isolates: counted apart */
            channel_release(((ObjChannel *)o)->ch);
        } else if (o->type == OBJ_ISOLATE) {
            isolate_release(((ObjIsolate *)o)->iso);
        } else if (o->type == OBJ_PACKAGE) {
            drop(&fl, OBJ_VAL(((ObjPackage *)o)->name));
        }  /* strs, natives and ranges own nothing else */
        pool_put(o, o->pool);
        lube_live_objects--;
    }
    if (fl.items != fl.small) free(fl.items);
}
