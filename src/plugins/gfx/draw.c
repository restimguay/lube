/*
 * draw.c (part of gfx.c) - drawing on a Canvas, clipped to its edges.
 * Colors are 0xAARRGGBB, with straight (not premultiplied) alpha; drawing
 * puts a color over what is there ("source over").
 */

/* c over d. */
static uint32_t blend(uint32_t d, uint32_t c) {
    uint32_t a = c >> 24;
    if (a == 255) return c;
    if (a == 0) return d;
    uint32_t na = 255 - a, da = d >> 24;
    uint32_t sr = (c >> 16) & 255, sg = (c >> 8) & 255, sb = c & 255;
    uint32_t dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    if (da == 255)
        return 0xFF000000u | ((sr * a + dr * na + 127) / 255) << 16 | ((sg * a + dg * na + 127) / 255) << 8 |
               ((sb * a + db * na + 127) / 255);
    uint32_t oa = a * 255 + da * na;  /* (the result's alpha, times 255) */
    if (oa == 0) return 0;
    uint32_t r = (sr * a * 255 + dr * da * na + oa / 2) / oa, g = (sg * a * 255 + dg * da * na + oa / 2) / oa,
             b = (sb * a * 255 + db * da * na + oa / 2) / oa;
    return ((oa + 127) / 255) << 24 | r << 16 | g << 8 | b;
}

/* c with its alpha times f (0 to 1). */
static uint32_t faded(uint32_t c, double f) {
    if (f >= 1) return c;
    uint32_t a = (uint32_t)((double)(c >> 24) * (f > 0 ? f : 0) + 0.5);
    return a << 24 | (c & 0xFFFFFF);
}

static void plot(Canvas *c, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    uint32_t *p = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
    *p = blend(*p, color);
}

/* Pixels x0..x1 (both in) of row y. */
static void span(Canvas *c, int x0, int x1, int y, uint32_t color) {
    if (y < 0 || y >= c->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= c->w) x1 = c->w - 1;
    uint32_t *p = &c->px[(size_t)y * (size_t)c->w];
    if (color >> 24 == 255) {
        for (int x = x0; x <= x1; x++) p[x] = color;
    } else if (color >> 24) {
        for (int x = x0; x <= x1; x++) p[x] = blend(p[x], color);
    }
}

static void fill_rect(Canvas *c, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int y1 = y + h > c->h ? c->h : y + h;
    for (int row = y < 0 ? 0 : y; row < y1; row++) span(c, x, x + w - 1, row, color);
}

static void rect(Canvas *c, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    span(c, x, x + w - 1, y, color);
    if (h > 1) span(c, x, x + w - 1, y + h - 1, color);
    for (int row = y + 1; row < y + h - 1; row++) {
        plot(c, x, row, color);
        if (w > 1) plot(c, x + w - 1, row, color);
    }
}

/* From (x0, y0) to (x1, y1), both ends in. */
static void line(Canvas *c, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (long n = 0; n < 1L << 26; n++) {  /* (bounded: a line far outside still ends) */
        plot(c, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* The half-width of each row of a circle of radius r (rows 0..r from its middle), by the midpoint method. */
static int *circle_rows(int r) {
    int *half = (int *)calloc((size_t)r + 1, sizeof(int));
    if (!half) abort();  /* LCOV_EXCL_LINE: out of memory */
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        if (half[y] < x) half[y] = x;
        if (half[x] < y) half[x] = y;
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
    return half;
}

static void fill_circle(Canvas *c, int cx, int cy, int r, uint32_t color) {
    if (r < 0) return;
    if (r > 1 << 20) r = 1 << 20;
    int *half = circle_rows(r);
    for (int dy = -r; dy <= r; dy++) {
        int h = half[dy < 0 ? -dy : dy];
        span(c, cx - h, cx + h, cy + dy, color);
    }
    free(half);
}

static void circle(Canvas *c, int cx, int cy, int r, uint32_t color) {
    if (r < 0) return;
    if (r > 1 << 20) r = 1 << 20;
    int *half = circle_rows(r);
    for (int dy = -r; dy <= r; dy++) {  /* each row: from just past the next row out (narrower) to its own end */
        int ady = dy < 0 ? -dy : dy, h = half[ady], outer = ady < r ? half[ady + 1] : -1;
        int from = outer + 1 < h ? outer + 1 : h;
        span(c, cx - h, cx - from, cy + dy, color);
        span(c, cx + (from > 0 ? from : 1), cx + h, cy + dy, color);  /* (the middle pixel only once) */
    }
    free(half);
}

/* The pixels whose middles are inside the triangle. */
static void fill_triangle(Canvas *c, double x1, double y1, double x2, double y2, double x3, double y3, uint32_t color) {
    double area = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
    if (area == 0) return;
    if (area < 0) {  /* (the other way round) */
        double tx = x2, ty = y2;
        x2 = x3, y2 = y3, x3 = tx, y3 = ty;
    }
    double minx = fmin(x1, fmin(x2, x3)), maxx = fmax(x1, fmax(x2, x3));
    double miny = fmin(y1, fmin(y2, y3)), maxy = fmax(y1, fmax(y2, y3));
    int x0 = minx < 0 ? 0 : (int)floor(minx), xe = maxx >= c->w ? c->w - 1 : (int)ceil(maxx);
    int y0 = miny < 0 ? 0 : (int)floor(miny), ye = maxy >= c->h ? c->h - 1 : (int)ceil(maxy);
    for (int y = y0; y <= ye; y++) {
        double py = y + 0.5;
        for (int x = x0; x <= xe; x++) {
            double px = x + 0.5;
            if ((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1) >= 0 && (x3 - x2) * (py - y2) - (y3 - y2) * (px - x2) >= 0 &&
                (x1 - x3) * (py - y3) - (y1 - y3) * (px - x3) >= 0)
                c->px[(size_t)y * (size_t)c->w + (size_t)x] = blend(c->px[(size_t)y * (size_t)c->w + (size_t)x], color);
        }
    }
}

/* How to draw one image on another. */
typedef struct {
    int sx, sy, sw, sh;     /* the part of the image (all of it, unless asked) */
    double scale_x, scale_y;  /* negative: flipped */
    double angle;           /* radians, clockwise, around the middle of where it goes */
    double alpha;           /* 0 to 1 */
} DrawOptions;

/* src's part at (x, y) of dst: the top left of where it would be, unrotated. */
static void draw_image(Canvas *dst, const Canvas *src, double x, double y, const DrawOptions *o) {
    int sx = o->sx < 0 ? 0 : o->sx, sy = o->sy < 0 ? 0 : o->sy;
    int ex = o->sx + o->sw > src->w ? src->w : o->sx + o->sw, ey = o->sy + o->sh > src->h ? src->h : o->sy + o->sh;
    if (ex <= sx || ey <= sy || o->scale_x == 0 || o->scale_y == 0) return;
    int plain = o->angle == 0 && o->scale_x == 1 && o->scale_y == 1;
    if (plain && x == floor(x) && y == floor(y)) {  /* (the common case: pixel for pixel) */
        int dx = (int)x + (sx - o->sx), dy = (int)y + (sy - o->sy);
        for (int row = sy; row < ey; row++) {
            int ty = dy + row - sy;
            if (ty < 0 || ty >= dst->h) continue;
            for (int col = sx; col < ex; col++) {
                int tx = dx + col - sx;
                if (tx < 0 || tx >= dst->w) continue;
                uint32_t *p = &dst->px[(size_t)ty * (size_t)dst->w + (size_t)tx];
                *p = blend(*p, faded(src->px[(size_t)row * (size_t)src->w + (size_t)col], o->alpha));
            }
        }
        return;
    }
    /* Otherwise each pixel where it goes asks which pixel of src lands on it. */
    double w = o->sw * fabs(o->scale_x), h = o->sh * fabs(o->scale_y);
    double cx = x + w / 2, cy = y + h / 2, cs = cos(o->angle), sn = sin(o->angle);
    if (fabs(cs) < 1e-12) cs = 0;  /* (quarter turns exactly: pixels on an edge stay in) */
    if (fabs(sn) < 1e-12) sn = 0;
    double ext_x = fabs(w / 2 * cs) + fabs(h / 2 * sn), ext_y = fabs(w / 2 * sn) + fabs(h / 2 * cs);
    int x0 = (int)floor(cx - ext_x), x1 = (int)ceil(cx + ext_x), y0 = (int)floor(cy - ext_y), y1 = (int)ceil(cy + ext_y);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;
    for (int ty = y0; ty < y1; ty++) {
        for (int tx = x0; tx < x1; tx++) {
            double px = tx + 0.5 - cx, py = ty + 0.5 - cy;
            double ux = px * cs + py * sn, uy = -px * sn + py * cs;  /* (turned back) */
            double u = (ux / o->scale_x) + o->sw / 2.0, v = (uy / o->scale_y) + o->sh / 2.0;
            if (u < 0 || v < 0 || u >= o->sw || v >= o->sh) continue;
            int col = o->sx + (int)u, row = o->sy + (int)v;
            if (col < sx || row < sy || col >= ex || row >= ey) continue;
            uint32_t *p = &dst->px[(size_t)ty * (size_t)dst->w + (size_t)tx];
            *p = blend(*p, faded(src->px[(size_t)row * (size_t)src->w + (size_t)col], o->alpha));
        }
    }
}
