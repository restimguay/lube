/*
 * lexer.c - turns source text into tokens.
 *
 * Statements end at a newline, so the lexer emits T_NEWLINE tokens, but only
 * where a statement could actually end:
 *   - the previous token can end a statement (a name, literal, `)`, `]`, `}`,
 *     `return`, `break` or `continue`), and
 *   - we are not inside ( ) or [ ]  (inside { } newlines count again), and
 *   - the next line does not start with `.`, `|`, `and`, `or`, `&&`, `||`,
 *     which lets long expressions and method chains span several lines.
 */
#include "lube.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NESTING 256

typedef struct {
    Source *src;
    const char *cur, *end, *line_start;
    int line;
    char brackets[MAX_NESTING];
    int depth;
    TokenType last;
    TokenArray *out;
} Lexer;

static const struct {
    const char *word;
    TokenType type;
} KEYWORDS[] = {
    {"and", T_AND},       {"break", T_BREAK},   {"catch", T_CATCH},   {"class", T_CLASS},
    {"continue", T_CONTINUE}, {"else", T_ELSE}, {"extends", T_EXTENDS}, {"false", T_FALSE},
    {"fn", T_FN},         {"for", T_FOR},       {"if", T_IF},         {"import", T_IMPORT},
    {"in", T_IN},         {"is", T_IS},         {"let", T_LET},       {"nil", T_NIL},
    {"not", T_NOT},       {"or", T_OR},         {"return", T_RETURN}, {"super", T_SUPER},
    {"this", T_THIS},
    {"throw", T_THROW},   {"true", T_TRUE},     {"try", T_TRY},       {"var", T_VAR},
    {"while", T_WHILE},
};

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (unsigned char)c >= 0x80;
}
static int is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }

static void lex_error(Lexer *L, int line, const char *line_start, const char *at, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    report_error(L->src, line, (int)(at - line_start) + 1, "syntax error", msg);
}

static Token *emit(Lexer *L, TokenType type, const char *start, int line, const char *line_start) {
    TokenArray *a = L->out;
    if (a->count == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 256;
        a->items = xrealloc(a->items, sizeof(Token) * (size_t)a->cap);
    }
    Token *t = &a->items[a->count++];
    t->type = type;
    t->start = start;
    t->length = (int)(L->cur - start);
    t->line = line;
    t->col = (int)(start - line_start) + 1;
    t->number = 0;
    L->last = type;
    return t;
}

static int match(Lexer *L, char c) {
    if (L->cur < L->end && *L->cur == c) {
        L->cur++;
        return 1;
    }
    return 0;
}

static int can_end_statement(TokenType t) {
    switch (t) {
    case T_IDENT: case T_NUMBER: case T_STRING: case T_TRUE: case T_FALSE: case T_NIL: case T_THIS:
    case T_RPAREN: case T_RBRACKET: case T_RBRACE: case T_RETURN: case T_BREAK: case T_CONTINUE:
        return 1;
    default:
        return 0;
    }
}

static int newline_matters(Lexer *L) {
    return L->depth == 0 || L->brackets[L->depth - 1] == '{';
}

static int starts_word(const char *p, const char *end, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(end - p) < n || memcmp(p, word, n) != 0) return 0;
    return p + n >= end || !is_ident_char(p[n]);
}

/* Is the comment [p, end) the line `#strict`? */
static int strict_directive(const char *p, const char *end) {
    if (end - p < 7 || memcmp(p, "#strict", 7)) return 0;
    for (p += 7; p < end; p++)
        if (*p != ' ' && *p != '\t' && *p != '\r') return 0;
    return 1;
}

/* Does the next non-blank line continue the current expression? */
static int next_line_continues(Lexer *L) {
    const char *p = L->cur, *end = L->end;
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p < end && (*p == '#' || (*p == '/' && p + 1 < end && p[1] == '/'))) {
            while (p < end && *p != '\n') p++;
            continue;
        }
        break;
    }
    if (p < end && *p == '|') return 1;  /* a pipe, or || */
    if (end - p >= 2) {
        if (p[0] == '&' && p[1] == '&') return 1;
        if (p[0] == '.' && p[1] != '.') return 1;
    }
    return starts_word(p, end, "and") || starts_word(p, end, "or");
}

static TokenType keyword_or_ident(const char *s, size_t n) {
    for (size_t i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++) {
        if (strlen(KEYWORDS[i].word) == n && memcmp(KEYWORDS[i].word, s, n) == 0) return KEYWORDS[i].type;
    }
    return T_IDENT;
}

#define MAX_STRING_NESTING 32

/* Given p at an opening quote, returns the position just past the closing
 * quote, or NULL if the string never ends (or nests more than
 * MAX_STRING_NESTING strings deep, which sets *too_deep). Double-quoted
 * strings may contain {interpolations}, which may themselves contain strings. */
static const char *skip_nested(const char *p, const char *end, int level, int *too_deep) {
    if (level > MAX_STRING_NESTING) {
        *too_deep = 1;
        return NULL;
    }
    char q = *p++;
    int depth = 0;
    while (p < end) {
        char c = *p;
        if (c == '\\') {
            if (p + 1 >= end) return NULL;
            p += 2;
            continue;
        }
        if (q == '"') {
            if (c == '{') { depth++; p++; continue; }
            if (c == '}' && depth > 0) { depth--; p++; continue; }
            if (depth > 0 && (c == '"' || c == '\'')) {
                p = skip_nested(p, end, level + 1, too_deep);
                if (!p) return NULL;
                continue;
            }
        }
        if (c == q && depth == 0) return p + 1;
        p++;
    }
    return NULL;
}

const char *skip_string_lit(const char *p, const char *end, int *too_deep) {
    *too_deep = 0;
    return skip_nested(p, end, 1, too_deep);
}

static int lex_number(Lexer *L, const char *start, int line, const char *ls) {
    char buf[128];
    size_t n = 0;
    const char *p = L->cur, *end = L->end;
#define PUT(c) do { if (n < sizeof buf - 1) buf[n++] = (c); } while (0)
    if (p[0] == '0' && p + 1 < end && (p[1] == 'x' || p[1] == 'X')) {
        PUT('0'); PUT('x');
        p += 2;
        if (!(p < end && is_hex(*p))) {
            lex_error(L, line, ls, start, "hexadecimal literal needs digits after '0x'");
            return 0;
        }
        while (p < end && (is_hex(*p) || *p == '_')) { if (*p != '_') PUT(*p); p++; }
    } else {
        while (p < end && (is_digit(*p) || *p == '_')) { if (*p != '_') PUT(*p); p++; }
        if (p + 1 < end && *p == '.' && is_digit(p[1])) {
            PUT('.');
            p++;
            while (p < end && (is_digit(*p) || *p == '_')) { if (*p != '_') PUT(*p); p++; }
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            const char *q = p + 1;
            if (q < end && (*q == '+' || *q == '-')) q++;
            if (q < end && is_digit(*q)) {
                while (p < q) { PUT(*p); p++; }
                while (p < end && is_digit(*p)) { PUT(*p); p++; }
            }
        }
    }
#undef PUT
    if (n >= sizeof buf - 1) {
        lex_error(L, line, ls, start, "number literal is too long");
        return 0;
    }
    if (p < end && is_ident_start(*p)) {
        lex_error(L, line, ls, p, "unexpected character '%c' in number", *p);
        return 0;
    }
    buf[n] = '\0';
    L->cur = p;
    Token *t = emit(L, T_NUMBER, start, line, ls);
    t->number = strtod(buf, NULL);
    return 1;
}

/* Tokenizes [begin, end). `line`/`line_start` give the position of `begin`
 * within the file (the parser re-lexes string interpolations this way).
 * Returns 1 on success; on failure the error has already been reported. */
int lex(Source *src, const char *begin, const char *end, int line, const char *line_start, TokenArray *out) {
    Lexer L;
    L.src = src;
    L.cur = begin;
    L.end = end;
    L.line = line;
    L.line_start = line_start;
    L.depth = 0;
    L.last = T_NEWLINE;
    L.out = out;
    if ((size_t)(end - begin) > LUBE_MAX_BYTES) {
        report_error(src, line, 0, "error", "the file is too large");
        return 0;
    }

    for (;;) {
        while (L.cur < L.end && (*L.cur == ' ' || *L.cur == '\t' || *L.cur == '\r')) L.cur++;
        if (L.cur >= L.end) break;

        const char *start = L.cur;
        int tline = L.line;
        const char *tls = L.line_start;
        char c = *L.cur;

        if (c == '#' || (c == '/' && L.cur + 1 < L.end && L.cur[1] == '/')) {  /* comment: # or // */
            while (L.cur < L.end && *L.cur != '\n') L.cur++;
            if (begin == src->text && strict_directive(start, L.cur)) {  /* #strict: typed functions */
                if (out->count > 0) {
                    lex_error(&L, tline, tls, start, "#strict must come first in the file, before any code");
                    return 0;
                }
                src->strict = 1;
            }
            continue;
        }
        if (c == '\n') {
            L.cur++;
            if (newline_matters(&L) && can_end_statement(L.last) && !next_line_continues(&L))
                emit(&L, T_NEWLINE, start, tline, tls);
            L.line++;
            L.line_start = L.cur;
            continue;
        }
        if (c == '\\') {  /* explicit line continuation */
            const char *p = L.cur + 1;
            while (p < L.end && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
            if (p < L.end && *p == '\n') {
                L.cur = p + 1;
                L.line++;
                L.line_start = L.cur;
                continue;
            }
            lex_error(&L, tline, tls, start, "unexpected '\\' (a line continuation must end the line)");
            return 0;
        }
        if (is_digit(c)) {
            if (!lex_number(&L, start, tline, tls)) return 0;
            continue;
        }
        if (is_ident_start(c)) {
            while (L.cur < L.end && is_ident_char(*L.cur)) L.cur++;
            emit(&L, keyword_or_ident(start, (size_t)(L.cur - start)), start, tline, tls);
            continue;
        }
        if (c == '"' || c == '\'') {
            int too_deep;
            const char *e = skip_string_lit(L.cur, L.end, &too_deep);
            if (!e) {
                if (too_deep)
                    lex_error(&L, tline, tls, start, "strings are nested too deeply inside interpolations");
                else if (c == '"' && memchr(start, '{', (size_t)(L.end - start)))
                    lex_error(&L, tline, tls, start,
                              "unterminated string (an unclosed '{' starts an interpolation; write \\{ for a literal brace)");
                else
                    lex_error(&L, tline, tls, start, "unterminated string");
                return 0;
            }
            for (const char *p = L.cur; p < e; p++) {
                if (*p == '\n') {
                    L.line++;
                    L.line_start = p + 1;
                }
            }
            L.cur = e;
            emit(&L, T_STRING, start, tline, tls);
            continue;
        }

        L.cur++;
        TokenType t;
        switch (c) {
        case '(': case '[': case '{':
            if (L.depth >= MAX_NESTING) {
                lex_error(&L, tline, tls, start, "brackets are nested too deeply");
                return 0;
            }
            L.brackets[L.depth++] = c;
            t = c == '(' ? T_LPAREN : c == '[' ? T_LBRACKET : T_LBRACE;
            break;
        case ')': case ']': case '}':
            if (L.depth > 0) L.depth--;
            t = c == ')' ? T_RPAREN : c == ']' ? T_RBRACKET : T_RBRACE;
            break;
        case ',': t = T_COMMA; break;
        case ':': t = T_COLON; break;
        case ';': t = T_SEMI; break;
        case '.':
            if (match(&L, '.')) t = match(&L, '=') ? T_DOTDOTEQ : T_DOTDOT;
            else t = T_DOT;
            break;
        case '+': t = match(&L, '=') ? T_PLUS_EQ : T_PLUS; break;
        case '-': t = match(&L, '=') ? T_MINUS_EQ : T_MINUS; break;
        case '*': t = match(&L, '*') ? T_POW : match(&L, '=') ? T_STAR_EQ : T_STAR; break;
        case '/': t = match(&L, '=') ? T_SLASH_EQ : T_SLASH; break;
        case '%': t = match(&L, '=') ? T_PERCENT_EQ : T_PERCENT; break;
        case '=': t = match(&L, '=') ? T_EQ : match(&L, '>') ? T_ARROW : T_ASSIGN; break;
        case '!': t = match(&L, '=') ? T_NEQ : T_BANG; break;
        case '<': t = match(&L, '=') ? T_LE : T_LT; break;
        case '>': t = match(&L, '=') ? T_GE : T_GT; break;
        case '|':
            if (match(&L, '|')) {
                t = T_OR;
            } else if (L.cur < L.end && *L.cur == '>') {
                lex_error(&L, tline, tls, start, "the pipe operator is '|', not '|>': write x | f(y)");
                return 0;
            } else {
                t = T_PIPE;
            }
            break;
        case '&': t = match(&L, '&') ? T_AND : T_AMP; break;
        default:
            if ((unsigned char)c < 0x20 || c == 0x7f)
                lex_error(&L, tline, tls, start, "unexpected control character (byte 0x%02x)", (unsigned char)c);
            else
                lex_error(&L, tline, tls, start, "unexpected character '%c'", c);
            return 0;
        }
        emit(&L, t, start, tline, tls);
    }

    if (can_end_statement(L.last)) emit(&L, T_NEWLINE, L.cur, L.line, L.line_start);
    emit(&L, T_EOF, L.cur, L.line, L.line_start);
    return 1;
}
