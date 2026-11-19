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

#define LUBE_VERSION "2.0.0"

/* Hard limits, checked before any size arithmetic so that no size can ever
 * overflow, and so one expression cannot claim all of the machine's memory.
 * Operations that know the size up front (like "x" * n) report an error;
 * growing past a limit step by step stops the program with "out of memory". */
#define LUBE_MIN_SIZE(a, b) ((a) < (b) ? (a) : (b))
#define LUBE_DEFAULT_MAX_ITEMS LUBE_MIN_SIZE((size_t)1 << 28, SIZE_MAX / 128)  /* list/object items */
#define LUBE_DEFAULT_MAX_BYTES LUBE_MIN_SIZE((size_t)1 << 30, SIZE_MAX / 4)    /* string, source file */
/* The environment variables LUBE_MAX_ITEMS and LUBE_MAX_BYTES can lower
 * these (never raise them), for example to sandbox a script. */
extern size_t lube_max_items, lube_max_bytes;
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
#define AS_STROBJ(v)   ((ObjStrObj *)(v).as.obj)
#define AS_CLASS(v)    ((ObjClass *)(v).as.obj)

/* ======================================================================
 * Heap objects. Each one counts its owners; it is freed when the count
 * drops to zero. Lists and maps are only changed in place when their count
 * is 1, so a change is never visible through another owner.
 * ==================================================================== */

typedef enum {
    OBJ_STRING, OBJ_LIST, OBJ_MAP, OBJ_FUNCTION, OBJ_NATIVE, OBJ_RANGE, OBJ_STROBJ, OBJ_CLASS
} ObjType;

struct Obj {
    size_t rc;              /* number of owners */
    ObjType type;
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
enum { TD_NUM, TD_BOOL, TD_INT, TD_STR, TD_STROBJ, TD_LIST, TD_MAP, TD_OBJECT, TD_FUNCTION, TD_RANGE, TD_CLASS };
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
    ObjString *name;
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
extern size_t lube_live_objects;
Obj *alloc_object(size_t size, ObjType type);
void obj_destroy(Obj *o);

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
    T_FOR, T_IF, T_IMPORT, T_IN, T_IS, T_LET, T_NIL, T_NOT, T_OR, T_RETURN,
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
    N_CONTINUE, N_THROW, N_TRY, N_IMPORT, N_CLASS
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
typedef struct {
    ObjString *name;
    VarKind kind;
    int index;
    int changeable;         /* locals only; globals are checked at run time */
    Origin origin;
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
    Node *cls;              /* TD_CLASS: the class's name (an N_VAR) */
    unsigned char set;
} TypeAnn;

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
    int ncaptures;
    int line;
    Source *src;
    int is_method;          /* its first parameter is the implicit `this` */
    ObjClass *owner;        /* the class a method belongs to (set when the class is made) */
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
        struct { VarRef var; Node *parent; NodeList fields, methods; Node *ctor; } klass;  /* N_CLASS */
        struct { NodeList keys, values; } map;                    /* N_MAP */
        VarRef var;                                               /* N_VAR */
        struct { Node *target, *value; TokenType op; } assign;    /* N_ASSIGN */
        struct { TokenType op; Node *operand; } unary;            /* N_UNARY */
        struct { TokenType op; Node *left, *right; } binary;      /* N_BINARY, N_AND, N_OR */
        struct { Node *callee; NodeList args; } call;             /* N_CALL */
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
        struct { ObjString *path; } import;                       /* N_IMPORT */
    } as;
};

/* ======================================================================
 * Interpreter state
 * ==================================================================== */

typedef struct { char *data; size_t len, cap; } StrBuf;

typedef struct Frame {              /* one active call (or script) */
    struct Frame *prev;
    const char *name;
    Source *src;
    int line;                       /* line currently executing */
    int base;                       /* first stack slot of this frame */
    ObjFunction *fn;                /* NULL for top-level code */
    int this_locked;                /* a method called on a value that cannot change */
} Frame;

#define LUBE_STACK_MAX (1 << 20)

struct Interp {
    Value stack[LUBE_STACK_MAX];    /* frames: parameters and locals */
    TypeDesc *decl;                 /* the declared type of each stack slot */
    int sp;
    Frame *frame;
    int this_locked_next;           /* passed from a method call to the new frame */

    ObjMap *strings;                /* interned names and literals */
    ObjMap *globals;
    ObjMap *constants;              /* names of globals declared with let/fn */
    ObjMap *modules;                /* imported file paths */
    ObjMap *global_type_index;      /* global name -> index into global_types */
    TypeDesc *global_types;         /* declared types of globals made with let/var */
    int nglobal_types, cap_global_types;
    ObjList *classes;               /* every class made: they live until shutdown */
    ObjString *chars[256];          /* cached one-byte strings */

    Value ret_val;                  /* value carried by a `return` */
    Value error_val;                /* value carried by a raised error */
    int error_line;
    Source *error_src;
    StrBuf trace;                   /* traceback collected while unwinding */
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
void report_error(Source *src, int line, int col, const char *kind, const char *msg);
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
void unchangeable_message(char *buf, size_t size, const char *name, Origin origin);

/* interp.c -- functions returning Value return ERR_VAL when they raise */
Interp *lube_new(char *stack_base);
size_t lube_free(Interp *I);
Source *lube_add_source(Interp *I, const char *path, char *text, size_t length);
int lube_check_source(Interp *I, Source *src);
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

#endif
