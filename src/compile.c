/*
 * compile.c - turns a resolved syntax tree into bytecode.
 *
 * The resolver has already given every local a slot and worked out what each
 * closure copies, so compiling is one walk over the tree. Each function gets
 * its own Code (see lube.h): instruction words, the source line of each word,
 * constants, and descriptors for the instructions that need more than one
 * operand. Everything is allocated in the program's arena, which lives as
 * long as any function made from it can run.
 *
 * Expressions leave one value on the operand stack (or "no value" for a call
 * whose result is not needed); statements leave the stack as they found it.
 * The compiler tracks the stack depth, so each function knows the most stack
 * it needs and the VM checks for room once per call.
 */
#include "lube.h"

#include <stdlib.h>
#include <string.h>

/* A growable array, copied into the arena when the function is done. */
#define GROW(arr, count, cap)                                                          \
    do {                                                                               \
        if ((count) == (cap)) {                                                        \
            (cap) = (cap) ? (cap) * 2 : 8;                                             \
            (arr) = xrealloc((arr), sizeof(*(arr)) * (size_t)(cap));                   \
        }                                                                              \
    } while (0)

typedef struct Block {          /* a block whose slots are cleared when it is left */
    int first, count;
    int locals_from;            /* the locals (debug info) declared in it start here */
    struct Block *prev;
} Block;

typedef struct Loop {
    int is_for;
    int continue_at;            /* while: the condition; for: FOR_NEXT */
    Block *outer;               /* the blocks around the loop (not cleared by break) */
    int *breaks;                /* jumps to patch to the loop's end */
    int nbreaks, capbreaks;
    struct Loop *prev;
} Loop;

typedef struct {
    Interp *I;
    Arena *arena;
    FuncDecl *d;
    int mode;                   /* RUN_FILE, RUN_REPL or RUN_HOT (top-level code only) */
    int line;                   /* the line given to the instructions emitted now */
    int depth, maxdepth;        /* operand stack depth */
    Block *blocks;
    Loop *loop;
    /* the Code being built */
    Instr *code; int *lines; int ncode, capcode;
    Value *consts; int nconsts, capconsts;
    FuncDecl **protos; int nprotos, capprotos;
    PlaceDesc *places; int nplaces, capplaces;
    CallDesc *calls; int ncalls, capcalls;
    MethodDesc *methods; int nmethods, capmethods;
    SuperDesc *supers; int nsupers, capsupers;
    AssignDesc *assigns; int nassigns, capassigns;
    ClassDesc *classes; int nclasses, capclasses;
    OnDesc **ons; int nons, capons;
    LocalWatch *lwatch; int nlwatch, caplwatch;
    Handler *handlers; int nhandlers, caphandlers;
    LocalInfo *locals; int nlocals, caplocals;
} Comp;

/* Debug info: local `name` lives in `slot` from code word `start` (until closed). */
static void note_local(Comp *C, ObjString *name, int slot, int start) {
    GROW(C->locals, C->nlocals, C->caplocals);
    C->locals[C->nlocals].name = name;
    C->locals[C->nlocals].slot = slot;
    C->locals[C->nlocals].start = start;
    C->locals[C->nlocals].end = -1;
    C->nlocals++;
}

/* ...and the locals noted since `from` end here (their scope is left). */
static void close_locals(Comp *C, int from) {
    for (int i = from; i < C->nlocals; i++)
        if (C->locals[i].end < 0) C->locals[i].end = C->ncode;
}

static void compile_expr(Comp *C, Node *n, int want_value);
static void compile_stmt(Comp *C, Node *n);
static FuncDecl *compile_function(Comp *parent, FuncDecl *d);
static void compile_lazy_global(Comp *C, Node *n);

/* ---------- emitting ---------- */

static void stack_effect(Comp *C, int delta) {
    C->depth += delta;
    if (C->depth > C->maxdepth) C->maxdepth = C->depth;
}

static int word(Comp *C, Instr w) {
    if (C->ncode >= MAX_ARG) lube_out_of_memory(); /* LCOV_EXCL_LINE: 16 million words for one function */
    if (C->ncode == C->capcode) {
        C->capcode = C->capcode ? C->capcode * 2 : 64;
        C->code = xrealloc(C->code, sizeof(Instr) * (size_t)C->capcode);
        C->lines = xrealloc(C->lines, sizeof(int) * (size_t)C->capcode);
    }
    C->code[C->ncode] = w;
    C->lines[C->ncode] = C->line;
    return C->ncode++;
}

/* Emits an instruction with a stack effect; returns where it is. */
static int emit(Comp *C, OpCode op, int arg, int delta) {
    int at = word(C, INSTR(op, arg));
    stack_effect(C, delta);
    return at;
}

static int emit2(Comp *C, OpCode op, int a, int b, int delta) {
    int at = emit(C, op, a, delta);
    word(C, (Instr)b);
    return at;
}

static void patch_to(Comp *C, int at, int target) {
    C->code[at] = INSTR(OP_OF(C->code[at]), target);
}

static void patch_word(Comp *C, int at, int value) { C->code[at] = (Instr)value; }

static int constant(Comp *C, Value v) {
    for (int i = 0; i < C->nconsts; i++)  /* strings are interned, so equal ones are the same object */
        if (C->consts[i].type == v.type && C->consts[i].as.u == v.as.u && C->consts[i].ikind == v.ikind) return i;
    GROW(C->consts, C->nconsts, C->capconsts);
    C->consts[C->nconsts] = v;
    return C->nconsts++;
}

static int name_const(Comp *C, ObjString *s) { return constant(C, OBJ_VAL(s)); }

/* ---------- places: x, x.a, x[i] ---------- */

static int place_depth(Node *n) {
    int len = 0;
    for (; n->kind == N_INDEX || n->kind == N_FIELD; n = n->kind == N_INDEX ? n->as.index.object : n->as.field.object)
        len++;
    return len;
}

static Node *place_root(Node *n) {
    while (n->kind == N_INDEX || n->kind == N_FIELD) n = n->kind == N_INDEX ? n->as.index.object : n->as.field.object;
    return n;
}

static int is_place_node(Node *n) { return place_root(n)->kind == N_VAR; }

static void push_keys(Comp *C, Node *n, PlaceDesc *p) {
    if (n->kind == N_VAR) return;
    int is_field = n->kind == N_FIELD;
    push_keys(C, is_field ? n->as.field.object : n->as.index.object, p);
    int saved = C->line;
    C->line = n->line;
    if (is_field) emit(C, OP_CONST, name_const(C, n->as.field.name), 1);
    else compile_expr(C, n->as.index.index, 1);
    C->line = saved;
    p->is_field[p->nkeys++] = (unsigned char)is_field;
}

/* Compiles the keys of a place, leaving them on the stack; returns the place
 * descriptor, or -1 (after emitting an error) for a path too long to change. */
static int compile_place(Comp *C, Node *n) {
    if (place_depth(n) > MAX_PATH) {
        emit(C, OP_FAIL, name_const(C, lube_intern(C->I, "this is nested too deeply to change in one step", 47)), 0);
        return -1;
    }
    PlaceDesc p;
    memset(&p, 0, sizeof p);
    p.var = place_root(n)->as.var;
    push_keys(C, n, &p);
    GROW(C->places, C->nplaces, C->capplaces);
    C->places[C->nplaces] = p;
    return C->nplaces++;
}

/* ---------- expressions ---------- */

/* Can this expression produce "no value" (a call to a function that returns nothing)? */
static int may_be_nothing(Node *n) {
    switch (n->kind) {
    case N_CALL:
    case N_METHOD:
    case N_SUPER:
        return 1;
    case N_AND:
    case N_OR:
        return may_be_nothing(n->as.binary.right);
    default:
        return 0;
    }
}

/* The name a "does not return a value" error gives, as a constant + 1 (0: none). */
static int nothing_name(Comp *C, Node *n) {
    if (n->kind == N_CALL && n->as.call.callee->kind == N_VAR) return name_const(C, n->as.call.callee->as.var.name) + 1;
    if (n->kind == N_METHOD) return name_const(C, n->as.method.name) + 1;
    return 0;
}

/* Pushes a variable's value. raw: a class stays a class (for calls and `is`). */
static void compile_var(Comp *C, VarRef *r, int raw) {
    switch (r->kind) {
    case VAR_LOCAL: emit(C, OP_LOCAL, r->index, 1); break;
    case VAR_CAPTURE: emit(C, OP_CAPTURE, r->index, 1); break;
    case VAR_SELF: emit(C, OP_SELF, 0, 1); break;
    default: emit(C, raw ? OP_GLOBAL : OP_GLOBAL_VALUE, name_const(C, r->name), 1); break;
    }
}

/* Arguments: values, or the keys of & places. Returns the call descriptor
 * and adds the stack slots the arguments take to *size. */
static int has_ref(NodeList *args);

static int compile_args(Comp *C, NodeList *args, int *size) {
    CallDesc cd;
    cd.nargs = args->count;
    cd.place = arena_alloc(C->arena, sizeof(int) * (size_t)(args->count ? args->count : 1));
    cd.refs = has_ref(args);
    *size = 0;
    for (int i = 0; i < args->count; i++) {
        Node *a = args->items[i];
        if (a->kind == N_REF) {
            int before = C->depth;
            cd.place[i] = compile_place(C, a->as.expr);
            *size += C->depth - before;
        } else {
            compile_expr(C, a, 1);
            cd.place[i] = -1;
            *size += 1;
        }
    }
    int grow = args->count - *size;  /* a & variable (no keys) needs its slot when the call starts */
    if (grow > 0) {
        stack_effect(C, grow);
        stack_effect(C, -grow);
    }
    GROW(C->calls, C->ncalls, C->capcalls);
    C->calls[C->ncalls] = cd;
    return C->ncalls++;
}

static int has_ref(NodeList *args) {
    for (int i = 0; i < args->count; i++)
        if (args->items[i]->kind == N_REF) return 1;
    return 0;
}

static void compile_call(Comp *C, Node *n) {
    Node *callee = n->as.call.callee;
    if (callee->kind == N_VAR) compile_var(C, &callee->as.var, 1);  /* a class named here is called */
    else compile_expr(C, callee, 1);
    C->line = n->line;
    NodeList *args = &n->as.call.args;
    int name = callee->kind == N_VAR ? name_const(C, callee->as.var.name) + 1 : 0;
    /* a top-level fn: callable when the call is made, unless something has replaced it since (an import
     * can): OP_CALL_FN checks that only then, with the name in the instruction for the message */
    int fn = n->as.call.known_fn && !has_ref(args) && args->count < 256 && name < 65536;
    if (!fn) emit(C, OP_CHECK_CALLABLE, name, 0);
    if (!has_ref(args)) {
        for (int i = 0; i < args->count; i++) compile_expr(C, args->items[i], 1);
        C->line = n->line;
        if (fn) emit(C, OP_CALL_FN, args->count | name << 8, -args->count);
        else emit(C, OP_CALL, args->count, -args->count);
        return;
    }
    emit(C, OP_CALL_PREP, 0, 0);  /* the callee stays below the arguments */
    int size;
    int cd = compile_args(C, args, &size);
    C->line = n->line;
    emit(C, OP_INVOKE, cd, -size);
}

static void compile_method(Comp *C, Node *n) {
    Node *recv = n->as.method.object;
    MethodDesc md;
    memset(&md, 0, sizeof md);
    md.name = n->as.method.name;
    md.fallback = n->as.method.fallback;
    int before = C->depth;
    if (is_place_node(recv)) {
        md.recv_place = compile_place(C, recv);
    } else {
        compile_expr(C, recv, 1);
        md.recv_place = -1;
    }
    GROW(C->methods, C->nmethods, C->capmethods);
    C->methods[C->nmethods] = md;
    C->line = n->line;
    emit(C, OP_METHOD_PREP, C->nmethods++, before - C->depth + 2);  /* the receiver becomes callee + receiver slot */
    int size;
    int cd = compile_args(C, &n->as.method.args, &size);
    C->line = n->line;
    emit(C, OP_INVOKE, cd, -1 - size);
}

static void compile_super(Comp *C, Node *n) {
    SuperDesc sd;
    sd.name = n->as.super_call.name;
    sd.self = n->as.super_call.self->as.var;
    sd.nargs = n->as.super_call.args.count;
    GROW(C->supers, C->nsupers, C->capsupers);
    C->supers[C->nsupers] = sd;
    int prep = emit2(C, OP_SUPER_PREP, C->nsupers++, 0, 2);  /* callee + receiver slot */
    int size;
    int cd = compile_args(C, &n->as.super_call.args, &size);
    C->line = n->line;
    emit(C, OP_INVOKE, cd, -1 - size);
    patch_word(C, prep + 1, C->ncode);  /* nothing to call: SUPER_PREP pushes no value and jumps here */
}

/* A change to a local that `on` blocks watch: its LocalWatch + 1 (0: none). */
static int local_watch(Comp *C, Node *root) {
    VarRef *r = &root->as.var;
    if (root->kind != N_VAR || r->kind != VAR_LOCAL || !r->watch || !r->watch->n) return 0;
    for (int i = 0; i < C->nlwatch; i++)
        if (C->lwatch[i].slot == r->index && C->lwatch[i].list == r->watch) return i + 1;
    GROW(C->lwatch, C->nlwatch, C->caplwatch);
    C->lwatch[C->nlwatch].slot = r->index;
    C->lwatch[C->nlwatch].list = r->watch;
    return ++C->nlwatch;
}

/* place op= value. As a statement (drop), the value assigned is not kept on
 * the stack (OP_ASSIGN_DROP), and a Number written there is not pushed
 * either (x += 1): unless `on` blocks watch the variable, which need it. */
static void compile_assign(Comp *C, Node *n, int drop) {
    Node *root = place_root(n->as.assign.target);
    int global = root->kind == N_VAR && root->as.var.kind == VAR_GLOBAL;
    int lw = local_watch(C, root);  /* (a change through & is watched where the loan ends) */
    int pop = drop && (global || lw);  /* the watching instructions take the value: pop it after them */
    if (pop) drop = 0;
    if (global) emit(C, OP_WATCH_BEFORE, name_const(C, root->as.var.name), 1);
    if (lw) emit(C, OP_LWATCH_BEFORE, lw - 1, 1);
    int before = C->depth;
    int place = compile_place(C, n->as.assign.target);
    int keys = C->depth - before;
    Node *value = n->as.assign.value;
    int k = -1;
    if (drop && value->kind == N_LITERAL && IS_NUM(value->as.literal)) k = constant(C, value->as.literal);
    else compile_expr(C, value, 1);
    C->line = n->line;
    if (place < 0) {  /* the path was too long: OP_FAIL raised before this runs */
        stack_effect(C, -(keys + (k < 0)));
        if (!drop) stack_effect(C, 1);
    } else {
        AssignDesc ad;
        ad.place = place;
        ad.op = n->as.assign.op;
        ad.global = global ? name_const(C, root->as.var.name) : -1;
        ad.kconst = k;
        GROW(C->assigns, C->nassigns, C->capassigns);
        C->assigns[C->nassigns] = ad;
        VarRef *v = &root->as.var;
        if (drop && keys == 0 && root->kind == N_VAR && v->kind == VAR_LOCAL && v->origin != ORIGIN_THIS) {
            emit(C, OP_SET_LOCAL, v->index, -(k < 0));  /* x op= value: the slot and operator in the code */
            word(C, (Instr)((int)ad.op | (k + 1) << 8));
            word(C, (Instr)C->nassigns++);  /* (the general way, for values that are not Numbers) */
        } else if (drop) {
            emit(C, OP_ASSIGN_DROP, C->nassigns++, -(keys + (k < 0)));
        } else {
            emit(C, OP_ASSIGN, C->nassigns++, -keys);
        }
    }
    if (global) emit(C, OP_WATCH_AFTER, name_const(C, root->as.var.name), -1);
    if (lw) emit(C, OP_LWATCH_AFTER, lw - 1, -1);
    if (pop) emit(C, OP_POP, 0, -1);
}

static int is_comparison(TokenType op) {
    return op == T_LT || op == T_LE || op == T_GT || op == T_GE || op == T_EQ || op == T_NEQ;
}

static int operand_kind(Node *n) {
    if (n->kind == N_VAR && n->as.var.kind == VAR_LOCAL) return OPND_LOCAL;
    return n->kind == N_LITERAL && IS_NUM(n->as.literal) ? OPND_CONST : OPND_STACK;
}

/* Compiles the operands of a op b, those that go on the stack; returns
 * their kinds (left | right << 2) and sets their indexes. The left one is
 * read in place only when the right one cannot change it first. */
static int compile_operands(Comp *C, Node *a, Node *b, int *ia, int *ib) {
    int kb = operand_kind(b), ka = kb == OPND_STACK ? OPND_STACK : operand_kind(a);
    if (ka == OPND_STACK) compile_expr(C, a, 1);
    if (kb == OPND_STACK) compile_expr(C, b, 1);
    *ia = ka == OPND_LOCAL ? a->as.var.index : ka == OPND_CONST ? constant(C, a->as.literal) : 0;
    *ib = kb == OPND_LOCAL ? b->as.var.index : kb == OPND_CONST ? constant(C, b->as.literal) : 0;
    return ka | kb << 2;
}

/* The condition of an if or while, and a jump (to patch) taken when it is
 * false. A comparison compares and jumps in one instruction (OP_CMP_JUMP),
 * reading locals and Numbers in place. */
static int compile_jump_unless(Comp *C, Node *cond, int line) {
    if (cond->kind == N_BINARY && is_comparison(cond->as.binary.op)) {
        int ia, ib, kinds = compile_operands(C, cond->as.binary.left, cond->as.binary.right, &ia, &ib);
        int stacked = ((kinds & 3) == OPND_STACK) + ((kinds >> 2) == OPND_STACK);
        C->line = cond->line;
        int at = emit2(C, OP_CMP_JUMP, 0, (int)cond->as.binary.op | kinds << 8, -stacked);
        word(C, (Instr)ia);
        word(C, (Instr)ib);
        return at;
    }
    compile_expr(C, cond, 1);
    C->line = line;
    return emit(C, OP_JUMP_IF_FALSE, 0, -1);
}

static void compile_expr(Comp *C, Node *n, int want_value) {
    if (!lube_stack_ok(C->I)) lube_out_of_memory(); /* LCOV_EXCL_LINE: the resolver limits tree depth first */
    int saved = C->line;
    C->line = n->line;
    switch (n->kind) {
    case N_LITERAL:
        emit(C, OP_CONST, constant(C, n->as.literal), 1);
        break;
    case N_INTERP:
        for (int i = 0; i < n->as.items.count; i++) compile_expr(C, n->as.items.items[i], 1);
        emit(C, OP_INTERP, n->as.items.count, 1 - n->as.items.count);
        break;
    case N_LIST:
        for (int i = 0; i < n->as.items.count; i++) compile_expr(C, n->as.items.items[i], 1);
        emit(C, OP_LIST, n->as.items.count, 1 - n->as.items.count);
        break;
    case N_TYPED_LIST:
        for (int i = 0; i < n->as.tlist.items.count; i++) {
            Node *item = n->as.tlist.items.items[i];
            compile_expr(C, item, 1);
            C->line = item->line;
            if (n->as.tlist.elem != TT_TUPLE) emit(C, OP_CHECK_ELEM, n->as.tlist.elem, 0);  /* (x[i, j]: anything) */
        }
        C->line = n->line;
        emit2(C, OP_TYPED_LIST, n->as.tlist.items.count, n->as.tlist.elem, 1 - n->as.tlist.items.count);
        break;
    case N_TYPED_MAP:
        emit(C, OP_TYPED_MAP, n->as.tmap.ktype | (n->as.tmap.vtype << 8), 1);
        break;
    case N_MAP:
        for (int i = 0; i < n->as.map.keys.count; i++) {
            compile_expr(C, n->as.map.keys.items[i], 1);
            C->line = n->line;
            emit(C, OP_CHECK_KEY, 0, 0);
            compile_expr(C, n->as.map.values.items[i], 1);
        }
        C->line = n->line;
        emit(C, OP_MAP, n->as.map.keys.count, 1 - 2 * n->as.map.keys.count);
        break;
    case N_VAR:
        compile_var(C, &n->as.var, 0);
        break;
    case N_SUPER:
        compile_super(C, n);
        break;
    case N_IS:
        compile_expr(C, n->as.is.value, 1);
        C->line = n->line;
        if (n->as.is.tag >= 0) {
            emit(C, OP_IS_TAG, n->as.is.tag, 0);
        } else {
            Node *cls = n->as.is.cls;
            if (cls->kind == N_VAR) compile_var(C, &cls->as.var, 1);
            else compile_expr(C, cls, 1);  /* package.Class */
            C->line = n->line;
            emit(C, OP_IS_CLASS, name_const(C, cls->kind == N_VAR ? cls->as.var.name : cls->as.field.name), -1);
        }
        break;
    case N_ASSIGN:
        compile_assign(C, n, 0);
        break;
    case N_UNARY:
        compile_expr(C, n->as.unary.operand, 1);
        C->line = n->line;
        emit(C, n->as.unary.op == T_MINUS ? OP_NEG : OP_NOT, 0, 0);
        break;
    case N_BINARY:
        if (operand_kind(n->as.binary.right) != OPND_STACK) {  /* x - 1, i + j: read in place */
            int ia, ib, kinds = compile_operands(C, n->as.binary.left, n->as.binary.right, &ia, &ib);
            C->line = n->line;
            emit(C, OP_BINARY2, (int)n->as.binary.op | kinds << 8, 1 - ((kinds & 3) == OPND_STACK));
            word(C, (Instr)ia);
            word(C, (Instr)ib);
            break;
        }
        compile_expr(C, n->as.binary.left, 1);
        compile_expr(C, n->as.binary.right, 1);
        C->line = n->line;
        emit(C, OP_BINARY, n->as.binary.op, -1);
        break;
    case N_AND:
    case N_OR: {
        compile_expr(C, n->as.binary.left, 1);
        C->line = n->line;
        int jump = emit(C, n->kind == N_AND ? OP_AND : OP_OR, 0, -1);
        compile_expr(C, n->as.binary.right, 0);
        patch_to(C, jump, C->ncode);
        break;
    }
    case N_CALL:
        compile_call(C, n);
        break;
    case N_METHOD:
        compile_method(C, n);
        break;
    case N_INDEX:
        if (operand_kind(n->as.index.index) != OPND_STACK) {  /* xs[i], xs[0]: read in place */
            int ia, ib, kinds = compile_operands(C, n->as.index.object, n->as.index.index, &ia, &ib);
            C->line = n->line;
            emit(C, OP_INDEX2, kinds, 1 - ((kinds & 3) == OPND_STACK));
            word(C, (Instr)ia);
            word(C, (Instr)ib);
            break;
        }
        compile_expr(C, n->as.index.object, 1);
        compile_expr(C, n->as.index.index, 1);
        C->line = n->line;
        emit(C, OP_INDEX, 0, -1);
        break;
    case N_FIELD:
        C->line = n->line;
        if (n->as.field.object->kind == N_VAR && n->as.field.object->as.var.kind == VAR_LOCAL) {  /* p.x: in place */
            emit2(C, OP_FIELD_LOCAL, n->as.field.object->as.var.index, name_const(C, n->as.field.name), 1);
            break;
        }
        compile_expr(C, n->as.field.object, 1);
        C->line = n->line;
        emit(C, OP_FIELD, name_const(C, n->as.field.name), 0);
        break;
    case N_FUNC: {
        FuncDecl *f = compile_function(C, n->as.func);
        GROW(C->protos, C->nprotos, C->capprotos);
        C->protos[C->nprotos] = f;
        emit(C, OP_CLOSURE, C->nprotos++, 1);
        break;
    }
    case N_RANGE:
        compile_expr(C, n->as.range.start, 1);
        C->line = n->line;
        emit(C, OP_RANGE_START, 0, 0);
        compile_expr(C, n->as.range.end, 1);
        C->line = n->line;
        emit(C, OP_RANGE, n->as.range.inclusive, -1);
        break;
    default:  /* LCOV_EXCL_START: the parser puts only expressions here (and & only in arguments) */
        stack_effect(C, 1);
        break;
        /* LCOV_EXCL_STOP */
    }
    C->line = n->line;
    if (want_value && may_be_nothing(n)) emit(C, OP_NEED_VALUE, nothing_name(C, n), 0);
    C->line = saved;
}

/* ---------- statements ---------- */

static void enter_block(Comp *C, Block *b, int first, int count) {
    b->first = first;
    b->count = count;
    b->locals_from = C->nlocals;
    b->prev = C->blocks;
    C->blocks = b;
}

static void leave_block(Comp *C, Block *b) {
    if (b->count) emit2(C, OP_CLEAR, b->first, b->count, 0);
    close_locals(C, b->locals_from);
    C->blocks = b->prev;
}

/* Clears the blocks between here and `until` (as leaving them would). */
static void clear_to(Comp *C, Block *until) {
    for (Block *b = C->blocks; b != until; b = b->prev)
        if (b->count) emit2(C, OP_CLEAR, b->first, b->count, 0);
}

static void compile_stmts(Comp *C, NodeList *stmts) {
    for (int i = 0; i < stmts->count; i++) compile_stmt(C, stmts->items[i]);
}

static void compile_block(Comp *C, Node *b) {
    Block blk;
    enter_block(C, &blk, b->as.block.first_slot, b->as.block.nslots);
    compile_stmts(C, &b->as.block.stmts);
    leave_block(C, &blk);
}

static void begin_loop(Comp *C, Loop *l, int is_for) {
    memset(l, 0, sizeof *l);
    l->is_for = is_for;
    l->outer = C->blocks;
    l->prev = C->loop;
    C->loop = l;
}

static void end_loop(Comp *C, Loop *l, int end) {
    for (int i = 0; i < l->nbreaks; i++) patch_to(C, l->breaks[i], end);
    free(l->breaks);
    C->loop = l->prev;
}

static void compile_class(Comp *C, Node *n) {
    ClassDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.name = n->as.klass.title;
    cd.key = n->as.klass.var.name;
    if (n->as.klass.parent) cd.parent = n->as.klass.parent->as.var;
    NodeList *fields = &n->as.klass.fields, *methods = &n->as.klass.methods;
    cd.nfields = fields->count;
    cd.fields = arena_alloc(C->arena, sizeof(ObjString *) * (size_t)(fields->count ? fields->count : 1));
    for (int i = 0; i < fields->count; i++) cd.fields[i] = fields->items[i]->as.let.var.name;
    cd.nmethods = methods->count;
    cd.methods = arena_alloc(C->arena, sizeof(FuncDecl *) * (size_t)(methods->count ? methods->count : 1));
    for (int i = 0; i < methods->count; i++) cd.methods[i] = compile_function(C, methods->items[i]->as.func);
    if (n->as.klass.ctor) cd.ctor = compile_function(C, n->as.klass.ctor->as.func);
    cd.hot = C->mode == RUN_HOT;
    GROW(C->classes, C->nclasses, C->capclasses);
    C->classes[C->nclasses] = cd;
    int c = C->nclasses++;
    emit(C, OP_CLASS_BEGIN, c, 2);  /* the class being made, and the one it replaces (or nothing) */
    for (int i = 0; i < fields->count; i++) {
        Node *f = fields->items[i];
        C->line = f->line;
        emit2(C, OP_CLASS_FIELD, c, i, 0);
        compile_expr(C, f->as.let.value, 1);
        C->line = f->line;
        emit2(C, OP_CLASS_FIELD_SET, c, i, -1);
    }
    C->line = n->line;
    emit(C, OP_CLASS_END, c, -2);
}

static void compile_let(Comp *C, Node *n) {
    VarRef *r = &n->as.let.var;
    compile_expr(C, n->as.let.value, 1);
    C->line = n->line;
    if (r->kind == VAR_LOCAL) {
        emit(C, OP_DEF_LOCAL, r->index, -1);
        note_local(C, r->name, r->index, C->ncode);
    }
    else emit2(C, OP_DEF_GLOBAL, name_const(C, r->name), n->as.let.is_fn ? 2 : n->as.let.is_var ? 0 : 1, -1);
}

static void compile_stmt(Comp *C, Node *n) {
    int saved = C->line;
    C->line = n->line;
    switch (n->kind) {
    case N_EXPR:
        if (n->as.expr->kind == N_ASSIGN) {  /* a statement: the value assigned is not needed */
            compile_assign(C, n->as.expr, 1);
            break;
        }
        compile_expr(C, n->as.expr, 0);
        emit(C, OP_POP, 0, -1);
        break;
    case N_LET:
        compile_let(C, n);
        break;
    case N_BLOCK:
        compile_block(C, n);
        break;
    case N_IF: {
        int to_else = compile_jump_unless(C, n->as.if_s.cond, n->line);
        compile_block(C, n->as.if_s.then_branch);
        if (n->as.if_s.else_branch) {
            int to_end = emit(C, OP_JUMP, 0, 0);
            patch_to(C, to_else, C->ncode);
            compile_stmt(C, n->as.if_s.else_branch);
            patch_to(C, to_end, C->ncode);
        } else {
            patch_to(C, to_else, C->ncode);
        }
    } break;
    case N_WHILE: {
        Loop loop;
        begin_loop(C, &loop, 0);
        int start = C->ncode;
        loop.continue_at = start;
        int to_end = compile_jump_unless(C, n->as.while_s.cond, n->line);
        compile_block(C, n->as.while_s.body);
        C->line = n->line;
        emit(C, OP_LOOP, start, 0);
        patch_to(C, to_end, C->ncode);
        end_loop(C, &loop, C->ncode);
    } break;
    case N_FOR: {
        compile_expr(C, n->as.for_s.iter, 1);
        C->line = n->line;
        emit(C, OP_FOR_PREP, 0, 2);  /* the iterable becomes the loop's state: it, position, count */
        Loop loop;
        begin_loop(C, &loop, 1);
        int next = C->ncode, locals_from = C->nlocals;
        loop.continue_at = next;
        int two = n->as.for_s.two;
        emit(C, OP_FOR_NEXT, 0, 0);
        word(C, (Instr)n->as.for_s.first.index);
        word(C, (Instr)(two ? n->as.for_s.second.index : -1));
        note_local(C, n->as.for_s.first.name, n->as.for_s.first.index, C->ncode);
        if (two) note_local(C, n->as.for_s.second.name, n->as.for_s.second.index, C->ncode);
        compile_stmts(C, &n->as.for_s.body->as.block.stmts);  /* the body's slots are the loop's */
        C->line = n->line;
        emit(C, OP_LOOP, next, 0);
        int end = C->ncode;
        patch_to(C, next, end);
        end_loop(C, &loop, end);
        close_locals(C, locals_from);
        if (n->as.for_s.nslots) emit2(C, OP_CLEAR, n->as.for_s.first_slot, n->as.for_s.nslots, 0);
        emit(C, OP_POPN, 3, -3);
    } break;
    case N_RETURN:
        if (n->as.expr) {
            compile_expr(C, n->as.expr, 0);
            C->line = n->line;
            emit(C, OP_RETURN, 0, -1);
        } else {
            emit(C, OP_RETURN_NIL, 0, 0);
        }
        break;
    case N_BREAK:
    case N_CONTINUE: {
        Loop *l = C->loop;
        clear_to(C, l->outer);
        if (n->kind == N_BREAK) {
            GROW(l->breaks, l->nbreaks, l->capbreaks);
            l->breaks[l->nbreaks++] = emit(C, OP_JUMP, 0, 0);
        } else {
            emit(C, OP_JUMP, l->continue_at, 0);
        }
        break;
    }
    case N_THROW:
        compile_expr(C, n->as.expr, 1);
        C->line = n->line;
        emit(C, OP_THROW, 0, -1);
        break;
    case N_TRY: {
        Handler h;
        h.start = C->ncode;
        h.sp = C->depth;
        h.clear_from = n->as.try_s.body->as.block.first_slot;
        h.catch_slot = n->as.try_s.has_var ? n->as.try_s.var.index : -1;
        compile_block(C, n->as.try_s.body);
        int to_end = emit(C, OP_JUMP, 0, 0);
        h.end = C->ncode;
        h.target = C->ncode;
        GROW(C->handlers, C->nhandlers, C->caphandlers);
        C->handlers[C->nhandlers++] = h;
        if (n->as.try_s.has_var) {
            int locals_from = C->nlocals;
            note_local(C, n->as.try_s.var.name, n->as.try_s.var.index, C->ncode);
            compile_stmts(C, &n->as.try_s.handler->as.block.stmts);
            close_locals(C, locals_from);
            if (n->as.try_s.nslots) emit2(C, OP_CLEAR, n->as.try_s.first_slot, n->as.try_s.nslots, 0);
        } else {
            compile_block(C, n->as.try_s.handler);
        }
        patch_to(C, to_end, C->ncode);
        break;
    }
    case N_IMPORT:
        if (n->as.import.path)
            emit(C, OP_IMPORT, name_const(C, n->as.import.path), 0);
        else  /* a package: loaded once; import a.b also puts the package in a global */
            emit2(C, OP_IMPORT_PACKAGE, name_const(C, n->as.import.pkg),
                  n->as.import.bind ? name_const(C, n->as.import.bind) + 1 : 0, 0);
        break;
    case N_CLASS:
        compile_class(C, n);
        break;
    case N_ON:
        compile_function(C, n->as.on.func);
        GROW(C->ons, C->nons, C->capons);
        C->ons[C->nons] = n->as.on.desc;
        emit(C, OP_ON, C->nons++, 0);
        break;
    default:  /* LCOV_EXCL_START: expression nodes are always wrapped in N_EXPR */
        break;
        /* LCOV_EXCL_STOP */
    }
    C->line = saved;
}

/* A hot reload applies a changed file's declarations only: functions are
 * replaced (the old ones kept alive), classes updated in place, new globals
 * made and imports and `on` blocks run; nothing else runs again. */
static void compile_hot_stmt(Comp *C, Node *n) {
    C->line = n->line;
    if (n->kind == N_IMPORT || n->kind == N_ON || n->kind == N_CLASS) {
        compile_stmt(C, n);
    } else if (n->kind == N_LET && n->as.let.is_fn) {
        emit(C, OP_HOT_KEEP, name_const(C, n->as.let.var.name), 0);
        compile_let(C, n);
    } else if (n->kind == N_LET) {  /* a variable that exists keeps its value */
        int skip = emit2(C, OP_IF_GLOBAL, name_const(C, n->as.let.var.name), 0, 0);
        compile_let(C, n);
        patch_word(C, skip + 1, C->ncode);
    }
}

/* In a new isolate, a global variable gets its value the first time it is
 * used: its starting value becomes a function, run then (OP_LAZY_GLOBAL). */
static void compile_lazy_global(Comp *C, Node *n) {
    FuncDecl *init = arena_alloc(C->arena, sizeof(FuncDecl));
    init->name = n->as.let.var.name;
    init->line = n->line;
    init->src = C->d->src;
    init->is_arrow = 1;
    init->body = n->as.let.value;
    C->line = n->line;
    compile_function(C, init);
    GROW(C->protos, C->nprotos, C->capprotos);
    C->protos[C->nprotos] = init;
    emit(C, OP_CLOSURE, C->nprotos++, 1);
    emit2(C, OP_LAZY_GLOBAL, name_const(C, n->as.let.var.name), n->as.let.is_var ? 0 : 1, -1);
}

/* ---------- functions ---------- */

#define FINISH(field, count)                                                                   \
    do {                                                                                       \
        d->code.field = arena_alloc(C->arena, sizeof(*C->field) * (size_t)((C->count) ? (C->count) : 1)); \
        if (C->count) memcpy(d->code.field, C->field, sizeof(*C->field) * (size_t)(C->count)); \
        d->code.count = C->count;                                                              \
        free(C->field);                                                                        \
    } while (0)

static void finish(Comp *C) {
    FuncDecl *d = C->d;
    d->code.code = arena_alloc(C->arena, sizeof(Instr) * (size_t)C->ncode);
    memcpy(d->code.code, C->code, sizeof(Instr) * (size_t)C->ncode);
    d->code.lines = arena_alloc(C->arena, sizeof(int) * (size_t)C->ncode);
    memcpy(d->code.lines, C->lines, sizeof(int) * (size_t)C->ncode);
    d->code.ncode = C->ncode;
    free(C->code);
    free(C->lines);
    FINISH(consts, nconsts);
    FINISH(protos, nprotos);
    FINISH(places, nplaces);
    FINISH(calls, ncalls);
    FINISH(methods, nmethods);
    FINISH(supers, nsupers);
    FINISH(assigns, nassigns);
    FINISH(classes, nclasses);
    FINISH(ons, nons);
    FINISH(lwatch, nlwatch);
    FINISH(handlers, nhandlers);
    close_locals(C, 0);
    FINISH(locals, nlocals);
    d->code.maxstack = C->maxdepth;
    d->code.gcache = arena_alloc(C->arena, sizeof(int) * (size_t)(C->nconsts ? C->nconsts : 1));
    for (int i = 0; i < C->nconsts; i++) d->code.gcache[i] = -1;
}

static void start(Comp *C, Interp *I, Arena *arena, FuncDecl *d, int mode) {
    memset(C, 0, sizeof *C);
    C->I = I;
    C->arena = arena;
    C->d = d;
    C->mode = mode;
    C->line = d->line;
}

static FuncDecl *compile_function(Comp *parent, FuncDecl *d) {
    Comp c, *C = &c;
    start(C, parent->I, parent->arena, d, RUN_FILE);
    d->has_default = arena_alloc(C->arena, (size_t)(d->nparams ? d->nparams : 1));
    d->plain = !d->is_ctor && !d->on_block;
    d->plain_method = d->is_method && !d->is_ctor;
    for (int i = 0; i < d->nparams; i++) {
        d->has_ref |= d->modes[i] == PARAM_REF;
        if (d->modes[i] != PARAM_LET || d->defaults[i] || (d->types && d->types[i].set && d->types[i].d.kind != TD_NUM)) {
            d->plain = 0;
            if (i > 0) d->plain_method = 0;  /* (`this`, parameter 0, is lent) */
        }
    }
    for (int i = 0; i < d->nparams; i++)  /* (debug info) an `on` block's (old, new) are in its function's frame */
        note_local(C, d->params[i], d->on_block ? d->on_block->param_slot[i] : i, 0);
    for (int i = 0; i < d->nparams; i++) {  /* defaults run in the new frame, for arguments not given */
        if (!d->defaults[i]) continue;
        d->has_default[i] = 1;
        C->line = d->defaults[i]->line;
        int skip = emit2(C, OP_ARG_DEFAULT, i, 0, 0);
        compile_expr(C, d->defaults[i], 1);
        C->line = d->line;
        emit(C, OP_DEF_PARAM, i, -1);
        patch_word(C, skip + 1, C->ncode);
    }
    if (d->is_arrow) {
        compile_expr(C, d->body, 0);
        C->line = d->body->line;
        emit(C, OP_RETURN, 0, -1);
    } else {
        compile_stmts(C, &d->body->as.block.stmts);  /* the body shares the parameters' scope */
        C->line = d->line;
        emit(C, OP_RETURN_NIL, 0, 0);
    }
    finish(C);
    return d;
}

/* A debugger expression (already wrapped in d and resolved). */
FuncDecl *compile_debug(Interp *I, FuncDecl *d, Arena *arena) {
    Comp c;
    start(&c, I, arena, d, RUN_FILE);
    return compile_function(&c, d);
}

FuncDecl *compile_program(Interp *I, Node *program, Source *src, Arena *arena, const char *name, int mode) {
    FuncDecl *d = arena_alloc(arena, sizeof(FuncDecl));
    d->name = lube_intern(I, name, strlen(name));
    d->line = 1;
    d->src = src;
    d->nslots = program->as.block.frame_slots;
    d->has_default = arena_alloc(arena, 1);
    Comp c, *C = &c;
    start(C, I, arena, d, mode);
    NodeList *stmts = &program->as.block.stmts;
    for (int i = 0; i < stmts->count; i++) {
        Node *s = stmts->items[i];
        if (mode == RUN_HOT) {
            compile_hot_stmt(C, s);
        } else if (mode == RUN_ISOLATE) {  /* a new isolate: the declarations only */
            if (s->kind == N_LET && !s->as.let.is_fn) compile_lazy_global(C, s);
            else if (s->kind == N_LET || s->kind == N_CLASS || s->kind == N_IMPORT || s->kind == N_ON) compile_stmt(C, s);
        } else if (mode == RUN_REPL && i == stmts->count - 1 && s->kind == N_EXPR && s->as.expr->kind != N_ASSIGN) {
            C->line = s->line;
            compile_expr(C, s->as.expr, 0);  /* the REPL prints a final expression's value */
            C->line = s->line;
            emit(C, OP_PRINT_REPL, 0, 0);
            emit(C, OP_POP, 0, -1);
        } else {  /* (a loop with machine code tries that first) */
            int native = mode == RUN_FILE && (s->kind == N_FOR || s->kind == N_WHILE) ? lube_native_loop(src, i) : -1;
            int skip = native >= 0 ? emit2(C, OP_NATIVE_LOOP, native, 0, 0) : -1;
            compile_stmt(C, s);
            if (skip >= 0) patch_word(C, skip + 1, C->ncode);
        }
    }
    C->line = stmts->count ? stmts->items[stmts->count - 1]->line : 1;
    emit(C, OP_RETURN_NIL, 0, 0);
    finish(C);
    return d;
}
