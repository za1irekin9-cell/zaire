// 617 — AI App Builder, Windows C++ build.
// Win32 controls + WinHTTP (built into Windows). No runtime, no curl, no installer.
//
// Build with MinGW-w64 (MSYS2 MINGW64 shell):
//   g++ -std=c++17 -O2 -Wall -Wextra -municode -mwindows -o 617.exe 617.cpp -lwinhttp -lgdi32 -luser32 -lkernel32
//
// Build with MSVC (Developer Command Prompt):
//   cl /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE 617.cpp /link winhttp.lib user32.lib gdi32.lib /SUBSYSTEM:WINDOWS
//
// Set ANTHROPIC_API_KEY to enable build plans (optional).
// Data: %USERPROFILE%\.617\ideas.dat (same format as the C build).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

namespace {

constexpr int kWinW = 900;
constexpr int kWinH = 640;
constexpr size_t kTitleMax = 120;
constexpr size_t kDescMax = 2000;
const std::string kModel = "claude-haiku-5-5";
const wchar_t kClassName[] = L"Baldwin617App";
constexpr UINT WM_APP_PLAN = WM_APP + 1;

enum ControlId {
    IDC_TITLE = 101,
    IDC_DESC,
    IDC_SAVE,
    IDC_DELETE,
    IDC_LIST,
    IDC_DETAIL,
    IDC_STATUS,
};

// ---------- data ----------

struct Idea {
    std::string id;
    std::string title;
    std::string desc;
    std::string plan;
    std::string err;
    std::string created;
    bool pending = false;
};

struct PlanResult {
    std::string id;
    std::string plan;
    std::string err;
};

std::vector<Idea> g_ideas;
int g_sel = -1;                 // index into g_ideas, -1 = none
std::string g_api_key;
std::wstring g_data_dir;
std::wstring g_data_file;

HWND g_hwnd, g_title, g_desc, g_list, g_detail, g_status, g_save, g_delete, g_tag;
HFONT g_font, g_big_font;
HBRUSH g_bg_brush, g_field_brush;
bool g_status_err = false;

const COLORREF c_bg = RGB(8, 8, 8);
const COLORREF c_field = RGB(12, 12, 12);
const COLORREF c_fg = RGB(245, 245, 245);
const COLORREF c_muted = RGB(153, 153, 153);
const COLORREF c_err = RGB(255, 138, 138);

// ---------- text helpers ----------

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Edit controls want CRLF line breaks.
std::wstring to_display(const std::string& s) {
    std::wstring out;
    for (wchar_t c : to_wide(s)) {
        if (c == L'\n') out += L"\r\n";
        else if (c != L'\r') out += c;
    }
    return out;
}

std::string read_control(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (n > 0) GetWindowTextW(h, w.data(), n + 1);
    std::wstring clean;
    for (wchar_t c : w)
        if (c != L'\r') clean += c;
    return to_utf8(clean);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string make_id() {
    static std::atomic<unsigned> seq{0};
    char buf[96];
    std::snprintf(buf, sizeof buf, "%llx-%lx-%x",
                  static_cast<unsigned long long>(std::time(nullptr)),
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  ++seq);
    return buf;
}

std::string now_string() {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    char buf[32] = {0};
    if (tm) std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", tm);
    return buf;
}

// ---------- storage ----------

std::string escape_field(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c != '\r') out += c;
    }
    return out;
}

std::string unescape_field(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += (s[i] == 'n') ? '\n' : s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

bool save_ideas() {
    CreateDirectoryW(g_data_dir.c_str(), nullptr);
    std::wstring tmp = g_data_file + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    for (const Idea& it : g_ideas) {
        std::string rec = "id=" + it.id + "\n" +
                          "title=" + escape_field(it.title) + "\n" +
                          "desc=" + escape_field(it.desc) + "\n" +
                          "plan=" + escape_field(it.plan) + "\n" +
                          "err=" + escape_field(it.err) + "\n" +
                          "created=" + escape_field(it.created) + "\n" +
                          "---\n";
        fwrite(rec.data(), 1, rec.size(), f);
    }
    bool ok = fclose(f) == 0;
    return ok && MoveFileExW(tmp.c_str(), g_data_file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void load_ideas() {
    FILE* f = _wfopen(g_data_file.c_str(), L"rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);

    Idea cur;
    bool open = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "---") {
            if (open) g_ideas.push_back(cur);
            cur = Idea{};
            open = false;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        open = true;
        std::string key = line.substr(0, eq);
        std::string raw = line.substr(eq + 1);
        if (key == "id") cur.id = raw;
        else if (key == "title") cur.title = unescape_field(raw);
        else if (key == "desc") cur.desc = unescape_field(raw);
        else if (key == "plan") cur.plan = unescape_field(raw);
        else if (key == "err") cur.err = unescape_field(raw);
        else if (key == "created") cur.created = unescape_field(raw);
    }
    if (open) g_ideas.push_back(cur);

    for (Idea& it : g_ideas)
        if (it.plan.empty() && it.err.empty())
            it.err = "No plan (app closed before it finished).";
}

// ---------- JSON (only what the API call needs) ----------

std::string json_quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += static_cast<char>(c);
            }
        }
    }
    return o + "\"";
}

void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Decodes a JSON string whose opening quote was just before `pos`.
// On return `pos` points at the closing quote.
std::string decode_json_string(const std::string& s, size_t& pos) {
    std::string out;
    while (pos < s.size() && s[pos] != '"') {
        char c = s[pos];
        if (c == '\\' && pos + 1 < s.size()) {
            char e = s[++pos];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': break;
            case 'b': case 'f': break;
            case 'u': {
                unsigned cp = static_cast<unsigned>(std::stoul(s.substr(pos + 1, 4), nullptr, 16));
                pos += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && pos + 6 < s.size() &&
                    s[pos + 1] == '\\' && s[pos + 2] == 'u') {
                    unsigned lo = static_cast<unsigned>(std::stoul(s.substr(pos + 3, 4), nullptr, 16));
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    pos += 6;
                }
                append_utf8(out, cp);
                break;
            }
            default: out += e;
            }
        } else {
            out += c;
        }
        ++pos;
    }
    return out;
}

// Given the index just after a key's closing quote, returns the index just after
// the opening quote of its string value, or npos.
size_t value_start(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    if (i >= s.size() || s[i] != ':') return std::string::npos;
    ++i;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    if (i >= s.size() || s[i] != '"') return std::string::npos;
    return i + 1;
}

std::string first_string_for_key(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    while (p != std::string::npos) {
        size_t v = value_start(json, p + needle.size());
        if (v != std::string::npos) {
            size_t pos = v;
            return decode_json_string(json, pos);
        }
        p = json.find(needle, p + 1);
    }
    return {};
}

// Joins every "text" block in a successful Messages API response.
std::string extract_plan(const std::string& body) {
    std::string plan;
    const std::string needle = "\"text\"";
    size_t p = body.find(needle);
    while (p != std::string::npos) {
        size_t v = value_start(body, p + needle.size());
        if (v != std::string::npos) {
            size_t pos = v;
            plan += decode_json_string(body, pos);
            p = body.find(needle, pos);
        } else {
            p = body.find(needle, p + 1);
        }
    }
    return trim(plan);
}

// ---------- Anthropic call (WinHTTP) ----------

bool post_messages(const std::string& body, DWORD& status, std::string& resp, std::string& err) {
    HINTERNET session = WinHttpOpen(L"617/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        err = "Plan request failed: WinHTTP unavailable (" + std::to_string(GetLastError()) + ").";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000);

    HINTERNET conn = WinHttpConnect(session, L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", L"/v1/messages", nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE)
                         : nullptr;
    bool ok = false;
    if (req) {
        std::wstring headers = L"Content-Type: application/json\r\n"
                               L"anthropic-version: 2023-06-01\r\n"
                               L"x-api-key: " + to_wide(g_api_key) + L"\r\n";
        ok = WinHttpAddRequestHeaders(req, headers.c_str(), static_cast<DWORD>(-1),
                                      WINHTTP_ADDREQ_FLAG_ADD) &&
             WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                const_cast<char*>(body.data()),
                                static_cast<DWORD>(body.size()),
                                static_cast<DWORD>(body.size()), 0) &&
             WinHttpReceiveResponse(req, nullptr);
    }

    if (ok) {
        DWORD sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &read)) { ok = false; break; }
            resp.append(chunk.data(), read);
        }
    } else {
        err = "Plan request failed: could not reach Anthropic (" +
              std::to_string(GetLastError()) + "). Check your network.";
    }

    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}

// Runs on a worker thread. Never touches window controls directly.
void run_plan(HWND hwnd, std::string id, std::string title, std::string desc) {
    auto* result = new PlanResult{id, {}, {}};
    try {
        std::string prompt =
            "Turn this app idea into a short build plan: a one-line pitch, the core "
            "features as a numbered list, and a suggested tech stack.\n\n"
            "Title: " + title + "\nDescription: " + desc;
        std::string body = "{\"model\":" + json_quote(kModel) +
                           ",\"max_tokens\":800,\"messages\":[{\"role\":\"user\",\"content\":" +
                           json_quote(prompt) + "}]}";

        DWORD status = 0;
        std::string resp;
        if (!post_messages(body, status, resp, result->err)) {
            // result->err already set
        } else if (status != 200) {
            std::string msg = first_string_for_key(resp, "message");
            result->err = msg.empty()
                ? "Plan request failed (HTTP " + std::to_string(status) + ")."
                : msg;
        } else {
            result->plan = extract_plan(resp);
            if (result->plan.empty()) result->err = "Empty plan returned.";
        }
    } catch (...) {
        result->err = "Plan request failed unexpectedly.";
    }

    if (!PostMessageW(hwnd, WM_APP_PLAN, 0, reinterpret_cast<LPARAM>(result)))
        delete result;
}

// ---------- UI ----------

void set_status(const std::string& msg, bool is_err) {
    g_status_err = is_err;
    SetWindowTextW(g_status, to_wide(msg).c_str());
    InvalidateRect(g_status, nullptr, TRUE);
}

void show_detail() {
    std::string text;
    if (g_sel < 0 || g_sel >= static_cast<int>(g_ideas.size())) {
        text = "Select an idea, or write a new one above.";
    } else {
        const Idea& it = g_ideas[g_sel];
        text = it.title + "\n" + it.created + "\n\n" + it.desc + "\n\nBuild plan\n";
        if (it.pending) text += "Building plan...";
        else if (!it.plan.empty()) text += it.plan;
        else text += it.err;
    }
    SetWindowTextW(g_detail, to_display(text).c_str());
    EnableWindow(g_delete, g_sel >= 0 && !g_ideas[g_sel].pending);
}

void refresh_list() {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    const int n = static_cast<int>(g_ideas.size());
    for (int i = n - 1; i >= 0; --i)
        SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(to_wide(g_ideas[i].title).c_str()));
    if (g_sel >= 0 && g_sel < n)
        SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(n - 1 - g_sel), 0);
    else
        g_sel = -1;
    show_detail();
}

void on_list_select() {
    LRESULT d = SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (d == LB_ERR) return;
    g_sel = static_cast<int>(g_ideas.size()) - 1 - static_cast<int>(d);
    show_detail();
}

void on_save() {
    std::string title = trim(read_control(g_title));
    std::string desc = trim(read_control(g_desc));
    if (title.empty() || desc.empty()) {
        set_status("Both fields are required.", true);
        return;
    }
    if (title.size() > kTitleMax * 4 || desc.size() > kDescMax * 4) {
        set_status("Title max 120 characters, description max 2000.", true);
        return;
    }

    Idea it;
    it.id = make_id();
    it.title = title;
    it.desc = desc;
    it.created = now_string();

    const bool have_key = !g_api_key.empty();
    if (have_key) it.pending = true;
    else it.err = "No ANTHROPIC_API_KEY set. Saved without a plan.";

    g_ideas.push_back(it);
    g_sel = static_cast<int>(g_ideas.size()) - 1;

    if (!save_ideas()) {
        g_ideas.pop_back();
        g_sel = -1;
        set_status("Could not write ideas file.", true);
        refresh_list();
        return;
    }

    SetWindowTextW(g_title, L"");
    SetWindowTextW(g_desc, L"");
    if (have_key) {
        std::thread(run_plan, g_hwnd, it.id, it.title, it.desc).detach();
        set_status("Saved. Building plan...", false);
    } else {
        set_status("Saved.", false);
    }
    refresh_list();
}

void on_delete() {
    if (g_sel < 0 || g_sel >= static_cast<int>(g_ideas.size())) return;
    Idea& it = g_ideas[g_sel];
    if (it.pending) {
        set_status("Wait for the plan to finish first.", true);
        return;
    }
    std::wstring msg = L"Delete '" + to_wide(it.title) + L"'? This cannot be undone.";
    if (MessageBoxW(g_hwnd, msg.c_str(), L"Delete idea", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    g_ideas.erase(g_ideas.begin() + g_sel);
    g_sel = -1;
    if (!save_ideas()) set_status("Could not write ideas file.", true);
    else set_status("Deleted.", false);
    refresh_list();
}

void on_plan_ready(PlanResult* r) {
    std::unique_ptr<PlanResult> owner(r);
    for (Idea& it : g_ideas) {
        if (it.id == r->id) {
            it.pending = false;
            it.plan = r->plan;
            it.err = r->err;
            break;
        }
    }
    save_ideas();
    set_status(r->plan.empty() ? "Plan failed. See details below." : "Plan ready.",
               r->plan.empty());
    refresh_list();
}

HWND make_child(const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, g_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return c;
}

void create_controls() {
    const DWORD edit_base = WS_BORDER | WS_TABSTOP;
    const DWORD multi = edit_base | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;

    g_tag = make_child(L"STATIC", L"617", SS_LEFT, 24, 18, 120, 36, 0);
    SendMessageW(g_tag, WM_SETFONT, reinterpret_cast<WPARAM>(g_big_font), TRUE);
    make_child(L"STATIC", L"AI APP BUILDER", SS_RIGHT, 600, 26, 276, 20, 0);

    make_child(L"STATIC", L"Ideas", SS_LEFT, 28, 70, 200, 20, 0);
    g_list = make_child(L"LISTBOX", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL |
                        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 24, 96, 232, 528, IDC_LIST);

    make_child(L"STATIC", L"New idea", SS_LEFT, 296, 70, 200, 20, 0);
    g_title = make_child(L"EDIT", L"", edit_base | ES_AUTOHSCROLL, 296, 96, 572, 26, IDC_TITLE);
    SendMessageW(g_title, EM_SETLIMITTEXT, kTitleMax, 0);
    g_desc = make_child(L"EDIT", L"", multi, 296, 130, 572, 130, IDC_DESC);
    SendMessageW(g_desc, EM_SETLIMITTEXT, kDescMax, 0);

    g_save = make_child(L"BUTTON", L"Save idea", WS_TABSTOP | BS_PUSHBUTTON,
                        296, 270, 110, 30, IDC_SAVE);
    g_status = make_child(L"STATIC", L"", SS_LEFT, 418, 276, 320, 20, IDC_STATUS);
    g_delete = make_child(L"BUTTON", L"Delete", WS_TABSTOP | BS_PUSHBUTTON,
                          748, 270, 120, 30, IDC_DELETE);

    make_child(L"STATIC", L"Details and build plan", SS_LEFT, 296, 318, 300, 20, 0);
    g_detail = make_child(L"EDIT", L"", multi | ES_READONLY, 296, 342, 572, 230, IDC_DETAIL);
}

void create_resources() {
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_big_font = CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_bg_brush = CreateSolidBrush(c_bg);
    g_field_brush = CreateSolidBrush(c_field);
}

void destroy_resources() {
    if (g_font) DeleteObject(g_font);
    if (g_big_font) DeleteObject(g_big_font);
    if (g_bg_brush) DeleteObject(g_bg_brush);
    if (g_field_brush) DeleteObject(g_field_brush);
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        create_resources();
        create_controls();
        SendMessageW(g_title, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        SendMessageW(g_title, EM_SETLIMITTEXT, kTitleMax, 0);
        if (g_api_key.empty()) set_status("No ANTHROPIC_API_KEY: ideas save without plans.", false);
        refresh_list();
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id == IDC_LIST && code == LBN_SELCHANGE) on_list_select();
        else if (id == IDC_SAVE && code == BN_CLICKED) on_save();
        else if (id == IDC_DELETE && code == BN_CLICKED) on_delete();
        return 0;
    }

    case WM_APP_PLAN:
        on_plan_ready(reinterpret_cast<PlanResult*>(lp));
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        HWND c = reinterpret_cast<HWND>(lp);
        if (c == g_detail) {
            SetTextColor(hdc, c_fg);
            SetBkColor(hdc, c_field);
            return reinterpret_cast<LRESULT>(g_field_brush);
        }
        if (c == g_status) SetTextColor(hdc, g_status_err ? c_err : c_muted);
        else if (c == g_tag) SetTextColor(hdc, c_fg);
        else SetTextColor(hdc, c_muted);
        SetBkColor(hdc, c_bg);
        return reinterpret_cast<LRESULT>(g_bg_brush);
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetTextColor(hdc, c_fg);
        SetBkColor(hdc, c_field);
        return reinterpret_cast<LRESULT>(g_field_brush);
    }

    case WM_DESTROY:
        destroy_resources();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool get_env(const wchar_t* name, std::wstring& out) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return false;
    out.assign(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, out.data(), n);
    if (got == 0) return false;
    out.resize(got);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int nshow) {
    std::wstring home;
    if (!get_env(L"USERPROFILE", home)) home = L".";
    g_data_dir = home + L"\\.617";
    g_data_file = g_data_dir + L"\\ideas.dat";

    std::wstring key_w;
    if (get_env(L"ANTHROPIC_API_KEY", key_w)) g_api_key = trim(to_utf8(key_w));
    load_ideas();

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(c_bg);
    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"Could not register the window class.", L"617", MB_ICONERROR);
        return 1;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, kWinW, kWinH};
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, kClassName, L"617 - AI App Builder", style,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, hinst, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Could not create the window.", L"617", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, nshow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
