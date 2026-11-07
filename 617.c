/*
 * 617 — AI App Builder, native desktop build.
 * One C file. Windows (Win32 GDI) and Linux/macOS (Xlib). No interpreter, no runtime.
 * Build plans are fetched with the system `curl` binary (ships with Windows 10+, macOS, Linux).
 *
 * Windows (MinGW-w64):  gcc -O2 -Wall -Wextra -mwindows -o 617.exe 617.c -lgdi32 -luser32
 * Linux:                cc -O2 -Wall -Wextra -o 617 617.c -lX11 -lpthread
 * macOS (XQuartz):      cc -O2 -Wall -Wextra -o 617 617.c -I/opt/X11/include -L/opt/X11/lib -lX11 -lpthread
 *
 * Run with ANTHROPIC_API_KEY set to enable build plans (optional).
 * Data: %USERPROFILE%\.617\ideas.dat on Windows, ~/.617/ideas.dat elsewhere.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define popen _popen
#define pclose _pclose
#define unlink _unlink
#else
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#endif

#define WIN_W 900
#define WIN_H 640
#define MAX_IDEAS 500
#define MAX_JOBS 8
#define TITLE_MAX 120
#define DESC_MAX 2000
#define PLAN_MAX 8000
#define LINE_BUF 1024
#define LIST_ROWS 22
#define MODEL_NAME "claude-haiku-5-5"

/* ---------- platform font + drawing layer ---------- */

#ifdef _WIN32
typedef struct {
    int ascent, descent;
    HFONT hf;
} AppFont;
static HWND g_hwnd;
static HDC g_dc; /* valid only inside WM_PAINT */
#else
typedef XFontStruct AppFont;
static Display *dpy;
static Window win;
static GC gc;
static Atom wm_delete;
#endif

/* ---------- state ---------- */

typedef struct {
    char id[64];
    char title[TITLE_MAX + 1];
    char desc[DESC_MAX + 1];
    char plan[PLAN_MAX + 1];
    char err[300];
    char created[32];
    int pending;
} Idea;

typedef struct {
    atomic_int in_use;
    atomic_int done;
    char id[64];
    char title[TITLE_MAX + 1];
    char desc[DESC_MAX + 1];
    char plan[PLAN_MAX + 1];
    char err[300];
} Job;

static Idea ideas[MAX_IDEAS];
static int nideas = 0;
static int sel = -1;
static int list_off = 0;
static int armed_delete = 0;
static int focus = 0; /* 0 = title, 1 = description */
static int dirty = 1;
static int have_key = 0;
static char data_dir[512], data_file[600];
static char title_buf[TITLE_MAX + 1];
static char desc_buf[DESC_MAX + 1];
static char status[200] = "";
static int status_err = 0;
static Job jobs[MAX_JOBS];

static AppFont font_obj, big_obj;
static AppFont *font, *big;
static int font_h;
static unsigned long c_bg, c_panel, c_field, c_border, c_fg, c_muted,
    c_err, c_sel, c_btn, c_btn_text;

/* ---------- small helpers ---------- */

static void needs_paint(void) {
#ifdef _WIN32
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
#else
    dirty = 1;
#endif
}

/* Reads one line of any length (portable replacement for getline). */
static int read_line(FILE *f, char **buf, size_t *cap) {
    size_t len = 0;
    if (!*buf) {
        *cap = 512;
        *buf = malloc(*cap);
        if (!*buf) return 0;
    }
    for (;;) {
        if (!fgets(*buf + len, (int)(*cap - len), f)) break;
        len += strlen(*buf + len);
        if (len > 0 && (*buf)[len - 1] == '\n') return 1;
        if (*cap - len <= 1) {
            *cap *= 2;
            char *nb = realloc(*buf, *cap);
            if (!nb) return 0;
            *buf = nb;
        }
    }
    return len > 0;
}

static void local_time(time_t t, struct tm *out) {
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

static void make_dir(const char *p) {
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0700);
#endif
}

static unsigned long current_pid(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

/* Escape \ and newlines so each record field stays on one line. */
static size_t esc_into(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    for (; *src && n + 2 < cap; src++) {
        if (*src == '\\') { dst[n++] = '\\'; dst[n++] = '\\'; }
        else if (*src == '\n') { dst[n++] = '\\'; dst[n++] = 'n'; }
        else if (*src == '\r') continue;
        else dst[n++] = *src;
    }
    dst[n] = 0;
    return n;
}

static void unesc_into(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    for (; *src && n + 1 < cap; src++) {
        if (*src == '\\' && src[1]) {
            src++;
            dst[n++] = (*src == 'n') ? '\n' : *src;
        } else {
            dst[n++] = *src;
        }
    }
    dst[n] = 0;
}

/* JSON-encode s into d. d must hold 6*strlen(s)+1 bytes. */
static size_t json_escape_into(const char *s, char *d) {
    size_t n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  d[n++] = '\\'; d[n++] = '"'; break;
        case '\\': d[n++] = '\\'; d[n++] = '\\'; break;
        case '\n': d[n++] = '\\'; d[n++] = 'n'; break;
        case '\r': d[n++] = '\\'; d[n++] = 'r'; break;
        case '\t': d[n++] = '\\'; d[n++] = 't'; break;
        default:
            if (c < 0x20) n += (size_t)sprintf(d + n, "\\u%04x", c);
            else d[n++] = (char)c;
        }
    }
    d[n] = 0;
    return n;
}

/* Find "key" : "value" in JSON text; return pointer just past the opening quote. */
static const char *find_string_value(const char *s, const char *key) {
    size_t kl = strlen(key);
    while ((s = strstr(s, key)) != NULL) {
        const char *p = s + kl;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == ':') {
            p++;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p == '"') return p + 1;
        }
        s += kl;
    }
    return NULL;
}

/* Decode a JSON string body starting at s into out. Returns pointer to closing quote. */
static const char *json_read_string(const char *s, char *out, size_t cap) {
    size_t n = 0;
    while (*s && *s != '"') {
        char c;
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = 0; break;
            case 'u':
                c = '?';
                for (int k = 0; k < 4 && s[1]; k++) s++;
                break;
            default: c = *s;
            }
        } else {
            c = *s;
        }
        if (c && n + 1 < cap) out[n++] = c;
        s++;
    }
    out[n] = 0;
    return s;
}

/* ---------- storage ---------- */

static void set_status(const char *msg, int is_err) {
    snprintf(status, sizeof status, "%s", msg);
    status_err = is_err;
}

static void save_all(void) {
    static char esc[PLAN_MAX * 2 + 64];
    char tmp[700];
    make_dir(data_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", data_file);
    FILE *f = fopen(tmp, "w");
    if (!f) { set_status("Could not write ideas file.", 1); return; }
    for (int i = 0; i < nideas; i++) {
        Idea *d = &ideas[i];
        fprintf(f, "id=%s\n", d->id);
        esc_into(d->title, esc, sizeof esc);   fprintf(f, "title=%s\n", esc);
        esc_into(d->desc, esc, sizeof esc);    fprintf(f, "desc=%s\n", esc);
        esc_into(d->plan, esc, sizeof esc);    fprintf(f, "plan=%s\n", esc);
        esc_into(d->err, esc, sizeof esc);     fprintf(f, "err=%s\n", esc);
        esc_into(d->created, esc, sizeof esc); fprintf(f, "created=%s\n", esc);
        fputs("---\n", f);
    }
    int ok = (fclose(f) == 0);
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, data_file, MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && (rename(tmp, data_file) == 0);
#endif
    if (!ok) set_status("Could not save ideas file.", 1);
}

static void load_all(void) {
    FILE *f = fopen(data_file, "r");
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    Idea *cur = NULL;

    while (read_line(f, &line, &cap)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = 0;
        if (len > 0 && line[len - 1] == '\r') line[--len] = 0;
        if (strcmp(line, "---") == 0) {
            if (cur) { nideas++; cur = NULL; }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq || nideas >= MAX_IDEAS) continue;
        if (!cur) {
            cur = &ideas[nideas];
            memset(cur, 0, sizeof *cur);
        }
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(line, "id"))           snprintf(cur->id, sizeof cur->id, "%s", v);
        else if (!strcmp(line, "title"))   unesc_into(v, cur->title, sizeof cur->title);
        else if (!strcmp(line, "desc"))    unesc_into(v, cur->desc, sizeof cur->desc);
        else if (!strcmp(line, "plan"))    unesc_into(v, cur->plan, sizeof cur->plan);
        else if (!strcmp(line, "err"))     unesc_into(v, cur->err, sizeof cur->err);
        else if (!strcmp(line, "created")) unesc_into(v, cur->created, sizeof cur->created);
    }
    if (cur) nideas++;
    free(line);
    fclose(f);

    for (int i = 0; i < nideas; i++) {
        if (!ideas[i].plan[0] && !ideas[i].err[0])
            snprintf(ideas[i].err, sizeof ideas[i].err,
                     "No plan (app closed before it finished).");
    }
}

/* ---------- plan jobs (worker threads) ---------- */

static void plan_thread(int slot) {
    Job *j = &jobs[slot];
    char req_path[700];
    char cmd[1600];
    char *prompt = NULL, *body = NULL, *resp = NULL;
    size_t plen, blen, n, cap = 65536, len = 0;
    FILE *f, *p;

    snprintf(req_path, sizeof req_path, "%s/.req-%d.json", data_dir, slot);

    plen = strlen(j->title) + strlen(j->desc) + 512;
    prompt = malloc(plen);
    if (!prompt) goto done;
    snprintf(prompt, plen,
             "Turn this app idea into a short build plan: a one-line pitch, "
             "the core features as a numbered list, and a suggested tech stack."
             "\n\nTitle: %s\nDescription: %s",
             j->title, j->desc);

    blen = strlen(prompt) * 6 + 512;
    body = malloc(blen);
    if (!body) goto done;
    n = (size_t)snprintf(body, blen,
        "{\"model\":\"%s\",\"max_tokens\":800,\"messages\":[{\"role\":\"user\",\"content\":\"",
        MODEL_NAME);
    n += json_escape_into(prompt, body + n);
    snprintf(body + n, blen - n, "\"}]}");

    f = fopen(req_path, "w");
    if (!f) {
        snprintf(j->err, sizeof j->err, "Could not write request file.");
        goto done;
    }
    fputs(body, f);
    fclose(f);

    /*
     * The key is passed through stdin as a header line, so it never appears
     * on the command line. Windows uses cmd.exe's (echo ...) grouping for this.
     */
#ifdef _WIN32
    snprintf(cmd, sizeof cmd,
        "(echo x-api-key: %%ANTHROPIC_API_KEY%%)| "
        "curl -sS --max-time 60 https://api.anthropic.com/v1/messages "
        "-H @- -H \"anthropic-version: 2023-06-01\" -H \"content-type: application/json\" "
        "--data-binary @\"%s\" 2>NUL",
        req_path);
#else
    snprintf(cmd, sizeof cmd,
        "printf 'x-api-key: %%s\\n' \"$ANTHROPIC_API_KEY\" | "
        "curl -sS --max-time 60 https://api.anthropic.com/v1/messages "
        "-H @- -H 'anthropic-version: 2023-06-01' -H 'content-type: application/json' "
        "--data-binary @'%s' 2>/dev/null",
        req_path);
#endif

    resp = malloc(cap);
    if (!resp) { unlink(req_path); goto done; }
    resp[0] = 0;
    p = popen(cmd, "r");
    if (p) {
        for (;;) {
            if (len + 1 >= cap) {
                cap *= 2;
                char *nr = realloc(resp, cap);
                if (!nr) break;
                resp = nr;
            }
            size_t r = fread(resp + len, 1, cap - len - 1, p);
            if (r == 0) break;
            len += r;
        }
        resp[len] = 0;
        pclose(p);
    }
    unlink(req_path);

    if (len == 0) {
        snprintf(j->err, sizeof j->err,
                 "Plan request failed: no response. Check your network and that curl is installed.");
    } else if (!strstr(resp, "\"content\"")) {
        const char *m = find_string_value(resp, "\"message\"");
        if (m) json_read_string(m, j->err, sizeof j->err);
        if (!j->err[0]) snprintf(j->err, sizeof j->err, "Plan request failed.");
    } else {
        const char *cur = resp;
        for (;;) {
            const char *v = find_string_value(cur, "\"text\"");
            if (!v) break;
            char piece[PLAN_MAX + 1];
            cur = json_read_string(v, piece, sizeof piece);
            size_t have = strlen(j->plan);
            size_t room = sizeof j->plan - have - 1;
            size_t add = strlen(piece);
            if (add > room) add = room;
            memcpy(j->plan + have, piece, add);
            j->plan[have + add] = 0;
            if (!*cur) break;
        }
        if (!j->plan[0]) snprintf(j->err, sizeof j->err, "Empty plan returned.");
    }

done:
    free(prompt);
    free(body);
    free(resp);
    atomic_store(&j->done, 1);
}

#ifdef _WIN32
static DWORD WINAPI plan_thread_entry(LPVOID arg) {
    plan_thread((int)(intptr_t)arg);
    return 0;
}
#else
static void *plan_thread_entry(void *arg) {
    plan_thread((int)(intptr_t)arg);
    return NULL;
}
#endif

static int start_plan_job(const Idea *d) {
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (atomic_load(&j->in_use)) continue;
        snprintf(j->id, sizeof j->id, "%s", d->id);
        snprintf(j->title, sizeof j->title, "%s", d->title);
        snprintf(j->desc, sizeof j->desc, "%s", d->desc);
        j->plan[0] = 0;
        j->err[0] = 0;
        atomic_store(&j->done, 0);
        atomic_store(&j->in_use, 1);
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, plan_thread_entry, (LPVOID)(intptr_t)i, 0, NULL);
        if (!h) { atomic_store(&j->in_use, 0); return 0; }
        CloseHandle(h);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, plan_thread_entry, (void *)(intptr_t)i) != 0) {
            atomic_store(&j->in_use, 0);
            return 0;
        }
        pthread_detach(t);
#endif
        return 1;
    }
    return 0;
}

/* Runs on the UI thread only. */
static void poll_jobs(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (!atomic_load(&j->in_use) || !atomic_load(&j->done)) continue;

        for (int k = 0; k < nideas; k++) {
            if (strcmp(ideas[k].id, j->id) == 0) {
                snprintf(ideas[k].plan, sizeof ideas[k].plan, "%s", j->plan);
                snprintf(ideas[k].err, sizeof ideas[k].err, "%s", j->err);
                ideas[k].pending = 0;
                break;
            }
        }
        save_all();
        set_status(j->plan[0] ? "Plan ready." : "Plan failed. See details below.",
                   j->plan[0] == 0);
        atomic_store(&j->done, 0);
        atomic_store(&j->in_use, 0);
        needs_paint();
    }
}

/* ---------- drawing primitives (per platform) ---------- */

static unsigned long color_of(const char *hex) {
#ifdef _WIN32
    unsigned int r = 0, g = 0, b = 0;
    if (hex[0] == '#') sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
    return RGB(r, g, b);
#else
    XColor c;
    Colormap cm = DefaultColormap(dpy, DefaultScreen(dpy));
    if (!XParseColor(dpy, cm, hex, &c) || !XAllocColor(dpy, cm, &c))
        return WhitePixel(dpy, DefaultScreen(dpy));
    return c.pixel;
#endif
}

static void prim_fill(int x, int y, int w, int h, unsigned long col) {
#ifdef _WIN32
    RECT r = { x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush((COLORREF)col);
    FillRect(g_dc, &r, b);
    DeleteObject(b);
#else
    XSetForeground(dpy, gc, col);
    XFillRectangle(dpy, win, gc, x, y, (unsigned)w, (unsigned)h);
#endif
}

static void prim_frame(int x, int y, int w, int h, unsigned long col) {
#ifdef _WIN32
    RECT r = { x, y, x + w + 1, y + h + 1 };
    HBRUSH b = CreateSolidBrush((COLORREF)col);
    FrameRect(g_dc, &r, b);
    DeleteObject(b);
#else
    XSetForeground(dpy, gc, col);
    XDrawRectangle(dpy, win, gc, x, y, (unsigned)w, (unsigned)h);
#endif
}

/* `top` is the top of the text line on both platforms. */
static void prim_text(AppFont *f, int x, int top, const char *s, unsigned long col) {
#ifdef _WIN32
    SelectObject(g_dc, f->hf);
    SetBkMode(g_dc, TRANSPARENT);
    SetTextColor(g_dc, (COLORREF)col);
    TextOutA(g_dc, x, top, s, (int)strlen(s));
#else
    XSetFont(dpy, gc, f->fid);
    XSetForeground(dpy, gc, col);
    XDrawString(dpy, win, gc, x, top + f->ascent, s, (int)strlen(s));
#endif
}

static int prim_text_w(AppFont *f, const char *s) {
#ifdef _WIN32
    SIZE sz = { 0, 0 };
    HGDIOBJ old = SelectObject(g_dc, f->hf);
    GetTextExtentPoint32A(g_dc, s, (int)strlen(s), &sz);
    SelectObject(g_dc, old);
    return (int)sz.cx;
#else
    return XTextWidth(f, s, (int)strlen(s));
#endif
}

static AppFont *load_font(AppFont *out, int win_height, int win_weight,
                       const char *win_face, const char *x_name, const char *x_fallback) {
#ifdef _WIN32
    (void)x_name; (void)x_fallback;
    out->hf = CreateFontA(win_height, 0, 0, 0, win_weight, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, win_face);
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, out->hf);
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    out->ascent = tm.tmAscent;
    out->descent = tm.tmDescent;
    (void)win_face;
    return out;
#else
    (void)win_height; (void)win_weight; (void)win_face;
    (void)out;
    XFontStruct *x = XLoadQueryFont(dpy, x_name);
    if (!x && x_fallback) x = XLoadQueryFont(dpy, x_fallback);
    return x;
#endif
}

/* ---------- text layout helpers (shared) ---------- */

static void text_at(AppFont *f, int x, int top, const char *s, unsigned long col) {
    prim_text(f, x, top, s, col);
}

static int text_w(AppFont *f, const char *s) {
    return prim_text_w(f, s);
}

static void fill(int x, int y, int w, int h, unsigned long col) { prim_fill(x, y, w, h, col); }
static void frame(int x, int y, int w, int h, unsigned long col) { prim_frame(x, y, w, h, col); }

static void fit_text(AppFont *f, const char *src, int maxw, char *out, size_t cap) {
    snprintf(out, cap, "%s", src);
    if (text_w(f, out) <= maxw) return;
    size_t l = strlen(out);
    while (l > 0 && text_w(f, out) + text_w(f, "...") > maxw) out[--l] = 0;
    strncat(out, "...", cap - strlen(out) - 1);
}

/* Shows the end of s so the caret stays visible. */
static void draw_tail(int x, int top, int maxw, const char *s, unsigned long col) {
    while (*s && text_w(font, s) > maxw) s++;
    text_at(font, x, top, s, col);
}

/* Word-wrapped text inside [x, x+width]; stops at `bottom`. Returns next line top. */
static int draw_wrapped(int x, int top, int width, int bottom, const char *s, unsigned long col) {
    int y = top;
    const char *p = s;
    char cur[LINE_BUF], trial[LINE_BUF], word[256];
    do {
        const char *pe = strchr(p, '\n');
        if (!pe) pe = p + strlen(p);
        cur[0] = 0;
        const char *w = p;
        while (w < pe) {
            while (w < pe && *w == ' ') w++;
            if (w >= pe) break;
            const char *we = w;
            while (we < pe && *we != ' ') we++;
            size_t wl = (size_t)(we - w);
            if (wl > sizeof word - 1) wl = sizeof word - 1;
            memcpy(word, w, wl);
            word[wl] = 0;
            if (cur[0]) snprintf(trial, sizeof trial, "%s %s", cur, word);
            else snprintf(trial, sizeof trial, "%s", word);
            if (cur[0] && text_w(font, trial) > width) {
                if (y + font_h <= bottom) text_at(font, x, y, cur, col);
                y += font_h;
                snprintf(cur, sizeof cur, "%s", word);
            } else {
                snprintf(cur, sizeof cur, "%s", trial);
            }
            w = we;
        }
        if (cur[0] && y + font_h <= bottom) text_at(font, x, y, cur, col);
        y += font_h;
        p = (*pe) ? pe + 1 : pe;
    } while (*p && y < bottom);
    return y;
}

static void clamp_scroll(void) {
    int max = nideas - LIST_ROWS;
    if (max < 0) max = 0;
    if (list_off > max) list_off = max;
    if (list_off < 0) list_off = 0;
}

static void render(void) {
    clamp_scroll();
    fill(0, 0, WIN_W, WIN_H, c_bg);

    /* header */
    text_at(big, 24, 18, "617", c_fg);
    const char *tag = "AI APP BUILDER";
    text_at(font, WIN_W - 24 - text_w(font, tag), 26, tag, c_muted);

    /* left: idea list */
    fill(16, 64, 248, 560, c_panel);
    frame(16, 64, 248, 560, c_border);
    text_at(font, 28, 80, "Ideas", c_fg);
    for (int i = 0; i < LIST_ROWS; i++) {
        int d = list_off + i;
        if (d >= nideas) break;
        int idx = nideas - 1 - d;
        int ry = 104 + i * 24;
        if (idx == sel) fill(24, ry, 232, 22, c_sel);
        char shown[160];
        fit_text(font, ideas[idx].title, 216, shown, sizeof shown);
        text_at(font, 32, ry + 4, shown, c_fg);
    }
    if (nideas == 0) text_at(font, 28, 104, "No ideas yet.", c_muted);

    /* right: composer */
    fill(280, 64, 604, 560, c_panel);
    frame(280, 64, 604, 560, c_border);
    text_at(font, 296, 80, "New idea", c_fg);

    fill(296, 108, 572, 28, c_field);
    frame(296, 108, 572, 28, focus == 0 ? c_fg : c_border);
    if (!title_buf[0] && focus != 0) {
        text_at(font, 304, 116, "App name", c_muted);
    } else {
        char tb[TITLE_MAX + 4];
        snprintf(tb, sizeof tb, "%s%s", title_buf, focus == 0 ? "|" : "");
        draw_tail(304, 116, 556, tb, c_fg);
    }

    fill(296, 144, 572, 132, c_field);
    frame(296, 144, 572, 132, focus == 1 ? c_fg : c_border);
    if (!desc_buf[0] && focus != 1) {
        text_at(font, 304, 152, "What should it do? Who is it for?", c_muted);
    } else {
        static char dv[DESC_MAX + 4];
        snprintf(dv, sizeof dv, "%s%s", desc_buf, focus == 1 ? "|" : "");
        draw_wrapped(304, 152, 556, 272, dv, c_fg);
    }

    fill(296, 286, 100, 30, c_btn);
    text_at(font, 326, 294, "Save", c_btn_text);
    if (status[0]) text_at(font, 410, 294, status, status_err ? c_err : c_muted);

    int can_delete = sel >= 0 && !ideas[sel].pending;
    frame(748, 286, 120, 30, can_delete ? c_fg : c_border);
    text_at(font, 760, 294, armed_delete ? "Confirm delete" : "Delete",
            can_delete ? c_fg : c_muted);

    /* detail */
    fill(296, 332, 572, 284, c_field);
    frame(296, 332, 572, 284, c_border);
    if (sel < 0) {
        text_at(font, 308, 344, "Select an idea, or write a new one above.", c_muted);
    } else {
        Idea *d = &ideas[sel];
        char head[160];
        fit_text(big, d->title, 548, head, sizeof head);
        text_at(big, 308, 342, head, c_fg);
        text_at(font, 308, 370, d->created, c_muted);
        int y = draw_wrapped(308, 394, 548, 606, d->desc, c_fg) + 8;
        if (y + font_h <= 606) text_at(font, 308, y, "Build plan", c_muted);
        y += font_h + 4;
        if (d->pending)      text_at(font, 308, y, "Building plan...", c_muted);
        else if (d->plan[0]) draw_wrapped(308, y, 548, 606, d->plan, c_fg);
        else if (d->err[0])  draw_wrapped(308, y, 548, 606, d->err, c_err);
        else                 text_at(font, 308, y, "No plan.", c_muted);
    }
}

/* ---------- input (shared logic) ---------- */

static void append_raw(char *buf, size_t cap, char c) {
    size_t len = strlen(buf);
    if (len + 1 < cap) { buf[len] = c; buf[len + 1] = 0; }
}

static void append_bytes(char *buf, size_t cap, const char *s, size_t n) {
    for (size_t k = 0; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c < 32 || c == 127) continue;
        append_raw(buf, cap, (char)c);
    }
}

static void backspace(char *s) {
    size_t l = strlen(s);
    if (!l) return;
    l--;
    while (l > 0 && ((unsigned char)s[l] & 0xC0) == 0x80) l--;
    s[l] = 0;
}

static void on_tab(void) { focus = 1 - focus; }

static void on_return(void) {
    if (focus == 0) focus = 1;
    else append_raw(desc_buf, sizeof desc_buf, '\n');
}

static void on_backspace(void) { backspace(focus == 0 ? title_buf : desc_buf); }

static void on_text(const char *s, size_t n) {
    if (focus == 0) append_bytes(title_buf, sizeof title_buf, s, n);
    else            append_bytes(desc_buf, sizeof desc_buf, s, n);
}

static int has_text(const char *s) {
    for (; *s; s++)
        if (!isspace((unsigned char)*s)) return 1;
    return 0;
}

static void do_save(void) {
    if (!has_text(title_buf) || !has_text(desc_buf)) {
        set_status("Both fields are required.", 1);
        return;
    }
    if (nideas >= MAX_IDEAS) {
        set_status("Idea limit reached (500).", 1);
        return;
    }

    static unsigned long seq = 0;
    seq++;
    Idea *d = &ideas[nideas];
    memset(d, 0, sizeof *d);
    snprintf(d->id, sizeof d->id, "%lx-%lx-%lx",
             (unsigned long)time(NULL), current_pid(), seq);
    snprintf(d->title, sizeof d->title, "%s", title_buf);
    snprintf(d->desc, sizeof d->desc, "%s", desc_buf);

    struct tm tmv;
    local_time(time(NULL), &tmv);
    strftime(d->created, sizeof d->created, "%Y-%m-%d %H:%M", &tmv);

    nideas++;
    sel = nideas - 1;
    armed_delete = 0;
    list_off = 0;

    if (have_key) {
        d->pending = 1;
        if (!start_plan_job(d)) {
            d->pending = 0;
            snprintf(d->err, sizeof d->err, "Too many plans running. Saved without a plan.");
        }
    } else {
        snprintf(d->err, sizeof d->err, "No ANTHROPIC_API_KEY set. Saved without a plan.");
    }

    save_all();
    title_buf[0] = 0;
    desc_buf[0] = 0;
    focus = 0;
    set_status(d->pending ? "Saved. Building plan..." : "Saved.", 0);
}

static void do_delete(void) {
    if (sel < 0) return;
    if (ideas[sel].pending) {
        set_status("Wait for the plan to finish first.", 1);
        return;
    }
    if (!armed_delete) {
        armed_delete = 1;
        set_status("Click Delete again to confirm.", 1);
        return;
    }
    memmove(&ideas[sel], &ideas[sel + 1], (size_t)(nideas - sel - 1) * sizeof(Idea));
    nideas--;
    sel = -1;
    armed_delete = 0;
    save_all();
    set_status("Deleted.", 0);
}

static void handle_click(int x, int y) {
    if (x >= 24 && x < 256 && y >= 104 && y < 104 + LIST_ROWS * 24) {
        int d = list_off + (y - 104) / 24;
        if (d < nideas) {
            sel = nideas - 1 - d;
            armed_delete = 0;
        }
        return;
    }
    if (x >= 296 && x < 868 && y >= 108 && y < 136) { focus = 0; return; }
    if (x >= 296 && x < 868 && y >= 144 && y < 276) { focus = 1; return; }
    if (x >= 296 && x < 396 && y >= 286 && y < 316) { do_save(); return; }
    if (x >= 748 && x < 868 && y >= 286 && y < 316) { do_delete(); return; }
}

static void scroll_list(int up) {
    list_off += up ? -3 : 3;
    clamp_scroll();
}

/* ---------- Windows: window + message loop ---------- */

#ifdef _WIN32

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        g_dc = BeginPaint(hwnd, &ps);
        render();
        EndPaint(hwnd, &ps);
        g_dc = NULL;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        handle_click((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
        needs_paint();
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        if (pt.x < 260) scroll_list(GET_WHEEL_DELTA_WPARAM(wp) > 0);
        needs_paint();
        return 0;
    }
    case WM_CHAR: {
        unsigned int c = (unsigned int)wp;
        char utf8[4];
        size_t n = 0;
        if (c == '\t') { on_tab(); }
        else if (c == '\r') { on_return(); }
        else if (c == '\b') { on_backspace(); }
        else if (c >= 32) {
            if (c < 0x80) { utf8[n++] = (char)c; }
            else if (c < 0x800) { utf8[n++] = (char)(0xC0 | (c >> 6)); utf8[n++] = (char)(0x80 | (c & 0x3F)); }
            else { utf8[n++] = (char)(0xE0 | (c >> 12)); utf8[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); utf8[n++] = (char)(0x80 | (c & 0x3F)); }
            on_text(utf8, n);
        }
        needs_paint();
        return 0;
    }
    case WM_TIMER:
        poll_jobs();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int run_windows(HINSTANCE hinst) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = "Baldwin617App";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    if (!RegisterClassA(&wc)) return 1;

    RECT r = { 0, 0, WIN_W, WIN_H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, FALSE);
    g_hwnd = CreateWindowExA(0, "Baldwin617App", "617 - AI App Builder",
                             WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             NULL, NULL, hinst, NULL);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, SW_SHOW);
    SetTimer(g_hwnd, 1, 50, NULL);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return (int)m.wParam;
}

#endif

/* ---------- main ---------- */

int main(void) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
#else
    const char *home = getenv("HOME");
#endif
    if (!home) home = ".";
    snprintf(data_dir, sizeof data_dir, "%s/.617", home);
    snprintf(data_file, sizeof data_file, "%s/ideas.dat", data_dir);

    const char *k = getenv("ANTHROPIC_API_KEY");
    if (k && *k) have_key = 1;
    load_all();

#ifdef _WIN32
    c_bg = color_of("#080808");
    c_panel = color_of("#111111");
    c_field = color_of("#0c0c0c");
    c_border = color_of("#303030");
    c_fg = color_of("#f5f5f5");
    c_muted = color_of("#999999");
    c_err = color_of("#ff8a8a");
    c_sel = color_of("#333333");
    c_btn = color_of("#ffffff");
    c_btn_text = c_bg;

    font = load_font(&font_obj, -16, FW_NORMAL, "Segoe UI", NULL, NULL);
    big = load_font(&big_obj, -22, FW_BOLD, "Segoe UI", NULL, NULL);
    font_h = font->ascent + font->descent;

    if (!have_key) set_status("No ANTHROPIC_API_KEY: ideas save without plans.", 0);
    return run_windows(GetModuleHandleA(NULL));
#else
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "617: cannot open a display. Run it from a desktop session.\n");
        return 1;
    }

    c_bg = color_of("#080808");
    c_panel = color_of("#111111");
    c_field = color_of("#0c0c0c");
    c_border = color_of("#303030");
    c_fg = color_of("#f5f5f5");
    c_muted = color_of("#999999");
    c_err = color_of("#ff8a8a");
    c_sel = color_of("#333333");
    c_btn = color_of("#ffffff");
    c_btn_text = c_bg;

    int scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 100, 80,
                              WIN_W, WIN_H, 0, c_border, c_bg);
    XStoreName(dpy, win, "617 - AI App Builder");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask |
                           StructureNotifyMask);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XMapWindow(dpy, win);
    gc = XCreateGC(dpy, win, 0, NULL);

    font = load_font(&font_obj, 0, 0, NULL,
                     "-*-helvetica-medium-r-normal--14-*-iso8859-1", "fixed");
    big = load_font(&big_obj, 0, 0, NULL,
                    "-*-helvetica-bold-r-normal--20-*-iso8859-1", NULL);
    if (!big) big = font;
    if (!font) {
        fprintf(stderr, "617: no usable X font found.\n");
        return 1;
    }
    font_h = font->ascent + font->descent;

    if (!have_key) set_status("No ANTHROPIC_API_KEY: ideas save without plans.", 0);

    int running = 1;
    while (running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
            case ConfigureNotify:
                dirty = 1;
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete) running = 0;
                break;
            case KeyPress: {
                char buf[32];
                KeySym ks;
                int n = XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
                if (n < 0) n = 0;
                if (ks == XK_Tab) on_tab();
                else if (ks == XK_Return || ks == XK_KP_Enter) on_return();
                else if (ks == XK_BackSpace) on_backspace();
                else if (n > 0) on_text(buf, (size_t)n);
                dirty = 1;
                break;
            }
            case ButtonPress:
                if (ev.xbutton.button == 4 || ev.xbutton.button == 5) {
                    if (ev.xbutton.x < 260) scroll_list(ev.xbutton.button == 4);
                } else if (ev.xbutton.button == 1) {
                    handle_click(ev.xbutton.x, ev.xbutton.y);
                }
                dirty = 1;
                break;
            default:
                break;
            }
        }
        poll_jobs();
        if (dirty) {
            render();
            XFlush(dpy);
            dirty = 0;
        }
        usleep(16000);
    }

    if (big != font) XFreeFont(dpy, big);
    XFreeFont(dpy, font);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
#endif
}
