/*
 * parser.c - recursive descent parser producing the AST.
 *
 * Operator precedence, lowest to highest:
 *
 *   =  +=  -=  *=  /=  %=      assignment (right associative)
 *   |                          pipe: x | f(y) is f(x, y)
 *   or  ||
 *   and &&
 *   not                        (so `not a == b` means `not (a == b)`)
 *   ==  !=
 *   <  <=  >  >=  in
 *   ..  ..=                    ranges
 *   +  -
 *   *  /  %
 *   -  !                       unary
 *   **                         power (right associative, binds tighter than unary minus)
 *   call()  index[]  .field  .method()
 */
#include "lube.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 400

/* All nodes of the program being parsed come from this arena, so the whole
 * tree is freed in one step (at exit, or right away after a syntax error).
 * Temporary buffers that are live while a syntax error unwinds are listed
 * in `pending` and freed by parse_program. */
static LUBE_THREAD_LOCAL Arena *tree;  /* per thread: isolates parse at the same time */

typedef struct {      /* used as stacks: strings nest inside interpolations */
    StrBuf bufs[64];
    int nbufs;
    TokenArray toks[64];
    int ntoks;
} Pending;

static LUBE_THREAD_LOCAL Pending pending;

typedef struct {
    Interp *I;
    Source *src;
    Token *toks;
    int count, pos;
    jmp_buf *jb;
    int depth;
} Parser;

static Node *expression(Parser *P);
static Node *pipe_expr(Parser *P);
static Node *statement(Parser *P);
static Node *block(Parser *P);
static Node *postfix(Parser *P);
static Node *index_item(Parser *P);

/* ---------- helpers ---------- */

static Token *peek(Parser *P) { return &P->toks[P->pos]; }

static Token *peek_at(Parser *P, int k) {
    int i = P->pos + k;
    return &P->toks[i < P->count ? i : P->count - 1];
}

static Token *advance(Parser *P) {
    Token *t = &P->toks[P->pos];
    if (t->type != T_EOF) P->pos++;
    return t;
}

static int check(Parser *P, TokenType type) { return peek(P)->type == type; }

static int match(Parser *P, TokenType type) {
    if (!check(P, type)) return 0;
    advance(P);
    return 1;
}

static const char *describe(Token *t, char *buf, size_t n) {
    if (t->type == T_EOF) return "end of file";
    if (t->type == T_NEWLINE) return "end of line";
    snprintf(buf, n, "'%.*s'", t->length > 24 ? 24 : t->length, t->start);
    return buf;
}

static void error_at(Parser *P, Token *t, const char *fmt, ...) {
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    report_error(P->src, t->line, t->col, "syntax error", msg);
    longjmp(*P->jb, 1);
}

static Token *expect(Parser *P, TokenType type, const char *what) {
    if (check(P, type)) return advance(P);
    char buf[64];
    error_at(P, peek(P), "expected %s but found %s", what, describe(peek(P), buf, sizeof buf));
    return NULL;
}

static void skip_newlines(Parser *P) {
    while (check(P, T_NEWLINE) || check(P, T_SEMI)) advance(P);
}

/* A simple statement must be followed by a newline, ';', '}' or the end. */
static void end_statement(Parser *P) {
    if (match(P, T_NEWLINE) || match(P, T_SEMI)) return;
    if (check(P, T_RBRACE) || check(P, T_EOF)) return;
    char buf[64];
    error_at(P, peek(P), "expected a new line or ';' after the statement, found %s",
             describe(peek(P), buf, sizeof buf));
}

static void enter(Parser *P) {
    if (++P->depth > MAX_DEPTH || !lube_stack_ok(P->I)) error_at(P, peek(P), "code is nested too deeply");
}

static void leave(Parser *P) { P->depth--; }

static int is_name_token(TokenType t) { return t == T_IDENT || (t >= T_AND && t <= T_WHILE); }

static Node *new_node(NodeKind kind, int line) {
    Node *n = arena_alloc(tree, sizeof(Node));
    n->kind = kind;
    n->line = line;
    return n;
}

static void list_add(NodeList *l, Node *n) {
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 4;
        l->items = arena_grow(tree, l->items, sizeof(Node *) * (size_t)l->cap, sizeof(Node *) * (size_t)cap);
        l->cap = cap;
    }
    l->items[l->count++] = n;
}

static void list_prepend(NodeList *l, Node *n) {
    list_add(l, n);
    memmove(l->items + 1, l->items, sizeof(Node *) * (size_t)(l->count - 1));
    l->items[0] = n;
}

static ObjString *name_of(Parser *P, Token *t) { return lube_intern(P->I, t->start, (size_t)t->length); }

/* Is n a variable, or an index/field chain that starts at a variable? */
static int is_place(Node *n) {
    while (n->kind == N_INDEX || n->kind == N_FIELD) n = n->kind == N_INDEX ? n->as.index.object : n->as.field.object;
    return n->kind == N_VAR;
}

static Node *literal(Value v, int line) {
    Node *n = new_node(N_LITERAL, line);
    n->as.literal = v;
    return n;
}

/* ---------- string literals ---------- */

/* Line number and line start of position p inside token t. */
static void position_in(Token *t, const char *p, int *line, const char **line_start) {
    *line = t->line;
    *line_start = t->start - (t->col - 1);
    for (const char *q = t->start; q < p; q++) {
        if (*q == '\n') {
            (*line)++;
            *line_start = q + 1;
        }
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put_utf8(StrBuf *sb, unsigned long cp) {
    if (cp < 0x80) {
        sb_putc(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(sb, (char)(0xC0 | (cp >> 6)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(sb, (char)(0xE0 | (cp >> 12)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(sb, (char)(0xF0 | (cp >> 18)));
        sb_putc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static void string_error(Parser *P, Token *t, const char *at, const char *fmt, const char *arg) {
    int line;
    const char *ls;
    position_in(t, at, &line, &ls);
    char msg[200];
    snprintf(msg, sizeof msg, fmt, arg);
    report_error(P->src, line, (int)(at - ls) + 1, "syntax error", msg);
    longjmp(*P->jb, 1);
}

/* Parses the expression inside "{...}" of an interpolated string. */
static Node *interpolation(Parser *P, Token *t, const char *begin, const char *end) {
    int line;
    const char *ls;
    position_in(t, begin, &line, &ls);
    if (pending.ntoks == 64) error_at(P, P->toks + P->pos, "strings are nested too deeply");
    TokenArray *toks = &pending.toks[pending.ntoks++];
    toks->items = NULL;
    toks->count = toks->cap = 0;
    if (!lex(P->src, begin, end, line, ls, toks)) longjmp(*P->jb, 1);
    Parser sub = *P;
    sub.toks = toks->items;
    sub.count = toks->count;
    sub.pos = 0;
    Node *e = expression(&sub);
    skip_newlines(&sub);
    if (!check(&sub, T_EOF)) {
        char buf[64];
        error_at(&sub, peek(&sub), "unexpected %s inside string interpolation", describe(peek(&sub), buf, sizeof buf));
    }
    free(toks->items);
    pending.ntoks--;
    return e;
}

/* "double quoted" strings support escapes and {interpolation};
 * 'single quoted' strings support escapes only. */
static Node *string_literal(Parser *P, Token *t) {
    const char *p = t->start + 1, *end = t->start + t->length - 1;
    int interpolate = t->start[0] == '"';
    if (pending.nbufs == 64) error_at(P, t, "strings are nested too deeply");
    StrBuf *sb = &pending.bufs[pending.nbufs++];
    sb_init(sb);
    Node *interp = NULL;

    while (p < end) {
        char c = *p;
        if (c == '\\') {
            char e = p[1];
            p += 2;
            switch (e) {
            case 'n': sb_putc(sb, '\n'); break;
            case 't': sb_putc(sb, '\t'); break;
            case 'r': sb_putc(sb, '\r'); break;
            case '0': sb_putc(sb, '\0'); break;
            case 'e': sb_putc(sb, 27); break;
            case '\\': case '"': case '\'': case '{': case '}': sb_putc(sb, e); break;
            case '\n': break;  /* backslash-newline: join lines */
            case 'x': {
                int hi = p < end ? hex_value(p[0]) : -1;
                int lo = p + 1 < end ? hex_value(p[1]) : -1;
                if (hi < 0 || lo < 0) string_error(P, t, p - 2, "%s", "\\x needs two hex digits");
                sb_putc(sb, (char)(hi * 16 + lo));
                p += 2;
                break;
            }
            case 'u': {
                const char *at = p - 2;
                if (p >= end || *p != '{') string_error(P, t, at, "%s", "expected \\u{XXXX}");
                p++;
                unsigned long cp = 0;
                int digits = 0;
                while (p < end && hex_value(*p) >= 0 && digits < 6) {
                    cp = cp * 16 + (unsigned long)hex_value(*p++);
                    digits++;
                }
                if (digits == 0 || p >= end || *p != '}' || cp > 0x10FFFF)
                    string_error(P, t, at, "%s", "invalid unicode escape, expected \\u{XXXX}");
                p++;
                put_utf8(sb, cp);
                break;
            }
            default: {
                char esc[3] = {'\\', e, 0};
                string_error(P, t, p - 2, "unknown escape sequence '%s'", esc);
            }
            }
            continue;
        }
        if (c == '{' && interpolate) {
            const char *begin = p + 1, *q = begin;
            int depth = 1;
            while (q < end) {
                if (*q == '"' || *q == '\'') {
                    int too_deep;
                    q = skip_string_lit(q, end, &too_deep);
                    if (!q) q = end;
                    continue;
                }
                if (*q == '{') depth++;
                else if (*q == '}' && --depth == 0) break;
                q++;
            }
            if (q >= end) string_error(P, t, p, "%s", "unclosed '{' in string (write \\{ for a literal brace)");
            if (q == begin) string_error(P, t, p, "%s", "empty interpolation '{}' (write \\{} for literal braces)");
            if (!interp) interp = new_node(N_INTERP, t->line);
            if (sb->len) {
                list_add(&interp->as.items, literal(OBJ_VAL(lube_intern(P->I, sb->data, sb->len)), t->line));
                sb->len = 0;
            }
            list_add(&interp->as.items, interpolation(P, t, begin, q));
            p = q + 1;
            continue;
        }
        sb_putc(sb, c);
        p++;
    }

    Node *result;
    if (!interp) {
        result = literal(OBJ_VAL(lube_intern(P->I, sb->data ? sb->data : "", sb->len)), t->line);
    } else {
        if (sb->len) list_add(&interp->as.items, literal(OBJ_VAL(lube_intern(P->I, sb->data, sb->len)), t->line));
        result = interp;
    }
    sb_free(sb);
    pending.nbufs--;
    return result;
}

/* ---------- functions ---------- */

/* A built-in type name inside Map[K, V]. */
static int builtin_type(Parser *P, const char *what) {
    Token *t = expect(P, T_IDENT, what);
    int tag = type_tag_from_name(t->start, (size_t)t->length);
    if (tag < 0) error_at(P, t, "'%.*s' is not a built-in type (Map[K, V] takes built-in types)", t->length, t->start);
    return tag;
}

/* The type after ':' on a parameter or a return value: a type name (str,
 * Number, List, ...), T[] for a typed list, Map[K, V], or a class name.
 * List and Map accept any list or map, as `x is List` does. */
static TypeAnn type_annotation(Parser *P) {
    TypeAnn t;
    memset(&t, 0, sizeof t);
    t.set = 1;
    Token *name = expect(P, T_IDENT, "a type after ':'");
    int tag = type_tag_from_name(name->start, (size_t)name->length);
    if (tag < 0) {  /* a class: looked up when the function is called */
        if (check(P, T_LBRACKET))
            error_at(P, peek(P), "typed lists hold built-in types, so '%.*s[]' is not a type (use List)", name->length,
                     name->start);
        t.d.kind = TD_CLASS;
        t.cls = name_of(P, name);
        return t;
    }
    if (match(P, T_LBRACKET)) {
        if (tag == TT_MAP && !check(P, T_RBRACKET)) {  /* Map[K, V] */
            t.d.kind = TD_MAP;
            t.d.a = (unsigned char)builtin_type(P, "a key type in Map[K, V]");
            expect(P, T_COMMA, "',' and a value type in Map[K, V]");
            t.d.b = (unsigned char)builtin_type(P, "a value type in Map[K, V]");
            expect(P, T_RBRACKET, "']' to close Map[K, V]");
            return t;
        }
        expect(P, T_RBRACKET, "']' (a list type is written like str[])");
        t.d.kind = TD_LIST;
        t.d.a = (unsigned char)tag;
        return t;
    }
    switch (tag) {
    case TT_STRING: t.d.kind = TD_STR; break;
    case TT_STROBJ: t.d.kind = TD_STROBJ; break;
    case TT_NUMBER: t.d.kind = TD_NUM; break;
    case TT_BOOL: t.d.kind = TD_BOOL; break;
    case TT_OBJECT: t.d.kind = TD_OBJECT; break;
    case TT_MAP: t.d.kind = TD_MAP; t.d.any = 1; break;
    case TT_LIST: t.d.kind = TD_LIST; t.d.any = 1; break;
    case TT_FUNCTION: t.d.kind = TD_FUNCTION; break;
    case TT_RANGE: t.d.kind = TD_RANGE; break;
    case TT_CHANNEL: t.d.kind = TD_CHANNEL; break;
    case TT_ISOLATE: t.d.kind = TD_ISOLATE; break;
    case TT_BYTES: t.d.kind = TD_BYTES; break;
    case TT_MONEY: t.d.kind = TD_MONEY; break;
    default: t.d.kind = TD_INT; t.d.a = (unsigned char)(tag - TT_INT8); break;
    }
    return t;
}

/* Parses "(params) { body }" or "(params) => expression", where each
 * parameter and the return value may declare a type:
 *   fn scale(by: Number, &shape: Shape, var times: int32 = 1): Bool { ... }
 * In a #strict file every parameter must have one, and a function without a
 * return type returns nothing. */
static FuncDecl *new_decl(Parser *P, ObjString *name, int line) {
    FuncDecl *d = arena_alloc(tree, sizeof(FuncDecl));
    d->name = name;
    d->line = line;
    d->src = P->src;
    d->strict = P->src->strict;
    return d;
}

/* "(params)" and an optional ": ReturnType". */
static void function_params(Parser *P, FuncDecl *d) {
    expect(P, T_LPAREN, "'(' to start the parameter list");
    int cap = 0, typed = 0;
    while (!check(P, T_RPAREN)) {
        /* `&name` changes the caller's variable; `var name` is a changeable copy */
        unsigned char mode = match(P, T_AMP) ? PARAM_REF : match(P, T_VAR) ? PARAM_VAR : PARAM_LET;
        Token *pt = expect(P, T_IDENT, "a parameter name");
        if (d->nparams == cap) {
            int old = cap;
            cap = cap ? cap * 2 : 4;
            d->params = arena_grow(tree, d->params, sizeof(ObjString *) * (size_t)old, sizeof(ObjString *) * (size_t)cap);
            d->defaults = arena_grow(tree, d->defaults, sizeof(Node *) * (size_t)old, sizeof(Node *) * (size_t)cap);
            d->modes = arena_grow(tree, d->modes, (size_t)old, (size_t)cap);
            d->types = arena_grow(tree, d->types, sizeof(TypeAnn) * (size_t)old, sizeof(TypeAnn) * (size_t)cap);
        }
        d->params[d->nparams] = name_of(P, pt);
        d->modes[d->nparams] = mode;
        d->defaults[d->nparams] = NULL;
        memset(&d->types[d->nparams], 0, sizeof(TypeAnn));
        if (match(P, T_COLON)) {
            d->types[d->nparams] = type_annotation(P);
            typed = 1;
        } else if (d->strict) {
            error_at(P, peek(P), "#strict: parameter '%.*s' needs a type, like %.*s: Number", pt->length, pt->start,
                     pt->length, pt->start);
        }
        if (check(P, T_ASSIGN)) {
            if (mode == PARAM_REF) error_at(P, peek(P), "a & parameter cannot have a default value");
            advance(P);
            d->defaults[d->nparams] = pipe_expr(P);
        }
        d->nparams++;
        if (!match(P, T_COMMA)) break;
    }
    expect(P, T_RPAREN, "')' to close the parameter list");
    if (!typed) d->types = NULL;
    if (match(P, T_COLON)) d->ret = type_annotation(P);
}

static FuncDecl *function_rest(Parser *P, ObjString *name, int line) {
    FuncDecl *d = new_decl(P, name, line);
    function_params(P, d);
    Token *t = peek(P);
    if (t->type == T_IDENT && t->length == 9 && !memcmp(t->start, "component", 9) && peek_at(P, 1)->type == T_LBRACE) {
        advance(P);  /* fn Card(title) component { ... } (a name anywhere else) */
        if (d->ret.set) error_at(P, t, "a component cannot have a return type: calling it makes the component");
        d->is_component = 1;
    }
    if (check(P, T_ARROW)) {
        Token *arrow = advance(P);
        d->is_arrow = 1;
        d->body = expression(P);
        /* `=> value` returns a value, so in #strict code it needs a return type
         * (a call may return nothing, which is checked when it runs) */
        NodeKind k = d->body->kind;
        if (d->strict && !d->ret.set && k != N_CALL && k != N_METHOD && k != N_SUPER)
            error_at(P, arrow, "#strict: this function returns a value, so it needs a return type, like ): Number => "
                               "(or use a { } body)");
    } else if (check(P, T_LBRACE)) {
        d->body = block(P);
    } else {
        char buf[64];
        error_at(P, peek(P), "expected '{' or '=>' after the parameter list, found %s", describe(peek(P), buf, sizeof buf));
    }
    return d;
}

/* ---------- expressions ---------- */

static void argument_list(Parser *P, NodeList *args) {
    /* the '(' has been consumed */
    skip_newlines(P);
    while (!check(P, T_RPAREN)) {
        if (check(P, T_AMP)) {  /* &place: lend a variable to be changed */
            Token *amp = advance(P);
            Node *ref = new_node(N_REF, amp->line);
            ref->as.expr = postfix(P);
            if (!is_place(ref->as.expr))
                error_at(P, amp, "'&' must be followed by a variable, a field like a.b, or an index like a[i]");
            list_add(args, ref);
        } else {
            list_add(args, expression(P));
        }
        skip_newlines(P);
        if (!match(P, T_COMMA)) break;
        skip_newlines(P);
    }
    expect(P, T_RPAREN, "')' to close the argument list");
}

static Node *list_literal(Parser *P, Token *open) {
    Node *n = new_node(N_LIST, open->line);
    skip_newlines(P);
    while (!check(P, T_RBRACKET)) {
        list_add(&n->as.items, expression(P));
        skip_newlines(P);
        if (!match(P, T_COMMA)) break;
        skip_newlines(P);
    }
    expect(P, T_RBRACKET, "']' to close the list");
    return n;
}

static Node *map_literal(Parser *P, Token *open) {
    Node *n = new_node(N_MAP, open->line);
    skip_newlines(P);
    while (!check(P, T_RBRACE)) {
        Token *k = peek(P);
        Node *key;
        if (is_name_token(k->type)) {
            advance(P);
            key = literal(OBJ_VAL(name_of(P, k)), k->line);
        } else if (k->type == T_STRING) {
            advance(P);
            key = string_literal(P, k);
        } else if (k->type == T_NUMBER) {
            advance(P);
            key = literal(NUM_VAL(k->number), k->line);
        } else if (k->type == T_LBRACKET) {
            advance(P);
            key = expression(P);
            expect(P, T_RBRACKET, "']' after the computed key");
        } else {
            char buf[64];
            error_at(P, k, "expected an object key (a name, string, number or [expression]) but found %s",
                     describe(k, buf, sizeof buf));
            return NULL;
        }
        expect(P, T_COLON, "':' after the map key");
        skip_newlines(P);
        list_add(&n->as.map.keys, key);
        list_add(&n->as.map.values, expression(P));
        skip_newlines(P);
        if (!match(P, T_COMMA)) break;
        skip_newlines(P);
    }
    expect(P, T_RBRACE, "'}' to close the map");
    return n;
}

/* A type name in an expression:
 *   String            the type's default value ("" here)
 *   String(x)         a conversion: a call to the built-in String
 *   String[]          an empty list that only holds strings
 *   String["a", "b"]  such a list with items */
/* Name() with no arguments. */
static Node *call_of(Parser *P, Token *t) {
    Node *call = new_node(N_CALL, t->line);
    call->as.call.callee = new_node(N_VAR, t->line);
    call->as.call.callee->as.var.name = name_of(P, t);
    return call;
}

static Node *empty_range(int line) {
    Node *r = new_node(N_RANGE, line);
    r->as.range.start = literal(NUM_VAL(0), line);
    r->as.range.end = literal(NUM_VAL(0), line);
    return r;
}

static int is_type_token(Token *t) {
    return t->type == T_IDENT && type_tag_from_name(t->start, (size_t)t->length) >= 0;
}

static Node *type_expression(Parser *P, Token *t, int tag) {
    /* Map[K, V]: a map whose keys have type K and values type V */
    if (tag == TT_MAP && check(P, T_LBRACKET) && is_type_token(peek_at(P, 1)) && peek_at(P, 2)->type == T_COMMA) {
        Token *open = advance(P);
        Token *k = advance(P);
        advance(P);  /* , */
        Token *v = peek(P);
        if (!is_type_token(v)) {
            char buf[64];
            error_at(P, v, "expected a value type after ',' in Map[...] but found %s", describe(v, buf, sizeof buf));
        }
        advance(P);
        expect(P, T_RBRACKET, "']' to close Map[...]");
        Node *n = new_node(N_TYPED_MAP, open->line);
        n->as.tmap.ktype = type_tag_from_name(k->start, (size_t)k->length);
        n->as.tmap.vtype = type_tag_from_name(v->start, (size_t)v->length);
        return n;
    }
    if (tag == TT_MAP && check(P, T_LBRACKET) && is_type_token(peek_at(P, 1)) && peek_at(P, 2)->type == T_RBRACKET)
        error_at(P, peek_at(P, 1), "Map[...] needs a key type and a value type, like Map[String, Number]");
    if (check(P, T_LBRACKET)) {
        Token *open = advance(P);
        Node *n = new_node(N_TYPED_LIST, open->line);
        n->as.tlist.elem = tag;
        skip_newlines(P);
        while (!check(P, T_RBRACKET)) {
            list_add(&n->as.tlist.items, expression(P));
            skip_newlines(P);
            if (!match(P, T_COMMA)) break;
            skip_newlines(P);
        }
        expect(P, T_RBRACKET, "']' to close the typed list");
        return n;
    }
    if (check(P, T_LPAREN)) {  /* conversion: the postfix loop makes it a call */
        Node *n = new_node(N_VAR, t->line);
        n->as.var.name = name_of(P, t);
        return n;
    }
    switch (tag) {  /* the default value */
    case TT_STRING: return literal(OBJ_VAL(lube_intern(P->I, "", 0)), t->line);
    case TT_STROBJ:  /* String(): a new String object holding "" */
        return call_of(P, t);
    case TT_NUMBER: return literal(NUM_VAL(0), t->line);
    case TT_BOOL: return literal(BOOL_VAL(0), t->line);
    case TT_OBJECT: return new_node(N_MAP, t->line);
    case TT_MAP: return new_node(N_TYPED_MAP, t->line);  /* an empty Map (keys of any type) */
    case TT_LIST: return new_node(N_LIST, t->line);
    case TT_FUNCTION:
        error_at(P, t, "Function has no default value: write a function with fn, or use Function[] for a list of them");
        return NULL;
    case TT_CHANNEL:  /* Channel: a new channel, as Channel() */
        return call_of(P, t);
    case TT_ISOLATE:
        error_at(P, t, "Isolate has no default value: start one with spawn(f, ...)");
        return NULL;
    case TT_RANGE:  /* the empty range 0..0 */
        return empty_range(t->line);
    case TT_BYTES:  /* Bytes: none yet, as Bytes() */
    case TT_MONEY:  /* Money: none of the default currency, as Money() */
        return call_of(P, t);
    default:  /* the sized integers: 0 */
        return literal(IK_SIGNED(tag - TT_INT8) ? SINT_VAL(tag - TT_INT8, 0) : UINT_VAL(tag - TT_INT8, 0), t->line);
    }
}

static Node *primary(Parser *P) {
    Token *t = advance(P);
    switch (t->type) {
    case T_NUMBER:
        return literal(NUM_VAL(t->number), t->line);
    case T_STRING:
        return string_literal(P, t);
    case T_TRUE:
        return literal(BOOL_VAL(1), t->line);
    case T_FALSE:
        return literal(BOOL_VAL(0), t->line);
    case T_NIL:  /* reserved only to explain its absence */
        error_at(P, t, "Lube has no nil (no null): give every variable a value, check object keys with has(), "
                       "or use get(object, key, default)");
        return NULL;
    case T_IDENT: {
        int tag = type_tag_from_name(t->start, (size_t)t->length);
        if (tag >= 0) return type_expression(P, t, tag);
        Node *n = new_node(N_VAR, t->line);
        n->as.var.name = name_of(P, t);
        return n;
    }
    case T_LPAREN: {
        Node *e = expression(P);
        expect(P, T_RPAREN, "')'");
        return e;
    }
    case T_LBRACKET:
        return list_literal(P, t);
    case T_LBRACE:
        return map_literal(P, t);
    case T_FN: {
        Node *n = new_node(N_FUNC, t->line);
        n->as.func = function_rest(P, NULL, t->line);
        return n;
    }
    case T_THIS: {  /* the instance a method runs on: its hidden first parameter */
        Node *n = new_node(N_VAR, t->line);
        n->as.var.name = lube_intern(P->I, "this", 4);
        return n;
    }
    case T_SUPER: {  /* super.method(args) or, in a constructor, super(args) */
        Node *n = new_node(N_SUPER, t->line);
        n->as.super_call.self = new_node(N_VAR, t->line);
        n->as.super_call.self->as.var.name = lube_intern(P->I, "this", 4);
        if (match(P, T_DOT)) {
            Token *name = expect(P, T_IDENT, "a method name after 'super.'");
            n->as.super_call.name = name_of(P, name);
        }
        expect(P, T_LPAREN, "'(' after super");
        argument_list(P, &n->as.super_call.args);
        return n;
    }
    default: {
        char buf[64];
        error_at(P, t, "expected an expression but found %s", describe(t, buf, sizeof buf));
        return NULL;
    }
    }
}

static Node *postfix(Parser *P) {
    Node *e = primary(P);
    for (;;) {
        Token *t = peek(P);
        if (t->type == T_LPAREN) {
            advance(P);
            Node *n = new_node(N_CALL, t->line);
            n->as.call.callee = e;
            argument_list(P, &n->as.call.args);
            e = n;
        } else if (t->type == T_LBRACKET) {
            advance(P);
            Node *n = new_node(N_INDEX, t->line);
            n->as.index.object = e;
            n->as.index.index = index_item(P);
            if (check(P, T_COMMA)) {  /* x[i, j]: the indexes, together (for a plugin's type) */
                Node *all = new_node(N_TYPED_LIST, t->line);
                all->as.tlist.elem = TT_TUPLE;
                list_add(&all->as.tlist.items, n->as.index.index);
                while (match(P, T_COMMA)) list_add(&all->as.tlist.items, index_item(P));
                n->as.index.index = all;
            }
            expect(P, T_RBRACKET, "']' after the index");
            e = n;
        } else if (t->type == T_DOT) {
            advance(P);
            Token *name = peek(P);
            if (!is_name_token(name->type)) {
                char buf[64];
                error_at(P, name, "expected a name after '.' but found %s", describe(name, buf, sizeof buf));
            }
            advance(P);
            if (match(P, T_LPAREN)) {
                Node *n = new_node(N_METHOD, name->line);
                n->as.method.object = e;
                n->as.method.name = name_of(P, name);
                n->as.method.fallback.name = n->as.method.name;
                argument_list(P, &n->as.method.args);
                e = n;
            } else {
                Node *n = new_node(N_FIELD, name->line);
                n->as.field.object = e;
                n->as.field.name = name_of(P, name);
                e = n;
            }
        } else {
            return e;
        }
    }
}

static Node *unary(Parser *P);

static Node *power(Parser *P) {
    Node *left = postfix(P);
    if (check(P, T_POW)) {
        Token *op = advance(P);
        Node *n = new_node(N_BINARY, op->line);
        n->as.binary.op = T_POW;
        n->as.binary.left = left;
        enter(P);  /* 2 ** 2 ** 2 ... recurses */
        n->as.binary.right = unary(P);
        leave(P);
        return n;
    }
    return left;
}

static Node *unary(Parser *P) {
    if (check(P, T_MINUS) || check(P, T_BANG)) {
        Token *op = advance(P);
        enter(P);
        Node *operand = unary(P);
        leave(P);
        if (op->type == T_MINUS && operand->kind == N_LITERAL && IS_NUM(operand->as.literal)) {
            operand->as.literal.as.number = -operand->as.literal.as.number;  /* fold -5 */
            return operand;
        }
        Node *n = new_node(N_UNARY, op->line);
        n->as.unary.op = op->type;
        n->as.unary.operand = operand;
        return n;
    }
    return power(P);
}

static Node *binary_node(TokenType op, Node *left, Node *right, int line) {
    Node *n = new_node(N_BINARY, line);
    n->as.binary.op = op;
    n->as.binary.left = left;
    n->as.binary.right = right;
    return n;
}

static Node *multiplicative(Parser *P) {
    Node *left = unary(P);
    while (check(P, T_STAR) || check(P, T_SLASH) || check(P, T_PERCENT) || check(P, T_AT)) {
        Token *op = advance(P);
        left = binary_node(op->type, left, unary(P), op->line);
    }
    return left;
}

static Node *additive(Parser *P) {
    Node *left = multiplicative(P);
    while (check(P, T_PLUS) || check(P, T_MINUS)) {
        Token *op = advance(P);
        left = binary_node(op->type, left, multiplicative(P), op->line);
    }
    return left;
}

/* `a..` before ',' or ']' (x[2.., 0]): a range without an end, which is inf. */
static int open_end(Parser *P, Token *op) { return op->type == T_DOTDOT && (check(P, T_COMMA) || check(P, T_RBRACKET)); }

static Node *range_expr(Parser *P) {
    Node *left = additive(P);
    if (check(P, T_DOTDOT) || check(P, T_DOTDOTEQ)) {
        Token *op = advance(P);
        Node *n = new_node(N_RANGE, op->line);
        n->as.range.start = left;
        n->as.range.end = open_end(P, op) ? literal(NUM_VAL(INFINITY), op->line) : additive(P);
        n->as.range.inclusive = op->type == T_DOTDOTEQ;
        return n;
    }
    return left;
}

/* An index in brackets: an expression, or a range without a start (..3, and .. alone: 0..inf). */
static Node *index_item(Parser *P) {
    if (!check(P, T_DOTDOT) && !check(P, T_DOTDOTEQ)) return expression(P);
    Token *op = advance(P);
    Node *n = new_node(N_RANGE, op->line);
    n->as.range.start = literal(NUM_VAL(0), op->line);
    n->as.range.end = open_end(P, op) ? literal(NUM_VAL(INFINITY), op->line) : additive(P);
    n->as.range.inclusive = op->type == T_DOTDOTEQ;
    return n;
}

static Node *comparison(Parser *P) {
    Node *left = range_expr(P);
    for (;;) {
        if (check(P, T_IS)) {  /* x is String, x is Dog */
            Token *op = advance(P);
            Token *t = expect(P, T_IDENT, "a type or class name after 'is'");
            Node *n = new_node(N_IS, op->line);
            n->as.is.value = left;
            n->as.is.tag = type_tag_from_name(t->start, (size_t)t->length);
            if (n->as.is.tag < 0 || check(P, T_DOT)) {  /* a class, perhaps in a package: x is geometry.Point */
                n->as.is.tag = -1;
                n->as.is.cls = new_node(N_VAR, t->line);
                n->as.is.cls->as.var.name = name_of(P, t);
                while (match(P, T_DOT)) {
                    Node *f = new_node(N_FIELD, t->line);
                    f->as.field.object = n->as.is.cls;
                    f->as.field.name = name_of(P, expect(P, T_IDENT, "a class name after '.'"));
                    n->as.is.cls = f;
                }
            }
            left = n;
        } else if (check(P, T_LT) || check(P, T_LE) || check(P, T_GT) || check(P, T_GE) || check(P, T_IN)) {
            Token *op = advance(P);
            left = binary_node(op->type, left, range_expr(P), op->line);
        } else {
            return left;
        }
    }
}

static Node *equality(Parser *P) {
    Node *left = comparison(P);
    while (check(P, T_EQ) || check(P, T_NEQ)) {
        Token *op = advance(P);
        left = binary_node(op->type, left, comparison(P), op->line);
    }
    return left;
}

static Node *not_expr(Parser *P) {
    if (check(P, T_NOT)) {
        Token *op = advance(P);
        enter(P);
        Node *n = new_node(N_UNARY, op->line);
        n->as.unary.op = T_NOT;
        n->as.unary.operand = not_expr(P);
        leave(P);
        return n;
    }
    return equality(P);
}

static Node *and_expr(Parser *P) {
    Node *left = not_expr(P);
    while (check(P, T_AND)) {
        Token *op = advance(P);
        Node *n = binary_node(T_AND, left, not_expr(P), op->line);
        n->kind = N_AND;
        left = n;
    }
    return left;
}

static Node *or_expr(Parser *P) {
    Node *left = and_expr(P);
    while (check(P, T_OR)) {
        Token *op = advance(P);
        Node *n = binary_node(T_OR, left, and_expr(P), op->line);
        n->kind = N_OR;
        left = n;
    }
    return left;
}

/* x | f(a, b)  becomes  f(x, a, b);   x | f  becomes  f(x) */
static Node *pipe_expr(Parser *P) {
    Node *left = or_expr(P);
    while (check(P, T_PIPE)) {
        Token *op = advance(P);
        Node *right = or_expr(P);
        if (right->kind == N_CALL) {
            list_prepend(&right->as.call.args, left);
            left = right;
        } else if (right->kind == N_METHOD) {
            list_prepend(&right->as.method.args, left);
            left = right;
        } else {
            Node *call = new_node(N_CALL, op->line);
            call->as.call.callee = right;
            list_add(&call->as.call.args, left);
            left = call;
        }
    }
    return left;
}

static Node *assignment(Parser *P) {
    Node *left = pipe_expr(P);
    TokenType t = peek(P)->type;
    if (t == T_ASSIGN || t == T_PLUS_EQ || t == T_MINUS_EQ || t == T_STAR_EQ || t == T_SLASH_EQ ||
        t == T_PERCENT_EQ) {
        Token *op = advance(P);
        if (!is_place(left))  /* the whole chain must start at a variable: not f()[0] or [1][0] */
            error_at(P, op, "can only assign to a variable, an index like a[i], or a field like a.b");
        Node *n = new_node(N_ASSIGN, op->line);
        n->as.assign.target = left;
        n->as.assign.op = t;
        enter(P);  /* a = b = c ... recurses */
        n->as.assign.value = assignment(P);
        leave(P);
        return n;
    }
    return left;
}

static Node *expression(Parser *P) {
    enter(P);
    Node *n = assignment(P);
    leave(P);
    return n;
}

/* ---------- statements ---------- */

static Node *block(Parser *P) {
    Token *open = expect(P, T_LBRACE, "'{' to start a block");
    Node *b = new_node(N_BLOCK, open->line);
    enter(P);
    for (;;) {
        skip_newlines(P);
        if (check(P, T_RBRACE)) break;
        if (check(P, T_EOF)) error_at(P, peek(P), "missing '}' to close the block opened on line %d", open->line);
        list_add(&b->as.block.stmts, statement(P));
    }
    advance(P);
    leave(P);
    return b;
}

/* let name = value   (can never change)
 * var name = value   (can be reassigned and changed in place) */
static Node *let_statement(Parser *P) {
    Token *kw = advance(P);
    int is_var = kw->type == T_VAR;
    Token *name = expect(P, T_IDENT, is_var ? "a variable name after 'var'" : "a name after 'let'");
    Node *n = new_node(N_LET, kw->line);
    n->as.let.var.name = name_of(P, name);
    n->as.let.is_var = is_var;
    if (match(P, T_ASSIGN)) {
        n->as.let.value = expression(P);
        /* `let f = fn(...)` names the function f, so it can call itself */
        if (n->as.let.value->kind == N_FUNC && !n->as.let.value->as.func->name)
            n->as.let.value->as.func->name = n->as.let.var.name;
    } else {  /* there is no nil to start from */
        error_at(P, peek(P), "'%s %s' needs a starting value (Lube has no nil)", is_var ? "var" : "let",
                 n->as.let.var.name->chars);
    }
    end_statement(P);
    return n;
}

static Node *fn_statement(Parser *P) {
    Token *kw = advance(P);
    Token *name = advance(P);
    Node *fn = new_node(N_FUNC, kw->line);
    fn->as.func = function_rest(P, name_of(P, name), kw->line);
    Node *n = new_node(N_LET, kw->line);
    n->as.let.var.name = fn->as.func->name;
    n->as.let.value = fn;
    n->as.let.is_fn = 1;
    if (fn->as.func->is_arrow) end_statement(P);
    return n;
}

/*  on score { ... }              runs when the var `score` changes
 *  on score (old, new) { ... }   ...and gets its value before and after
 *  on score, lives { ... }       runs when any of them changes
 *  on this.hp (old, new) { ... } a field of one
 *  on score (old, new) when new > old { ... }   only when the condition holds */
static Node *on_statement(Parser *P) {
    Token *kw = advance(P);
    Node *n = new_node(N_ON, kw->line);
    StrBuf label;  /* the block's name in tracebacks: "on score, lives" */
    sb_init(&label);
    sb_puts(&label, "on ");
    int cap = 0;
    do {
        Token *t = check(P, T_THIS) ? advance(P) : expect(P, T_IDENT, "a variable name after 'on'");
        if (n->as.on.count == cap) {
            int old = cap;
            cap = cap ? cap * 2 : 4;
            n->as.on.items = arena_grow(tree, n->as.on.items, sizeof(OnItem) * (size_t)old, sizeof(OnItem) * (size_t)cap);
        }
        OnItem *it = &n->as.on.items[n->as.on.count++];
        memset(it, 0, sizeof *it);
        it->name = name_of(P, t);
        if (n->as.on.count > 1) sb_puts(&label, ", ");
        sb_putn(&label, t->start, (size_t)t->length);
        int pcap = 0;
        while (match(P, T_DOT)) {  /* on this.hp: a field */
            Token *f = expect(P, T_IDENT, "a field name after '.'");
            if (it->npath == pcap) {
                int old = pcap;
                pcap = pcap ? pcap * 2 : 2;
                it->path = arena_grow(tree, it->path, sizeof(ObjString *) * (size_t)old, sizeof(ObjString *) * (size_t)pcap);
            }
            it->path[it->npath++] = name_of(P, f);
            sb_putc(&label, '.');
            sb_putn(&label, f->start, (size_t)f->length);
        }
    } while (match(P, T_COMMA));
    FuncDecl *d = new_decl(P, lube_intern(P->I, label.data, label.len), kw->line);
    sb_free(&label);
    if (check(P, T_LPAREN)) {
        if (n->as.on.count > 1)
            error_at(P, peek(P), "an 'on' block for several variables has no (old, new): read the variables in the block");
        Token *open = peek(P);
        function_params(P, d);
        if (d->nparams != 2 || d->modes[0] == PARAM_REF || d->modes[1] == PARAM_REF || d->ret.set || d->defaults[0] ||
            d->defaults[1])
            error_at(P, open, "an 'on' block takes (old, new): the value before the change and after it");
    }
    Node *cond = NULL;
    Token *t = peek(P);
    if (t->type == T_IDENT && t->length == 4 && !memcmp(t->start, "when", 4)) {  /* when: only a word here */
        advance(P);
        cond = expression(P);
    }
    if (!check(P, T_LBRACE)) expect(P, T_LBRACE, "'{', (old, new) or 'when' after the variable names");
    d->body = block(P);
    if (cond) {  /* when c { body } is { if c { body } } */
        Node *test = new_node(N_IF, cond->line);
        test->as.if_s.cond = cond;
        test->as.if_s.then_branch = d->body;
        d->body = new_node(N_BLOCK, kw->line);
        list_add(&d->body->as.block.stmts, test);
    }
    n->as.on.func = d;
    return n;
}

/* Adds the hidden first parameter `this` to a method. */
static void add_this(Parser *P, FuncDecl *d) {
    int n = d->nparams + 1;
    ObjString **params = arena_alloc(tree, sizeof(ObjString *) * (size_t)n);
    Node **defaults = arena_alloc(tree, sizeof(Node *) * (size_t)n);
    unsigned char *modes = arena_alloc(tree, (size_t)n);
    TypeAnn *types = d->types ? arena_alloc(tree, sizeof(TypeAnn) * (size_t)n) : NULL;
    params[0] = lube_intern(P->I, "this", 4);
    modes[0] = PARAM_REF;
    if (types) memset(&types[0], 0, sizeof(TypeAnn));  /* `this` is always its class */
    for (int i = 0; i < d->nparams; i++) {
        params[i + 1] = d->params[i];
        defaults[i + 1] = d->defaults[i];
        modes[i + 1] = d->modes[i];
        if (types) types[i + 1] = d->types[i];
    }
    d->params = params;
    d->defaults = defaults;
    d->modes = modes;
    d->types = types;
    d->nparams = n;
    d->is_method = 1;
}

/*  class Dog extends Animal {
 *    var sound = "woof"              fields, each with a starting value
 *    constructor(name) { ... }       optional
 *    fn speak() => this.sound        methods
 *  }                                                                      */
static Node *class_statement(Parser *P) {
    Token *kw = advance(P);
    Token *name = expect(P, T_IDENT, "a class name after 'class'");
    Node *n = new_node(N_CLASS, kw->line);
    n->as.klass.var.name = name_of(P, name);
    n->as.klass.title = n->as.klass.var.name;
    if (match(P, T_EXTENDS)) {
        Token *parent = expect(P, T_IDENT, "a class name after 'extends'");
        n->as.klass.parent = new_node(N_VAR, parent->line);
        n->as.klass.parent->as.var.name = name_of(P, parent);
    }
    Token *open = expect(P, T_LBRACE, "'{' to start the class body");
    for (;;) {
        skip_newlines(P);
        if (match(P, T_RBRACE)) break;
        Token *t = peek(P);
        if (t->type == T_EOF) error_at(P, t, "missing '}' to close the class opened on line %d", open->line);
        if (t->type == T_VAR) {
            advance(P);
            Token *field = expect(P, T_IDENT, "a field name after 'var'");
            Node *f = new_node(N_LET, field->line);
            f->as.let.var.name = name_of(P, field);
            f->as.let.is_var = 1;
            expect(P, T_ASSIGN, "'=' and a starting value for the field (there is no nil)");
            f->as.let.value = expression(P);
            list_add(&n->as.klass.fields, f);
            end_statement(P);
        } else if (t->type == T_IDENT && t->length == 11 && !memcmp(t->start, "constructor", 11)) {
            advance(P);
            if (n->as.klass.ctor) error_at(P, t, "a class has only one constructor");
            Node *f = new_node(N_FUNC, t->line);
            f->as.func = function_rest(P, name_of(P, t), t->line);
            f->as.func->is_ctor = 1;
            if (f->as.func->ret.set) error_at(P, t, "a constructor cannot have a return type: it always makes the instance");
            add_this(P, f->as.func);
            n->as.klass.ctor = f;
        } else if (t->type == T_FN) {
            advance(P);
            Token *mname = expect(P, T_IDENT, "a method name after 'fn'");
            Node *f = new_node(N_FUNC, mname->line);
            f->as.func = function_rest(P, name_of(P, mname), mname->line);
            add_this(P, f->as.func);
            list_add(&n->as.klass.methods, f);
            if (f->as.func->is_arrow) end_statement(P);
        } else {
            char buf[64];
            error_at(P, t, "expected 'var', 'fn' or 'constructor' in the class body but found %s",
                     describe(t, buf, sizeof buf));
        }
    }
    return n;
}

static Node *if_statement(Parser *P) {
    Token *kw = advance(P);
    Node *n = new_node(N_IF, kw->line);
    n->as.if_s.cond = expression(P);
    n->as.if_s.then_branch = block(P);
    int save = P->pos;
    skip_newlines(P);
    if (match(P, T_ELSE)) {
        enter(P);  /* each `else if` nests one level deeper */
        if (check(P, T_IF)) n->as.if_s.else_branch = if_statement(P);
        else n->as.if_s.else_branch = block(P);
        leave(P);
    } else {
        P->pos = save;
    }
    return n;
}

static Node *while_statement(Parser *P) {
    Token *kw = advance(P);
    Node *n = new_node(N_WHILE, kw->line);
    n->as.while_s.cond = expression(P);
    n->as.while_s.body = block(P);
    return n;
}

static Node *for_statement(Parser *P) {
    Token *kw = advance(P);
    Node *n = new_node(N_FOR, kw->line);
    Token *first = expect(P, T_IDENT, "a loop variable after 'for'");
    n->as.for_s.first.name = name_of(P, first);
    if (match(P, T_COMMA)) {
        Token *second = expect(P, T_IDENT, "a second loop variable after ','");
        n->as.for_s.second.name = name_of(P, second);
        n->as.for_s.two = 1;
    }
    expect(P, T_IN, "'in' after the loop variable");
    n->as.for_s.iter = expression(P);
    n->as.for_s.body = block(P);
    return n;
}

static Node *try_statement(Parser *P) {
    Token *kw = advance(P);
    Node *n = new_node(N_TRY, kw->line);
    n->as.try_s.body = block(P);
    skip_newlines(P);
    expect(P, T_CATCH, "'catch' after the try block");
    if (check(P, T_IDENT)) {
        n->as.try_s.var.name = name_of(P, advance(P));
        n->as.try_s.has_var = 1;
    }
    n->as.try_s.handler = block(P);
    return n;
}

/* a.b.c: a package name (the words joined by dots, interned). */
static ObjString *dotted_name(Parser *P, const char *what, int allow_star, int *star) {
    StrBuf sb;
    sb_init(&sb);
    Token *t = expect(P, T_IDENT, what);
    sb_putn(&sb, t->start, (size_t)t->length);
    while (check(P, T_DOT)) {
        advance(P);
        if (allow_star && check(P, T_STAR)) {
            advance(P);
            *star = 1;
            break;
        }
        t = expect(P, T_IDENT, "a name after '.'");
        sb_putc(&sb, '.');
        sb_putn(&sb, t->start, (size_t)t->length);
    }
    ObjString *s = lube_intern(P->I, sb.data, sb.len);
    sb_free(&sb);
    return s;
}

/*  import "file"           runs file.lube (once); its names join this file's
 *  import a.b.Name         the name Name from package a.b (also: as Alias)
 *  import a.b.*            every public name of package a.b
 *  import a.b              the package itself, as b (also: as alias): b.Name */
static Node *import_statement(Parser *P) {
    Token *kw = advance(P);
    Node *n = new_node(N_IMPORT, kw->line);
    if (check(P, T_STRING)) {
        Token *path = advance(P);
        Node *lit = string_literal(P, path);
        if (lit->kind != N_LITERAL) error_at(P, path, "the import path cannot use interpolation");
        n->as.import.path = AS_STRING(lit->as.literal);
    } else {
        if (!check(P, T_IDENT)) expect(P, T_STRING, "a package name (like lube.geometry.Point) or a file path string after 'import'");
        n->as.import.dotted = dotted_name(P, "a package name", 1, &n->as.import.star);
        Token *t = peek(P);
        if (t->type == T_IDENT && t->length == 2 && !memcmp(t->start, "as", 2)) {  /* as: only a word here */
            if (n->as.import.star) error_at(P, t, "an import with * brings in every name, so it takes no 'as'");
            advance(P);
            n->as.import.alias = name_of(P, expect(P, T_IDENT, "a name after 'as'"));
        }
    }
    end_statement(P);
    return n;
}

static Node *statement(Parser *P) {
    Token *t = peek(P);
    Node *n;
    switch (t->type) {
    case T_LET:
    case T_VAR:
        return let_statement(P);
    case T_FN:
        if (peek_at(P, 1)->type == T_IDENT) return fn_statement(P);
        break;
    case T_IF:
        return if_statement(P);
    case T_WHILE:
        return while_statement(P);
    case T_FOR:
        return for_statement(P);
    case T_TRY:
        return try_statement(P);
    case T_IMPORT:
        return import_statement(P);
    case T_PACKAGE:
        error_at(P, t, "the package line must be the first line of a file");
        return NULL;
    case T_CLASS:
        return class_statement(P);
    case T_ON:
        return on_statement(P);
    case T_LBRACE:
        return block(P);
    case T_RETURN:
        advance(P);
        n = new_node(N_RETURN, t->line);
        if (!check(P, T_NEWLINE) && !check(P, T_SEMI) && !check(P, T_RBRACE) && !check(P, T_EOF))
            n->as.expr = expression(P);
        end_statement(P);
        return n;
    case T_BREAK:
    case T_CONTINUE:
        advance(P);
        n = new_node(t->type == T_BREAK ? N_BREAK : N_CONTINUE, t->line);
        end_statement(P);
        return n;
    case T_THROW:
        advance(P);
        n = new_node(N_THROW, t->line);
        n->as.expr = expression(P);
        end_statement(P);
        return n;
    default:
        break;
    }
    n = new_node(N_EXPR, t->line);
    n->as.expr = expression(P);
    end_statement(P);
    return n;
}

/* Parses a whole file into an N_BLOCK of top-level statements, allocated
 * in a new arena returned through *arena. On a syntax error it reports the
 * error, frees everything and returns NULL. */
Node *parse_program(Interp *I, Source *src, Arena **arena) {
    *arena = NULL;
    TokenArray toks = {0};
    if (!lex(src, src->text, src->text + src->length, 1, src->text, &toks)) {
        free(toks.items);
        return NULL;
    }
    tree = arena_new();
    pending.nbufs = pending.ntoks = 0;
    jmp_buf jb;
    Parser P;
    P.I = I;
    P.src = src;
    P.toks = toks.items;
    P.count = toks.count;
    P.pos = 0;
    P.jb = &jb;
    P.depth = 0;

    src->package = NULL;
    if (setjmp(jb) != 0) {  /* syntax error: free everything */
        while (pending.nbufs > 0) sb_free(&pending.bufs[--pending.nbufs]);
        while (pending.ntoks > 0) free(pending.toks[--pending.ntoks].items);
        free(toks.items);
        arena_free(tree);
        tree = NULL;
        return NULL;
    }
    Node *program = new_node(N_BLOCK, 1);  /* (after setjmp: nothing it changes is used after a longjmp) */
    skip_newlines(&P);
    if (check(&P, T_PACKAGE)) {  /* package a.b: the file's names live in package a.b */
        advance(&P);
        src->package = dotted_name(&P, "a package name after 'package'", 0, NULL);
        end_statement(&P);
    }
    for (;;) {
        skip_newlines(&P);
        if (check(&P, T_EOF)) break;
        Node *s = statement(&P);
        int last = P.pos - 1;  /* (its last token, newlines and ; aside) */
        while (last > 0 && (P.toks[last].type == T_NEWLINE || P.toks[last].type == T_SEMI)) last--;
        s->end_line = P.toks[last].line;
        list_add(&program->as.block.stmts, s);
    }
    free(toks.items);
    *arena = tree;
    tree = NULL;
    return program;
}
