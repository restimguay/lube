/*
 * win32.c (part of gfx.c) - windows on Windows: user32 and gdi32, which
 * every Windows has. Each frame goes to the window with StretchDIBits,
 * fitted (as large as fits, in whole pixels where it can, black around it).
 */
#include <windowsx.h>

typedef struct {
    HWND hwnd;
    BITMAPINFO bmi;
    int sw, sh;          /* the window's inside, in pixels */
    WCHAR high;          /* the first half of a character typed in two (UTF-16) */
} W32;

/* A Windows virtual-key code, as a key. */
static int vk_key(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return K_A + (int)(vk - 'A');
    if (vk >= '0' && vk <= '9') return K_0 + (int)(vk - '0');
    if (vk >= VK_F1 && vk <= VK_F12) return K_F1 + (int)(vk - VK_F1);
    switch (vk) {
    case VK_SPACE: return K_SPACE;
    case VK_RETURN: return K_ENTER;
    case VK_ESCAPE: return K_ESCAPE;
    case VK_TAB: return K_TAB;
    case VK_BACK: return K_BACKSPACE;
    case VK_DELETE: return K_DELETE;
    case VK_INSERT: return K_INSERT;
    case VK_HOME: return K_HOME;
    case VK_END: return K_END;
    case VK_PRIOR: return K_PAGE_UP;
    case VK_NEXT: return K_PAGE_DOWN;
    case VK_LEFT: return K_LEFT;
    case VK_RIGHT: return K_RIGHT;
    case VK_UP: return K_UP;
    case VK_DOWN: return K_DOWN;
    case VK_SHIFT: return K_SHIFT;
    case VK_CONTROL: return K_CONTROL;
    case VK_MENU: return K_ALT;
    case VK_LWIN: case VK_RWIN: return K_COMMAND;
    case VK_CAPITAL: return K_CAPS_LOCK;
    case VK_OEM_MINUS: return K_MINUS;
    case VK_OEM_PLUS: return K_EQUALS;
    case VK_OEM_4: return K_LEFT_BRACKET;
    case VK_OEM_6: return K_RIGHT_BRACKET;
    case VK_OEM_1: return K_SEMICOLON;
    case VK_OEM_7: return K_QUOTE;
    case VK_OEM_COMMA: return K_COMMA;
    case VK_OEM_PERIOD: return K_PERIOD;
    case VK_OEM_2: return K_SLASH;
    case VK_OEM_5: return K_BACKSLASH;
    case VK_OEM_3: return K_BACKQUOTE;
    default: return 0;
    }
}

static void w32_blit(Win *w, HDC dc) {
    W32 *s = (W32 *)w->sys;
    double ox, oy, sc;
    lube_gfx_fit(w->c.w, w->c.h, s->sw, s->sh, &ox, &oy, &sc);
    int dw = (int)(w->c.w * sc), dh = (int)(w->c.h * sc), x = (int)ox, y = (int)oy;
    SetStretchBltMode(dc, COLORONCOLOR);  /* (nearest pixel: sharp) */
    StretchDIBits(dc, x, y, dw, dh, 0, 0, w->c.w, w->c.h, w->c.px, &s->bmi, DIB_RGB_COLORS, SRCCOPY);
    PatBlt(dc, 0, 0, s->sw, y, BLACKNESS);  /* (black around it) */
    PatBlt(dc, 0, y + dh, s->sw, s->sh - y - dh, BLACKNESS);
    PatBlt(dc, 0, y, x, dh, BLACKNESS);
    PatBlt(dc, x + dw, y, s->sw - x - dw, dh, BLACKNESS);
}

static void w32_mouse(Win *w, LPARAM lp, int kind, int button) {
    W32 *s = (W32 *)w->sys;
    Event e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.button = button;
    to_window(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), s->sw, s->sh, &e.x, &e.y);
    push_event(w, &e);
}

static LRESULT CALLBACK w32_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Win *w = active;
    if (!w || !w->sys || ((W32 *)w->sys)->hwnd != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    W32 *s = (W32 *)w->sys;
    Event e;
    memset(&e, 0, sizeof e);
    switch (msg) {
    case WM_CLOSE: closed_by_user(w); return 0;
    case WM_SIZE:
        s->sw = LOWORD(lp);
        s->sh = HIWORD(lp);
        return 0;
    case WM_SETFOCUS: push_simple(w, LUBE_GFX_FOCUS, 0, 0, 0); return 0;
    case WM_KILLFOCUS: push_simple(w, LUBE_GFX_BLUR, 0, 0, 0); return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        e.kind = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ? LUBE_GFX_KEY_DOWN : LUBE_GFX_KEY_UP;
        e.key = vk_key(wp);
        e.repeat = e.kind == LUBE_GFX_KEY_DOWN && (lp >> 30) & 1;
        if (e.key) push_event(w, &e);
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;  /* (alt-F4 still closes it) */
        return 0;
    case WM_CHAR: {  /* the text typed, from UTF-16 */
        WCHAR c = (WCHAR)wp;
        if (c >= 0xD800 && c < 0xDC00) {
            s->high = c;
            return 0;
        }
        unsigned long cp = c;
        if (c >= 0xDC00 && c < 0xE000) cp = s->high ? 0x10000 + (((unsigned long)s->high - 0xD800) << 10) + (c - 0xDC00) : 0;
        s->high = 0;
        if (cp < 32 || cp == 127) return 0;
        e.kind = LUBE_GFX_TEXT;
        if (cp < 0x80) {
            e.text[0] = (char)cp;
        } else if (cp < 0x800) {
            e.text[0] = (char)(0xC0 | (cp >> 6));
            e.text[1] = (char)(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            e.text[0] = (char)(0xE0 | (cp >> 12));
            e.text[1] = (char)(0x80 | ((cp >> 6) & 63));
            e.text[2] = (char)(0x80 | (cp & 63));
        } else {
            e.text[0] = (char)(0xF0 | (cp >> 18));
            e.text[1] = (char)(0x80 | ((cp >> 12) & 63));
            e.text[2] = (char)(0x80 | ((cp >> 6) & 63));
            e.text[3] = (char)(0x80 | (cp & 63));
        }
        push_event(w, &e);
        return 0;
    }
    case WM_MOUSEMOVE: w32_mouse(w, lp, LUBE_GFX_MOUSE_MOVE, 0); return 0;
    case WM_LBUTTONDOWN: SetCapture(hwnd); w32_mouse(w, lp, LUBE_GFX_MOUSE_DOWN, 1); return 0;
    case WM_RBUTTONDOWN: SetCapture(hwnd); w32_mouse(w, lp, LUBE_GFX_MOUSE_DOWN, 2); return 0;
    case WM_MBUTTONDOWN: SetCapture(hwnd); w32_mouse(w, lp, LUBE_GFX_MOUSE_DOWN, 3); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); w32_mouse(w, lp, LUBE_GFX_MOUSE_UP, 1); return 0;
    case WM_RBUTTONUP: ReleaseCapture(); w32_mouse(w, lp, LUBE_GFX_MOUSE_UP, 2); return 0;
    case WM_MBUTTONUP: ReleaseCapture(); w32_mouse(w, lp, LUBE_GFX_MOUSE_UP, 3); return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        e.kind = LUBE_GFX_WHEEL;
        if (msg == WM_MOUSEWHEEL) e.dy = GET_WHEEL_DELTA_WPARAM(wp) / 120.0;
        else e.dx = GET_WHEEL_DELTA_WPARAM(wp) / 120.0;
        push_event(w, &e);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        w32_blit(w, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static WCHAR *wide(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    WCHAR *s = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(n > 0 ? n : 1));
    if (!s) abort();
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, s, n)) s[0] = 0;
    return s;
}

static const char *w32_open(Win *w, const char *title, int scale) {
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        typedef BOOL(WINAPI * Aware)(void);  /* (sharp on a screen scaled up: Vista and later) */
        Aware aware = (Aware)(void (*)(void))GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware");
        if (aware) aware();
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = w32_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"LubeWindow";
        if (!RegisterClassExW(&wc)) return "cannot make a window (RegisterClassEx failed)";
        registered = 1;
    }
    W32 *s = (W32 *)calloc(1, sizeof(W32));
    if (!s) abort();
    s->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s->bmi.bmiHeader.biWidth = w->c.w;
    s->bmi.bmiHeader.biHeight = -w->c.h;  /* (top row first) */
    s->bmi.bmiHeader.biPlanes = 1;
    s->bmi.bmiHeader.biBitCount = 32;
    s->bmi.bmiHeader.biCompression = BI_RGB;
    RECT r = {0, 0, w->c.w * scale, w->c.h * scale};
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    WCHAR *t = wide(title);
    w->sys = s;  /* (before the messages CreateWindow sends) */
    s->hwnd = CreateWindowExW(0, L"LubeWindow", t, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                              NULL, NULL, inst, NULL);
    free(t);
    if (!s->hwnd) {
        free(s);
        w->sys = NULL;
        return "cannot make a window (CreateWindowEx failed)";
    }
    RECT client;
    GetClientRect(s->hwnd, &client);
    s->sw = client.right;
    s->sh = client.bottom;
    ShowWindow(s->hwnd, SW_SHOW);
    UpdateWindow(s->hwnd);
    return NULL;
}

static void w32_pump(Win *w) {
    MSG m;
    while (w->open && PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static void w32_present(Win *w) {
    w32_pump(w);  /* (a window that is shown but never asked for its events still answers) */
    if (!w->open) return;
    W32 *s = (W32 *)w->sys;
    HDC dc = GetDC(s->hwnd);
    w32_blit(w, dc);
    ReleaseDC(s->hwnd, dc);
}

static void w32_close(Win *w) {
    W32 *s = (W32 *)w->sys;
    w->sys = NULL;  /* (its messages from now on are not this window's) */
    DestroyWindow(s->hwnd);
    free(s);
}

static void w32_title(Win *w, const char *title) {
    WCHAR *t = wide(title);
    SetWindowTextW(((W32 *)w->sys)->hwnd, t);
    free(t);
}

static const Backend WIN32_BACKEND = {w32_open, w32_pump, w32_present, w32_close, w32_title};
