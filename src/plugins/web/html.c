/*
 * web/html.c - the web plugin's HTML: tag helpers make Nodes, and a page's
 * view (Nodes, text, lists, components) is written out as HTML, every piece
 * of text escaped. (Part of web.c.)
 *
 * The rules that keep a page safe are here:
 *   - text and attribute values are escaped; only raw(html) goes out as it is;
 *   - a Node is made by the helpers alone, so data (an object from a request,
 *     say) can never become markup;
 *   - no <script>, <style>, <base> or the page's own <html>, <head>, <body>;
 *   - on... attributes take handlers (functions), never code: the page's
 *     script calls them, and the page runs no code written inline (its CSP);
 *   - a URL attribute (href, src, action...) keeps only http:, https:,
 *     mailto:, tel: and URLs on this site: javascript:, data: and the rest
 *     become about:invalid#refused;
 *   - style="..." is blocked by the page's CSP: it is refused, unless
 *     policy inline_styles allows it.
 */

typedef struct {
    char *name;
    LubeValue value;
} Attr;

enum { N_ELEMENT, N_RAW };

typedef struct {
    int kind;
    int is_void;   /* <br>, <img>...: no children, no closing tag */
    char *tag;     /* N_ELEMENT */
    char *text;    /* N_RAW: its HTML */
    size_t ntext;
    Attr *attrs;
    int nattrs;
    LubeValue *kids;
    int nkids;
} Node;

static LubeType *NodeType;

static void node_release(void *data) {
    Node *n = (Node *)data;
    for (int i = 0; i < n->nattrs; i++) {
        free(n->attrs[i].name);
        lube->release(n->attrs[i].value);
    }
    for (int i = 0; i < n->nkids; i++) lube->release(n->kids[i]);
    free(n->attrs);
    free(n->kids);
    free(n->tag);
    free(n->text);
}

static int node_describe(const void *data, char *buf, size_t size) {
    const Node *n = (const Node *)data;
    if (n->kind == N_RAW) return snprintf(buf, size, "Node(raw)");
    return snprintf(buf, size, "Node(<%s>)", n->tag);
}

static char *copy_str(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) abort();  /* LCOV_EXCL_LINE */
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static const char *const VOID_TAGS[] = {"area", "br", "col", "embed", "hr", "img", "input", "source", "track", "wbr", NULL};
/* The page's own, or code and styles the page's CSP would block anyway. */
static const char *const REFUSED_TAGS[] = {"script", "style", "base", "html", "head", "body", "title", "meta", "link", "noscript", NULL};
/* The events a handler can take. Those with a value give it to the handler: input and change the element's value
 * (a Bool for a checkbox), submit the form's fields (an object), keydown and keyup the key's name. */
static const char *const EVENTS[] = {"click", "dblclick", "input", "change", "submit", "keydown", "keyup",
                                     "focus", "blur", "mouseenter", "mouseleave", NULL};
/* Attributes that hold a URL. */
static const char *const URL_ATTRS[] = {"href", "src", "action", "formaction", "poster", "cite", "data",
                                        "xlink:href", "ping", "background", "manifest", NULL};

static int in_list(const char *const *list, const char *s) {
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], s)) return 1;
    return 0;
}

/* A tag or attribute name: a letter, then letters, digits and - (and : and _ in attributes). */
static int good_name(const char *s, int attr) {
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) return 0;
    for (const char *p = s + 1; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') continue;
        if (attr && (c == ':' || c == '_')) continue;
        return 0;
    }
    return strlen(s) < 48;
}

static int is_function(LubeValue v) { return lube->kind(v) == LUBE_OTHER && !strcmp(lube->type_name(v), "Function"); }
static int is_component(LubeValue v) { return lube->kind(v) == LUBE_OTHER && !strcmp(lube->type_name(v), "Component"); }

/* Whether v can be a child: text, a Number, a Node, a component, a List (of children), or nothing. */
static int child_ok(LubeValue v) {
    int k = lube->kind(v);
    return k == LUBE_STR || k == LUBE_NUMBER || k == LUBE_NOTHING || k == LUBE_LIST || lube->foreign(v, NodeType) ||
           is_component(v);
}

static const char *const CHILD_KINDS = "text, a Number, a Node, a component, a List of those, or nothing";

/* A Node with this tag, its attributes from the object first (if there is one) and its children after. */
static LubeValue make_node(LubeVM *vm, const char *tag, const char *fn, int argc, const LubeValue *argv) {
    void *data;
    int first = 0;
    LubeValue attrs = lube->nothing();
    if (argc && lube->kind(argv[0]) == LUBE_OBJECT) attrs = argv[first++];
    int is_void = in_list(VOID_TAGS, tag);
    if (is_void && argc > first) return lube->error(vm, "%s() makes <%s>, which has no children", fn, tag);
    for (int i = first; i < argc; i++)
        if (!child_ok(argv[i]))
            return lube->error(vm, "%s(): a child is %s, but %s is not one", fn, CHILD_KINDS, lube->a_type(argv[i]));
    LubeValue keys = lube->kind(attrs) == LUBE_OBJECT ? lube->object_keys(vm, attrs) : lube->new_list(vm);
    size_t nkeys = lube->list_len(keys);
    for (size_t i = 0; i < nkeys; i++) {  /* checked before anything is made */
        const char *name = lube->str(lube->list_get(keys, i), NULL);
        LubeValue v = lube->nothing();
        lube->object_get(vm, attrs, name, &v);
        int k = lube->kind(v);
        const char *why = NULL;
        if (!good_name(name, 1)) why = "is not an attribute's name";
        else if (!strncmp(name, "on", 2) && !in_list(EVENTS, name + 2))
            why = "is not an event a handler can take (click, dblclick, input, change, submit, keydown, keyup, focus, "
                  "blur, mouseenter, mouseleave)";
        else if (!strncmp(name, "on", 2) && !is_function(v))
            why = "takes a handler (a function), never code: the page runs no code written inline";
        else if (!strncmp(name, "data-on-", 8) || !strncmp(name, "data-lube", 9)) why = "is the page script's own";
        else if (!strcmp(name, "srcdoc")) why = "is refused: it is a page of HTML, unescaped";
        else if (strncmp(name, "on", 2) && k != LUBE_STR && k != LUBE_NUMBER && k != LUBE_BOOL && k != LUBE_NOTHING)
            why = "takes text, a Number, a Bool or nothing";
        if (why) {
            LubeValue e = lube->error(vm, "%s(): the attribute '%s' %s", fn, name, why);
            lube->release(keys);
            return e;
        }
    }
    LubeValue node = lube->new_foreign(vm, NodeType, &data);
    Node *n = (Node *)data;
    n->kind = N_ELEMENT;
    n->is_void = is_void;
    n->tag = copy_str(tag, strlen(tag));
    n->attrs = nkeys ? (Attr *)calloc(nkeys, sizeof(Attr)) : NULL;
    for (size_t i = 0; i < nkeys; i++) {
        const char *name = lube->str(lube->list_get(keys, i), NULL);
        LubeValue v = lube->nothing();
        lube->object_get(vm, attrs, name, &v);
        n->attrs[n->nattrs].name = copy_str(name, strlen(name));
        n->attrs[n->nattrs++].value = lube->retain(v);
    }
    lube->release(keys);
    if (argc > first) n->kids = (LubeValue *)calloc((size_t)(argc - first), sizeof(LubeValue));
    for (int i = first; i < argc; i++) n->kids[n->nkids++] = lube->retain(argv[i]);
    return node;
}

/* el(tag, attributes?, children...): any element the helpers do not name. */
FN(f_el) {
    const char *tag = lube->str(argv[0], NULL);
    if (!tag) return lube->error(vm, "el() needs a tag's name (text) first, not %s", lube->a_type(argv[0]));
    if (!good_name(tag, 0)) return lube->error(vm, "el(): '%s' is not a tag's name", tag);
    char low[64];
    size_t n = strlen(tag);
    for (size_t i = 0; i <= n; i++) low[i] = (char)(tag[i] >= 'A' && tag[i] <= 'Z' ? tag[i] + 32 : tag[i]);
    if (in_list(REFUSED_TAGS, low))
        return lube->error(vm, "el(): <%s> is refused: the page makes its own <html>, <head> and <body>, and runs only "
                               "the scripts and stylesheets on its site (app.page(..., {scripts: [...], styles: [...]}))",
                           low);
    return make_node(vm, low, "el", argc - 1, argv + 1);
}

/* raw(html): HTML that goes out as it is. Only for HTML the program made: never for what a request brought. */
FN(f_raw) {
    size_t len;
    const char *s = lube->str(argv[0], &len);
    if (!s) return lube->error(vm, "raw() needs text (HTML), not %s", lube->a_type(argv[0]));
    void *data;
    LubeValue node = lube->new_foreign(vm, NodeType, &data);
    Node *n = (Node *)data;
    n->kind = N_RAW;
    n->text = copy_str(s, len);
    n->ntext = len;
    return node;
}

/* The tag helpers: div(...), p(...)... one C function each (a LubeFn knows only its arguments). field() is
 * <input> (input() and time() are Lube's own). */
#define TAGS(X)                                                                                                      \
    X(a) X(abbr) X(article) X(aside) X(audio) X(b) X(blockquote) X(br) X(button) X(canvas) X(caption) X(code)       \
    X(col) X(colgroup) X(dd) X(del) X(details) X(dfn) X(dialog) X(div) X(dl) X(dt) X(em) X(fieldset)                \
    X(figcaption) X(figure) X(footer) X(form) X(h1) X(h2) X(h3) X(h4) X(h5) X(h6) X(header) X(hr) X(i) X(img)       \
    X(ins) X(kbd) X(label) X(legend) X(li) X(main) X(mark) X(menu) X(meter) X(nav) X(ol) X(optgroup) X(option)      \
    X(output) X(p) X(picture) X(pre) X(progress) X(q) X(s) X(samp) X(section) X(select) X(small) X(source) X(span)  \
    X(strong) X(sub) X(summary) X(sup) X(table) X(tbody) X(td) X(textarea) X(tfoot) X(th) X(thead) X(tr) X(track)  \
    X(u) X(ul) X(video) X(wbr)

#define TAG_FN(t) \
    FN(t_##t) { return make_node(vm, #t, #t, argc, argv); }
TAGS(TAG_FN)
FN(t_field) { return make_node(vm, "input", "field", argc, argv); }

static void declare_tags(LubePlugin *plugin) {
#define TAG_DECLARE(t) lube->function(plugin, #t, t_##t, 0, -1);
    TAGS(TAG_DECLARE)
    lube->function(plugin, "field", t_field, 0, -1);
    lube->function(plugin, "el", f_el, 1, -1);
    lube->function(plugin, "raw", f_raw, 1, 1);
}

/* ---- writing HTML ---- */

typedef struct {
    LubeVM *vm;
    Buf out;
    int inline_styles;      /* policy: style="..." allowed */
    const uint64_t *key;    /* a page's key, for handler ids; NULL: handlers are refused (no page) */
    LubeValue *handlers;    /* the handlers met, in order (retained) */
    int nh, caph;
    int failed;             /* 1: why says what is wrong; 2: Lube code failed (its error is raised: err) */
    char why[512];
    LubeValue err;
} Render;

static void render_fail(Render *r, const char *format, ...) {
    if (r->failed) return;
    va_list ap;
    va_start(ap, format);
    vsnprintf(r->why, sizeof r->why, format, ap);
    va_end(ap);
    r->failed = 1;
}

/* What went wrong, as text, and the error gone (a Lube error is taken: see take_error). */
static void render_error(Render *r, char *text, size_t size) {
    if (r->failed == 2) lube->take_error(r->vm, text, size);
    else snprintf(text, size, "%s", r->why);
}

static void handler_id(const uint64_t key[2], int k, char out[17]) {
    static const char hex[] = "0123456789abcdef";
    uint64_t h = siphash(key, (uint64_t)k);
    for (int i = 0; i < 16; i++) out[i] = hex[(h >> (60 - 4 * i)) & 15];
    out[16] = 0;
}

/* A URL keeps its scheme only if it is http, https, mailto or tel (or it has none: a URL on this site). Browsers
 * drop tabs and newlines in a scheme, and leading spaces and controls, so those are dropped here too. */
static int url_ok(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && (unsigned char)s[i] <= 0x20) i++;
    char scheme[16];
    size_t k = 0;
    for (; i < n; i++) {
        char c = s[i];
        if (c == '\t' || c == '\n' || c == '\r') continue;
        if (c == '/' || c == '?' || c == '#') return 1;  /* before any ':': a path */
        if (c == ':') break;
        if (k == sizeof scheme - 1) return 0;
        scheme[k++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    if (i == n) return 1;  /* no ':' at all */
    scheme[k] = 0;
    return !strcmp(scheme, "http") || !strcmp(scheme, "https") || !strcmp(scheme, "mailto") || !strcmp(scheme, "tel");
}

static void render(Render *r, LubeValue v, int depth);

static void render_attrs(Render *r, const Node *n) {
    for (int i = 0; i < n->nattrs && !r->failed; i++) {
        const char *name = n->attrs[i].name;
        LubeValue v = n->attrs[i].value;
        int k = lube->kind(v);
        if (!strncmp(name, "on", 2)) {
            if (!r->key) {
                render_fail(r, "<%s %s=...>: a handler works only in a page (app.page)", n->tag, name);
                return;
            }
            if (r->nh == r->caph) {
                r->caph = r->caph ? r->caph * 2 : 8;
                r->handlers = (LubeValue *)realloc(r->handlers, sizeof(LubeValue) * (size_t)r->caph);
                if (!r->handlers) abort();  /* LCOV_EXCL_LINE */
            }
            char id[17];
            handler_id(r->key, r->nh, id);
            r->handlers[r->nh++] = lube->retain(v);
            buf_fmt(&r->out, " data-on-%s=\"%s\"", name + 2, id);
            continue;
        }
        if (k == LUBE_NOTHING || (k == LUBE_BOOL && !lube->truthy(v))) continue;
        if (!strcmp(name, "style") && !r->inline_styles) {
            render_fail(r, "<%s style=...>: the page's CSP blocks inline styles: use a class and a stylesheet "
                           "(app.page(..., {styles: [...]})), or App({policy: {inline_styles: true}})", n->tag);
            return;
        }
        buf_add(&r->out, " ", 1);
        buf_str(&r->out, strcmp(name, "key") ? name : "data-key");  /* key: matches a list's items as they move */
        if (k == LUBE_BOOL) continue;  /* true: the attribute alone */
        char num[64];
        size_t len;
        const char *s;
        if (k == LUBE_NUMBER) {
            lube->format_number(lube->number(v), num, sizeof num);
            s = num;
            len = strlen(num);
        } else {
            s = lube->str(v, &len);
        }
        if (in_list(URL_ATTRS, name) && !url_ok(s, len)) s = "about:invalid#refused", len = strlen(s);
        buf_add(&r->out, "=\"", 2);
        buf_escape(&r->out, s, len);
        buf_add(&r->out, "\"", 1);
    }
}

#define MAX_DEPTH 200  /* elements and components inside each other: deeper is a view that makes itself */

static void render(Render *r, LubeValue v, int depth) {
    if (r->failed) return;
    if (depth > MAX_DEPTH) {
        render_fail(r, "the view is more than %d levels deep (does a component show itself?)", MAX_DEPTH);
        return;
    }
    int k = lube->kind(v);
    const Node *n;
    if (k == LUBE_NOTHING) return;
    if (k == LUBE_STR) {
        size_t len;
        const char *s = lube->str(v, &len);
        buf_escape(&r->out, s, len);
    } else if (k == LUBE_NUMBER) {
        char num[64];
        lube->format_number(lube->number(v), num, sizeof num);
        buf_str(&r->out, num);
    } else if (k == LUBE_LIST) {
        size_t len = lube->list_len(v);
        for (size_t i = 0; i < len && !r->failed; i++) render(r, lube->list_get(v, i), depth + 1);
    } else if ((n = (const Node *)lube->foreign(v, NodeType)) != NULL) {
        if (n->kind == N_RAW) {
            buf_add(&r->out, n->text, n->ntext);
            return;
        }
        buf_add(&r->out, "<", 1);
        buf_str(&r->out, n->tag);
        render_attrs(r, n);
        buf_add(&r->out, ">", 1);
        if (n->is_void) return;
        for (int i = 0; i < n->nkids && !r->failed; i++) render(r, n->kids[i], depth + 1);
        buf_fmt(&r->out, "</%s>", n->tag);
    } else if (is_component(v)) {
        LubeValue view = lube->call_method(r->vm, v, "view", 0, NULL);
        if (lube->failed(view)) {
            r->failed = 2;
            r->err = view;
            return;
        }
        render(r, view, depth + 1);
        lube->release(view);
    } else {
        render_fail(r, "a view shows %s, but %s is not one", CHILD_KINDS, lube->a_type(v));
    }
}

static void render_drop_handlers(LubeValue *h, int n) {
    for (int i = 0; i < n; i++) lube->release(h[i]);
    free(h);
}

/* html(view): the view as HTML text (for a page of one's own, or an email): no handlers in it. */
FN(f_html) {
    Render r;
    memset(&r, 0, sizeof r);
    r.vm = vm;
    r.inline_styles = 1;
    render(&r, argv[0], 0);
    render_drop_handlers(r.handlers, r.nh);
    LubeValue out = r.failed == 2 ? r.err  /* (the view's own error, as it is) */
                    : r.failed    ? lube->error(vm, "html(): %s", r.why)
                                  : lube->new_str(vm, r.out.p ? r.out.p : "", r.out.n);
    buf_free(&r.out);
    return out;
}
