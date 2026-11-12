/*
 * main.c - Lilac Win32 Markdown viewer application.
 * Internal strings, paths, and document/layout structures are strictly UTF-8;
 * UTF-16 conversion is performed only at Win32 API boundaries.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include "util.h"
#include "doc.h"
#include "layout.h"
#include "export.h"
#include "../res/resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "msimg32.lib")

static const wchar_t g_className[] = L"LilacMainWindow";
static const char g_sampleMarkdown[] =
    "# Lilac - Markdown 뷰어\n\n"
    "이 페이지는 파일을 열기 전 기본으로 표시되는 페이지입니다. 위쪽 메뉴에서 Markdown 파일을 열어 주십시오.\n";

#define LONG_PATH_MAX 32768

typedef struct AppState {
    HWND    hWnd;
    Doc    *doc;
    Layout *layout;
    char    currentPath[LONG_PATH_MAX];
    float   baseSize;
    int     darkMode;
    int     scrollY;
    int     maxScrollY;
    int     clientW, clientH;
    int     hoverLink;
    int     hoverText;
    HCURSOR hHandCursor;
    HCURSOR hArrowCursor;
    HCURSOR hIBeamCursor;

    /* Text selection (UTF-8 byte offsets in layout->text) */
    int     selStart;
    int     selEnd;
    int     selAnchor;
    int     isSelecting;
    int     pendingLink;
    POINT   downPt;
} AppState;

static AppState g_app;

#define MAX_RECENT 5
static char g_recentFiles[MAX_RECENT][LONG_PATH_MAX];
static int  g_recentCount = 0;

typedef struct SearchMatch {
    int   toffStart;
    int   toffEnd;
    float y;
} SearchMatch;

typedef struct SearchState {
    int          active;
    SearchMatch *matches;
    int          matchCount;
    int          matchCap;
    int          currentIndex;
    HWND         hPanel;
    HWND         hEdit;
    HWND         hStaticCount;
    HWND         hBtnPrev;
    HWND         hBtnNext;
    HWND         hBtnClose;
    HFONT        hFont;
} SearchState;

static SearchState g_search;

/* ---------------- Win32 Boundary Helpers (UTF-8 <-> UTF-16) ---------------- */

static void win_set_window_text(HWND hWnd, const char *utf8)
{
    wchar_t *w = utf8_to_utf16(utf8 ? utf8 : "", -1, NULL);
    SetWindowTextW(hWnd, w ? w : L"");
    free(w);
}

static void win_set_dlg_item_text(HWND hDlg, int id, const char *utf8)
{
    wchar_t *w = utf8_to_utf16(utf8 ? utf8 : "", -1, NULL);
    SetDlgItemTextW(hDlg, id, w ? w : L"");
    free(w);
}

static void win_get_dlg_item_text(HWND hDlg, int id, char *out, int outCap)
{
    wchar_t wbuf[512] = {0};
    char *u8;
    if (!out || outCap <= 0) return;
    GetDlgItemTextW(hDlg, id, wbuf, 512);
    u8 = utf16_to_utf8(wbuf, -1, NULL);
    strncpy_s(out, (size_t)outCap, u8 ? u8 : "", _TRUNCATE);
    free(u8);
}

static void win_reg_set_sz(HKEY hKey, const wchar_t *name, const char *utf8)
{
    int wlen = 0;
    wchar_t *w = utf8_to_utf16(utf8 ? utf8 : "", -1, &wlen);
    if (w) {
        RegSetValueExW(hKey, name, 0, REG_SZ, (const BYTE *)w, (DWORD)(wlen + 1) * sizeof(wchar_t));
        free(w);
    }
}

static int win_reg_query_sz(HKEY hKey, const wchar_t *name, char *out, int outCap)
{
    DWORD bytes = 0;
    wchar_t *wbuf;
    char *u8;
    if (!out || outCap <= 0) return 0;
    out[0] = 0;
    if (RegQueryValueExW(hKey, name, NULL, NULL, NULL, &bytes) != ERROR_SUCCESS || bytes == 0) {
        return 0;
    }
    wbuf = (wchar_t *)calloc((bytes / sizeof(wchar_t)) + 2, sizeof(wchar_t));
    if (!wbuf) return 0;
    if (RegQueryValueExW(hKey, name, NULL, NULL, (LPBYTE)wbuf, &bytes) != ERROR_SUCCESS) {
        free(wbuf);
        return 0;
    }
    u8 = utf16_to_utf8(wbuf, -1, NULL);
    free(wbuf);
    if (!u8) return 0;
    strncpy_s(out, (size_t)outCap, u8, _TRUNCATE);
    free(u8);
    return out[0] != 0;
}

/* ---------------- File Association ---------------- */

static int register_file_association(int reg)
{
    wchar_t exePath[MAX_PATH];
    wchar_t cmdBuf[MAX_PATH + 32];
    HKEY hKey;

    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    swprintf_s(cmdBuf, MAX_PATH + 32, L"\"%ls\" \"%%1\"", exePath);

    if (reg) {
        /* HKCU\Software\Classes\.md = Lilac.Document */
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.md", 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE *)L"Lilac.Document", (DWORD)(wcslen(L"Lilac.Document") + 1) * sizeof(wchar_t));
            RegCloseKey(hKey);
        }

        /* HKCU\Software\Classes\Lilac.Document */
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Lilac.Document", 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE *)L"Markdown Document", (DWORD)(wcslen(L"Markdown Document") + 1) * sizeof(wchar_t));
            RegCloseKey(hKey);
        }

        /* HKCU\Software\Classes\Lilac.Document\DefaultIcon */
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Lilac.Document\\DefaultIcon", 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            wchar_t iconBuf[MAX_PATH + 8];
            swprintf_s(iconBuf, MAX_PATH + 8, L"\"%ls\",0", exePath);
            RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE *)iconBuf, (DWORD)(wcslen(iconBuf) + 1) * sizeof(wchar_t));
            RegCloseKey(hKey);
        }

        /* HKCU\Software\Classes\Lilac.Document\shell\open\command */
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Lilac.Document\\shell\\open\\command", 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE *)cmdBuf, (DWORD)(wcslen(cmdBuf) + 1) * sizeof(wchar_t));
            RegCloseKey(hKey);
        }

        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
        return 0;
    } else {
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Lilac.Document\\shell\\open\\command");
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Lilac.Document\\shell\\open");
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Lilac.Document\\shell");
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Lilac.Document\\DefaultIcon");
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Lilac.Document");
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\.md");
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
        return 0;
    }
}

/* ---------------- Font & Preferences Settings ---------------- */

static void update_recent_menu(void);

static void settings_load(void)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Lilac\\Settings", 0, KEY_READ, &hKey) == ERROR_SUCCESS ||
        RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MarkView\\Settings", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buf[1024] = {0};
        DWORD val = 0, valSz = sizeof(DWORD);

        if (win_reg_query_sz(hKey, L"BodyFontChain", buf, sizeof(buf))) {
            font_set_family_chain(FAM_BODY, buf);
        } else if (win_reg_query_sz(hKey, L"BodyFont", buf, sizeof(buf))) {
            font_set_family_face(FAM_BODY, buf);
        }

        if (win_reg_query_sz(hKey, L"MonoFontChain", buf, sizeof(buf))) {
            font_set_family_chain(FAM_MONO, buf);
        } else if (win_reg_query_sz(hKey, L"MonoFont", buf, sizeof(buf))) {
            font_set_family_face(FAM_MONO, buf);
        }

        /* Recent Files */
        if (RegQueryValueExW(hKey, L"RecentCount", NULL, NULL, (LPBYTE)&val, &valSz) == ERROR_SUCCESS) {
            int count = (int)val;
            int i;
            if (count < 0) count = 0;
            if (count > MAX_RECENT) count = MAX_RECENT;
            g_recentCount = 0;
            for (i = 0; i < count; i++) {
                wchar_t keyName[32];
                swprintf_s(keyName, 32, L"RecentFile%d", i);
                if (win_reg_query_sz(hKey, keyName, g_recentFiles[g_recentCount], LONG_PATH_MAX)) {
                    g_recentCount++;
                }
            }
        }

        RegCloseKey(hKey);
    }
}

static void settings_save(void)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Lilac\\Settings", 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        const char *b = font_get_family_face(FAM_BODY);
        const char *m = font_get_family_face(FAM_MONO);
        const char *bChain = font_get_family_chain(FAM_BODY);
        const char *mChain = font_get_family_chain(FAM_MONO);
        DWORD rCount = (DWORD)g_recentCount;
        int i;

        win_reg_set_sz(hKey, L"BodyFont", b);
        win_reg_set_sz(hKey, L"MonoFont", m);
        win_reg_set_sz(hKey, L"BodyFontChain", bChain);
        win_reg_set_sz(hKey, L"MonoFontChain", mChain);

        RegSetValueExW(hKey, L"RecentCount", 0, REG_DWORD, (const BYTE *)&rCount, sizeof(DWORD));
        for (i = 0; i < g_recentCount; i++) {
            wchar_t keyName[32];
            swprintf_s(keyName, 32, L"RecentFile%d", i);
            win_reg_set_sz(hKey, keyName, g_recentFiles[i]);
        }

        RegCloseKey(hKey);
    }
}

static void recent_add(const char *path)
{
    int i, found = -1;
    char *fullPath;
    const char *targetPath;
    if (!path || !path[0]) return;

    fullPath = path_full(path);
    targetPath = fullPath ? fullPath : path;

    for (i = 0; i < g_recentCount; i++) {
        if (!ascii_icmp(g_recentFiles[i], targetPath)) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        char *tmp = _strdup(g_recentFiles[found]);
        for (i = found; i > 0; i--) {
            strncpy_s(g_recentFiles[i], LONG_PATH_MAX, g_recentFiles[i - 1], _TRUNCATE);
        }
        strncpy_s(g_recentFiles[0], LONG_PATH_MAX, tmp ? tmp : targetPath, _TRUNCATE);
        free(tmp);
    } else {
        int count = g_recentCount < MAX_RECENT ? g_recentCount : MAX_RECENT - 1;
        for (i = count; i > 0; i--) {
            strncpy_s(g_recentFiles[i], LONG_PATH_MAX, g_recentFiles[i - 1], _TRUNCATE);
        }
        strncpy_s(g_recentFiles[0], LONG_PATH_MAX, targetPath, _TRUNCATE);
        if (g_recentCount < MAX_RECENT) g_recentCount++;
    }

    free(fullPath);
    settings_save();
    update_recent_menu();
}

static void recent_remove(int idx)
{
    int i;
    if (idx < 0 || idx >= g_recentCount) return;
    for (i = idx; i < g_recentCount - 1; i++) {
        strncpy_s(g_recentFiles[i], LONG_PATH_MAX, g_recentFiles[i + 1], _TRUNCATE);
    }
    g_recentCount--;
    settings_save();
    update_recent_menu();
}

static void update_recent_menu(void)
{
    HMENU hMainMenu, hFileMenu, hRecentMenu;
    if (!g_app.hWnd) return;
    hMainMenu = GetMenu(g_app.hWnd);
    if (!hMainMenu) return;
    hFileMenu = GetSubMenu(hMainMenu, 0);
    if (!hFileMenu) return;
    hRecentMenu = GetSubMenu(hFileMenu, 1);
    if (!hRecentMenu) return;

    while (GetMenuItemCount(hRecentMenu) > 0) {
        RemoveMenu(hRecentMenu, 0, MF_BYPOSITION);
    }

    if (g_recentCount == 0) {
        AppendMenuW(hRecentMenu, MF_STRING | MF_GRAYED, IDM_RECENT_EMPTY, L"(최근 파일 없음)");
    } else {
        int i;
        for (i = 0; i < g_recentCount; i++) {
            size_t ulen = strlen(g_recentFiles[i]) + 32;
            char *u8Item = (char *)malloc(ulen);
            if (u8Item) {
                wchar_t *wItem;
                snprintf(u8Item, ulen, "&%d. %s", i + 1, g_recentFiles[i]);
                wItem = utf8_to_utf16(u8Item, -1, NULL);
                if (wItem) {
                    AppendMenuW(hRecentMenu, MF_STRING, IDM_RECENT_FILE_1 + i, wItem);
                    free(wItem);
                }
                free(u8Item);
            }
        }
        AppendMenuW(hRecentMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hRecentMenu, MF_STRING, IDM_RECENT_CLEAR, L"목록 지우기(&C)");
    }
    DrawMenuBar(g_app.hWnd);
}

static int pick_font_face(HWND hWnd, char *outFace, int outCap, int fixedOnly)
{
    CHOOSEFONTW cf = {0};
    LOGFONTW lf = {0};
    HDC hdc;
    wchar_t *winit;

    if (!outFace || outCap <= 0) return 0;
    hdc = GetDC(hWnd);
    winit = utf8_to_utf16(outFace, -1, NULL);

    lf.lfHeight = -16;
    if (winit) {
        wcsncpy_s(lf.lfFaceName, LF_FACESIZE, winit, _TRUNCATE);
        free(winit);
    }

    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = hWnd;
    cf.hDC = hdc;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS;
    if (fixedOnly) cf.Flags |= CF_FIXEDPITCHONLY;

    if (ChooseFontW(&cf)) {
        char *u8 = utf16_to_utf8(lf.lfFaceName, -1, NULL);
        if (u8) {
            strncpy_s(outFace, (size_t)outCap, u8, _TRUNCATE);
            free(u8);
        }
        ReleaseDC(hWnd, hdc);
        return 1;
    }
    ReleaseDC(hWnd, hdc);
    return 0;
}

static void rebuild_layout(void);

static INT_PTR CALLBACK FontDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        win_set_dlg_item_text(hDlg, IDC_EDIT_BODY_CHAIN, font_get_family_chain(FAM_BODY));
        win_set_dlg_item_text(hDlg, IDC_EDIT_MONO_CHAIN, font_get_family_chain(FAM_MONO));
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_CHANGE_BODY: {
            char picked[128] = {0};
            if (pick_font_face(hDlg, picked, sizeof(picked), 0)) {
                char cur[512];
                char updated[640];
                win_get_dlg_item_text(hDlg, IDC_EDIT_BODY_CHAIN, cur, sizeof(cur));
                snprintf(updated, sizeof(updated), "%s, %s", picked, cur);
                font_set_family_chain(FAM_BODY, updated);
                win_set_dlg_item_text(hDlg, IDC_EDIT_BODY_CHAIN, font_get_family_chain(FAM_BODY));
            }
            return TRUE;
        }

        case IDC_BTN_CHANGE_MONO: {
            char picked[128] = {0};
            if (pick_font_face(hDlg, picked, sizeof(picked), 1)) {
                char cur[512];
                char updated[640];
                win_get_dlg_item_text(hDlg, IDC_EDIT_MONO_CHAIN, cur, sizeof(cur));
                snprintf(updated, sizeof(updated), "%s, %s", picked, cur);
                font_set_family_chain(FAM_MONO, updated);
                win_set_dlg_item_text(hDlg, IDC_EDIT_MONO_CHAIN, font_get_family_chain(FAM_MONO));
            }
            return TRUE;
        }

        case IDC_BTN_RESET: {
            font_set_family_chain(FAM_BODY, NULL);
            font_set_family_chain(FAM_MONO, NULL);
            win_set_dlg_item_text(hDlg, IDC_EDIT_BODY_CHAIN, font_get_family_chain(FAM_BODY));
            win_set_dlg_item_text(hDlg, IDC_EDIT_MONO_CHAIN, font_get_family_chain(FAM_MONO));
            return TRUE;
        }

        case IDOK: {
            char bChain[512];
            char mChain[512];
            win_get_dlg_item_text(hDlg, IDC_EDIT_BODY_CHAIN, bChain, sizeof(bChain));
            win_get_dlg_item_text(hDlg, IDC_EDIT_MONO_CHAIN, mChain, sizeof(mChain));
            font_set_family_chain(FAM_BODY, bChain);
            font_set_family_chain(FAM_MONO, mChain);
            settings_save();
            rebuild_layout();
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---------------- Text Selection & Search ---------------- */

static void do_scroll(int newPos);

static int is_word_cp(uint32_t cp)
{
    if (cp <= 32) return 0;
    if (cp < 128 && strchr(" \t\r\n.,()[]{}<>\"';:!?`~|/\\=+-*&^%$#@", (int)cp)) return 0;
    return 1;
}

static void copy_selection_to_clipboard(void)
{
    int s, e, len, i, extra = 0, wlen = 0;
    char *norm;
    wchar_t *wtext;
    HGLOBAL hMem;

    if (!g_app.layout || !g_app.layout->text) return;
    s = min(g_app.selStart, g_app.selEnd);
    e = max(g_app.selStart, g_app.selEnd);
    if (s < 0 || e <= s || s >= g_app.layout->textlen) return;
    if (e > g_app.layout->textlen) e = g_app.layout->textlen;
    len = e - s;
    if (len <= 0) return;

    /* Count lone \n to convert to \r\n in UTF-8 buffer */
    for (i = 0; i < len; i++) {
        if (g_app.layout->text[s + i] == '\n') {
            if (i == 0 || g_app.layout->text[s + i - 1] != '\r') {
                extra++;
            }
        }
    }

    if (len > 0x3FFFFFFF || extra < 0 || extra > 0x3FFFFFFF) return;
    norm = (char *)malloc((size_t)len + (size_t)extra + 1);
    if (!norm) return;
    {
        int outIdx = 0;
        int cap = len + extra;
        for (i = 0; i < len && outIdx < cap; i++) {
            char c = g_app.layout->text[s + i];
            if (c == '\n' && (i == 0 || g_app.layout->text[s + i - 1] != '\r') && outIdx + 1 < cap) {
                norm[outIdx++] = '\r';
            }
            norm[outIdx++] = c;
        }
        norm[outIdx] = 0;
        wtext = utf8_to_utf16(norm, outIdx, &wlen);
    }
    free(norm);
    if (!wtext) return;

    if (!OpenClipboard(g_app.hWnd)) {
        free(wtext);
        return;
    }
    EmptyClipboard();

    hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wlen + 1) * sizeof(wchar_t));
    if (hMem) {
        wchar_t *dst = (wchar_t *)GlobalLock(hMem);
        if (dst) {
            memcpy(dst, wtext, (size_t)(wlen + 1) * sizeof(wchar_t));
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();
    free(wtext);
}

static uint32_t fold_cp(uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    return cp;
}

static int utf8_match_here(const char *hay, int hayLen, const char *needle, int needleLen)
{
    int hi = 0, ni = 0;
    while (ni < needleLen) {
        uint32_t hc, nc;
        if (hi >= hayLen) return -1;
        hc = fold_cp(utf8_next(hay, hayLen, &hi));
        nc = fold_cp(utf8_next(needle, needleLen, &ni));
        if (hc != nc) return -1;
    }
    return hi;
}

static void search_perform(void)
{
    wchar_t wq[256] = {0};
    char *q = NULL;
    int qlen = 0;
    int i;
    if (!g_search.hEdit) return;
    GetWindowTextW(g_search.hEdit, wq, 256);
    q = utf16_to_utf8(wq, -1, &qlen);

    if (!q || qlen == 0 || !g_app.layout || !g_app.layout->text || g_app.layout->textlen == 0) {
        free(q);
        g_search.matchCount = 0;
        g_search.currentIndex = -1;
        if (g_search.hStaticCount) {
            win_set_window_text(g_search.hStaticCount, "");
            InvalidateRect(g_search.hStaticCount, NULL, TRUE);
        }
        InvalidateRect(g_app.hWnd, NULL, FALSE);
        return;
    }

    g_search.matchCount = 0;
    i = 0;
    while (i <= g_app.layout->textlen - qlen) {
        int matchedBytes = utf8_match_here(g_app.layout->text + i, g_app.layout->textlen - i, q, qlen);
        if (matchedBytes > 0) {
            float my = 0;
            if (g_search.matchCount >= g_search.matchCap) {
                int newCap = g_search.matchCap > 0 ? g_search.matchCap * 2 : 64;
                SearchMatch *nm;
                if (g_search.matchCap > 0x3FFFFFFF || (size_t)newCap > SIZE_MAX / sizeof(SearchMatch)) break;
                nm = (SearchMatch *)realloc(g_search.matches, (size_t)newCap * sizeof(SearchMatch));
                if (!nm) break;
                g_search.matches = nm;
                g_search.matchCap = newCap;
            }
            g_search.matches[g_search.matchCount].toffStart = i;
            g_search.matches[g_search.matchCount].toffEnd = i + matchedBytes;
            layout_text_pos(g_app.layout, i, NULL, NULL, &my);
            g_search.matches[g_search.matchCount].y = my;
            g_search.matchCount++;
        }
        /* Advance to next UTF-8 codepoint boundary */
        (void)utf8_next(g_app.layout->text, g_app.layout->textlen, &i);
    }
    free(q);

    if (g_search.matchCount > 0) {
        int best = 0;
        char countBuf[32];
        for (i = 0; i < g_search.matchCount; i++) {
            if (g_search.matches[i].y >= (float)g_app.scrollY) {
                best = i;
                break;
            }
        }
        g_search.currentIndex = best;
        do_scroll((int)g_search.matches[best].y - g_app.clientH / 3);
        snprintf(countBuf, sizeof(countBuf), "%d / %d", best + 1, g_search.matchCount);
        win_set_window_text(g_search.hStaticCount, countBuf);
        InvalidateRect(g_search.hStaticCount, NULL, TRUE);
        UpdateWindow(g_search.hStaticCount);
    } else {
        g_search.currentIndex = -1;
        win_set_window_text(g_search.hStaticCount, "결과 없음");
        InvalidateRect(g_search.hStaticCount, NULL, TRUE);
        UpdateWindow(g_search.hStaticCount);
    }
    InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void search_next(void)
{
    if (g_search.matchCount > 0) {
        char countBuf[32];
        g_search.currentIndex = (g_search.currentIndex + 1) % g_search.matchCount;
        do_scroll((int)g_search.matches[g_search.currentIndex].y - g_app.clientH / 3);
        snprintf(countBuf, sizeof(countBuf), "%d / %d", g_search.currentIndex + 1, g_search.matchCount);
        win_set_window_text(g_search.hStaticCount, countBuf);
        InvalidateRect(g_search.hStaticCount, NULL, TRUE);
        UpdateWindow(g_search.hStaticCount);
        InvalidateRect(g_app.hWnd, NULL, FALSE);
    }
}

static void search_prev(void)
{
    if (g_search.matchCount > 0) {
        char countBuf[32];
        g_search.currentIndex = (g_search.currentIndex - 1 + g_search.matchCount) % g_search.matchCount;
        do_scroll((int)g_search.matches[g_search.currentIndex].y - g_app.clientH / 3);
        snprintf(countBuf, sizeof(countBuf), "%d / %d", g_search.currentIndex + 1, g_search.matchCount);
        win_set_window_text(g_search.hStaticCount, countBuf);
        InvalidateRect(g_search.hStaticCount, NULL, TRUE);
        UpdateWindow(g_search.hStaticCount);
        InvalidateRect(g_app.hWnd, NULL, FALSE);
    }
}

static void search_update_font(void);

static void search_open(void)
{
    g_search.active = 1;
    if (g_search.hPanel) {
        int px = g_app.clientW - 365;
        search_update_font();
        if (px < 10) px = 10;
        SetWindowPos(g_search.hPanel, HWND_TOP, px, 10, 350, 36, SWP_SHOWWINDOW);
        ShowWindow(g_search.hPanel, SW_SHOW);
        RedrawWindow(g_search.hPanel, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        SetFocus(g_search.hEdit);
        SendMessageW(g_search.hEdit, EM_SETSEL, 0, -1);
        search_perform();
    }
}

static void search_close(void)
{
    g_search.active = 0;
    if (g_search.hPanel) {
        ShowWindow(g_search.hPanel, SW_HIDE);
    }
    SetFocus(g_app.hWnd);
    InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static WNDPROC g_oldEditProc = NULL;
static LRESULT CALLBACK SearchEditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    }

    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            if (GetKeyState(VK_SHIFT) & 0x8000) search_prev();
            else search_next();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            search_close();
            return 0;
        }
        if (wParam == VK_DOWN) {
            search_next();
            return 0;
        }
        if (wParam == VK_UP) {
            search_prev();
            return 0;
        }
        if ((wParam == 'F' || wParam == 'f') && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hWnd, EM_SETSEL, 0, -1);
            return 0;
        }
    }

    if (msg == WM_CHAR) {
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
            return 0;
        }
    }

    if (msg == WM_KEYUP) {
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
            return 0;
        }
    }

    return CallWindowProcW(g_oldEditProc, hWnd, msg, wParam, lParam);
}

static LRESULT CALLBACK SearchPanelProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT r;
        COLORREF bg = g_app.darkMode ? RGB(34, 39, 46) : RGB(255, 255, 255);
        COLORREF border = g_app.darkMode ? RGB(68, 76, 86) : RGB(208, 215, 222);
        HBRUSH br = CreateSolidBrush(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldP, oldB;
        GetClientRect(hWnd, &r);
        oldP = SelectObject(hdc, pen);
        oldB = SelectObject(hdc, br);
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, 8, 8);
        SelectObject(hdc, oldB);
        SelectObject(hdc, oldP);
        DeleteObject(pen);
        DeleteObject(br);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        COLORREF bg = g_app.darkMode ? RGB(34, 39, 46) : RGB(255, 255, 255);
        COLORREF fg = g_app.darkMode ? RGB(240, 246, 252) : RGB(36, 41, 47);
        static HBRUSH s_editBr = NULL;
        static COLORREF s_lastEditBg = 0xFFFFFFFF;
        SetTextColor(hdc, fg);
        SetBkColor(hdc, bg);
        if (!s_editBr || s_lastEditBg != bg) {
            if (s_editBr) DeleteObject(s_editBr);
            s_editBr = CreateSolidBrush(bg);
            s_lastEditBg = bg;
        }
        return (LRESULT)s_editBr;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        COLORREF bg = g_app.darkMode ? RGB(34, 39, 46) : RGB(255, 255, 255);
        COLORREF fg = g_app.darkMode ? RGB(140, 150, 160) : RGB(87, 96, 106);
        static HBRUSH s_staticBr = NULL;
        static COLORREF s_lastStaticBg = 0xFFFFFFFF;
        SetTextColor(hdc, fg);
        SetBkColor(hdc, bg);
        SetBkMode(hdc, OPAQUE);
        if (!s_staticBr || s_lastStaticBg != bg) {
            if (s_staticBr) DeleteObject(s_staticBr);
            s_staticBr = CreateSolidBrush(bg);
            s_lastStaticBg = bg;
        }
        return (LRESULT)s_staticBr;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
        COLORREF bg = g_app.darkMode ? RGB(45, 52, 62) : RGB(243, 244, 246);
        COLORREF fg = g_app.darkMode ? RGB(220, 230, 242) : RGB(36, 41, 47);
        COLORREF border = g_app.darkMode ? RGB(70, 80, 92) : RGB(208, 215, 222);
        const wchar_t *text = L"";
        HBRUSH br;
        HPEN pen;
        HGDIOBJ oldB, oldP, oldF;

        if (dis->itemState & ODS_SELECTED) {
            bg = g_app.darkMode ? RGB(65, 75, 88) : RGB(220, 225, 232);
        }

        br = CreateSolidBrush(bg);
        pen = CreatePen(PS_SOLID, 1, border);
        oldB = SelectObject(dis->hDC, br);
        oldP = SelectObject(dis->hDC, pen);
        RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 4, 4);
        SelectObject(dis->hDC, oldP);
        SelectObject(dis->hDC, oldB);
        DeleteObject(pen);
        DeleteObject(br);

        if (dis->CtlID == IDC_SEARCH_PREV) text = L"\x25B2";
        else if (dis->CtlID == IDC_SEARCH_NEXT) text = L"\x25BC";
        else if (dis->CtlID == IDC_SEARCH_CLOSE) text = L"\x2715";

        SetTextColor(dis->hDC, fg);
        SetBkMode(dis->hDC, TRANSPARENT);
        oldF = SelectObject(dis->hDC, g_search.hFont);
        DrawTextW(dis->hDC, text, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dis->hDC, oldF);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_SEARCH_EDIT:
            if (HIWORD(wParam) == EN_CHANGE) {
                search_perform();
            }
            break;
        case IDC_SEARCH_PREV:
            search_prev();
            break;
        case IDC_SEARCH_NEXT:
            search_next();
            break;
        case IDC_SEARCH_CLOSE:
            search_close();
            break;
        }
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void search_update_font(void)
{
    Font *primary = font_primary(FAM_BODY, 0, 0);
    const char *face = (primary && primary->face[0]) ? primary->face : font_get_family_face(FAM_BODY);
    wchar_t *wface;
    UINT dpi = 96;
    int fontHeight;
    typedef UINT (WINAPI *GetDpiForWindowProc)(HWND);
    HMODULE hUser = GetModuleHandleW(L"user32.dll");

    if (!face || !face[0]) face = "맑은 고딕";
    if (hUser) {
        GetDpiForWindowProc pGetDpi = (GetDpiForWindowProc)GetProcAddress(hUser, "GetDpiForWindow");
        if (pGetDpi && g_app.hWnd) dpi = pGetDpi(g_app.hWnd);
    }
    fontHeight = -MulDiv(10, dpi, 72);

    if (g_search.hFont) {
        DeleteObject(g_search.hFont);
        g_search.hFont = NULL;
    }

    wface = utf8_to_utf16(face, -1, NULL);
    g_search.hFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 wface ? wface : L"맑은 고딕");
    free(wface);

    if (!g_search.hFont) {
        g_search.hFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"맑은 고딕");
    }

    if (g_search.hEdit && g_search.hFont) {
        SendMessageW(g_search.hEdit, WM_SETFONT, (WPARAM)g_search.hFont, TRUE);
    }
    if (g_search.hStaticCount && g_search.hFont) {
        SendMessageW(g_search.hStaticCount, WM_SETFONT, (WPARAM)g_search.hFont, TRUE);
    }
    if (g_search.hPanel) {
        RedrawWindow(g_search.hPanel, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

static void search_init(HWND hParent, HINSTANCE hInst)
{
    WNDCLASSEXW pc = {0};
    pc.cbSize = sizeof(pc);
    pc.style = CS_HREDRAW | CS_VREDRAW;
    pc.lpfnWndProc = SearchPanelProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    pc.hbrBackground = NULL;
    pc.lpszClassName = L"LilacSearchPanel";
    RegisterClassExW(&pc);

    g_search.hPanel = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        L"LilacSearchPanel", L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 350, 36,
        hParent, (HMENU)2100, hInst, NULL);

    g_search.hEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL,
        10, 7, 185, 22,
        g_search.hPanel, (HMENU)IDC_SEARCH_EDIT, hInst, NULL);
    g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_search.hEdit, GWLP_WNDPROC, (LONG_PTR)SearchEditSubclassProc);

    g_search.hStaticCount = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_CENTER | SS_CENTERIMAGE,
        200, 8, 65, 20,
        g_search.hPanel, (HMENU)IDC_SEARCH_STATIC, hInst, NULL);

    g_search.hBtnPrev = CreateWindowExW(
        0, L"BUTTON", L"\x25B2",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        270, 6, 24, 24,
        g_search.hPanel, (HMENU)IDC_SEARCH_PREV, hInst, NULL);

    g_search.hBtnNext = CreateWindowExW(
        0, L"BUTTON", L"\x25BC",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        296, 6, 24, 24,
        g_search.hPanel, (HMENU)IDC_SEARCH_NEXT, hInst, NULL);

    g_search.hBtnClose = CreateWindowExW(
        0, L"BUTTON", L"\x2715",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        322, 6, 24, 24,
        g_search.hPanel, (HMENU)IDC_SEARCH_CLOSE, hInst, NULL);

    search_update_font();
}

/* ---------------- Document Loading & Layout ---------------- */

static void update_window_title(void)
{
    char title[1024];
    if (g_app.currentPath[0]) {
        snprintf(title, sizeof(title), "%s - Lilac", path_name(g_app.currentPath));
    } else {
        snprintf(title, sizeof(title), "%s - Lilac", g_app.doc && g_app.doc->title ? g_app.doc->title : "Lilac");
    }
    win_set_window_text(g_app.hWnd, title);
}

static void rebuild_layout(void)
{
    LayoutOptions opt = {0};
    SCROLLINFO si = {0};

    if (g_app.layout) {
        layout_free(g_app.layout);
        g_app.layout = NULL;
    }
    if (!g_app.doc) return;

    opt.width = (float)g_app.clientW;
    opt.pageHeight = 0; /* Continuous mode */
    opt.marginL = 24.0f;
    opt.marginR = 24.0f;
    opt.marginT = 24.0f;
    opt.marginB = 32.0f;
    opt.maxContentWidth = 860.0f;
    opt.baseSize = g_app.baseSize;
    opt.dark = g_app.darkMode;

    g_app.layout = layout_build(g_app.doc, &opt);

    /* Update vertical scrollbar */
    g_app.maxScrollY = (int)g_app.layout->pageHeight - g_app.clientH;
    if (g_app.maxScrollY < 0) g_app.maxScrollY = 0;
    if (g_app.scrollY > g_app.maxScrollY) g_app.scrollY = g_app.maxScrollY;

    if (g_search.active && g_search.matchCount > 0) {
        int m;
        for (m = 0; m < g_search.matchCount; m++) {
            float my = 0;
            layout_text_pos(g_app.layout, g_search.matches[m].toffStart, NULL, NULL, &my);
            g_search.matches[m].y = my;
        }
    }

    if (g_search.hPanel) {
        search_update_font();
    }

    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (int)g_app.layout->pageHeight;
    si.nPage = g_app.clientH;
    si.nPos = g_app.scrollY;
    SetScrollInfo(g_app.hWnd, SB_VERT, &si, TRUE);

    InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void open_file(const char *path)
{
    Doc *newDoc = doc_load_file(path);
    if (!newDoc) {
        size_t mlen = strlen(path) + 128;
        char *u8Msg = (char *)malloc(mlen);
        if (u8Msg) {
            wchar_t *wMsg;
            snprintf(u8Msg, mlen, "파일을 열 수 없습니다:\n%s", path);
            wMsg = utf8_to_utf16(u8Msg, -1, NULL);
            MessageBoxW(g_app.hWnd, wMsg ? wMsg : L"파일을 열 수 없습니다.", L"오류", MB_OK | MB_ICONERROR);
            free(wMsg);
            free(u8Msg);
        } else {
            MessageBoxW(g_app.hWnd, L"파일을 열 수 없습니다.", L"오류", MB_OK | MB_ICONERROR);
        }
        return;
    }

    if (g_app.doc) doc_free(g_app.doc);
    g_app.doc = newDoc;
    strncpy_s(g_app.currentPath, LONG_PATH_MAX, path, _TRUNCATE);
    doc_resolve_images(g_app.doc, 0);

    recent_add(path);
    g_app.selStart = -1;
    g_app.selEnd = -1;
    g_app.selAnchor = -1;
    g_app.scrollY = 0;
    update_window_title();
    rebuild_layout();
    if (g_search.active) search_perform();
}

static void load_sample_content(void)
{
    if (g_app.doc) doc_free(g_app.doc);
    g_app.doc = doc_parse(g_sampleMarkdown, strlen(g_sampleMarkdown), NULL);
    g_app.currentPath[0] = 0;
    g_app.selStart = -1;
    g_app.selEnd = -1;
    g_app.selAnchor = -1;
    g_app.scrollY = 0;
    update_window_title();
    rebuild_layout();
    if (g_search.active) search_perform();
}

/* ---------------- GDI Rendering ---------------- */

static void render_display_list(HDC hdc, Layout *L, int scrollY, int clientW, int clientH)
{
    RECT rc = {0, 0, clientW, clientH};
    HBRUSH bgBrush = CreateSolidBrush(RGB((L->background >> 16) & 0xFF, (L->background >> 8) & 0xFF, L->background & 0xFF));
    int i, layer;

    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextAlign(hdc, TA_BASELINE | TA_LEFT);

    for (layer = 0; layer < 2; layer++) {
        for (i = 0; i < L->nitems; i++) {
            const Item *it = &L->items[i];
            float drawY = it->y - (float)scrollY;
            float itemTop, itemBot;

            if (it->layer != layer) continue;
            if (it->type == IT_GLYPHS) {
                itemTop = drawY - it->size * 1.25f;
                itemBot = drawY + it->size * 0.65f;
            } else {
                itemTop = min(drawY, drawY + it->h) - it->lw;
                itemBot = max(drawY, drawY + it->h) + it->lw;
            }
            if (itemBot < -50.0f || itemTop > (float)clientH + 100.0f) continue;

            switch (it->type) {
            case IT_FILL: {
                COLORREF col = RGB((it->color >> 16) & 0xFF, (it->color >> 8) & 0xFF, it->color & 0xFF);
                HBRUSH br = CreateSolidBrush(col);
                RECT r = {(int)round(it->x), (int)round(drawY), (int)round(it->x + it->w), (int)round(drawY + it->h)};
                if (it->radius > 0) {
                    HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
                    HGDIOBJ oldP = SelectObject(hdc, nullPen);
                    HGDIOBJ oldB = SelectObject(hdc, br);
                    RoundRect(hdc, r.left, r.top, r.right, r.bottom, (int)(it->radius * 2), (int)(it->radius * 2));
                    SelectObject(hdc, oldB);
                    SelectObject(hdc, oldP);
                } else {
                    FillRect(hdc, &r, br);
                }
                DeleteObject(br);
                break;
            }

            case IT_STROKE: {
                COLORREF col = RGB((it->color >> 16) & 0xFF, (it->color >> 8) & 0xFF, it->color & 0xFF);
                HPEN pen = CreatePen(PS_SOLID, max(1, (int)round(it->lw)), col);
                HGDIOBJ oldP = SelectObject(hdc, pen);
                HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                RECT r = {(int)round(it->x), (int)round(drawY), (int)round(it->x + it->w), (int)round(drawY + it->h)};
                if (it->radius > 0) {
                    RoundRect(hdc, r.left, r.top, r.right, r.bottom, (int)(it->radius * 2), (int)(it->radius * 2));
                } else {
                    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
                }
                SelectObject(hdc, oldB);
                SelectObject(hdc, oldP);
                DeleteObject(pen);
                break;
            }

            case IT_LINE: {
                COLORREF col = RGB((it->color >> 16) & 0xFF, (it->color >> 8) & 0xFF, it->color & 0xFF);
                HPEN pen = CreatePen(PS_SOLID, max(1, (int)round(it->lw)), col);
                HGDIOBJ oldP = SelectObject(hdc, pen);
                MoveToEx(hdc, (int)round(it->x), (int)round(drawY), NULL);
                LineTo(hdc, (int)round(it->x + it->w), (int)round(drawY + it->h));
                SelectObject(hdc, oldP);
                DeleteObject(pen);
                break;
            }

            case IT_DISC: {
                COLORREF col = RGB((it->color >> 16) & 0xFF, (it->color >> 8) & 0xFF, it->color & 0xFF);
                HBRUSH br = CreateSolidBrush(col);
                HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
                HGDIOBJ oldP = SelectObject(hdc, nullPen);
                HGDIOBJ oldB = SelectObject(hdc, br);
                int ix = (int)round(it->x);
                int iy = (int)round(drawY);
                int iw = max(2, (int)round(it->w));
                int ih = max(2, (int)round(it->h));
                Ellipse(hdc, ix, iy, ix + iw, iy + ih);
                SelectObject(hdc, oldB);
                SelectObject(hdc, oldP);
                DeleteObject(br);
                break;
            }

            case IT_CHECK: {
                COLORREF col = RGB((it->color >> 16) & 0xFF, (it->color >> 8) & 0xFF, it->color & 0xFF);
                HPEN pen = CreatePen(PS_SOLID, max(2, (int)round(it->lw)), col);
                HGDIOBJ oldP = SelectObject(hdc, pen);
                POINT pts[3];
                pts[0].x = (int)round(it->x + it->w * 0.2f);
                pts[0].y = (int)round(drawY + it->h * 0.55f);
                pts[1].x = (int)round(it->x + it->w * 0.42f);
                pts[1].y = (int)round(drawY + it->h * 0.75f);
                pts[2].x = (int)round(it->x + it->w * 0.8f);
                pts[2].y = (int)round(drawY + it->h * 0.28f);
                Polyline(hdc, pts, 3);
                SelectObject(hdc, oldP);
                DeleteObject(pen);
                break;
            }

            case IT_IMAGE: {
                Image *img = it->img;
                if (img && img->w > 0 && img->h > 0 && img->pixels) {
                    BITMAPINFO bi = {0};
                    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bi.bmiHeader.biWidth = img->w;
                    bi.bmiHeader.biHeight = -img->h; /* top-down */
                    bi.bmiHeader.biPlanes = 1;
                    bi.bmiHeader.biBitCount = 32;
                    bi.bmiHeader.biCompression = BI_RGB;

                    StretchDIBits(hdc,
                                  (int)round(it->x), (int)round(drawY), (int)round(it->w), (int)round(it->h),
                                  0, 0, img->w, img->h,
                                  img->pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
                }
                break;
            }

            case IT_GLYPHS: {
                if (it->font && it->n > 0) {
                    int pixelSize = (int)round(it->size);
                    HFONT hf = (HFONT)font_hfont(it->font, pixelSize);
                    COLORREF col = RGB((it->color >> 16) & 0xFF, (it->color >> 8) & 0xFF, it->color & 0xFF);
                    HGDIOBJ oldF;
                    int stackDx[256];
                    int *dx = (it->n <= 256) ? stackDx : (int *)malloc((size_t)it->n * sizeof(int));
                    int g;
                    float asc, dsc;
                    int topY, botY;

                    if (!hf) {
                        if (dx && dx != stackDx) free(dx);
                        continue;
                    }

                    /* Calculate vertical bounding box for highlights */
                    asc = (it->font && it->font->upem > 0)
                        ? ((float)it->font->ascent * it->size / (float)it->font->upem)
                        : (it->size * 0.85f);
                    dsc = (it->font && it->font->upem > 0)
                        ? ((float)it->font->descent * it->size / (float)it->font->upem)
                        : (it->size * 0.25f);
                    topY = (int)round(drawY - asc - 1.0f);
                    botY = (int)round(drawY + dsc + 1.0f);

                    /* 1. Search match highlights */
                    if (g_search.active && g_search.matchCount > 0 && it->toff) {
                        int m;
                        for (m = 0; m < g_search.matchCount; m++) {
                            SearchMatch *sm = &g_search.matches[m];
                            float mx1 = -1.0f, mx2 = -1.0f;
                            if (sm->toffEnd <= it->toff[0]) continue;
                            if (sm->toffStart >= it->tend) break;

                            for (g = 0; g < it->n; g++) {
                                if (it->toff[g] >= sm->toffStart && it->toff[g] < sm->toffEnd) {
                                    float gx1 = it->x + (it->dx ? it->dx[g] : 0);
                                    float gx2 = (g + 1 < it->n) ? (it->x + it->dx[g + 1]) : (it->x + it->w);
                                    if (mx1 < 0.0f || gx1 < mx1) mx1 = gx1;
                                    if (gx2 > mx2) mx2 = gx2;
                                }
                            }
                            if (mx1 >= 0.0f) {
                                RECT mr = {(int)round(mx1), topY, (int)round(mx2), botY};
                                COLORREF mCol = (m == g_search.currentIndex)
                                    ? (g_app.darkMode ? RGB(210, 105, 30) : RGB(255, 150, 50))
                                    : (g_app.darkMode ? RGB(90, 80, 20) : RGB(255, 235, 120));
                                HBRUSH mBr = CreateSolidBrush(mCol);
                                FillRect(hdc, &mr, mBr);
                                DeleteObject(mBr);
                            }
                        }
                    }

                    /* 2. Selection highlight */
                    if (g_app.selStart >= 0 && g_app.selEnd >= 0 && g_app.selStart != g_app.selEnd && it->toff) {
                        int s1 = min(g_app.selStart, g_app.selEnd);
                        int s2 = max(g_app.selStart, g_app.selEnd);
                        if (it->toff[0] < s2 && it->tend > s1) {
                            float sx1 = -1.0f, sx2 = -1.0f;
                            for (g = 0; g < it->n; g++) {
                                if (it->toff[g] >= s1 && it->toff[g] < s2) {
                                    float gx1 = it->x + (it->dx ? it->dx[g] : 0);
                                    float gx2 = (g + 1 < it->n) ? (it->x + it->dx[g + 1]) : (it->x + it->w);
                                    if (sx1 < 0.0f || gx1 < sx1) sx1 = gx1;
                                    if (gx2 > sx2) sx2 = gx2;
                                }
                            }
                            if (s2 >= it->tend && sx1 >= 0.0f) {
                                sx2 = it->x + it->w;
                            }
                            if (sx1 >= 0.0f) {
                                RECT sr = {(int)round(sx1), topY, (int)round(sx2), botY};
                                COLORREF sCol = g_app.darkMode ? RGB(38, 79, 120) : RGB(180, 215, 255);
                                HBRUSH sBr = CreateSolidBrush(sCol);
                                FillRect(hdc, &sr, sBr);
                                DeleteObject(sBr);
                            }
                        }
                    }

                    oldF = SelectObject(hdc, hf);
                    SetTextColor(hdc, col);

                    if (dx && it->dx) {
                        for (g = 0; g < it->n - 1; g++) {
                            dx[g] = (int)round(it->dx[g + 1]) - (int)round(it->dx[g]);
                        }
                        dx[it->n - 1] = (int)round(it->w) - (int)round(it->dx[it->n - 1]);
                    }

                    ExtTextOutW(hdc, (int)round(it->x), (int)round(drawY),
                                ETO_GLYPH_INDEX, NULL, (LPCWSTR)it->gids, it->n, dx);

                    if (dx && dx != stackDx) free(dx);
                    SelectObject(hdc, oldF);
                }
                break;
            }
            }
        }
    }
}

/* ---------------- Window Procedure ---------------- */

static int is_over_text(float x, float y)
{
    int i;
    if (!g_app.layout) return 0;
    for (i = 0; i < g_app.layout->nitems; i++) {
        const Item *it = &g_app.layout->items[i];
        if (it->type == IT_GLYPHS && it->page == 0 && it->n > 0) {
            float top = it->y - it->size;
            float bottom = it->y + it->size * 0.45f;
            if (y >= top && y <= bottom && x >= it->x - 4.0f && x <= it->x + it->w + 8.0f) {
                return 1;
            }
        }
    }
    return 0;
}

static void do_scroll(int newPos)
{
    SCROLLINFO si = {0};
    if (newPos < 0) newPos = 0;
    if (newPos > g_app.maxScrollY) newPos = g_app.maxScrollY;
    if (newPos != g_app.scrollY) {
        g_app.scrollY = newPos;
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        si.nPos = g_app.scrollY;
        SetScrollInfo(g_app.hWnd, SB_VERT, &si, TRUE);
        InvalidateRect(g_app.hWnd, NULL, FALSE);
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = ((LPCREATESTRUCTW)lParam)->hInstance;
        HICON hIcon, hIconSm;
        g_app.hWnd = hWnd;
        hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
        if (hIcon) SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        if (hIconSm) SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);
        DragAcceptFiles(hWnd, TRUE);

        search_init(hWnd, hInst);
        update_recent_menu();
        return 0;
    }

    case WM_SIZE: {
        g_app.clientW = LOWORD(lParam);
        g_app.clientH = HIWORD(lParam);
        if (g_search.hPanel) {
            int px = g_app.clientW - 365;
            if (px < 10) px = 10;
            SetWindowPos(g_search.hPanel, HWND_TOP, px, 10, 350, 36, SWP_NOACTIVATE | (g_search.active ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        }
        rebuild_layout();
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBm = CreateCompatibleBitmap(hdc, g_app.clientW, g_app.clientH);
        HGDIOBJ oldBm = SelectObject(memDC, memBm);

        if (g_app.layout) {
            render_display_list(memDC, g_app.layout, g_app.scrollY, g_app.clientW, g_app.clientH);
        }

        BitBlt(hdc, 0, 0, g_app.clientW, g_app.clientH, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBm);
        DeleteObject(memBm);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_VSCROLL: {
        SCROLLINFO si = {0};
        int pos = g_app.scrollY;
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hWnd, SB_VERT, &si);

        switch (LOWORD(wParam)) {
        case SB_LINEUP: pos -= 30; break;
        case SB_LINEDOWN: pos += 30; break;
        case SB_PAGEUP: pos -= g_app.clientH; break;
        case SB_PAGEDOWN: pos += g_app.clientH; break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = g_app.maxScrollY; break;
        }
        do_scroll(pos);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        do_scroll(g_app.scrollY - (delta / WHEEL_DELTA) * 60);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int cx = GET_X_LPARAM(lParam);
        int cy = GET_Y_LPARAM(lParam);
        int docX = cx;
        int docY = cy + g_app.scrollY;
        int link = -1, overTxt;

        if (g_app.isSelecting) {
            int hit;
            /* Auto-scroll when dragging outside client area */
            if (cy < 0) {
                do_scroll(g_app.scrollY + cy);
            } else if (cy > g_app.clientH) {
                do_scroll(g_app.scrollY + (cy - g_app.clientH));
            }

            hit = layout_hit_text(g_app.layout, 0, (float)docX, (float)(cy + g_app.scrollY));
            if (hit >= 0 && g_app.selAnchor >= 0) {
                if (hit != g_app.selEnd) {
                    g_app.selStart = g_app.selAnchor;
                    g_app.selEnd = hit;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            SetCursor(g_app.hIBeamCursor);
            return 0;
        }

        if (g_app.layout) {
            link = layout_hit_link(g_app.layout, 0, (float)docX, (float)docY);
        }
        overTxt = is_over_text((float)docX, (float)docY);

        if (link != g_app.hoverLink || overTxt != g_app.hoverText) {
            g_app.hoverLink = link;
            g_app.hoverText = overTxt;
            if (link >= 0) SetCursor(g_app.hHandCursor);
            else if (overTxt) SetCursor(g_app.hIBeamCursor);
            else SetCursor(g_app.hArrowCursor);
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            if (g_app.isSelecting) {
                SetCursor(g_app.hIBeamCursor);
                return TRUE;
            }
            if (g_app.hoverLink >= 0) {
                SetCursor(g_app.hHandCursor);
                return TRUE;
            }
            if (g_app.hoverText) {
                SetCursor(g_app.hIBeamCursor);
                return TRUE;
            }
            SetCursor(g_app.hArrowCursor);
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam) + g_app.scrollY;
        int link = -1;
        int hit;
        if (g_app.layout) {
            link = layout_hit_link(g_app.layout, 0, (float)x, (float)y);
        }
        g_app.pendingLink = link;
        g_app.downPt.x = GET_X_LPARAM(lParam);
        g_app.downPt.y = GET_Y_LPARAM(lParam);

        hit = layout_hit_text(g_app.layout, 0, (float)x, (float)y);
        if (hit >= 0) {
            if (!(GetKeyState(VK_SHIFT) & 0x8000)) {
                g_app.selAnchor = hit;
                g_app.selStart = hit;
                g_app.selEnd = hit;
            } else {
                g_app.selEnd = hit;
            }
        } else {
            g_app.selAnchor = -1;
            g_app.selStart = -1;
            g_app.selEnd = -1;
        }
        g_app.isSelecting = 1;
        SetCapture(hWnd);
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_app.isSelecting) {
            int dx, dy;
            ReleaseCapture();
            g_app.isSelecting = 0;

            dx = abs(GET_X_LPARAM(lParam) - g_app.downPt.x);
            dy = abs(GET_Y_LPARAM(lParam) - g_app.downPt.y);

            /* If it was a click without dragging */
            if (dx < 4 && dy < 4) {
                if (g_app.pendingLink >= 0 && g_app.pendingLink < g_app.doc->nlinks) {
                    const char *target = g_app.doc->links[g_app.pendingLink];
                    if (target[0] == '#') {
                        const Anchor *anc = layout_find_anchor(g_app.layout, target + 1);
                        if (anc) do_scroll((int)anc->y - 20);
                    } else if (link_is_safe_external(target)) {
                        wchar_t *wtarget = utf8_to_utf16(target, -1, NULL);
                        if (wtarget) {
                            ShellExecuteW(hWnd, L"open", wtarget, NULL, NULL, SW_SHOWNORMAL);
                            free(wtarget);
                        }
                    } else if (g_app.doc->dir && !path_is_remote(target) &&
                               !strchr(target, ':') && target[0] != '\\' && target[0] != '/') {
                        char *joined = path_join(g_app.doc->dir, target);
                        if (joined && !path_is_remote(joined) && path_is_within(joined, g_app.doc->dir) && file_exists(joined)) {
                            open_file(joined);
                        }
                        free(joined);
                    }
                    g_app.selStart = -1;
                    g_app.selEnd = -1;
                    InvalidateRect(hWnd, NULL, FALSE);
                } else {
                    /* Plain click deselects if empty */
                    if (g_app.selStart == g_app.selEnd) {
                        g_app.selStart = -1;
                        g_app.selEnd = -1;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                }
            } else {
                if (g_app.selStart == g_app.selEnd) {
                    g_app.selStart = -1;
                    g_app.selEnd = -1;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
        }
        g_app.pendingLink = -1;
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam) + g_app.scrollY;
        int hit = layout_hit_text(g_app.layout, 0, (float)x, (float)y);
        if (hit >= 0 && g_app.layout && g_app.layout->text && g_app.layout->textlen > 0) {
            const char *txt = g_app.layout->text;
            int len = g_app.layout->textlen;
            int s, e, tmp;
            uint32_t hitCp;
            if (hit >= len) hit = len - 1;
            /* Align hit to start of UTF-8 codepoint */
            while (hit > 0 && (((unsigned char)txt[hit] & 0xC0) == 0x80)) hit--;
            tmp = hit;
            hitCp = utf8_next(txt, len, &tmp);
            s = hit;
            e = tmp;
            if (is_word_cp(hitCp)) {
                while (s > 0) {
                    int prev = s - 1;
                    int p2;
                    uint32_t cp;
                    while (prev > 0 && (((unsigned char)txt[prev] & 0xC0) == 0x80)) prev--;
                    p2 = prev;
                    cp = utf8_next(txt, len, &p2);
                    if (!is_word_cp(cp)) break;
                    s = prev;
                }
                while (e < len) {
                    int next = e;
                    uint32_t cp = utf8_next(txt, len, &next);
                    if (!is_word_cp(cp)) break;
                    e = next;
                }
            }
            g_app.selStart = s;
            g_app.selEnd = e;
            g_app.selAnchor = s;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_CONTEXTMENU: {
        POINT pt;
        HMENU hMenu;
        int hasSel;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        if (pt.x == -1 && pt.y == -1) {
            pt.x = 50;
            pt.y = 50;
            ClientToScreen(hWnd, &pt);
        }
        hMenu = CreatePopupMenu();
        hasSel = (g_app.selStart >= 0 && g_app.selEnd >= 0 && g_app.selStart != g_app.selEnd);
        AppendMenuW(hMenu, MF_STRING | (hasSel ? 0 : MF_GRAYED), IDM_EDIT_COPY, L"복사(&C)\tCtrl+C");
        AppendMenuW(hMenu, MF_STRING, IDM_EDIT_SELECT_ALL, L"모두 선택(&A)\tCtrl+A");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, IDM_EDIT_FIND, L"찾기(&F)...\tCtrl+F");

        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
        DestroyMenu(hMenu);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT len = DragQueryFileW(hDrop, 0, NULL, 0);
        if (len > 0) {
            wchar_t *dropped = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
            if (dropped) {
                if (DragQueryFileW(hDrop, 0, dropped, len + 1)) {
                    char *u8Path = utf16_to_utf8(dropped, -1, NULL);
                    if (u8Path) {
                        open_file(u8Path);
                        free(u8Path);
                    }
                }
                free(dropped);
            }
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FILE_OPEN: {
            OPENFILENAMEW ofn = {0};
            wchar_t *fileBuf = (wchar_t *)calloc(LONG_PATH_MAX, sizeof(wchar_t));
            if (!fileBuf) return 0;
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"Markdown 문서 (*.md;*.markdown;*.txt)\0*.md;*.markdown;*.mdown;*.txt\0모든 파일 (*.*)\0*.*\0";
            ofn.lpstrFile = fileBuf;
            ofn.nMaxFile = LONG_PATH_MAX;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                char *u8Path = utf16_to_utf8(fileBuf, -1, NULL);
                if (u8Path) {
                    open_file(u8Path);
                    free(u8Path);
                }
            }
            free(fileBuf);
            return 0;
        }

        /* Recent files */
        case IDM_RECENT_FILE_1:
        case IDM_RECENT_FILE_2:
        case IDM_RECENT_FILE_3:
        case IDM_RECENT_FILE_4:
        case IDM_RECENT_FILE_5: {
            int idx = LOWORD(wParam) - IDM_RECENT_FILE_1;
            if (idx >= 0 && idx < g_recentCount) {
                if (file_exists(g_recentFiles[idx])) {
                    open_file(g_recentFiles[idx]);
                } else {
                    size_t mlen = strlen(g_recentFiles[idx]) + 128;
                    char *u8Msg = (char *)malloc(mlen);
                    if (u8Msg) {
                        wchar_t *wMsg;
                        snprintf(u8Msg, mlen, "파일을 찾을 수 없습니다:\n%s\n\n최근 파일 목록에서 제거합니다.", g_recentFiles[idx]);
                        wMsg = utf8_to_utf16(u8Msg, -1, NULL);
                        if (wMsg) {
                            MessageBoxW(hWnd, wMsg, L"파일 없음", MB_OK | MB_ICONWARNING);
                            free(wMsg);
                        }
                        free(u8Msg);
                    }
                    recent_remove(idx);
                }
            }
            return 0;
        }

        case IDM_RECENT_CLEAR:
            g_recentCount = 0;
            settings_save();
            update_recent_menu();
            return 0;

        /* Edit commands */
        case IDM_EDIT_COPY:
            copy_selection_to_clipboard();
            return 0;

        case IDM_EDIT_SELECT_ALL:
            if (g_app.layout && g_app.layout->textlen > 0) {
                g_app.selStart = 0;
                g_app.selEnd = g_app.layout->textlen;
                g_app.selAnchor = 0;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;

        case IDM_EDIT_FIND:
            search_open();
            return 0;

        case IDM_EDIT_FIND_NEXT:
            if (g_search.active) search_next();
            else search_open();
            return 0;

        case IDM_EDIT_FIND_PREV:
            if (g_search.active) search_prev();
            else search_open();
            return 0;

        case IDM_FILE_EXPORT_PDF: {
            OPENFILENAMEW ofn = {0};
            wchar_t *fileBuf = (wchar_t *)calloc(LONG_PATH_MAX, sizeof(wchar_t));
            char err[256] = {0};
            if (!fileBuf) return 0;
            if (g_app.currentPath[0]) {
                char *defName = path_with_ext(path_name(g_app.currentPath), ".pdf");
                wchar_t *wdef = utf8_to_utf16(defName ? defName : "Document.pdf", -1, NULL);
                if (wdef) {
                    wcsncpy_s(fileBuf, LONG_PATH_MAX, wdef, _TRUNCATE);
                    free(wdef);
                }
                free(defName);
            } else {
                wcscpy_s(fileBuf, LONG_PATH_MAX, L"Document.pdf");
            }
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"PDF 문서 (*.pdf)\0*.pdf\0모든 파일 (*.*)\0*.*\0";
            ofn.lpstrFile = fileBuf;
            ofn.nMaxFile = LONG_PATH_MAX;
            ofn.lpstrDefExt = L"pdf";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                char *u8Out = utf16_to_utf8(fileBuf, -1, NULL);
                if (u8Out && export_pdf(g_app.doc, u8Out, err, sizeof(err)) == 0) {
                    MessageBoxW(hWnd, L"PDF 문서를 성공적으로 내보냈습니다.", L"완료", MB_OK | MB_ICONINFORMATION);
                } else {
                    wchar_t *werr = err[0] ? utf8_to_utf16(err, -1, NULL) : NULL;
                    MessageBoxW(hWnd, werr ? werr : L"PDF 내보내기 실패", L"오류", MB_OK | MB_ICONERROR);
                    free(werr);
                }
                free(u8Out);
            }
            free(fileBuf);
            return 0;
        }

        case IDM_FILE_EXPORT_DOCX: {
            OPENFILENAMEW ofn = {0};
            wchar_t *fileBuf = (wchar_t *)calloc(LONG_PATH_MAX, sizeof(wchar_t));
            char err[256] = {0};
            if (!fileBuf) return 0;
            if (g_app.currentPath[0]) {
                char *defName = path_with_ext(path_name(g_app.currentPath), ".docx");
                wchar_t *wdef = utf8_to_utf16(defName ? defName : "Document.docx", -1, NULL);
                if (wdef) {
                    wcsncpy_s(fileBuf, LONG_PATH_MAX, wdef, _TRUNCATE);
                    free(wdef);
                }
                free(defName);
            } else {
                wcscpy_s(fileBuf, LONG_PATH_MAX, L"Document.docx");
            }
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"Microsoft Word 문서 (*.docx)\0*.docx\0모든 파일 (*.*)\0*.*\0";
            ofn.lpstrFile = fileBuf;
            ofn.nMaxFile = LONG_PATH_MAX;
            ofn.lpstrDefExt = L"docx";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                char *u8Out = utf16_to_utf8(fileBuf, -1, NULL);
                if (u8Out && docx_write(g_app.doc, u8Out, err, sizeof(err)) == 0) {
                    MessageBoxW(hWnd, L"Word (DOCX) 문서를 성공적으로 내보냈습니다.", L"완료", MB_OK | MB_ICONINFORMATION);
                } else {
                    wchar_t *werr = err[0] ? utf8_to_utf16(err, -1, NULL) : NULL;
                    MessageBoxW(hWnd, werr ? werr : L"DOCX 내보내기 실패", L"오류", MB_OK | MB_ICONERROR);
                    free(werr);
                }
                free(u8Out);
            }
            free(fileBuf);
            return 0;
        }

        case IDM_FILE_REGISTER:
            register_file_association(1);
            MessageBoxW(hWnd, L"*.md 파일의 기본 프로그램으로 등록했습니다.\n이제 .md 파일을 더블 클릭하여 바로 열 수 있습니다.",
                        L"연결 완료", MB_OK | MB_ICONINFORMATION);
            return 0;

        case IDM_FILE_UNREGISTER:
            register_file_association(0);
            MessageBoxW(hWnd, L"*.md 파일 기본 프로그램 연결이 해제되었습니다.",
                        L"해제 완료", MB_OK | MB_ICONINFORMATION);
            return 0;

        case IDM_FILE_EXIT:
            DestroyWindow(hWnd);
            return 0;

        case IDM_VIEW_ZOOMIN:
            if (g_app.baseSize < 32.0f) {
                g_app.baseSize += 2.0f;
                rebuild_layout();
            }
            return 0;

        case IDM_VIEW_ZOOMOUT:
            if (g_app.baseSize > 10.0f) {
                g_app.baseSize -= 2.0f;
                rebuild_layout();
            }
            return 0;

        case IDM_VIEW_ZOOMRESET:
            g_app.baseSize = 15.0f;
            rebuild_layout();
            return 0;

        case IDM_VIEW_DARKMODE:
            g_app.darkMode = !g_app.darkMode;
            if (g_search.hPanel) {
                RedrawWindow(g_search.hPanel, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            }
            rebuild_layout();
            return 0;

        case IDM_VIEW_REFRESH:
            if (g_app.currentPath[0]) {
                char *copy = _strdup(g_app.currentPath);
                if (copy) {
                    open_file(copy);
                    free(copy);
                }
            } else {
                load_sample_content();
            }
            return 0;

        case IDM_SETTINGS_FONT:
            DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_FONT_DIALOG), hWnd, FontDlgProc);
            return 0;

        case IDM_SETTINGS_BODY_FONT: {
            char face[128];
            strncpy_s(face, sizeof(face), font_get_family_face(FAM_BODY), _TRUNCATE);
            if (pick_font_face(hWnd, face, sizeof(face), 0)) {
                font_set_family_face(FAM_BODY, face);
                settings_save();
                rebuild_layout();
            }
            return 0;
        }

        case IDM_SETTINGS_MONO_FONT: {
            char face[128];
            strncpy_s(face, sizeof(face), font_get_family_face(FAM_MONO), _TRUNCATE);
            if (pick_font_face(hWnd, face, sizeof(face), 1)) {
                font_set_family_face(FAM_MONO, face);
                settings_save();
                rebuild_layout();
            }
            return 0;
        }

        case IDM_SETTINGS_RESET_FONT: {
            font_set_family_chain(FAM_BODY, NULL);
            font_set_family_chain(FAM_MONO, NULL);
            settings_save();
            rebuild_layout();
            MessageBoxW(hWnd, L"글꼴 설정이 기본값으로 초기화되었습니다.",
                        L"글꼴 초기화", MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        case IDM_HELP_ABOUT:
            MessageBoxW(hWnd,
                        L"Lilac the Markdown Viewer\n\n"
                        L"by ym2413-opll, Version 1.3.0.\n",
                        L"정보", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_RETURN:
            if (g_search.active) {
                if (GetKeyState(VK_SHIFT) & 0x8000) search_prev();
                else search_next();
                return 0;
            }
            break;
        case VK_ESCAPE:
            if (g_search.active) {
                search_close();
                return 0;
            }
            if (g_app.selStart != g_app.selEnd) {
                g_app.selStart = -1;
                g_app.selEnd = -1;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            break;
        case VK_UP: do_scroll(g_app.scrollY - 40); return 0;
        case VK_DOWN: do_scroll(g_app.scrollY + 40); return 0;
        case VK_PRIOR: do_scroll(g_app.scrollY - g_app.clientH); return 0;
        case VK_NEXT: do_scroll(g_app.scrollY + g_app.clientH); return 0;
        case VK_HOME: do_scroll(0); return 0;
        case VK_END: do_scroll(g_app.maxScrollY); return 0;
        }
        break;

    case WM_DESTROY:
        if (g_search.hFont) DeleteObject(g_search.hFont);
        if (g_search.matches) free(g_search.matches);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

/* ---------------- Entry Point ---------------- */

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
                    _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    WNDCLASSEXW wc = {0};
    MSG msg;
    HACCEL hAccel;
    HWND hWnd;
    int argc = 0, i;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = NULL;

    (void)hPrevInstance;
    (void)lpCmdLine;

    if (argc > 0 && wargv) {
        argv = (char **)calloc((size_t)argc, sizeof(char *));
        if (argv) {
            for (i = 0; i < argc; i++) {
                argv[i] = utf16_to_utf8(wargv[i], -1, NULL);
            }
        }
        LocalFree(wargv);
    }

    /* Handle command-line arguments */
    if (argc >= 2 && argv && argv[1]) {
        if (!ascii_icmp(argv[1], "--register") || !ascii_icmp(argv[1], "/register")) {
            register_file_association(1);
            MessageBoxW(NULL, L"Lilac이 *.md 파일 기본 프로그램으로 성공적으로 등록되었습니다.", L"Lilac", MB_OK | MB_ICONINFORMATION);
            for (i = 0; i < argc; i++) free(argv[i]);
            free(argv);
            return 0;
        }
        if (!ascii_icmp(argv[1], "--unregister") || !ascii_icmp(argv[1], "/unregister")) {
            register_file_association(0);
            MessageBoxW(NULL, L"Lilac *.md 파일 연결이 해제되었습니다.", L"Lilac", MB_OK | MB_ICONINFORMATION);
            for (i = 0; i < argc; i++) free(argv[i]);
            free(argv);
            return 0;
        }
        if (!ascii_icmp(argv[1], "--export-pdf") && argc >= 4 && argv[2] && argv[3]) {
            char err[256] = {0};
            Doc *doc;
            font_init();
            settings_load();
            doc = doc_load_file(argv[2]);
            if (!doc) {
                for (i = 0; i < argc; i++) free(argv[i]);
                free(argv);
                return 2;
            }
            if (export_pdf(doc, argv[3], err, sizeof(err)) != 0) {
                doc_free(doc);
                font_shutdown();
                for (i = 0; i < argc; i++) free(argv[i]);
                free(argv);
                return 3;
            }
            doc_free(doc);
            font_shutdown();
            for (i = 0; i < argc; i++) free(argv[i]);
            free(argv);
            return 0;
        }
        if (!ascii_icmp(argv[1], "--export-docx") && argc >= 4 && argv[2] && argv[3]) {
            char err[256] = {0};
            Doc *doc;
            font_init();
            settings_load();
            doc = doc_load_file(argv[2]);
            if (!doc) {
                for (i = 0; i < argc; i++) free(argv[i]);
                free(argv);
                return 2;
            }
            if (docx_write(doc, argv[3], err, sizeof(err)) != 0) {
                doc_free(doc);
                font_shutdown();
                for (i = 0; i < argc; i++) free(argv[i]);
                free(argv);
                return 3;
            }
            doc_free(doc);
            font_shutdown();
            for (i = 0; i < argc; i++) free(argv[i]);
            free(argv);
            return 0;
        }
    }

    /* Initialize application state */
    memset(&g_app, 0, sizeof g_app);
    g_app.baseSize = 15.0f;
    g_app.darkMode = 0;
    g_app.hoverLink = -1;
    g_app.selStart = -1;
    g_app.selEnd = -1;
    g_app.selAnchor = -1;
    g_app.pendingLink = -1;
    g_app.hHandCursor = LoadCursor(NULL, IDC_HAND);
    g_app.hArrowCursor = LoadCursor(NULL, IDC_ARROW);
    g_app.hIBeamCursor = LoadCursor(NULL, IDC_IBEAM);

    font_init();
    settings_load();

    /* Register Window Class */
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    wc.hCursor = g_app.hArrowCursor;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = g_className;
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);

    if (!RegisterClassExW(&wc)) {
        if (argv) {
            for (i = 0; i < argc; i++) free(argv[i]);
            free(argv);
        }
        return 1;
    }

    hWnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        g_className,
        L"Lilac",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1000, 750,
        NULL, NULL, hInstance, NULL);

    if (!hWnd) {
        if (argv) {
            for (i = 0; i < argc; i++) free(argv[i]);
            free(argv);
        }
        return 1;
    }

    g_app.hWnd = hWnd;

    /* If a file was passed on the command line (e.g. double click), open it */
    if (argc >= 2 && argv && argv[1] && argv[1][0] != '-' && argv[1][0] != '/') {
        open_file(argv[1]);
    } else {
        load_sample_content();
    }
    if (argv) {
        for (i = 0; i < argc; i++) free(argv[i]);
        free(argv);
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_ACCEL));

    while (GetMessageW(&msg, NULL, 0, 0)) {
        HWND hFocus = GetFocus();
        if (hFocus == g_search.hEdit) {
            if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                continue;
            }
            if (msg.message == WM_CHAR && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
                continue;
            }
            if (msg.message == WM_KEYDOWN && (msg.wParam == 'C' || msg.wParam == 'c' || msg.wParam == 'A' || msg.wParam == 'a' || msg.wParam == 'V' || msg.wParam == 'v') && (GetKeyState(VK_CONTROL) & 0x8000)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                continue;
            }
        }
        if (!TranslateAcceleratorW(hWnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_app.layout) layout_free(g_app.layout);
    if (g_app.doc) doc_free(g_app.doc);
    font_shutdown();

    return (int)msg.wParam;
}
