/*
 * lube.h - shared definitions for the Lube interpreter.
 *
 * Lube is a small, dynamically typed scripting language. Its interpreter is
 * written from scratch in C99 and depends on nothing but the C standard
 * library: no parser generators, no third-party code.
 *
 *   source text -> lexer -> tokens -> parser -> AST -> resolver -> interpreter
 *
 * Memory model (no garbage collector):
 *   - Values have *value semantics*: assigning or passing a list or map gives
 *     the receiver its own copy. Copies are cheap because data is shared until
 *     one side changes it (copy-on-write, using a reference count).
 *   - Closures capture copies of the variables they use, never references.
 *   - Therefore no value can ever contain itself: there are no cycles, and
 *     every value is freed the moment its last owner lets go of it.
 *   - In-place changes go through `&` (pass a variable for changing) or
 *     assignment to a `var`. Bindings made with `let` can never change.
 *
 *   lexer.c     turns source text into tokens (with automatic semicolons)
 *   parser.c    builds an abstract syntax tree by recursive descent
 *   resolver.c  binds variables to frame slots, checks let/var and closures
 *   interp.c    walks the tree and executes it
 *   value.c     strings, lists, maps, ranges, printing, equality
 *   memory.c    reference counts and freeing
 *   builtins.c  the standard library (print, len, map, filter, ...)
 *   util.c      string buffers, file reading, error reporting
 *   main.c      command line interface and REPL
 */
#ifndef LUBE_H
#define LUBE_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#define LUBE_VERSION "0.1.0"

/* Hard limits, checked before any size arithmetic so that no size can ever
 * overflow, and so one expression cannot claim all of the machine's memory.
 * Operations that know the size up front (like "x" * n) report an error;
 * growing past a limit step by step stops the program with "out of memory". */
#define LUBE_MIN_SIZE(a, b) ((a) < (b) ? (a) : (b))
#define LUBE_DEFAULT_MAX_ITEMS LUBE_MIN_SIZE((size_t)1 << 28, SIZE_MAX / 128)  /* list/object items */
#define LUBE_DEFAULT_MAX_BYTES LUBE_MIN_SIZE((size_t)1 << 30, SIZE_MAX / 4)    /* string, source file */
/* The environment variables LUBE_MAX_ITEMS and LUBE_MAX_BYTES can lower
 * these (never raise them), for example to sandbox a script. */
/* Storage each thread has its own copy of: every isolate runs on its own
 * thread with its own heap, so per-thread counters and buffers need no locks. */
#ifndef LUBE_THREAD_LOCAL
#if defined(_MSC_VER)
#define LUBE_THREAD_LOCAL __declspec(thread)
#else
#define LUBE_THREAD_LOCAL __thread
#endif
#endif

extern size_t lube_max_items, lube_max_bytes;  /* set once, before any isolate starts */
#define LUBE_MAX_ITEMS lube_max_items
#define LUBE_MAX_BYTES lube_max_bytes
#define LUBE_MAX_TREE_DEPTH 2500                /* nesting of the syntax tree */

/* Keeps rarely used code (error messages, places) out of the recursive
 * evaluator's stack frames, so deep recursion uses less C stack. */
#if defined(__GNUC__) || defined(__clang__)
#define LUBE_COLD __attribute__((noinline, cold))
#define LUBE_NOINLINE __attribute__((noinline))
#else
#define LUBE_COLD
#define LUBE_NOINLINE
#endif

typedef struct Interp Interp;
typedef struct Node Node;
typedef struct FuncDecl FuncDecl;
typedef struct Source Source;

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjList ObjList;
typedef struct ObjMap ObjMap;
typedef struct ObjFunction ObjFunction;
typedef struct ObjNative ObjNative;
typedef struct ObjRange ObjRange;
typedef struct ObjStrObj ObjStrObj;
typedef struct ObjClass ObjClass;

/* ======================================================================
 * Values
 * ==================================================================== */

typedef enum {
    VAL_NIL, VAL_BOOL, VAL_NUM, VAL_INT, VAL_OBJ,
    VAL_ERR,    /* internal: "an error is being raised", never stored */
    VAL_LENT    /* internal: a global whose value is lent out with & */
} ValueType;

/* Sized integers (integer.c). Arithmetic on them is checked: a result that
 * does not fit is an error, never a silent wraparound. */
typedef enum { IK_I8, IK_I16, IK_I32, IK_I64, IK_U8, IK_U16, IK_U32, IK_U64 } IntKind;
#define IK_SIGNED(k) ((k) <= IK_I64)

typedef struct {
    ValueType type;
    unsigned char ikind;    /* VAL_INT only: which IntKind */
    union {
        int boolean;
        double number;      /* VAL_NUM: every plain number is a double */
        int64_t i;          /* VAL_INT, signed kinds */
        uint64_t u;         /* VAL_INT, unsigned kinds */
        Obj *obj;
    } as;
} Value;

#define NIL_VAL      ((Value){.type = VAL_NIL, .as = {.number = 0}})
#define BOOL_VAL(b)  ((Value){.type = VAL_BOOL, .as = {.boolean = (b) ? 1 : 0}})
#define NUM_VAL(n)   ((Value){.type = VAL_NUM, .as = {.number = (n)}})
#define OBJ_VAL(o)   ((Value){.type = VAL_OBJ, .as = {.obj = (Obj *)(o)}})
#define ERR_VAL      ((Value){.type = VAL_ERR, .as = {.number = 0}})
#define LENT_VAL     ((Value){.type = VAL_LENT, .as = {.number = 0}})
#define SINT_VAL(k, x) ((Value){.type = VAL_INT, .ikind = (unsigned char)(k), .as = {.i = (x)}})
#define UINT_VAL(k, x) ((Value){.type = VAL_INT, .ikind = (unsigned char)(k), .as = {.u = (x)}})

#define IS_NIL(v)    ((v).type == VAL_NIL)
#define IS_BOOL(v)   ((v).type == VAL_BOOL)
#define IS_NUM(v)    ((v).type == VAL_NUM)
#define IS_INT(v)    ((v).type == VAL_INT)
#define IS_NUMERIC(v) (IS_NUM(v) || IS_INT(v))
#define IS_OBJ(v)    ((v).type == VAL_OBJ)
#define IS_ERR(v)    ((v).type == VAL_ERR)
#define OBJ_TYPE(v)  ((v).as.obj->type)

#define IS_STRING(v)   (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_STRING)
#define IS_LIST(v)     (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_LIST)
#define IS_MAP(v)      (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_MAP)
#define IS_FUNCTION(v) (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_FUNCTION)
#define IS_NATIVE(v)   (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_NATIVE)
#define IS_RANGE(v)    (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_RANGE)
#define IS_CHANNEL(v)  (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_CHANNEL)
#define IS_ISOLATE(v)  (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_ISOLATE)
#define IS_PACKAGE(v)  (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_PACKAGE)
#define IS_STROBJ(v)   (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_STROBJ)
#define IS_CLASS(v)    (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_CLASS)
#define IS_INSTANCE(v) (IS_MAP(v) && AS_MAP(v)->klass != NULL)
#define IS_TEXT(v)     (IS_STRING(v) || IS_STROBJ(v))  /* a str or a String */
#define IS_CALLABLE(v) (IS_FUNCTION(v) || IS_NATIVE(v))

#define AS_STRING(v)   ((ObjString *)(v).as.obj)
#define AS_LIST(v)     ((ObjList *)(v).as.obj)
#define AS_MAP(v)      ((ObjMap *)(v).as.obj)
#define AS_FUNCTION(v) ((ObjFunction *)(v).as.obj)
#define AS_NATIVE(v)   ((ObjNative *)(v).as.obj)
#define AS_RANGE(v)    ((ObjRange *)(v).as.obj)
#define AS_CHANNEL(v)  ((ObjChannel *)(v).as.obj)
#define AS_ISOLATE(v)  ((ObjIsolate *)(v).as.obj)
#define AS_PACKAGE(v)  ((ObjPackage *)(v).as.obj)
#define AS_STROBJ(v)   ((ObjStrObj *)(v).as.obj)
#define AS_CLASS(v)    ((ObjClass *)(v).as.obj)

/* ======================================================================
 * Heap objects. Each one counts its owners; it is freed when the count
 * drops to zero. Lists and maps are only changed in place when their count
 * is 1, so a change is never visible through another owner.
 * ==================================================================== */

typedef enum {
    OBJ_STRING, OBJ_LIST, OBJ_MAP, OBJ_FUNCTION, OBJ_NATIVE, OBJ_RANGE, OBJ_STROBJ, OBJ_CLASS,
    OBJ_CHANNEL, OBJ_ISOLATE, OBJ_PACKAGE
} ObjType;

struct Obj {
    size_t rc;              /* number of owners */
    ObjType type;
    unsigned char pool;     /* memory.c: the free list its memory goes back to (0: malloc's) */
};

struct ObjString {          /* a str: byte string, NUL terminated */
    Obj obj;
    size_t length, capacity;
    uint32_t hash;
    int hashed;
    char chars[];
};

/* Type names. A bare type name is that type's default value (Object is {},
 * String is "", int32 is 0), Type(x) converts, and Type[] is a typed list. */
typedef enum {
    TT_ANY, TT_STRING, TT_STROBJ, TT_NUMBER, TT_BOOL, TT_OBJECT, TT_MAP, TT_LIST, TT_FUNCTION, TT_RANGE,
    TT_CHANNEL, TT_ISOLATE,
    TT_INT8, TT_INT16, TT_INT32, TT_INT64, TT_UINT8, TT_UINT16, TT_UINT32, TT_UINT64
} TypeTag;
#define TT_COUNT (TT_UINT64 + 1)

struct ObjList {
    Obj obj;
    size_t count, capacity;
    Value *items;
    int elem;               /* TypeTag every item must have; TT_ANY = untyped */
};

typedef struct {
    Value key;
    Value value;
    uint32_t hash;
    int live;               /* 0 once the entry has been deleted */
} MapEntry;

struct ObjMap {             /* insertion-ordered hash map */
    Obj obj;
    MapEntry *entries;      /* entries in insertion order */
    size_t used;            /* entries[0..used) are occupied (live or dead) */
    size_t count;           /* live entries */
    size_t entry_cap;
    int32_t *index;         /* open addressing table: positions in entries */
    size_t index_cap;       /* always a power of two */
    /* An Object ({...}) has String, Number or Bool keys. A Map takes keys of
     * any type, compared by contents; Map[K, V] also checks both types. */
    unsigned char dict;     /* 1 for a Map */
    unsigned char ktype, vtype;  /* TypeTags for Map[K, V]; TT_ANY otherwise */
    ObjClass *klass;        /* for an instance of a class: its class (fields are the entries) */
};

/* A declared type: what a var, a parameter or a class field may hold. A var
 * keeps the type it starts with; values of a subtype (a subclass, or any
 * object for Object) are accepted too. */
enum {
    TD_NUM, TD_BOOL, TD_INT, TD_STR, TD_STROBJ, TD_LIST, TD_MAP, TD_OBJECT, TD_FUNCTION, TD_RANGE,
    TD_CHANNEL, TD_ISOLATE,
    TD_CLASSVAL,  /* a class itself (from class_of or find_class) */
    TD_CLASS      /* an instance of a class or its subclasses */
};
typedef struct {
    unsigned char kind;     /* TD_* */
    unsigned char a, b;     /* TD_INT: kind; TD_LIST: element tag; TD_MAP: key and value tags */
    unsigned char any;      /* TD_LIST, TD_MAP: any list or map (a parameter declared List or Map) */
    ObjClass *cls;          /* TD_CLASS */
} TypeDesc;

typedef Value (*NativeFn)(Interp *I, int argc, Value *argv);

struct ObjNative {
    Obj obj;
    const char *name;
    NativeFn fn;
    int min_args, max_args; /* max_args < 0 means "any number" */
    int changes_first;      /* takes its first argument with & (push, pop, ...) */
};

struct ObjFunction {
    Obj obj;
    FuncDecl *decl;
    int ncaptures;
    Value captures[];       /* copies of the outer variables it uses */
};

/* A channel or an isolate lives outside any one isolate's heap (isolate.c);
 * each isolate that holds one has its own wrapper object. */
typedef struct ObjChannel {
    Obj obj;
    struct Channel *ch;
} ObjChannel;

typedef struct ObjIsolate {
    Obj obj;
    struct Isolate *iso;
} ObjIsolate;

typedef struct ObjPackage {  /* import a.b: the package, for b.Name */
    Obj obj;
    ObjString *name;
} ObjPackage;

struct ObjRange {
    Obj obj;
    double start, end, step;
    int inclusive;
};

/* A class. Classes are created once, at the top level of a file, and live
 * until the interpreter shuts down (instances and declared types point to
 * them). Every class extends Object; `parent` is NULL for a direct child. */
struct ObjClass {
    Obj obj;
    ObjString *name;        /* as written: Point */
    ObjString *key;         /* the global it is in: Point, or lube.geometry::Point in a package */
    ObjClass *parent;
    int nfields;            /* all fields, the parent's first */
    ObjString **field_names;
    Value *field_defaults;  /* starting values, copied into each new instance */
    TypeDesc *field_types;  /* fields keep the type they start with */
    ObjMap *methods;        /* this class's own methods: name -> function */
    Value constructor;      /* this class's constructor, or NIL_VAL */
};

/* A String: an object wrapping a str, with read-only properties computed
 * from the text (length, value, upper, lower, chars, words). A plain str
 * ("hello") has no properties. */
struct ObjStrObj {
    Obj obj;
    ObjString *text;
};

/* The text of a str or a String (borrowed). */
static inline ObjString *text_of(Value v) {
    return v.as.obj->type == OBJ_STRING ? (ObjString *)v.as.obj : ((ObjStrObj *)v.as.obj)->text;
}

/* memory.c */
extern LUBE_THREAD_LOCAL size_t lube_live_objects;  /* objects alive in this thread's isolate */
Obj *alloc_object(size_t size, ObjType type);
Obj *realloc_object(Obj *o, size_t old_size, size_t size);
void obj_destroy(Obj *o);
void lube_pool_drain(void);
void *lube_block_alloc(size_t size);
void lube_block_free(void *p, size_t size);
void *lube_block_realloc(void *p, size_t old, size_t size);

static inline Value retain(Value v) {
    if (v.type == VAL_OBJ) v.as.obj->rc++;
    return v;
}

static inline void release(Value v) {
    if (v.type == VAL_OBJ && --v.as.obj->rc == 0) obj_destroy(v.as.obj);
}

/* ======================================================================
 * Source files and tokens
 * ==================================================================== */

struct Source {
    char *path;
    char *text;
    size_t length;
    int strict;             /* the file starts with #strict: every fn declares its types */
    ObjString *package;     /* `package a.b`: the file's package (NULL: the main namespace) */
    ObjMap *aliases;        /* the names it imported (for the debugger's expressions) */
    Source *next;
};

typedef enum {
    T_EOF, T_NEWLINE,
    T_IDENT, T_NUMBER, T_STRING,
    T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_LBRACE, T_RBRACE,
    T_COMMA, T_DOT, T_DOTDOT, T_DOTDOTEQ, T_COLON, T_SEMI, T_AMP,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_POW,
    T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ, T_PERCENT_EQ,
    T_ASSIGN, T_EQ, T_BANG, T_NEQ, T_LT, T_LE, T_GT, T_GE,
    T_PIPE, T_ARROW,
    /* keywords: keep this block contiguous, from T_AND to T_WHILE */
    T_AND, T_BREAK, T_CATCH, T_CLASS, T_CONTINUE, T_ELSE, T_EXTENDS, T_FALSE, T_FN,
    T_FOR, T_IF, T_IMPORT, T_IN, T_IS, T_LET, T_NIL, T_NOT, T_ON, T_OR, T_PACKAGE, T_RETURN,
    T_SUPER, T_THIS, T_THROW, T_TRUE, T_TRY, T_VAR, T_WHILE
} TokenType;

typedef struct {
    TokenType type;
    const char *start;      /* points into the source text */
    int length;
    int line, col;
    double number;          /* value of a T_NUMBER token */
} Token;

typedef struct {
    Token *items;
    int count, cap;
} TokenArray;

/* ======================================================================
 * Abstract syntax tree
 * ==================================================================== */

typedef enum {
    /* expressions */
    N_LITERAL, N_INTERP, N_LIST, N_MAP, N_VAR, N_ASSIGN, N_UNARY, N_BINARY,
    N_AND, N_OR, N_CALL, N_METHOD, N_INDEX, N_FIELD, N_FUNC, N_RANGE,
    N_REF,  /* `&place` in an argument list */
    N_TYPED_LIST,  /* String[] or String["a", "b"] */
    N_TYPED_MAP,   /* Map or Map[Object, String] */
    N_SUPER,       /* super.method(args) or super(args) */
    N_IS,          /* x is Type */
    /* statements */
    N_EXPR, N_LET, N_BLOCK, N_IF, N_WHILE, N_FOR, N_RETURN, N_BREAK,
    N_CONTINUE, N_THROW, N_TRY, N_IMPORT, N_CLASS,
    N_ON           /* on score (old, new) { ... }: runs when a variable changes */
} NodeKind;

typedef struct {
    Node **items;
    int count, cap;
} NodeList;

typedef enum { VAR_GLOBAL, VAR_LOCAL, VAR_CAPTURE, VAR_SELF } VarKind;

/* Where a name came from; decides whether it may change, and how an error
 * explains it. Only ORIGIN_VAR (and var/& parameters) can change. */
typedef enum {
    ORIGIN_VAR, ORIGIN_LET, ORIGIN_PARAM, ORIGIN_LOOP, ORIGIN_CATCH,
    ORIGIN_FN, ORIGIN_BUILTIN, ORIGIN_CAPTURE, ORIGIN_SELF, ORIGIN_THIS, ORIGIN_CLASS
} Origin;

/* A variable reference, filled in by the resolver:
 *   VAR_LOCAL    slot `index` of the current call frame
 *   VAR_CAPTURE  the running closure's captured copy number `index`
 *   VAR_SELF     the running function itself (for recursion)
 *   VAR_GLOBAL   looked up by name at run time */
typedef struct OnList OnList;

/* lube --build --native: a function compiled to machine code (see native.c).
 * fn returns 0 with the result in *out, or 1 to let the VM run the call
 * itself (an error to report, or calls too deep). Bools are 0 or 1. */
typedef int (*LubeNativeFn)(const double *args, double *out, int depth_left);
typedef struct LubeNative {
    const char *key;            /* the fn's global */
    const char *sig;            /* its parameters' types, ':', its return type: N (Number), B (Bool), or none;
                                   L, O: a list of records, a record (then fnv takes the arguments as they are) */
    uint64_t src_hash;          /* of the source file it was compiled from (natives fit only that text) */
    LubeNativeFn fn;
    int (*fnv)(Interp *I, Value *args, double *out, int depth_left);
} LubeNative;

/* Records (native.c): the Number fields of an object, copied to and from an array. */
#define LUBE_REC_MAX 16
typedef struct {  /* per Interp, and per class: where the fields are */
    Interp *I;
    ObjString *names[LUBE_REC_MAX];
    struct ObjClass *klass;
    int pos[LUBE_REC_MAX];
    struct ObjClass *exact;  /* the class they must be (methods are called on them), once found by its key */
} LubeRecCache;
int lube_rec_load(Interp *I, LubeRecCache *c, Value v, const char *const *fields, int nf, const char *cls, double *out);
int lube_reclist_load(Interp *I, LubeRecCache *c, Value v, const char *const *fields, int nf, const char *cls, double **out, int64_t *n,
                      double *buf, size_t bufsize);
void lube_rec_store(Interp *I, LubeRecCache *c, Value *slot, int nf, const double *vals, const double *orig);
void lube_reclist_store(Interp *I, LubeRecCache *c, Value *list, int nf, const double *vals, const double *orig);
Value *lube_native_global(Interp *I, const char *name, int written);
int lube_numlist_load(Value v, int bools, int typed, void **a, int64_t *n);
void lube_numlist_store(Value *slot, int bools, void *a, int64_t n);
extern const LubeNative lube_natives[];  /* (nativetab.c, or the native build's own) */
extern const int lube_nnatives;

typedef struct {
    ObjString *name;
    VarKind kind;
    int index;
    int changeable;         /* locals only; globals are checked at run time */
    Origin origin;
    OnList *watch;          /* a changeable local: the `on` blocks that watch it (the list may be empty) */
    /* (run time, a global) caches: where it is in I->globals (+1; 0: not yet), and its
     * origin (-1: a var) as of I->epoch == cepoch */
    int gslot;
    unsigned cepoch;
    int corigin;
} VarRef;

typedef struct {
    VarKind kind;           /* VAR_LOCAL, VAR_CAPTURE or VAR_SELF */
    int index;
} CaptureSrc;

enum { PARAM_LET = 0, PARAM_VAR = 1, PARAM_REF = 2 };

/* A type written on a parameter (x: Number) or a return value (): str).
 * A built-in type is complete in `d`; a class is named by `cls` and looked
 * up when the function is called. `set` is 0 where no type was written. */
typedef struct {
    TypeDesc d;
    ObjString *cls;         /* TD_CLASS: the class's name */
    unsigned char set;
} TypeAnn;

/* ======================================================================
 * Bytecode: compile.c turns a resolved syntax tree into it, interp.c runs
 * it. An instruction is one 32-bit word, the opcode in the low 8 bits and
 * an operand in the high 24; a few take more operands in the words after.
 * ==================================================================== */

typedef uint32_t Instr;
#define INSTR(op, a) ((Instr)(op) | ((Instr)(a) << 8))
#define OP_OF(i) ((int)((i) & 0xffu))
#define ARG_OF(i) ((int)((i) >> 8))
#define MAX_ARG 0xffffff
#define MAX_PATH 16         /* the longest x.a[b].c path one assignment or & can change */

typedef enum {
    OP_CONST, OP_POP, OP_POPN, OP_LOCAL, OP_CAPTURE, OP_SELF, OP_GLOBAL, OP_GLOBAL_VALUE,
    OP_DEF_LOCAL, OP_DEF_GLOBAL, OP_CLEAR, OP_NEED_VALUE, OP_INTERP, OP_LIST, OP_TYPED_LIST,
    OP_CHECK_ELEM, OP_MAP, OP_CHECK_KEY, OP_TYPED_MAP, OP_BINARY, OP_NEG, OP_NOT, OP_JUMP,
    OP_JUMP_IF_FALSE, OP_AND, OP_OR, OP_LOOP, OP_IS_TAG, OP_IS_CLASS, OP_RANGE_START, OP_RANGE,
    OP_INDEX, OP_FIELD, OP_CLOSURE, OP_CHECK_CALLABLE, OP_CALL, OP_CALL_PREP, OP_METHOD_PREP,
    OP_SUPER_PREP, OP_INVOKE, OP_RETURN, OP_RETURN_NIL, OP_THROW, OP_FOR_PREP, OP_FOR_NEXT,
    OP_WATCH_BEFORE, OP_WATCH_AFTER, OP_ASSIGN, OP_IMPORT, OP_CLASS_BEGIN, OP_CLASS_FIELD,
    OP_CLASS_FIELD_SET, OP_CLASS_END, OP_ON, OP_LWATCH_BEFORE, OP_LWATCH_AFTER,
    OP_ASSIGN_DROP, OP_BINARY2, OP_CMP_JUMP, OP_SET_LOCAL, OP_CALL_FN, OP_INDEX2, OP_FIELD_LOCAL, OP_ARG_DEFAULT, OP_DEF_PARAM, OP_PRINT_REPL,
    OP_HOT_KEEP, OP_IF_GLOBAL, OP_FAIL, OP_LAZY_GLOBAL, OP_IMPORT_PACKAGE,
    OP_NATIVE_LOOP,  /* a top-level loop's machine code (lube --build --native): skips its bytecode when it ran */
    OP_COUNT
} OpCode;

/* Where an operand of OP_BINARY2 or OP_CMP_JUMP comes from: a local's slot
 * or a Number constant, read in place, or else the stack. */
enum { OPND_STACK, OPND_LOCAL, OPND_CONST };

typedef struct {            /* x, x.a or x[i][j]: a variable, then keys taken from the stack */
    VarRef var;
    int nkeys;
    unsigned char is_field[MAX_PATH];
} PlaceDesc;

typedef struct {            /* a call's arguments: each a value, or & a place */
    int nargs;
    int *place;             /* place[i]: an index into Code.places, or -1 for a value */
    int refs;               /* some argument is & a place */
} CallDesc;

typedef struct {            /* x.name(args) */
    ObjString *name;
    int recv_place;         /* the receiver's place, or -1 when it is not a variable path */
    VarRef fallback;        /* the variable `name`: x.f(a) can mean f(x, a) */
    ObjClass *cache_class;  /* (run time) the method last found here, for instances of this class */
    Value cache_method;     /* (borrowed: classes and their methods live until shutdown) */
    unsigned cache_epoch;   /* ...as of this I->epoch */
} MethodDesc;

typedef struct {            /* super.name(args), or super(args) when name is NULL */
    ObjString *name;
    VarRef self;            /* `this` */
    int nargs;
} SuperDesc;

typedef struct {            /* place op value, where op is = or += and the like */
    int place;
    TokenType op;
    int global;             /* for a global var: its name's constant (else -1) */
    int kconst;             /* OP_ASSIGN_DROP: the value is this Number constant (else -1: on the stack) */
} AssignDesc;

typedef struct {            /* class Name extends Parent { fields, methods, constructor } */
    ObjString *name;        /* as written */
    ObjString *key;         /* the global it goes in (a.b::Name in package a.b) */
    VarRef parent;          /* parent.name is NULL without extends */
    int nfields;
    ObjString **fields;
    int nmethods;
    FuncDecl **methods;
    FuncDecl *ctor;         /* NULL without a constructor */
    int hot;                /* compiled for a hot reload: update the class that exists */
} ClassDesc;

typedef struct {            /* what an `on` block watches: a variable, or a field path in one */
    ObjString *name;        /* as written (the root variable) */
    VarRef root;            /* (resolver) a local's slot, or a global's name */
    ObjString **path;       /* on this.hp: the fields after the variable (npath of them) */
    int npath;
} OnItem;

typedef struct {            /* a variable an `on` block's code uses from around it */
    ObjString *name;
    int slot;
} OnUse;

/* on a, b (old, new) { ... }. At the top level of a file the block is a
 * function that lasts as long as the program. Anywhere else (in a function,
 * a method or a block) it runs on the frame it was declared in, seeing that
 * frame's variables live, and lasts until the block of code around it ends. */
typedef struct OnDesc {
    OnItem *items;
    int count;
    FuncDecl *func;         /* the block (with (old, new) as its parameters) */
    int frame;              /* runs on a frame (else: a top-level watcher) */
    int token;              /* frame: the hidden slot that holds the block while it is in scope */
    int param_slot[2];      /* frame: where (old, new) go */
    int first, nslots;      /* frame: the block's own slots, cleared after each run */
    OnUse *uses;            /* frame: the variables from around it that it reads */
    int nuses, capuses;
} OnDesc;

struct OnList {             /* the `on` blocks watching one local variable */
    OnDesc **ons;
    int n, cap;
};

typedef struct {            /* a local variable the code changes, which `on` blocks watch */
    int slot;
    OnList *list;
} LocalWatch;

typedef struct {            /* a local variable, for the debugger */
    ObjString *name;
    int slot;
    int start, end;         /* the code words while it holds its value */
} LocalInfo;

typedef struct {            /* try { body } catch e { ... } */
    int start, end;         /* the body: code words [start, end) */
    int target;             /* where the catch begins */
    int sp;                 /* operand stack depth (above the slots) when the try began */
    int catch_slot;         /* the caught error's slot, or -1 */
    int clear_from;         /* the body's slots (and every slot above) are cleared first */
} Handler;

typedef struct {
    Instr *code;
    int *lines;             /* the source line of each code word */
    int ncode;
    Value *consts;          /* numbers, bools and interned strings */
    int nconsts;
    FuncDecl **protos;      /* functions made by OP_CLOSURE */
    int nprotos;
    PlaceDesc *places;
    int nplaces;
    CallDesc *calls;
    int ncalls;
    MethodDesc *methods;
    int nmethods;
    SuperDesc *supers;
    int nsupers;
    AssignDesc *assigns;
    int nassigns;
    ClassDesc *classes;
    int nclasses;
    OnDesc **ons;
    int nons;
    LocalWatch *lwatch;     /* for OP_LWATCH_BEFORE/AFTER */
    int nlwatch;
    Handler *handlers;
    int nhandlers;
    int maxstack;           /* the most operand stack slots it needs */
    LocalInfo *locals;      /* its locals' names (for the debugger) */
    int nlocals;
    int *gcache;            /* per constant: where that global was last found in I->globals (or -1) */
} Code;

struct FuncDecl {
    ObjString *name;        /* NULL for anonymous functions */
    ObjString **params;
    Node **defaults;        /* defaults[i] is NULL when param i has none */
    unsigned char *modes;   /* PARAM_LET, PARAM_VAR or PARAM_REF */
    TypeAnn *types;         /* types[i] for param i; NULL when no parameter has a type */
    TypeAnn ret;            /* the return type, if written */
    int strict;             /* written in a #strict file */
    int nparams;
    Node *body;             /* N_BLOCK, or an expression when is_arrow */
    int is_arrow;
    int nslots;             /* frame size: parameters plus locals */
    CaptureSrc *captures;
    ObjString **capnames;   /* the captured variables' names (for the debugger) */
    int ncaptures;
    int line;
    Source *src;
    int is_method;          /* its first parameter is the implicit `this` */
    int is_ctor;            /* a class's constructor */
    ObjString *key;         /* a top-level fn: the global it is in (its package's name for it) */
    ObjClass *owner;        /* the class a method belongs to (set when the class is made) */
    unsigned char *has_default;  /* has_default[i]: param i has a default value */
    struct OnDesc *on_block;     /* an `on` block that runs on its function's frame */
    unsigned char has_ref;       /* a parameter takes & (compile.c) */
    unsigned char plain;         /* (compile.c) no & or var parameter, none with a default, each untyped or a
                                    Number, not a constructor: OP_CALL starts it without enter() */
    unsigned char plain_method;  /* a method whose other parameters are like that: OP_INVOKE's fast path */
    const LubeNative *native;    /* compiled to machine code (lube --build --native), or NULL */
    Code code;              /* its bytecode (compile.c) */
};

struct Node {
    NodeKind kind;
    int line;
    union {
        Value literal;                                            /* N_LITERAL */
        NodeList items;                                           /* N_INTERP, N_LIST */
        struct { NodeList items; int elem; } tlist;               /* N_TYPED_LIST */
        struct { int ktype, vtype; } tmap;                        /* N_TYPED_MAP */
        struct { ObjString *name; NodeList args; Node *self; } super_call;   /* N_SUPER (name NULL: constructor) */
        struct { Node *value; int tag; Node *cls; } is;           /* N_IS: tag >= 0, or a class name */
        struct { VarRef var; Node *parent; NodeList fields, methods; Node *ctor;
                 ObjString *title; } klass;                       /* N_CLASS (title: its name as written) */
        struct { NodeList keys, values; } map;                    /* N_MAP */
        VarRef var;                                               /* N_VAR */
        struct { Node *target, *value; TokenType op; } assign;    /* N_ASSIGN */
        struct { TokenType op; Node *operand; } unary;            /* N_UNARY */
        struct { TokenType op; Node *left, *right; } binary;      /* N_BINARY, N_AND, N_OR */
        struct { Node *callee; NodeList args; int known_fn; } call;  /* N_CALL (known_fn: a top-level fn is called) */
        struct { Node *object; ObjString *name; NodeList args;
                 VarRef fallback; } method;                       /* N_METHOD */
        struct { Node *object, *index; } index;                   /* N_INDEX */
        struct { Node *object; ObjString *name; } field;          /* N_FIELD */
        FuncDecl *func;                                           /* N_FUNC */
        struct { Node *start, *end; int inclusive; } range;       /* N_RANGE */
        Node *expr;                             /* N_EXPR, N_RETURN, N_THROW, N_REF */
        struct { VarRef var; Node *value; int is_var, is_fn; } let;  /* N_LET */
        struct { NodeList stmts; int first_slot, nslots,
                 frame_slots; } block;     /* frame_slots: program root only */
        struct { Node *cond, *then_branch, *else_branch; } if_s;  /* N_IF */
        struct { Node *cond, *body; } while_s;                    /* N_WHILE */
        struct { VarRef first, second; int two; Node *iter, *body;
                 int first_slot, nslots; } for_s;                 /* N_FOR */
        struct { Node *body, *handler; VarRef var; int has_var;
                 int first_slot, nslots; } try_s;                 /* N_TRY */
        struct { ObjString *path;             /* import "file" (NULL for a package import) */
                 ObjString *dotted;           /* import a.b.Name: as written */
                 ObjString *alias;            /* ... as alias */
                 int star;                    /* import a.b.* */
                 ObjString *pkg;              /* (resolver) the package to load */
                 ObjString *bind; } import;   /* (resolver) import a.b: the global its package object goes in */
        struct { OnItem *items; int count; FuncDecl *func; struct OnDesc *desc; } on;  /* N_ON */
    } as;
};

/* ======================================================================
 * Interpreter state
 * ==================================================================== */

typedef struct { char *data; size_t len, cap; } StrBuf;

/* A top-level `on` block: runs fn when one of the global vars it watches changes. */
typedef struct Watcher {
    OnItem *items;                  /* none once replaced by a hot reload */
    int nnames;
    Value fn;                       /* the block: a function of () or (old, new) */
    const char *file;               /* the file it was declared in */
    int running;                    /* its own changes do not run it again */
} Watcher;

/* An `on` block declared in a function (or a block): it runs on that
 * function's frame while the code around it is running. */
typedef struct FrameWatcher {
    struct OnDesc *od;
    int base;                       /* the frame's first slot */
    int depth;                      /* how many frames there were with it the innermost */
    Value token;                    /* what its hidden slot holds while it is in scope */
    int running;
} FrameWatcher;

/* A place being changed or lent: a variable and the keys of a path in it. */
typedef struct {
    const VarRef *root;
    Value keys[MAX_PATH];
    unsigned char is_field[MAX_PATH];
    int nkeys;
} Place;

/* A call whose arguments are being evaluated: made by CALL_PREP,
 * METHOD_PREP or SUPER_PREP, used by INVOKE. The callee is on the stack
 * at `callee`; a method's receiver slot follows it (has_slot). */
enum { SELF_NONE, SELF_VALUE, SELF_LOCKED, SELF_LEND };
typedef struct {
    int callee;                     /* stack index of the callee */
    int has_slot;                   /* a receiver slot follows the callee (methods, super) */
    int mode;                       /* SELF_*: how the receiver is passed */
    Place place;                    /* SELF_LEND: the receiver's place */
} PendingCall;

typedef struct Frame {              /* one active call (or script) */
    struct Frame *prev;
    const char *name;
    Source *src;
    int line;                       /* the line, for frames not running bytecode */
    int base;                       /* first stack slot of this frame */
    ObjFunction *fn;                /* NULL for top-level code */
    int this_locked;                /* a method called on a value that cannot change */
    FuncDecl *proto;                /* the code it runs */
    const Instr *pc;                /* the instruction running (or the call it waits for) */
    int argc;                       /* arguments given (the rest take their defaults) */
    int call_base;                  /* the caller's stack top before the callee was pushed */
    int pending;                    /* pending calls below this frame */
    unsigned char construct;        /* a constructor: the call's result is `this` */
    unsigned char entry;            /* called from C: return there when done */
    unsigned char block;            /* an `on` block running on its function's frame (same base) */
    int opbase;                     /* where its operand stack starts */
    int dbg_line;                   /* the line the debugger last saw this frame on (-1: none yet) */
    struct Loan *loans;             /* what the call lent with &, returned when it ends */
    int nloans;
} Frame;

#define LUBE_STACK_MAX (1 << 20)
#ifndef LUBE_STACK_SIZE
#define LUBE_STACK_SIZE (8u * 1024u * 1024u)  /* the C stack each thread (the main one, each isolate) runs on */
#endif

struct Interp {
    TypeDesc *decl;                 /* the declared type of each stack slot */
    int sp;
    Frame *frame;
    int this_locked_next;           /* passed from a method call to the new frame */
    struct FrameChunk **chunks;     /* frames live here, in blocks that never move */
    int nchunks;
    int depth;                      /* frames in use */
    PendingCall *pending;           /* calls whose arguments are being evaluated */
    int npending, cap_pending;
    struct Loan **loan_pool;        /* spare loan arrays (room for 4), to reuse */
    int nloan_pool;

    ObjMap *strings;                /* interned names and literals */
    ObjMap *globals;
    ObjMap *constants;              /* names of globals declared with let/fn */
    ObjMap *modules;                /* imported file paths */
    ObjMap *global_type_index;      /* global name -> index into global_types */
    TypeDesc *global_types;         /* declared types of globals made with let/var */
    int nglobal_types, cap_global_types;
    ObjList *classes;               /* every class made: they live until shutdown */
    ObjMap *bundle;                 /* a built program's files (path -> text), or NULL */
    ObjMap *watch;                  /* files the program was loaded from (path -> text), for hot reload */
    ObjList *hot_old;               /* functions and classes replaced by hot reload: kept until shutdown */
    struct Debugger *dbg;           /* while debugging (NULL otherwise) */
    struct Watcher *watchers;       /* the top-level `on` blocks, in the order they were made */
    int nwatchers, cap_watchers;
    struct FrameWatcher *fwatch;    /* the `on` blocks running on frames (innermost frame's last) */
    int nfwatch, cap_fwatch;
    struct Isolate *isolate;        /* in a spawned isolate: its handle (NULL in the main one) */
    int starting;                   /* an isolate is running the program's declarations */
    ObjMap *lazy;                   /* in an isolate: globals not used yet (name -> [initializer, kind]) */
    char *root;                     /* the program's folder: packages are found from here (then LUBE_PATH) */
    ObjMap *packages;               /* packages loaded */
    ObjMap *pkg_names;              /* package -> the names it declares (for imports) */
    ObjMap *main_aliases;           /* names the main namespace imported (kept from one REPL line to the next) */
    struct ProgFile *program;       /* the top-level sources run, in order (for spawn) */
    int nprogram, cap_program;
    int hot;                        /* lube --hot: reload changed files while running */
    int hot_busy;                   /* a reload is applying a file */
    unsigned hot_ticks;             /* blocks run since the last look at the files */
    unsigned epoch;                 /* changes when a global becomes a constant (or stops being one) or a
                                       class is made: VarRef and MethodDesc caches older than it are stale */
    long hot_last;                  /* clock() when the files were last looked at */
    ObjList *imports_found;         /* while building: the import paths the resolver sees */
    ObjString *chars[256];          /* cached one-byte strings */

    Value ret_val;                  /* value carried by a `return` */
    Value error_val;                /* value carried by a raised error */
    int error_line;
    Source *error_src;
    StrBuf trace;                   /* traceback collected while unwinding */
    StrBuf interp;                  /* OP_INTERP's text, kept between uses */
    int trace_count;                /* frames the error passed through */
    int trace_shown;                /* distinct lines kept in `trace` */
    int trace_hidden;               /* frames left out after the first lines */
    int trace_repeat;               /* repeats of the previous line not yet noted */
    char trace_prev[320];           /* previous line, to fold repeats */
    char trace_last[320];           /* outermost frame, kept when trace is cut */

    char *stack_base;
    size_t stack_budget;
    uint64_t rng;
    Source *sources;
    struct Arena **arenas;          /* syntax trees of every program run */
    int narenas, cap_arenas;
    Value stack[LUBE_STACK_MAX];    /* frames: parameters and locals (last: the fields above stay
                                       close to the struct's start, cheap to reach) */
};

/* ======================================================================
 * Functions shared between files
 * ==================================================================== */

/* util.c */
typedef struct Arena Arena;
Arena *arena_new(void);
void *arena_alloc(Arena *a, size_t n);
void *arena_grow(Arena *a, void *old, size_t old_size, size_t new_size);
void arena_free(Arena *a);
void sb_init(StrBuf *sb);
void sb_putc(StrBuf *sb, char c);
void sb_puts(StrBuf *sb, const char *s);
void sb_putn(StrBuf *sb, const char *s, size_t n);
void sb_printf(StrBuf *sb, const char *fmt, ...);
void sb_free(StrBuf *sb);
void lube_out_of_memory(void);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrndup(const char *s, size_t n);
char *read_file(const char *path, size_t *length);
void lube_slashes(char *path);  /* Windows: \ becomes / (Lube's paths use /) */
#ifdef _WIN32
#define LUBE_PATH_SEP ';'  /* between the folders of LUBE_PATH (C:/ has a colon) */
#else
#define LUBE_PATH_SEP ':'
#endif
void report_error(Source *src, int line, int col, const char *kind, const char *msg);
extern void (*lube_output)(const char *text, size_t len, int is_err);
void lube_print(int err, const char *fmt, ...);
void print_source_line(Source *src, int line, int col);

/* value.c */
uint32_t hash_bytes(const char *s, size_t n);
ObjString *alloc_string(size_t length);
ObjString *new_string(const char *chars, size_t length);
ObjString *string_append(ObjString *s, const char *chars, size_t n);
uint32_t string_hash(ObjString *s);
ObjString *lube_intern(Interp *I, const char *chars, size_t length);
long str_find(const char *hay, size_t hlen, const char *needle, size_t nlen, size_t from);
size_t utf8_seq_len(unsigned char lead);
ObjList *new_list(void);
void list_reserve(ObjList *l, size_t n);
void list_push(ObjList *l, Value v);
ObjList *list_copy(ObjList *l);
ObjMap *new_map(void);
Value *map_find(ObjMap *m, Value key);
int map_get(ObjMap *m, Value key, Value *out);
void map_set(ObjMap *m, Value key, Value value);
int map_delete(ObjMap *m, Value key, Value *removed);
ObjMap *map_copy(ObjMap *m);
int is_key(Value v);
int map_key_ok(ObjMap *m, Value key);
void make_unique(Value *slot);
ObjFunction *new_function(FuncDecl *decl, int ncaptures);
ObjNative *new_native(const char *name, NativeFn fn, int min, int max, int changes_first);
ObjRange *new_range(double start, double end, double step, int inclusive);
ObjStrObj *new_strobj(ObjString *text);
TypeDesc desc_of(Value v);
int desc_accepts(TypeDesc d, Value v, Value *out);
const char *desc_name(TypeDesc d);
const char *ann_name(const TypeAnn *t);
const char *with_article(const char *name);
int is_subclass(ObjClass *c, ObjClass *ancestor);
double range_length(ObjRange *r);
int type_tag_from_name(const char *s, size_t n);
const char *tag_name(int tag);
const char *list_type_name(int tag);
int value_has_type(Value v, int tag);
const char *a_type(Value v);
int is_truthy(Value v);
int values_equal(Value a, Value b);
const char *type_name(Value v);
void format_number(char *buf, size_t size, double n);
void append_value(StrBuf *sb, Value v, int quote);
ObjString *value_to_string(Value v);
ObjString *value_repr(Value v);

/* integer.c */
const char *int_type_name(int kind);
double numeric_value(Value v);
int numeric_compare(Value a, Value b);
int int_exact_double(Value v, double *out);
void int_format(char *buf, size_t size, Value v);
Value int_convert(Interp *I, Value v, int kind);
Value int_from_whole(Interp *I, double d, int kind);
int int_try_whole(double d, int kind, Value *out);
Value int_arith(Interp *I, int op, Value a, Value b);
Value int_negate(Interp *I, Value v);
Value int_abs(Interp *I, Value v);

/* lexer.c */
int lex(Source *src, const char *begin, const char *end, int line,
        const char *line_start, TokenArray *out);
const char *skip_string_lit(const char *p, const char *end, int *too_deep);

/* parser.c: the tree lives in *arena (NULL is returned on a syntax error,
 * with nothing left allocated) */
Node *parse_program(Interp *I, Source *src, Arena **arena);

/* resolver.c */
int resolve_program(Interp *I, Node *program, Source *src, Arena *arena);
typedef struct {            /* a variable a debugger expression can see */
    ObjString *name;
    VarKind kind;           /* VAR_LOCAL (a slot of the paused frame) or VAR_CAPTURE */
    int index;
} DebugVar;
int resolve_debug(Interp *I, FuncDecl *d, Source *src, Arena *arena, Source *home, DebugVar *vars, int nvars);

/* compile.c: the program's code; mode is RUN_FILE, RUN_REPL or RUN_HOT */
enum { RUN_FILE, RUN_REPL, RUN_HOT, RUN_ISOLATE };
FuncDecl *compile_program(Interp *I, Node *program, Source *src, Arena *arena, const char *name, int mode);
FuncDecl *compile_debug(Interp *I, FuncDecl *d, Arena *arena);

/* debug.c: the debugger (lube --debug, and the core of lube --dap) */
struct Debugger;
void debug_hook(Interp *I);             /* before each instruction, while debugging */
void debug_error(Interp *I);            /* an error was just raised (frames still there) */
void debug_left_frame(Interp *I);       /* a frame returned */
void lube_debug_terminal(Interp *I);    /* start debugging with the terminal prompt */
int lube_frame_line(Frame *f);
Value lube_closure_in(Interp *I, FuncDecl *d, Frame *f);
void lube_keep_arena(Interp *I, Arena *a);
void unchangeable_message(char *buf, size_t size, const char *name, Origin origin);

/* interp.c -- functions returning Value return ERR_VAL when they raise */
Interp *lube_new(char *stack_base);
size_t lube_free(Interp *I);
Source *lube_add_source(Interp *I, const char *path, char *text, size_t length);
int lube_check_source(Interp *I, Source *src);
void lube_module_path(StrBuf *path, const char *importer, const char *rel);
int lube_build(Interp *I, const char *self, const char *main_path, const char *out_path, int native);
int lube_run_bundle(Interp *I, const char *argv0, int argc, char **argv);
ObjList *lube_hot_reload(Interp *I);
Value lube_find_method(ObjClass *c, ObjString *name);
Value lube_call_method(Interp *I, Value method, Value self, int argc, Value *argv);
void register_reflection(Interp *I);
int lube_run_source(Interp *I, Source *src, int repl);
int lube_run_file(Interp *I, const char *path);
Value lube_call(Interp *I, Value fn, int argc, Value *argv);
Value lube_throw(Interp *I, Value v);
Value lube_error(Interp *I, const char *fmt, ...);
void lube_define(Interp *I, const char *name, Value v);
int lube_contains(Interp *I, Value container, Value item);
Value lube_arith(Interp *I, TokenType op, Value a, Value b);
Value lube_check_elem(Interp *I, const char *holder, int tag, Value v);
Value lube_string_property(Interp *I, Value s, ObjString *name);
int lube_stack_ok(Interp *I);

/* builtins.c */
void register_builtins(Interp *I);

/* thread.c: the operating system's threads, for isolates */
typedef struct LubeLock LubeLock;   /* a mutex with a condition to wait on */
LubeLock *lock_new(void);         /* lives as long as the process */
void lock_acquire(LubeLock *l);
void lock_release(LubeLock *l);
void lock_wait(LubeLock *l);        /* releases the lock while waiting for lock_notify */
void lock_notify(LubeLock *l);      /* wakes every waiter */
int thread_start(void (*fn)(void *), void *arg, size_t stack_size);
int lube_cores(void);
char **os_files(const char *dir, const char *ext, int *n);  /* sorted; free each and the array */
#define os_lube_files(dir, n) os_files(dir, ".lube", n)

/* native.c */
int lube_native_build(Interp *I, int nfiles, char **keys, char **texts, size_t *lens, const char *argv0,
                      const char *exe_out);
void lube_native_attach(Interp *I, FuncDecl *program, Source *src);
int lube_native_loop(Source *src, int index);
int lube_global_watched(Interp *I, ObjString *name);
uint64_t lube_hash_text(const char *text, size_t len);
#ifdef _WIN32
char *os_self_path(void);  /* the running program (malloc'd), or NULL */
#endif

/* isolate.c: isolates (each on its own thread) and the channels between them */
void register_isolates(Interp *I);
void channel_release(struct Channel *ch);
void isolate_release(struct Isolate *iso);
int channel_next(Interp *I, Value ch, Value *out);  /* for x in ch: 1 = a value, 0 = closed, -1 = error */
void lube_note_program(Interp *I, const char *path, const char *text, size_t len);
int lube_isolates_leaked(void);
void lube_wait_isolates(void);
Value lube_join_isolate(Interp *I, Value iso);
int lube_run_declarations(Interp *I, const char *path, const char *text, size_t len);
int lube_find_package(Interp *I, ObjString *pkg, StrBuf *folder);
char **lube_package_files(Interp *I, const char *folder, int *n);
ObjMap *lube_package_names(Interp *I, ObjString *pkg, int *bad);
ObjString *lube_global_key(Interp *I, ObjString *pkg, ObjString *name);
const char *lube_shown(ObjString *key);
void lube_free_program(Interp *I);

#endif
