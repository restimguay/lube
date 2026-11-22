/*
 * web/http.c - reading HTTP/1.1 requests, strictly. (Part of web.c.)
 *
 * Anything a lenient server would guess at is refused, so that no proxy in
 * front of the server can read a request differently from it (request
 * smuggling): lines end with CRLF and nothing else; no space before a
 * header's colon, no folded lines, no control characters; one Host, one
 * Content-Length (digits only), and no Transfer-Encoding; the target is a
 * path ("/..."), with no "." or ".." in it, its %-escapes well formed and
 * its text UTF-8.
 */

enum { M_GET = 1, M_HEAD = 2, M_POST = 4, M_PUT = 8, M_PATCH = 16, M_DELETE = 32, M_OPTIONS = 64 };
static const char *const METHOD_NAMES[] = {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", NULL};

typedef struct {
    char *name;   /* lowercase */
    char *value;  /* without the spaces around it */
} Header;

typedef struct {
    int method;           /* M_GET... */
    const char *method_name;
    char *path;           /* the path, %-decoded ("/a b") */
    char **segs;          /* its segments, each decoded alone (so "%2F" stays inside its segment) */
    int nsegs;
    char *query;          /* after '?', as it came (or "") */
    Header *h;
    int nh;
    int http10;
    long long length;     /* the body's (0 if none) */
    int expect_continue;
    int close, keep_alive, upgrade;  /* from Connection */
} Req;

static void req_free(Req *r) {
    for (int i = 0; i < r->nh; i++) {
        free(r->h[i].name);
        free(r->h[i].value);
    }
    for (int i = 0; i < r->nsegs; i++) free(r->segs[i]);
    free(r->h);
    free(r->segs);
    free(r->path);
    free(r->query);
    memset(r, 0, sizeof *r);
}

/* The request's header `name` (lowercase), or NULL. */
static const char *req_header(const Req *r, const char *name) {
    for (int i = 0; i < r->nh; i++)
        if (!strcmp(r->h[i].name, name)) return r->h[i].value;
    return NULL;
}

static int header_count(const Req *r, const char *name) {
    int n = 0;
    for (int i = 0; i < r->nh; i++) n += !strcmp(r->h[i].name, name);
    return n;
}

static int tchar(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

/* Whether the comma-separated list of tokens has `token` (in any case). */
static int has_token(const char *list, const char *token) {
    size_t n = strlen(token);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - start) == n) {
            size_t i = 0;
            while (i < n && (start[i] | 32) == (token[i] | 32)) i++;
            if (i == n) return 1;
        }
    }
    return 0;
}

/* %-decodes s[0..n) into out (with '+' as a space, in a query): 0 if an escape is not well formed, or the text
 * has a NUL or is not UTF-8. */
static int url_decode(const char *s, size_t n, int plus, Buf *out) {
    out->n = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '%') {
            int hi = i + 2 < n ? hex_digit(s[i + 1]) : -1, lo = i + 2 < n ? hex_digit(s[i + 2]) : -1;
            if (hi < 0 || lo < 0) return 0;
            c = (char)(hi << 4 | lo);
            i += 2;
        } else if (c == '+' && plus) {
            c = ' ';
        }
        if (!c) return 0;
        buf_add(out, &c, 1);
    }
    buf_add(out, "", 1);
    out->n--;
    return utf8_ok(out->p, out->n);
}

typedef struct {
    size_t max_url;
    int max_headers;
    long long max_body;
} Limits;

/* Whether a head that has not ended yet is already wrong: a LF with no CR before it, a NUL, or a CR with no LF
 * after it. (A server that waits for its CRLF CRLF would wait for nothing.) */
static int head_broken(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!s[i] || (s[i] == '\n' && (!i || s[i - 1] != '\r')) || (s[i] == '\r' && i + 1 < n && s[i + 1] != '\n'))
            return 1;
    return 0;
}

/* Reads a request's head (head[0..n), the request line and header lines, each ending in CRLF, and the empty line
 * after them not included) into r: 0, or the status that refuses it. */
static int parse_head(char *head, size_t n, const Limits *lim, Req *r) {
    memset(r, 0, sizeof *r);
    for (size_t i = 0; i < n; i++)  /* no CR or LF but in a CRLF; no NUL */
        if (!head[i] || (head[i] == '\r' && (i + 1 >= n || head[i + 1] != '\n')) ||
            (head[i] == '\n' && (i == 0 || head[i - 1] != '\r')))
            return 400;
    head[n] = 0;
    char *line = head, *eol = strstr(line, "\r\n");
    if (!eol) eol = head + n;
    *eol = 0;
    /* the request line: METHOD SP target SP HTTP/1.x */
    char *sp1 = strchr(line, ' ');
    if (!sp1) return 400;
    *sp1 = 0;
    for (char *p = line; *p; p++)
        if (!tchar(*p)) return 400;
    if (!*line) return 400;
    for (int i = 0; METHOD_NAMES[i]; i++)
        if (!strcmp(line, METHOD_NAMES[i])) r->method = 1 << i, r->method_name = METHOD_NAMES[i];
    char *target = sp1 + 1, *sp2 = strchr(target, ' ');
    if (!sp2) return 400;
    *sp2 = 0;
    char *version = sp2 + 1;
    if (strlen(target) > lim->max_url) return 414;
    if (!strcmp(version, "HTTP/1.1")) r->http10 = 0;
    else if (!strcmp(version, "HTTP/1.0")) r->http10 = 1;
    else if (!strncmp(version, "HTTP/", 5) && strlen(version) == 8 && version[6] == '.') return 505;
    else return 400;
    if (target[0] != '/') return 400;  /* only a path: no "http://host/...", no "*" */
    for (char *p = target; *p; p++)
        if ((unsigned char)*p <= 0x20 || *p == 0x7f || *p == '#' || *p == '\\') return 400;
    if (!r->method) return 501;
    /* the target: its segments, decoded each, then the path, decoded whole */
    char *q = strchr(target, '?');
    size_t plen = q ? (size_t)(q - target) : strlen(target);
    r->query = copy_str(q ? q + 1 : "", q ? strlen(q + 1) : 0);
    Buf d = {0};
    int ok = url_decode(target, plen, 0, &d);
    r->path = copy_str(d.p ? d.p : "", d.n);
    for (size_t i = 1; ok && plen > 1 && i <= plen; i++) {  /* each segment after a '/' ("/" has none) */
        size_t start = i;
        while (i < plen && target[i] != '/') i++;
        ok = url_decode(target + start, i - start, 0, &d);
        if (!ok) break;
        if (!strcmp(d.p, ".") || !strcmp(d.p, "..")) ok = 0;
        r->segs = (char **)realloc(r->segs, sizeof(char *) * (size_t)(r->nsegs + 1));
        r->segs[r->nsegs++] = copy_str(d.p, d.n);
    }
    if (ok) ok = url_decode(r->query, strlen(r->query), 1, &d);  /* (its pairs are split later: well formed whole) */
    buf_free(&d);
    if (!ok) return 400;
    /* the header lines */
    char *p = eol + 2;
    while (p < head + n) {
        eol = strstr(p, "\r\n");
        if (!eol) eol = head + n;
        *eol = 0;
        if (*p == ' ' || *p == '\t') return 400;  /* a folded line */
        char *colon = p;
        while (*colon && tchar(*colon)) colon++;
        if (*colon != ':' || colon == p) return 400;  /* (a space before the colon, or none) */
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *end = v + strlen(v);
        while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
        *end = 0;
        for (char *c = v; *c; c++)
            if (((unsigned char)*c < 0x20 && *c != '\t') || *c == 0x7f) return 400;
        if (r->nh == lim->max_headers) return 431;
        r->h = (Header *)realloc(r->h, sizeof(Header) * (size_t)(r->nh + 1));
        for (char *c = p; *c; c++) *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
        r->h[r->nh].name = copy_str(p, strlen(p));
        r->h[r->nh++].value = copy_str(v, (size_t)(end - v));
        p = eol + 2;
    }
    /* what the headers say */
    int hosts = header_count(r, "host");
    if (hosts > 1 || (!r->http10 && !hosts)) return 400;
    const char *host = req_header(r, "host");
    for (const char *c = host ? host : ""; *c; c++)  /* a name or address, and a port */
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr(".-:[]", *c)))
            return 400;
    if (req_header(r, "transfer-encoding")) return req_header(r, "content-length") ? 400 : 501;
    if (header_count(r, "content-length") > 1) return 400;
    const char *cl = req_header(r, "content-length");
    if (cl) {
        if (!*cl) return 400;
        long long len = 0;
        for (const char *c = cl; *c; c++) {
            if (*c < '0' || *c > '9') return 400;
            if (len > lim->max_body) return 413;
            len = len * 10 + (*c - '0');
        }
        if (len > lim->max_body) return 413;
        r->length = len;
    }
    const char *conn = req_header(r, "connection");
    if (conn) {
        r->close = has_token(conn, "close");
        r->keep_alive = has_token(conn, "keep-alive");
        r->upgrade = has_token(conn, "upgrade");
    }
    const char *expect = req_header(r, "expect");
    if (expect) {
        if (!has_token(expect, "100-continue") || strchr(expect, ',')) return 417;
        r->expect_continue = 1;
    }
    return 0;
}

static const char *status_text(int status) {
    switch (status) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "";
    }
}
