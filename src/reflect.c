/*
 * reflect.c - reflection: a program looking at its own classes, instances,
 * functions and globals.
 *
 *   class_of(x)               the class of an instance
 *   parent_of(c)              the class c extends (c: a class or an instance)
 *   fields(c)                 [{name, type}] for each field
 *   methods(c)                the names of the methods, inherited ones too
 *   signature(f)              {name, params: [{name, type, mode, has_default}],
 *   signature(c, "method")     returns, builtin}
 *   get_field(x, name)        a field or key, by name
 *   set_field(&x, name, v)    change one, checked against its declared type
 *   call_method(x, name, ...) call a method by name
 *   has_method(x, name)
 *   classes()                 every class the program has made
 *   find_class(name)          a class, from its name
 *   globals()                 the names of the program's global variables,
 *                             functions and classes
 *
 * A class is a value too: it prints as <class Dog>, type() calls it Class,
 * and c.name is its name. (The class's own name alone, Dog, still makes a
 * default instance; a variable holding a class is the class itself.)
 */
#include "lube.h"

#include <string.h>

#define NATIVE(name) static Value name(Interp *I, int argc, Value *argv)
#define FNAME (__func__ + 2)  /* "n_fields" -> "fields" */

/* The class an argument stands for: a class, or an instance's class. */
static ObjClass *class_arg(Interp *I, const char *fn, Value v) {
    if (IS_CLASS(v)) return AS_CLASS(v);
    if (CLASS_OF(v)) return CLASS_OF(v);  /* (an instance, or a plugin's object) */
    lube_error(I, "%s() needs a class or an instance of one, not %s", fn, a_type(v));
    return NULL;
}

/* The text of a name argument, or NULL after raising an error. */
static ObjString *name_arg(Interp *I, const char *fn, Value v) {
    if (IS_TEXT(v)) return text_of(v);
    lube_error(I, "%s() needs a name (a str) as its second argument, not %s", fn, a_type(v));
    return NULL;
}

static void put(Interp *I, ObjMap *m, const char *key, Value v) {
    map_set(m, retain(OBJ_VAL(lube_intern(I, key, strlen(key)))), v);
}

static Value text(const char *s) { return OBJ_VAL(new_string(s, strlen(s))); }

static ObjList *str_list(void) {
    ObjList *l = new_list();
    l->elem = TT_STRING;
    return l;
}

NATIVE(n_class_of) {
    (void)argc;
    ObjClass *c = CLASS_OF(argv[0]);
    if (!c) return lube_error(I, "class_of() needs an instance of a class, not %s", a_type(argv[0]));
    return retain(OBJ_VAL(c));
}

NATIVE(n_parent_of) {
    (void)argc;
    ObjClass *c = class_arg(I, FNAME, argv[0]);
    if (!c) return ERR_VAL;
    if (!c->parent) return lube_error(I, "%s has no parent class: it extends Object directly", c->name->chars);
    return retain(OBJ_VAL(c->parent));
}

NATIVE(n_fields) {
    (void)argc;
    ObjClass *c = class_arg(I, FNAME, argv[0]);
    if (!c) return ERR_VAL;
    ObjList *out = new_list();
    for (int i = 0; i < c->nfields; i++) {
        ObjMap *f = new_map();
        put(I, f, "name", retain(OBJ_VAL(c->field_names[i])));
        put(I, f, "type", text(desc_name(c->field_types[i])));
        list_push(out, OBJ_VAL(f));
    }
    return OBJ_VAL(out);
}

NATIVE(n_methods) {
    (void)argc;
    ObjClass *c = class_arg(I, FNAME, argv[0]);
    if (!c) return ERR_VAL;
    ObjList *out = str_list();
    for (; c; c = c->parent) {  /* its own methods first, then the inherited ones */
        ObjMap *m = c->methods;
        for (size_t i = 0; i < m->used; i++) {
            if (!m->entries[i].live) continue;
            int seen = 0;
            for (size_t j = 0; j < out->count && !seen; j++) seen = values_equal(out->items[j], m->entries[i].key);
            if (!seen) list_push(out, retain(m->entries[i].key));
        }
    }
    return OBJ_VAL(out);
}

/* {name, params, returns, builtin} for a function; `skip` hides the
 * leading `this` of a method or constructor. */
static Value describe_function(Interp *I, const char *name, FuncDecl *d, int skip, const char *returns) {
    ObjMap *sig = new_map();
    put(I, sig, "name", text(name));
    ObjList *params = new_list();
    for (int i = skip; i < d->nparams; i++) {
        ObjMap *p = new_map();
        put(I, p, "name", retain(OBJ_VAL(d->params[i])));
        put(I, p, "type", text(d->types && d->types[i].set ? ann_name(&d->types[i]) : "any"));
        put(I, p, "mode", text(d->modes[i] == PARAM_REF ? "&" : d->modes[i] == PARAM_VAR ? "var" : "let"));
        put(I, p, "has_default", BOOL_VAL(d->has_default[i]));
        list_push(params, OBJ_VAL(p));
    }
    put(I, sig, "params", OBJ_VAL(params));
    if (!returns) returns = d->ret.set ? ann_name(&d->ret) : d->strict ? "nothing" : "any";
    put(I, sig, "returns", text(returns));
    put(I, sig, "builtin", BOOL_VAL(0));
    return OBJ_VAL(sig);
}

static Value builtin_signature(Interp *I, const char *name) {
    Value sig = describe_function(I, name, &(FuncDecl){0}, 0, "any");
    put(I, AS_MAP(sig), "builtin", BOOL_VAL(1));
    return sig;
}

NATIVE(n_signature) {
    Value f = argv[0];
    if (argc == 2) {  /* signature(Dog, "speak"): a method */
        ObjClass *c = class_arg(I, FNAME, f);
        ObjString *name = c ? name_arg(I, FNAME, argv[1]) : NULL;
        if (!name) return ERR_VAL;
        Value m = lube_find_method(c, name);
        if (IS_NIL(m)) return lube_error(I, "%s has no method '%s'", c->name->chars, name->chars);
        if (IS_NATIVE(m)) return builtin_signature(I, name->chars);  /* (a plugin's) */
        return describe_function(I, name->chars, AS_FUNCTION(m)->decl, 1, NULL);
    }
    if (IS_CLASS(f)) {  /* a class: its constructor (its own or the nearest parent's) */
        ObjClass *c = AS_CLASS(f), *owner = c;
        while (owner && IS_NIL(owner->constructor)) owner = owner->parent;
        if (owner && IS_NATIVE(owner->constructor)) return builtin_signature(I, c->name->chars);  /* (a plugin's) */
        if (owner) return describe_function(I, c->name->chars, AS_FUNCTION(owner->constructor)->decl, 1, c->name->chars);
        return describe_function(I, c->name->chars, &(FuncDecl){0}, 0, c->name->chars);  /* Dog(): no arguments */
    }
    if (IS_FUNCTION(f)) {
        FuncDecl *d = AS_FUNCTION(f)->decl;
        return describe_function(I, d->name ? d->name->chars : "<lambda>", d, 0, NULL);
    }
    if (IS_NATIVE(f)) return builtin_signature(I, AS_NATIVE(f)->name);
    return lube_error(I, "signature() needs a function or a class, not %s", a_type(f));
}

NATIVE(n_get_field) {
    (void)argc;
    if (!IS_MAP(argv[0])) return lube_error(I, "get_field() needs an object or an instance, not %s", a_type(argv[0]));
    ObjString *name = name_arg(I, FNAME, argv[1]);
    if (!name) return ERR_VAL;
    Value v;
    if (map_get(AS_MAP(argv[0]), OBJ_VAL(name), &v)) return retain(v);
    return lube_error(I, "%s has no field '%s'", a_type(argv[0]), name->chars);
}

NATIVE(n_set_field) {  /* takes its first argument with & */
    (void)argc;
    if (!IS_MAP(argv[0]) || AS_MAP(argv[0])->dict)
        return lube_error(I, "set_field() needs an object or an instance, not %s", a_type(argv[0]));
    ObjString *name = name_arg(I, FNAME, argv[1]);
    if (!name) return ERR_VAL;
    Value v = argv[2];
    ObjClass *c = AS_MAP(argv[0])->klass;
    if (c) {  /* an instance: only its declared fields, each keeping its type */
        int f = -1;
        for (int i = 0; i < c->nfields && f < 0; i++)
            if (c->field_names[i]->length == name->length && !memcmp(c->field_names[i]->chars, name->chars, name->length))
                f = i;
        if (f < 0) return lube_error(I, "%s has no field '%s'", with_article(c->name->chars), name->chars);
        if (!desc_accepts(c->field_types[f], argv[2], &v))
            return lube_error(I, "field '%s' of %s is %s, so it cannot hold %s", name->chars, c->name->chars,
                              with_article(desc_name(c->field_types[f])), a_type(argv[2]));
    }
    make_unique(&argv[0]);
    map_set(AS_MAP(argv[0]), retain(OBJ_VAL(name)), retain(v));
    return NIL_VAL;
}

NATIVE(n_call_method) {
    ObjString *name = name_arg(I, FNAME, argv[1]);
    if (!name) return ERR_VAL;
    Value self = argv[0];
    if (IS_INSTANCE(self)) {  /* a method of its class; it cannot change the instance */
        Value m = lube_find_method(AS_MAP(self)->klass, name);
        if (!IS_NIL(m)) return lube_call_method(I, m, self, argc - 2, argv + 2);
    }
    if (IS_FOREIGN(self)) {  /* a plugin's method: x first */
        Value m = lube_find_method(AS_FOREIGN(self)->klass, name);
        if (!IS_NIL(m)) {
            Value saved = argv[1];
            argv[1] = self;
            Value r = lube_call(I, m, argc - 1, argv + 1);
            argv[1] = saved;
            return r;
        }
    }
    Value f;
    if (IS_MAP(self) && map_get(AS_MAP(self), OBJ_VAL(name), &f) && IS_CALLABLE(f))  /* a function in a field */
        return lube_call(I, f, argc - 2, argv + 2);
    if (map_get(I->globals, OBJ_VAL(name), &f) && IS_CALLABLE(f)) {  /* x.f(...) is f(x, ...) */
        Value saved = argv[1];  /* owned by the caller's slot: put back below */
        argv[1] = self;         /* so the arguments are x, ... */
        Value r = lube_call(I, f, argc - 1, argv + 1);
        argv[1] = saved;
        return r;
    }
    return lube_error(I, "%s has no method '%s'", a_type(self), name->chars);
}

NATIVE(n_has_method) {
    (void)argc;
    ObjString *name = name_arg(I, FNAME, argv[1]);
    if (!name) return ERR_VAL;
    Value f;
    if (CLASS_OF(argv[0]) && !IS_NIL(lube_find_method(CLASS_OF(argv[0]), name))) return BOOL_VAL(1);
    return BOOL_VAL(IS_MAP(argv[0]) && map_get(AS_MAP(argv[0]), OBJ_VAL(name), &f) && IS_CALLABLE(f));
}

NATIVE(n_classes) {
    (void)argc;
    (void)argv;
    ObjList *out = new_list();
    for (size_t i = 0; i < I->classes->count; i++) list_push(out, retain(I->classes->items[i]));
    return OBJ_VAL(out);
}

NATIVE(n_find_class) {
    (void)argc;
    if (!IS_TEXT(argv[0])) return lube_error(I, "find_class() needs a name (a str), not %s", a_type(argv[0]));
    Value c;
    if (map_get(I->globals, OBJ_VAL(text_of(argv[0])), &c) && IS_CLASS(c)) return retain(c);
    return lube_error(I, "there is no class named '%s'", text_of(argv[0])->chars);
}

NATIVE(n_globals) {
    (void)argc;
    (void)argv;
    ObjList *out = str_list();
    ObjMap *g = I->globals;
    for (size_t i = 0; i < g->used; i++) {  /* in the order the program made them; built-ins left out */
        Value origin;
        if (!g->entries[i].live) continue;
        if (map_get(I->constants, g->entries[i].key, &origin) && (int)origin.as.number == ORIGIN_BUILTIN) continue;
        list_push(out, retain(g->entries[i].key));
    }
    return OBJ_VAL(out);
}

static const struct {
    const char *name;
    NativeFn fn;
    int min, max;
    int changes_first;
} REFLECTION[] = {
    LUBE_BUILTIN(class_of, n_class_of, 1, 1, 0)
    LUBE_BUILTIN(parent_of, n_parent_of, 1, 1, 0)
    LUBE_BUILTIN(fields, n_fields, 1, 1, 0)
    LUBE_BUILTIN(methods, n_methods, 1, 1, 0)
    LUBE_BUILTIN(signature, n_signature, 1, 2, 0)
    LUBE_BUILTIN(get_field, n_get_field, 2, 2, 0)
    LUBE_BUILTIN(set_field, n_set_field, 3, 3, 1)
    LUBE_BUILTIN(call_method, n_call_method, 2, -1, 0)
    LUBE_BUILTIN(has_method, n_has_method, 2, 2, 0)
    LUBE_BUILTIN(classes, n_classes, 0, 0, 0)
    LUBE_BUILTIN(find_class, n_find_class, 1, 1, 0)
    LUBE_BUILTIN(globals, n_globals, 0, 0, 0)
    {NULL, NULL, 0, 0, 0}  /* (the end) */
};

void register_reflection(Interp *I) {
    for (size_t i = 0; REFLECTION[i].name; i++)
        lube_define(I, REFLECTION[i].name, OBJ_VAL(new_native(REFLECTION[i].name, REFLECTION[i].fn, REFLECTION[i].min,
                                                              REFLECTION[i].max, REFLECTION[i].changes_first)));
}
