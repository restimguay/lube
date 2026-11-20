/*
 * resolver.c - a pass over the AST that runs before execution.
 *
 * For every name it decides where the value lives at run time:
 *   - a slot in the current call frame (parameters and locals)
 *   - a captured copy inside the running closure
 *   - the running function itself (a function calling itself by name)
 *   - a global, looked up by name
 *
 * Each function gets one flat frame. Variables of nested blocks get their
 * own slots, which are reused once the block ends, and the interpreter
 * clears them at block exit so values are freed as soon as they go out of
 * scope.
 *
 * A closure captures *copies* of outer variables when it is created. The
 * resolver works out which ones each function needs (including those a
 * nested function needs through it) and records where to copy them from.
 *
 * It also enforces the rules that keep things simple and safe:
 *   - names declared with let (and fn, loop variables, parameters) never change
 *   - a closure cannot change the variables it captured
 *   - return/break/continue only where they make sense
 *
 * An `on` block inside a function is not a closure: it runs on the
 * function's own frame, so its parameters and locals get slots in that
 * frame (kept while the code around the block runs), and it reads and
 * changes the function's variables directly.
 */
#include "lube.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ObjString *name;
    int slot;
    int depth;
    int changeable;
    Origin origin;
    OnList *watch;             /* changeable: the `on` blocks watching it */
} Local;

typedef struct FnCtx {
    struct FnCtx *parent;
    FuncDecl *decl;            /* NULL for top-level code */
    Local *locals;
    int nlocals, cap_locals;
    int depth;                 /* block nesting; 0 = function body / top level */
    int nslots, max_slots;
    int loop_depth;
    CaptureSrc *caps;
    ObjString **capnames;
    int ncaps, cap_caps;
    DebugVar *dvars;           /* a debugger expression: the paused frame's variables */
    int ndvars;
    OnDesc **ons;              /* the `on` blocks being resolved in this frame, innermost last */
    int nons, cap_ons;
} FnCtx;

typedef struct {
    ObjString *name;
    Origin origin;
} GlobalDecl;

typedef struct {
    Interp *I;
    Source *src;
    jmp_buf jb;
    Arena *tmp;                /* scratch memory, freed when resolving ends */
    Arena *tree;               /* the program's arena */
    FnCtx *fn;
    int depth;                 /* current nesting of the syntax tree */
    GlobalDecl *globals;       /* top-level declarations seen so far */
    int nglobals, cap_globals;
    /* namespaces: a file with `package a.b` keeps its globals as a.b::name */
    ObjString *ns;             /* the file's package, or NULL (the main namespace) */
    ObjMap *pkg_names;         /* names the package's files declare (NULL if not found) */
    ObjMap *own;               /* names this file declares at the top level */
    ObjMap *aliases;           /* imported names -> the globals they are */
} Resolver;

static void resolve(Resolver *R, Node *n);

static void rerror(Resolver *R, int line, const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    report_error(R->src, line, 0, "error", msg);
    longjmp(R->jb, 1);
}

/* ---------- scopes and slots ---------- */

static int begin_scope(Resolver *R) {
    R->fn->depth++;
    return R->fn->nslots;
}

/* Ends a block scope; returns how many slots it declared directly. */
static int end_scope(Resolver *R, int start) {
    FnCtx *f = R->fn;
    int n = f->nslots - start;
    while (f->nlocals > 0 && f->locals[f->nlocals - 1].depth == f->depth) f->nlocals--;
    f->depth--;
    f->nslots = start;
    return n;
}

/* Declares a name in the current scope: returns its slot, or -1 for a
 * global (top-level declarations outside any block). */
static int declare(Resolver *R, ObjString *name, int line, int changeable, Origin origin) {
    FnCtx *f = R->fn;
    if (type_tag_from_name(name->chars, name->length) >= 0)
        rerror(R, line, "'%s' is a type name, so it cannot be used as a name here", name->chars);
    if (!f->decl && f->depth == 0) return -1;
    for (int i = f->nlocals - 1; i >= 0 && f->locals[i].depth == f->depth; i--)
        if (f->locals[i].name == name) rerror(R, line, "'%s' is already declared in this scope", name->chars);
    if (f->nlocals == f->cap_locals) {
        int cap = f->cap_locals ? f->cap_locals * 2 : 16;
        f->locals = arena_grow(R->tmp, f->locals, sizeof(Local) * (size_t)f->cap_locals, sizeof(Local) * (size_t)cap);
        f->cap_locals = cap;
    }
    Local *l = &f->locals[f->nlocals++];
    l->name = name;
    l->slot = f->nslots++;
    l->depth = f->depth;
    l->changeable = changeable;
    l->origin = origin;
    l->watch = changeable ? arena_alloc(R->tree, sizeof(OnList)) : NULL;  /* filled by any `on` that watches it */
    if (f->nslots > f->max_slots) f->max_slots = f->nslots;
    return l->slot;
}

static void note_global(Resolver *R, ObjString *name, Origin origin) {
    if (R->nglobals == R->cap_globals) {
        int cap = R->cap_globals ? R->cap_globals * 2 : 16;
        R->globals = arena_grow(R->tmp, R->globals, sizeof(GlobalDecl) * (size_t)R->cap_globals,
                                sizeof(GlobalDecl) * (size_t)cap);
        R->cap_globals = cap;
    }
    R->globals[R->nglobals].name = name;
    R->globals[R->nglobals].origin = origin;
    R->nglobals++;
}

/* Is this global known to be unchangeable? Checks this program's earlier
 * declarations, then built-ins and globals from earlier runs (REPL). */
static int global_is_frozen(Resolver *R, ObjString *name, Origin *origin) {
    for (int i = R->nglobals - 1; i >= 0; i--) {
        if (R->globals[i].name == name) {
            *origin = R->globals[i].origin;
            return *origin != ORIGIN_VAR;
        }
    }
    Value v;
    if (map_get(R->I->constants, OBJ_VAL(name), &v)) {
        *origin = (Origin)(int)v.as.number;
        return 1;
    }
    return 0;
}

static void bind(VarRef *ref, int slot, int changeable, Origin origin) {
    ref->kind = slot < 0 ? VAR_GLOBAL : VAR_LOCAL;
    ref->index = slot < 0 ? 0 : slot;
    ref->changeable = changeable;
    ref->origin = origin;
}

/* ---------- name lookup and captures ---------- */

static Local *find_local(FnCtx *f, ObjString *name) {
    for (int i = f->nlocals - 1; i >= 0; i--)  /* names are interned: compare pointers */
        if (f->locals[i].name == name) return &f->locals[i];
    return NULL;
}

static int add_capture(Resolver *R, FnCtx *f, VarKind kind, int index, ObjString *name) {
    for (int i = 0; i < f->ncaps; i++)
        if (f->caps[i].kind == kind && f->caps[i].index == index) return i;
    if (f->ncaps == f->cap_caps) {
        int cap = f->cap_caps ? f->cap_caps * 2 : 8;
        f->caps = arena_grow(R->tmp, f->caps, sizeof(CaptureSrc) * (size_t)f->cap_caps, sizeof(CaptureSrc) * (size_t)cap);
        f->capnames = arena_grow(R->tmp, f->capnames, sizeof(ObjString *) * (size_t)f->cap_caps,
                                 sizeof(ObjString *) * (size_t)cap);
        f->cap_caps = cap;
    }
    f->caps[f->ncaps].kind = kind;
    f->caps[f->ncaps].index = index;
    f->capnames[f->ncaps] = name;
    return f->ncaps++;
}

/* ---------- namespaces ---------- */

/* The global a top-level declaration of `name` makes in this file. */
static ObjString *decl_key(Resolver *R, ObjString *name) {
    return R->ns ? lube_global_key(R->I, R->ns, name) : name;
}

/* The global a name used in this file means: an imported name; in a package,
 * a name the package declares, else a built-in, else the package's own
 * (perhaps undefined) name; in the main namespace, the name itself. */
static ObjString *ref_key(Resolver *R, ObjString *name) {
    Value v;
    if (map_get(R->aliases, OBJ_VAL(name), &v)) return AS_STRING(v);
    if (!R->ns) return name;
    if (!map_get(R->own, OBJ_VAL(name), NULL) && !(R->pkg_names && map_get(R->pkg_names, OBJ_VAL(name), NULL)) &&
        map_get(R->I->constants, OBJ_VAL(name), &v) && (int)v.as.number == ORIGIN_BUILTIN)
        return name;
    return lube_global_key(R->I, R->ns, name);
}

static ObjMap *package_names(Resolver *R, ObjString *pkg, int line) {
    int bad;
    ObjMap *names = lube_package_names(R->I, pkg, &bad);
    if (bad) rerror(R, line, "package %s has errors (see above)", pkg->chars);
    return names;
}

/* A dotted import: what it brings in (see import_statement in parser.c). */
static void resolve_import(Resolver *R, Node *n) {
    ObjString *dotted = n->as.import.dotted;
    ObjMap *names = package_names(R, dotted, n->line);
    if (n->as.import.star) {  /* every public name of the package */
        if (!names) rerror(R, n->line, "cannot find package %s (in the program's folder or LUBE_PATH)", dotted->chars);
        for (size_t i = 0; i < names->used; i++) {
            ObjString *name = AS_STRING(names->entries[i].key);
            if (name->chars[0] != '_')
                map_set(R->aliases, retain(OBJ_VAL(name)), retain(OBJ_VAL(lube_global_key(R->I, dotted, name))));
        }
        n->as.import.pkg = dotted;
        return;
    }
    if (names) {  /* the package itself: import a.b gives b, a package object */
        const char *dot = strrchr(dotted->chars, '.');
        ObjString *name = n->as.import.alias ? n->as.import.alias
                          : dot ? lube_intern(R->I, dot + 1, strlen(dot + 1)) : dotted;
        n->as.import.pkg = dotted;
        n->as.import.bind = decl_key(R, name);
        map_set(R->own, retain(OBJ_VAL(name)), BOOL_VAL(1));
        return;
    }
    const char *dot = strrchr(dotted->chars, '.');  /* else: a name in a package */
    if (!dot) rerror(R, n->line, "cannot find package %s (in the program's folder or LUBE_PATH)", dotted->chars);
    ObjString *pkg = lube_intern(R->I, dotted->chars, (size_t)(dot - dotted->chars));
    ObjString *name = lube_intern(R->I, dot + 1, strlen(dot + 1));
    names = package_names(R, pkg, n->line);
    if (!names) rerror(R, n->line, "cannot find package %s (in the program's folder or LUBE_PATH)", pkg->chars);
    if (name->chars[0] == '_') rerror(R, n->line, "'%s' is private to package %s", name->chars, pkg->chars);
    if (!map_get(names, OBJ_VAL(name), NULL)) rerror(R, n->line, "package %s has no '%s'", pkg->chars, name->chars);
    map_set(R->aliases, retain(OBJ_VAL(n->as.import.alias ? n->as.import.alias : name)),
            retain(OBJ_VAL(lube_global_key(R->I, pkg, name))));
    n->as.import.pkg = pkg;
}

/* An `on` block's code uses local l from around it (the block cannot run
 * while l is lent out: it would find nothing there). */
static void note_use(Resolver *R, OnDesc *od, Local *l) {
    for (int i = 0; i < od->nuses; i++)
        if (od->uses[i].slot == l->slot) return;
    if (od->nuses == od->capuses) {
        int cap = od->capuses ? od->capuses * 2 : 4;
        od->uses = arena_grow(R->tree, od->uses, sizeof(OnUse) * (size_t)od->capuses, sizeof(OnUse) * (size_t)cap);
        od->capuses = cap;
    }
    od->uses[od->nuses].name = l->name;
    od->uses[od->nuses].slot = l->slot;
    od->nuses++;
}

static VarRef lookup(Resolver *R, FnCtx *f, ObjString *name, int allow_self) {
    VarRef r;
    memset(&r, 0, sizeof r);
    r.name = name;
    r.kind = VAR_GLOBAL;
    r.index = 0;
    r.changeable = 0;
    r.origin = ORIGIN_VAR;
    r.watch = NULL;

    Local *l = find_local(f, name);
    if (l) {
        r.kind = VAR_LOCAL;
        r.index = l->slot;
        r.changeable = l->changeable;
        r.origin = l->origin;
        r.watch = l->watch;
        for (int i = 0; i < f->nons; i++)  /* declared outside an `on` block being resolved */
            if (l->slot < f->ons[i]->first) note_use(R, f->ons[i], l);
        return r;
    }
    for (int i = f->ndvars - 1; i >= 0; i--) {  /* the paused frame's variables (the innermost of a name wins) */
        if (f->dvars[i].name == name) {
            r.kind = f->dvars[i].kind;
            r.index = f->dvars[i].index;
            r.origin = ORIGIN_CAPTURE;
            return r;
        }
    }
    if (allow_self && f->decl && f->decl->name == name) {
        r.kind = VAR_SELF;
        r.origin = ORIGIN_SELF;
        return r;
    }
    if (!f->parent) {  /* global */
        r.name = ref_key(R, name);
        return r;
    }
    VarRef outer = lookup(R, f->parent, name, 1);
    if (outer.kind == VAR_GLOBAL) {
        r.name = outer.name;
        return r;
    }
    r.kind = VAR_CAPTURE;
    r.index = add_capture(R, f, outer.kind, outer.index, name);
    r.origin = ORIGIN_CAPTURE;
    return r;
}

/* The target of an assignment or of `&`: its root variable must be one this
 * code is allowed to change. (Globals declared with let are checked at run
 * time, since globals can come from other files.) */
static void resolve_target(Resolver *R, Node *t, int line) {
    Node *root_target = t;
    while (t->kind == N_INDEX || t->kind == N_FIELD) {
        if (t->kind == N_INDEX) {
            resolve(R, t->as.index.index);
            t = t->as.index.object;
        } else {
            t = t->as.field.object;
        }
    }
    if (t->kind != N_VAR) rerror(R, line, "can only change a variable, or an index or field of one");
    if (t == root_target && !strcmp(t->as.var.name->chars, "this"))
        rerror(R, line, "cannot replace 'this' itself; change its fields instead");
    VarRef r = lookup(R, R->fn, t->as.var.name, 0);
    Origin origin = r.origin;
    if (r.kind == VAR_GLOBAL ? global_is_frozen(R, r.name, &origin) : !r.changeable) {
        char msg[300];
        unchangeable_message(msg, sizeof msg, t->as.var.name->chars, origin);
        rerror(R, line, "%s", msg);
    }
    t->as.var = r;
}

/* ---------- nodes ---------- */

static void resolve_list(Resolver *R, NodeList *l) {
    for (int i = 0; i < l->count; i++) resolve(R, l->items[i]);
}

static void resolve_block(Resolver *R, Node *b) {
    int start = begin_scope(R);
    resolve_list(R, &b->as.block.stmts);
    b->as.block.first_slot = start;
    b->as.block.nslots = end_scope(R, start);
}

static void resolve_function(Resolver *R, FuncDecl *d) {
    FnCtx f;
    memset(&f, 0, sizeof f);
    f.parent = R->fn;
    f.decl = d;
    R->fn = &f;
    for (int i = 0; i < d->nparams && d->types; i++)  /* classes named in types: the globals they are */
        if (d->types[i].set && d->types[i].d.kind == TD_CLASS) d->types[i].cls = ref_key(R, d->types[i].cls);
    if (d->ret.set && d->ret.d.kind == TD_CLASS) d->ret.cls = ref_key(R, d->ret.cls);

    for (int i = 0; i < d->nparams; i++) {
        for (int j = 0; j < i; j++)
            if (d->params[j] == d->params[i]) rerror(R, d->line, "duplicate parameter '%s'", d->params[i]->chars);
        /* a default sees only the parameters before it: those already have values */
        if (d->defaults[i]) resolve(R, d->defaults[i]);
        declare(R, d->params[i], d->line, d->modes[i] != PARAM_LET, d->is_method && i == 0 ? ORIGIN_THIS : ORIGIN_PARAM);
    }
    if (d->is_arrow) resolve(R, d->body);
    else resolve_list(R, &d->body->as.block.stmts);  /* the body shares the parameters' scope */

    d->nslots = f.max_slots;
    d->ncaptures = f.ncaps;
    if (f.ncaps) {
        d->captures = arena_alloc(R->tree, sizeof(CaptureSrc) * (size_t)f.ncaps);
        memcpy(d->captures, f.caps, sizeof(CaptureSrc) * (size_t)f.ncaps);
        d->capnames = arena_alloc(R->tree, sizeof(ObjString *) * (size_t)f.ncaps);
        memcpy(d->capnames, f.capnames, sizeof(ObjString *) * (size_t)f.ncaps);
    }
    R->fn = f.parent;
}

static void resolve_node(Resolver *R, Node *n);

/* An `on` block that runs on this frame: a hidden slot (holding the block
 * while it is in scope), then the block's parameters and locals. All of
 * them stay taken until the code around the block ends. */
static void resolve_frame_block(Resolver *R, OnDesc *od, FuncDecl *d) {
    FnCtx *f = R->fn;
    od->token = f->nslots++;
    int saved_max = f->max_slots > f->nslots ? f->max_slots : f->nslots, saved_loops = f->loop_depth;
    f->max_slots = f->nslots;  /* measures the block's own slots */
    f->loop_depth = 0;         /* break and continue cannot leave it */
    od->first = f->nslots;
    if (f->nons == f->cap_ons) {
        int cap = f->cap_ons ? f->cap_ons * 2 : 4;
        f->ons = arena_grow(R->tmp, f->ons, sizeof(OnDesc *) * (size_t)f->cap_ons, sizeof(OnDesc *) * (size_t)cap);
        f->cap_ons = cap;
    }
    f->ons[f->nons++] = od;
    int start = begin_scope(R);
    for (int i = 0; i < d->nparams; i++) od->param_slot[i] = declare(R, d->params[i], d->line, 0, ORIGIN_PARAM);
    resolve_list(R, &d->body->as.block.stmts);
    end_scope(R, start);
    f->nons--;
    od->nslots = f->max_slots - od->first;
    d->nslots = f->max_slots;
    d->on_block = od;
    f->nslots = f->max_slots;
    if (saved_max > f->max_slots) f->max_slots = saved_max;
    f->loop_depth = saved_loops;
}

/* on a, this.b (old, new) { ... }: what it watches, then the block. At the
 * top level of a file it watches globals and is a function of its own;
 * anywhere else it can watch the variables there too, and runs on the frame. */
static void resolve_on(Resolver *R, Node *n) {
    FnCtx *f = R->fn;
    OnDesc *od = arena_alloc(R->tree, sizeof(OnDesc));
    od->items = n->as.on.items;
    od->count = n->as.on.count;
    od->func = n->as.on.func;
    od->frame = f->decl || f->depth > 0;
    n->as.on.desc = od;
    for (int i = 0; i < od->count; i++) {
        OnItem *it = &od->items[i];
        VarRef r = lookup(R, f, it->name, 0);
        if (r.kind == VAR_GLOBAL && !strcmp(it->name->chars, "this"))
            rerror(R, n->line, "'this' can only be used inside a class's methods and constructor");
        Origin origin = r.origin;
        int frozen = r.kind == VAR_GLOBAL ? global_is_frozen(R, r.name, &origin) : !r.changeable;
        if (frozen) {
            char msg[300];
            unchangeable_message(msg, sizeof msg, it->name->chars, origin);
            rerror(R, n->line, "this 'on' block would never run: %s", msg);
        }
        it->root = r;
        if (r.kind == VAR_LOCAL) {  /* the code that changes it runs the block (see compile.c) */
            OnList *w = r.watch;
            if (w->n == w->cap) {
                int cap = w->cap ? w->cap * 2 : 2;
                w->ons = arena_grow(R->tree, w->ons, sizeof(OnDesc *) * (size_t)w->cap, sizeof(OnDesc *) * (size_t)cap);
                w->cap = cap;
            }
            w->ons[w->n++] = od;
        }
    }
    if (od->frame) resolve_frame_block(R, od, od->func);
    else resolve_function(R, od->func);
}

/* Long chains like 1 + 1 + ... build deep trees without deep nesting in
 * the source, so the tree depth is limited here, before anything else
 * walks the tree recursively. */
static void resolve(Resolver *R, Node *n) {
    if (!n) return;
    if (++R->depth > LUBE_MAX_TREE_DEPTH || !lube_stack_ok(R->I))
        rerror(R, n->line, "this code is nested too deeply (split long expressions into smaller parts)");
    resolve_node(R, n);
    R->depth--;
}

static void resolve_node(Resolver *R, Node *n) {
    switch (n->kind) {
    case N_IMPORT:
        if (n->as.import.dotted && (R->fn->decl || R->fn->depth > 0))
            rerror(R, n->line, "package imports go at the top level of a file");
        if (R->I->imports_found) {  /* building an executable: note every module and package to pack */
            if (n->as.import.path) {
                list_push(R->I->imports_found, retain(OBJ_VAL(n->as.import.path)));
            } else {
                StrBuf sb;
                sb_init(&sb);
                sb_putc(&sb, '\x01');  /* marks a package */
                sb_puts(&sb, n->as.import.pkg->chars);
                list_push(R->I->imports_found, OBJ_VAL(new_string(sb.data, sb.len)));
                sb_free(&sb);
            }
        }
        break;
    case N_LITERAL:
    case N_TYPED_MAP:
        break;
    case N_INTERP:
    case N_LIST:
        resolve_list(R, &n->as.items);
        break;
    case N_TYPED_LIST:
        resolve_list(R, &n->as.tlist.items);
        break;
    case N_MAP:
        resolve_list(R, &n->as.map.keys);
        resolve_list(R, &n->as.map.values);
        break;
    case N_VAR: {
        ObjString *name = n->as.var.name;
        n->as.var = lookup(R, R->fn, name, 1);
        if (n->as.var.kind == VAR_GLOBAL && !strcmp(name->chars, "this"))
            rerror(R, n->line, "'this' can only be used inside a class's methods and constructor");
        break;
    }
    case N_SUPER:
        if (!R->fn->decl || !R->fn->decl->is_method)
            rerror(R, n->line, "'super' can only be used directly inside a class's methods and constructor");
        if (!n->as.super_call.name && strcmp(R->fn->decl->name->chars, "constructor"))
            rerror(R, n->line, "super(...) calls the parent's constructor, so it can only be used in a constructor");
        resolve(R, n->as.super_call.self);
        resolve_list(R, &n->as.super_call.args);
        break;
    case N_IS:
        resolve(R, n->as.is.value);
        resolve(R, n->as.is.cls);
        break;
    case N_ON:
        resolve_on(R, n);
        break;
    case N_CLASS: {
        if (R->fn->decl || R->fn->depth > 0)
            rerror(R, n->line, "classes can only be declared at the top level of a file");
        VarRef *v = &n->as.klass.var;
        bind(v, declare(R, v->name, n->line, 0, ORIGIN_CLASS), 0, ORIGIN_CLASS);
        v->name = decl_key(R, v->name);
        note_global(R, v->name, ORIGIN_CLASS);
        resolve(R, n->as.klass.parent);
        NodeList *fields = &n->as.klass.fields, *methods = &n->as.klass.methods;
        for (int i = 0; i < fields->count; i++) {
            Node *f = fields->items[i];
            for (int j = 0; j < i; j++)
                if (fields->items[j]->as.let.var.name == f->as.let.var.name)
                    rerror(R, f->line, "field '%s' is declared twice", f->as.let.var.name->chars);
            resolve(R, f->as.let.value);
        }
        for (int i = 0; i < methods->count; i++) {
            FuncDecl *d = methods->items[i]->as.func;
            for (int j = 0; j < i; j++)
                if (methods->items[j]->as.func->name == d->name)
                    rerror(R, d->line, "method '%s' is declared twice", d->name->chars);
            for (int j = 0; j < fields->count; j++)
                if (fields->items[j]->as.let.var.name == d->name)
                    rerror(R, d->line, "'%s' is both a field and a method", d->name->chars);
            resolve_function(R, d);
        }
        if (n->as.klass.ctor) {
            FuncDecl *d = n->as.klass.ctor->as.func;
            for (int i = 1; i < d->nparams; i++)
                if (d->modes[i] == PARAM_REF) rerror(R, d->line, "a constructor cannot take & parameters");
            resolve_function(R, d);
        }
        break;
    }
    case N_ASSIGN:
        resolve(R, n->as.assign.value);
        resolve_target(R, n->as.assign.target, n->line);
        break;
    case N_REF:
        resolve_target(R, n->as.expr, n->line);
        break;
    case N_UNARY:
        resolve(R, n->as.unary.operand);
        break;
    case N_BINARY:
    case N_AND:
    case N_OR:
        resolve(R, n->as.binary.left);
        resolve(R, n->as.binary.right);
        break;
    case N_CALL: {
        Node *callee = n->as.call.callee;
        ObjString *raw = callee->kind == N_VAR ? callee->as.var.name : NULL;
        resolve(R, callee);
        resolve_list(R, &n->as.call.args);
        if (raw && callee->as.var.kind == VAR_GLOBAL) {  /* a top-level fn (of this file, or made before): always callable */
            Value v;
            Origin origin;
            n->as.call.known_fn = (map_get(R->own, OBJ_VAL(raw), &v) && IS_NUM(v) && v.as.number == 2 &&
                                   callee->as.var.name == decl_key(R, raw)) ||
                                  (global_is_frozen(R, callee->as.var.name, &origin) && origin == ORIGIN_FN);
        }
        break;
    }
    case N_METHOD:
        resolve(R, n->as.method.object);
        resolve_list(R, &n->as.method.args);
        n->as.method.fallback = lookup(R, R->fn, n->as.method.name, 1);
        break;
    case N_INDEX:
        resolve(R, n->as.index.object);
        resolve(R, n->as.index.index);
        break;
    case N_FIELD:
        resolve(R, n->as.field.object);
        break;
    case N_FUNC:
        resolve_function(R, n->as.func);
        break;
    case N_RANGE:
        resolve(R, n->as.range.start);
        resolve(R, n->as.range.end);
        break;
    case N_EXPR:
    case N_THROW:
        resolve(R, n->as.expr);
        break;
    case N_RETURN: {  /* in an `on` block's code: ends the block */
        FuncDecl *d = R->fn->nons ? R->fn->ons[R->fn->nons - 1]->func : R->fn->decl;
        if (!d) rerror(R, n->line, "'return' can only be used inside a function");
        const char *fname = d->name ? d->name->chars : "this function";
        const char *paren = d->name ? "()" : "";
        if (!n->as.expr && d->ret.set)
            rerror(R, n->line, "%s%s must return %s, so 'return' needs a value", fname, paren,
                   with_article(ann_name(&d->ret)));
        if (n->as.expr && d->strict && !d->ret.set)
            rerror(R, n->line, "#strict: %s%s has no return type, so it cannot return a value (declare one, like ): Number)",
                   fname, paren);
        resolve(R, n->as.expr);
        break;
    }
    case N_BREAK:
    case N_CONTINUE:
        if (R->fn->loop_depth == 0)
            rerror(R, n->line, "'%s' can only be used inside a loop", n->kind == N_BREAK ? "break" : "continue");
        break;
    case N_LET: {
        VarRef *v = &n->as.let.var;
        if (n->as.let.is_fn) {
            /* declared first; inside its body the name means the function itself */
            bind(v, declare(R, v->name, n->line, 0, ORIGIN_FN), 0, ORIGIN_FN);
            if (v->kind == VAR_GLOBAL) {
                v->name = decl_key(R, v->name);
                n->as.let.value->as.func->key = v->name;  /* spawn() finds it by this */
                note_global(R, v->name, ORIGIN_FN);
            }
            resolve(R, n->as.let.value);
        } else {
            /* resolved first: in `let x = x + 1` the right side is the outer x */
            resolve(R, n->as.let.value);
            int var = n->as.let.is_var;
            Origin origin = var ? ORIGIN_VAR : ORIGIN_LET;
            bind(v, declare(R, v->name, n->line, var, origin), var, origin);
            if (v->kind == VAR_GLOBAL) {
                v->name = decl_key(R, v->name);
                note_global(R, v->name, origin);
            }
        }
        break;
    }
    case N_BLOCK:
        resolve_block(R, n);
        break;
    case N_IF:
        resolve(R, n->as.if_s.cond);
        resolve_block(R, n->as.if_s.then_branch);
        resolve(R, n->as.if_s.else_branch);  /* an N_BLOCK or another N_IF */
        break;
    case N_WHILE:
        resolve(R, n->as.while_s.cond);
        R->fn->loop_depth++;
        resolve_block(R, n->as.while_s.body);
        R->fn->loop_depth--;
        break;
    case N_FOR: {
        resolve(R, n->as.for_s.iter);
        int start = begin_scope(R);
        bind(&n->as.for_s.first, declare(R, n->as.for_s.first.name, n->line, 0, ORIGIN_LOOP), 0, ORIGIN_LOOP);
        if (n->as.for_s.two)
            bind(&n->as.for_s.second, declare(R, n->as.for_s.second.name, n->line, 0, ORIGIN_LOOP), 0, ORIGIN_LOOP);
        R->fn->loop_depth++;
        resolve_list(R, &n->as.for_s.body->as.block.stmts);
        R->fn->loop_depth--;
        n->as.for_s.first_slot = start;
        n->as.for_s.nslots = end_scope(R, start);
        break;
    }
    case N_TRY:
        resolve_block(R, n->as.try_s.body);
        if (n->as.try_s.has_var) {
            int start = begin_scope(R);
            bind(&n->as.try_s.var, declare(R, n->as.try_s.var.name, n->line, 0, ORIGIN_CATCH), 0, ORIGIN_CATCH);
            resolve_list(R, &n->as.try_s.handler->as.block.stmts);
            n->as.try_s.first_slot = start;
            n->as.try_s.nslots = end_scope(R, start);
        } else {
            resolve_block(R, n->as.try_s.handler);
        }
        break;
    }
}

int resolve_program(Interp *I, Node *program, Source *src, Arena *tree) {
    Resolver R;
    FnCtx top;
    memset(&top, 0, sizeof top);
    R.I = I;
    R.src = src;
    R.tmp = arena_new();
    R.tree = tree;
    R.fn = &top;
    R.depth = 0;
    R.globals = NULL;
    R.nglobals = R.cap_globals = 0;
    R.ns = src->package;
    R.pkg_names = NULL;
    R.own = new_map();
    R.aliases = new_map();
    if (setjmp(R.jb) != 0) {
        release(OBJ_VAL(R.own));
        release(OBJ_VAL(R.aliases));
        arena_free(R.tmp);
        return 0;
    }
    NodeList *stmts = &program->as.block.stmts;
    if (!R.ns && I->main_aliases)  /* the main namespace keeps its imports (from one REPL line to the next) */
        for (size_t i = 0; i < I->main_aliases->used; i++)
            map_set(R.aliases, retain(I->main_aliases->entries[i].key), retain(I->main_aliases->entries[i].value));
    for (int i = 0; i < stmts->count; i++) {  /* the names this file declares */
        Node *s = stmts->items[i];
        ObjString *name = s->kind == N_LET ? s->as.let.var.name : s->kind == N_CLASS ? s->as.klass.var.name : NULL;
        if (!name) continue;
        int fn = s->kind == N_LET && s->as.let.is_fn && !map_get(R.own, OBJ_VAL(name), NULL);  /* 2: a fn (declared once) */
        map_set(R.own, retain(OBJ_VAL(name)), NUM_VAL(fn ? 2 : 1));
    }
    if (R.ns) R.pkg_names = package_names(&R, R.ns, 1);
    for (int i = 0; i < stmts->count; i++)  /* imports first: they decide what names mean */
        if (stmts->items[i]->kind == N_IMPORT && stmts->items[i]->as.import.dotted) resolve_import(&R, stmts->items[i]);
    resolve_list(&R, stmts);
    program->as.block.frame_slots = top.max_slots;
    if (!R.ns) {
        if (!I->main_aliases) I->main_aliases = new_map();
        for (size_t i = 0; i < R.aliases->used; i++)
            map_set(I->main_aliases, retain(R.aliases->entries[i].key), retain(R.aliases->entries[i].value));
    }
    release(OBJ_VAL(R.own));
    if (src->aliases) release(OBJ_VAL(src->aliases));
    src->aliases = R.aliases;  /* kept for the debugger's expressions */
    arena_free(R.tmp);
    return 1;
}

/* A debugger expression, wrapped in the function d: resolved as if written
 * in the paused code (in file `home`), seeing the paused frame's variables. */
int resolve_debug(Interp *I, FuncDecl *d, Source *src, Arena *tree, Source *home, DebugVar *vars, int nvars) {
    Resolver R;
    FnCtx top;
    memset(&top, 0, sizeof top);
    top.dvars = vars;
    top.ndvars = nvars;
    R.I = I;
    R.src = src;
    R.tmp = arena_new();
    R.tree = tree;
    R.fn = &top;
    R.depth = 0;
    R.globals = NULL;
    R.nglobals = R.cap_globals = 0;
    R.ns = home ? home->package : NULL;
    R.pkg_names = NULL;
    R.own = new_map();
    R.aliases = new_map();
    if (setjmp(R.jb) != 0) {
        release(OBJ_VAL(R.own));
        release(OBJ_VAL(R.aliases));
        arena_free(R.tmp);
        return 0;
    }
    if (home && home->aliases)
        for (size_t i = 0; i < home->aliases->used; i++)
            map_set(R.aliases, retain(home->aliases->entries[i].key), retain(home->aliases->entries[i].value));
    if (R.ns) R.pkg_names = package_names(&R, R.ns, 1);
    resolve_function(&R, d);
    release(OBJ_VAL(R.own));
    release(OBJ_VAL(R.aliases));
    arena_free(R.tmp);
    return 1;
}
