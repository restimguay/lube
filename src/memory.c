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

LUBE_THREAD_LOCAL size_t lube_live_objects = 0;
size_t lube_max_items = LUBE_DEFAULT_MAX_ITEMS;
size_t lube_max_bytes = LUBE_DEFAULT_MAX_BYTES;

Obj *alloc_object(size_t size, ObjType type) {
    Obj *o = xmalloc(size);
    o->rc = 1;
    o->type = type;
    lube_live_objects++;
    return o;
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
            free(l->items);
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
        } else if (o->type == OBJ_CHANNEL) {  /* shared between isolates: counted apart */
            channel_release(((ObjChannel *)o)->ch);
        } else if (o->type == OBJ_ISOLATE) {
            isolate_release(((ObjIsolate *)o)->iso);
        } else if (o->type == OBJ_PACKAGE) {
            drop(&fl, OBJ_VAL(((ObjPackage *)o)->name));
        }  /* strs, natives and ranges own nothing else */
        free(o);
        lube_live_objects--;
    }
    if (fl.items != fl.small) free(fl.items);
}
