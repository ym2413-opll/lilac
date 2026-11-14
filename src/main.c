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
    "이 페이지는 파일을 열기 전 기본으로 표시되는 화면입니다. 위쪽 메뉴에서 Markdown 파일을 열어 주십시오.\n";

#define LONG_PATH_MAX   32768
#define MAX_SPLIT_PANES 6
#define SPLITTER_W      5
#define SCROLLBAR_W     10
#define TOC_HEADER_H    34
#define TOC_ITEM_H      26
#define TOC_MIN_W       160
#define TOC_MAX_W       440
#define TOC_TRIGGER_PX  12
#define TOC_HIDE_MARGIN 18
#define IDT_TOC_ANIM    1
#define TOC_ANIM_DUR    0.22f
#define PANE_MIN_W      140

enum {
    DRAG_NONE = 0,
    DRAG_SELECT = 1,
    DRAG_PANE_SB = 2,
    DRAG_TOC_SB = 3,
    DRAG_TOC_SPLIT = 4,
    DRAG_PANE_SPLIT = 5
};

enum {
    SPLIT_HIT_NONE = -1,
    SPLIT_HIT_TOC  = 100
};

typedef struct DocPane {
    Layout *layout;
    RECT    rect;           /* pane rect in client coordinates */
    RECT    closeRect;      /* subtle top-right close button rect (when paneCount > 1) */
    RECT    sbTrackRect;    /* vertical scrollbar track rect in client coords */
    RECT    sbThumbRect;    /* vertical scrollbar thumb rect in client coords */
    float   widthRatio;     /* fraction of pane area width (sum == 1.0) */
    int     scrollY;
    int     maxScrollY;
    int     selStart;       /* UTF-8 byte offsets in layout->text */
    int     selEnd;
    int     selAnchor;
    int     hoverLink;
    int     hoverText;
    int     pendingLink;
} DocPane;

typedef struct TocEntry {
    int   level;            /* 1..6 */
    char  anchorName[256];
    char  title[512];
} TocEntry;

typedef struct AppState {
    HWND     hWnd;
    Doc     *doc;
    char     currentPath[LONG_PATH_MAX];
    float    baseSize;
    int      darkMode;
    int      clientW, clientH;

    DocPane  panes[MAX_SPLIT_PANES];
    int      paneCount;
    int      activePane;
    int      docAreaLeft;
    int      docAreaRight;

    /* TOC sidebar */
    int      tocVisible;
    int      tocSide;       /* 0 = left, 1 = right */
    int      tocWidth;
    int      tocOpen;
    float    tocAnim;       /* 0.0 (hidden) .. 1.0 (shown) */
    int      tocAnimActive;
    LARGE_INTEGER tocAnimLastQpc;
    float    uiFontPx;
    int      tocScrollY;
    int      tocMaxScrollY;
    int      tocHoverIdx;
    int      tocHoverBtn;   /* 0 = none, 1 = swap side, 2 = close */
    int      tocMinLevel;
    RECT     tocRect;
    RECT     tocSplitRect;
    RECT     tocSwapBtnRect;
    RECT     tocCloseBtnRect;
    RECT     tocSbTrackRect;
    RECT     tocSbThumbRect;
    TocEntry *tocItems;
    int      tocCount;
    int      tocCap;
    HFONT    hUiFont;
    HFONT    hUiBoldFont;

    /* Drag & hover states */
    int      dragMode;
    int      dragIdx;
    int      dragStartCoord;
    int      dragStartVal;
    float    dragStartRatioA;
    float    dragStartRatioB;
    POINT    downPt;
    int      hoverClosePane;
    int      hoverPaneSb;
    int      hoverTocSb;
    int      hoverSplitter;
    int      trackingMouse;

    HCURSOR  hHandCursor;
    HCURSOR  hArrowCursor;
    HCURSOR  hIBeamCursor;
    HCURSOR  hSizeWECursor;
} AppState;

static AppState g_app;

static DocPane *active_pane(void)
{
    if (g_app.activePane < 0 || g_app.activePane >= g_app.paneCount) {
        g_app.activePane = 0;
    }
    return &g_app.panes[g_app.activePane];
}

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
    DWORD type = 0;
    wchar_t *wbuf;
    char *u8;
    if (!out || outCap <= 0) return 0;
    out[0] = 0;
    if (RegQueryValueExW(hKey, name, NULL, &type, NULL, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) ||
        bytes == 0 || bytes > 65536u || (bytes % sizeof(wchar_t)) != 0) {
        return 0;
    }
    wbuf = (wchar_t *)calloc((bytes / sizeof(wchar_t)) + 2, sizeof(wchar_t));
    if (!wbuf) return 0;
    if (RegQueryValueExW(hKey, name, NULL, &type, (LPBYTE)wbuf, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
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
    DWORD pathLen;
    HKEY hKey;

    if (reg) {
        pathLen = GetModuleFileNameW(NULL, exePath, MAX_PATH);
        if (pathLen == 0 || pathLen >= MAX_PATH) return -1;
        swprintf_s(cmdBuf, MAX_PATH + 32, L"\"%ls\" \"%%1\"", exePath);
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

        /* TOC preferences */
        valSz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"TocVisible", NULL, NULL, (LPBYTE)&val, &valSz) == ERROR_SUCCESS) {
            g_app.tocVisible = val ? 1 : 0;
        }
        valSz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"TocSide", NULL, NULL, (LPBYTE)&val, &valSz) == ERROR_SUCCESS) {
            g_app.tocSide = (val == 1) ? 1 : 0;
        }
        valSz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"TocWidth", NULL, NULL, (LPBYTE)&val, &valSz) == ERROR_SUCCESS) {
            int w = (int)val;
            if (w < TOC_MIN_W) w = TOC_MIN_W;
            if (w > TOC_MAX_W) w = TOC_MAX_W;
            g_app.tocWidth = w;
        }

        /* Recent Files */
        valSz = sizeof(DWORD);
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
        DWORD tVis = (DWORD)(g_app.tocVisible ? 1 : 0);
        DWORD tSide = (DWORD)(g_app.tocSide ? 1 : 0);
        DWORD tWidth = (DWORD)g_app.tocWidth;
        int i;

        win_reg_set_sz(hKey, L"BodyFont", b);
        win_reg_set_sz(hKey, L"MonoFont", m);
        win_reg_set_sz(hKey, L"BodyFontChain", bChain);
        win_reg_set_sz(hKey, L"MonoFontChain", mChain);

        RegSetValueExW(hKey, L"TocVisible", 0, REG_DWORD, (const BYTE *)&tVis, sizeof(DWORD));
        RegSetValueExW(hKey, L"TocSide", 0, REG_DWORD, (const BYTE *)&tSide, sizeof(DWORD));
        RegSetValueExW(hKey, L"TocWidth", 0, REG_DWORD, (const BYTE *)&tWidth, sizeof(DWORD));
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
        AppendMenuW(hRecentMenu, MF_STRING, IDM_RECENT_CLEAR, L"목록 비우기(&C)");
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

static void pane_scroll(int paneIdx, int newPos);
static void do_scroll(int newPos);
static void update_search_panel_pos(int show);

static int is_word_cp(uint32_t cp)
{
    if (cp <= 32) return 0;
    if (cp < 128 && strchr(" \t\r\n.,()[]{}<>\"';:!?`~|/\\=+-*&^%$#@", (int)cp)) return 0;
    return 1;
}

static void copy_selection_to_clipboard(void)
{
    DocPane *p = active_pane();
    int s, e, len, i, extra = 0, wlen = 0;
    char *norm;
    wchar_t *wtext;
    HGLOBAL hMem;

    if (!p->layout || !p->layout->text) return;
    s = min(p->selStart, p->selEnd);
    e = max(p->selStart, p->selEnd);
    if (s < 0 || e <= s || s >= p->layout->textlen) return;
    if (e > p->layout->textlen) e = p->layout->textlen;
    len = e - s;
    if (len <= 0) return;

    /* Count lone \n to convert to \r\n in UTF-8 buffer */
    for (i = 0; i < len; i++) {
        if (p->layout->text[s + i] == '\n') {
            if (i == 0 || p->layout->text[s + i - 1] != '\r') {
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
            char c = p->layout->text[s + i];
            if (c == '\n' && (i == 0 || p->layout->text[s + i - 1] != '\r') && outIdx + 1 < cap) {
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

static void search_jump_to(int matchIdx)
{
    DocPane *p = active_pane();
    char countBuf[32];
    float my = 0.0f;
    if (matchIdx < 0 || matchIdx >= g_search.matchCount || !p->layout) return;
    g_search.currentIndex = matchIdx;
    layout_text_pos(p->layout, g_search.matches[matchIdx].toffStart, NULL, NULL, &my);
    g_search.matches[matchIdx].y = my;
    do_scroll((int)my - g_app.clientH / 3);
    snprintf(countBuf, sizeof(countBuf), "%d / %d", matchIdx + 1, g_search.matchCount);
    win_set_window_text(g_search.hStaticCount, countBuf);
    InvalidateRect(g_search.hStaticCount, NULL, TRUE);
    UpdateWindow(g_search.hStaticCount);
    InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void search_perform(void)
{
    DocPane *p = active_pane();
    wchar_t wq[256] = {0};
    char *q = NULL;
    int qlen = 0;
    int i;
    if (!g_search.hEdit) return;
    GetWindowTextW(g_search.hEdit, wq, 256);
    q = utf16_to_utf8(wq, -1, &qlen);

    if (!q || qlen == 0 || !p->layout || !p->layout->text || p->layout->textlen == 0) {
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
    while (i <= p->layout->textlen - qlen) {
        int matchedBytes = utf8_match_here(p->layout->text + i, p->layout->textlen - i, q, qlen);
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
            layout_text_pos(p->layout, i, NULL, NULL, &my);
            g_search.matches[g_search.matchCount].y = my;
            g_search.matchCount++;
        }
        /* Advance to next UTF-8 codepoint boundary */
        (void)utf8_next(p->layout->text, p->layout->textlen, &i);
    }
    free(q);

    if (g_search.matchCount > 0) {
        int best = 0;
        for (i = 0; i < g_search.matchCount; i++) {
            if (g_search.matches[i].y >= (float)p->scrollY) {
                best = i;
                break;
            }
        }
        search_jump_to(best);
    } else {
        g_search.currentIndex = -1;
        win_set_window_text(g_search.hStaticCount, "일치 없음");
        InvalidateRect(g_search.hStaticCount, NULL, TRUE);
        UpdateWindow(g_search.hStaticCount);
        InvalidateRect(g_app.hWnd, NULL, FALSE);
    }
}

static void search_step(int delta)
{
    if (g_search.matchCount > 0) {
        int next = (g_search.currentIndex + delta + g_search.matchCount) % g_search.matchCount;
        search_jump_to(next);
    }
}

static void search_next(void) { search_step(1); }
static void search_prev(void) { search_step(-1); }

static void search_update_font(void);

static void search_open(void)
{
    g_search.active = 1;
    if (g_search.hPanel) {
        search_update_font();
        update_search_panel_pos(1);
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

/* ---------------- Layout Geometry, TOC & Split Panes ---------------- */

static void calc_scrollbar_thumb(const RECT *track, int contentH, int viewH,
                                 int scrollY, int maxScrollY, RECT *outThumb)
{
    int trackH, usable, thumbH, travel, thumbTop;
    SetRectEmpty(outThumb);
    if (!track || maxScrollY <= 0 || viewH <= 0 || contentH <= viewH) return;
    trackH = track->bottom - track->top;
    if (trackH <= 8) return;
    usable = trackH - 4;
    thumbH = (int)((double)viewH * usable / (double)contentH);
    if (thumbH < 24) thumbH = 24;
    if (thumbH > usable) thumbH = usable;
    travel = usable - thumbH;
    thumbTop = track->top + 2 + (travel > 0 ? (int)((double)scrollY * travel / (double)maxScrollY + 0.5) : 0);
    SetRect(outThumb, track->left, thumbTop, track->right, thumbTop + thumbH);
}

static int scrollbar_pos_from_y(const RECT *track, const RECT *thumb, int maxScrollY, int y)
{
    int usable = (track->bottom - track->top) - 4;
    int thumbH = thumb->bottom - thumb->top;
    int travel = usable - thumbH;
    int rel;
    if (travel <= 0 || maxScrollY <= 0) return 0;
    rel = y - (track->top + 2) - thumbH / 2;
    if (rel < 0) rel = 0;
    if (rel > travel) rel = travel;
    return (int)((double)rel * maxScrollY / (double)travel + 0.5);
}

static void draw_scrollbar(HDC hdc, const RECT *track, const RECT *thumb, int hovered, int dark)
{
    COLORREF thumbCol;
    HBRUSH br;
    HPEN nullPen;
    HGDIOBJ oldP, oldB;
    int inset;
    if (thumb->bottom <= thumb->top) return;

    if (hovered) {
        HBRUSH trBr = CreateSolidBrush(dark ? RGB(26, 31, 38) : RGB(240, 242, 245));
        FillRect(hdc, track, trBr);
        DeleteObject(trBr);
    }

    thumbCol = dark
        ? (hovered ? RGB(115, 125, 138) : RGB(72, 80, 90))
        : (hovered ? RGB(145, 152, 162) : RGB(192, 198, 206));
    inset = hovered ? 1 : 2;
    br = CreateSolidBrush(thumbCol);
    nullPen = (HPEN)GetStockObject(NULL_PEN);
    oldP = SelectObject(hdc, nullPen);
    oldB = SelectObject(hdc, br);
    RoundRect(hdc, track->left + inset, thumb->top, track->right - inset + 1, thumb->bottom + 1, 6, 6);
    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(br);
}

static void pane_init(DocPane *p, float ratio)
{
    memset(p, 0, sizeof(*p));
    p->widthRatio = ratio;
    p->selStart = -1;
    p->selEnd = -1;
    p->selAnchor = -1;
    p->hoverLink = -1;
    p->pendingLink = -1;
}

static void pane_free_layout(DocPane *p)
{
    if (p->layout) {
        layout_free(p->layout);
        p->layout = NULL;
    }
}

static void pane_update_thumb(DocPane *p)
{
    int contentH = p->layout ? (int)p->layout->pageHeight : 0;
    int viewH = p->rect.bottom - p->rect.top;
    calc_scrollbar_thumb(&p->sbTrackRect, contentH, viewH, p->scrollY, p->maxScrollY, &p->sbThumbRect);
}

static void toc_update_thumb(void)
{
    int listTop = g_app.tocRect.top + TOC_HEADER_H;
    int viewH = g_app.tocRect.bottom - listTop;
    int contentH = g_app.tocCount * TOC_ITEM_H + 12;
    calc_scrollbar_thumb(&g_app.tocSbTrackRect, contentH, viewH, g_app.tocScrollY, g_app.tocMaxScrollY, &g_app.tocSbThumbRect);
}

static void ui_update_fonts(void)
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
    fontHeight = -MulDiv(9, dpi, 72);
    g_app.uiFontPx = 12.5f * ((float)dpi / 96.0f);

    if (g_app.hUiFont) { DeleteObject(g_app.hUiFont); g_app.hUiFont = NULL; }
    if (g_app.hUiBoldFont) { DeleteObject(g_app.hUiBoldFont); g_app.hUiBoldFont = NULL; }

    wface = utf8_to_utf16(face, -1, NULL);
    g_app.hUiFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                wface ? wface : L"맑은 고딕");
    g_app.hUiBoldFont = CreateFontW(fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                    wface ? wface : L"맑은 고딕");
    free(wface);
}

static int get_effective_toc_width(void)
{
    int maxAllowed;
    int w;
    if (!g_app.tocVisible || g_app.clientW < 280) return 0;
    maxAllowed = min(TOC_MAX_W, g_app.clientW - 180);
    if (maxAllowed < TOC_MIN_W) maxAllowed = TOC_MIN_W;
    w = g_app.tocWidth;
    if (w < TOC_MIN_W) w = TOC_MIN_W;
    if (w > maxAllowed) w = maxAllowed;
    return w;
}

static int toc_is_interactive(void)
{
    return g_app.tocVisible && g_app.tocAnim > 0.1f && !IsRectEmpty(&g_app.tocRect);
}

static void recalc_toc_rect(void)
{
    int cw = g_app.clientW;
    int ch = g_app.clientH;
    int tocW = get_effective_toc_width();
    int viewH, contentH;

    SetRectEmpty(&g_app.tocRect);
    SetRectEmpty(&g_app.tocSplitRect);
    SetRectEmpty(&g_app.tocSwapBtnRect);
    SetRectEmpty(&g_app.tocCloseBtnRect);
    SetRectEmpty(&g_app.tocSbTrackRect);

    if (tocW > 0 && g_app.tocAnim > 0.001f && cw > 0 && ch > 0) {
        float a = g_app.tocAnim;
        float eased = a * a * a * (a * (a * 6.0f - 15.0f) + 10.0f);
        int slidePx = (int)round((double)tocW * (double)eased);
        int panelLeft;

        if (g_app.tocSide == 0) {
            panelLeft = slidePx - tocW;
            SetRect(&g_app.tocRect, panelLeft, 0, panelLeft + tocW, ch);
            SetRect(&g_app.tocSplitRect, panelLeft + tocW, 0, panelLeft + tocW + SPLITTER_W, ch);
        } else {
            panelLeft = cw - slidePx;
            SetRect(&g_app.tocRect, panelLeft, 0, panelLeft + tocW, ch);
            SetRect(&g_app.tocSplitRect, panelLeft - SPLITTER_W, 0, panelLeft, ch);
        }
        SetRect(&g_app.tocCloseBtnRect, g_app.tocRect.right - 28, 6, g_app.tocRect.right - 6, 28);
        SetRect(&g_app.tocSwapBtnRect, g_app.tocRect.right - 52, 6, g_app.tocRect.right - 30, 28);
        SetRect(&g_app.tocSbTrackRect, g_app.tocRect.right - SCROLLBAR_W, TOC_HEADER_H, g_app.tocRect.right, g_app.tocRect.bottom);
    }

    viewH = ch - TOC_HEADER_H;
    contentH = g_app.tocCount * TOC_ITEM_H + 12;
    if (viewH < 0) viewH = 0;
    g_app.tocMaxScrollY = contentH - viewH;
    if (g_app.tocMaxScrollY < 0) g_app.tocMaxScrollY = 0;
    if (g_app.tocScrollY > g_app.tocMaxScrollY) g_app.tocScrollY = g_app.tocMaxScrollY;
    toc_update_thumb();
}

static void toc_set_open(int open)
{
    int target = (g_app.tocVisible && open) ? 1 : 0;
    if (g_app.tocOpen == target) {
        if ((target && g_app.tocAnim >= 1.0f) || (!target && g_app.tocAnim <= 0.0f)) return;
    }
    g_app.tocOpen = target;
    if (g_app.hWnd) {
        if (!g_app.tocAnimActive) {
            QueryPerformanceCounter(&g_app.tocAnimLastQpc);
            g_app.tocAnimActive = 1;
        }
        SetTimer(g_app.hWnd, IDT_TOC_ANIM, 10, NULL);
    } else {
        g_app.tocAnim = target ? 1.0f : 0.0f;
        g_app.tocAnimActive = 0;
        recalc_toc_rect();
    }
}

static void recalc_geometry(void)
{
    int cw = g_app.clientW;
    int ch = g_app.clientH;
    int docL = 0, docR = cw;
    int n = g_app.paneCount;
    int availW, minW, i, x;
    float sumRatio = 0.0f;
    int widths[MAX_SPLIT_PANES] = {0};

    if (n < 1) { n = 1; g_app.paneCount = 1; }
    if (n > MAX_SPLIT_PANES) { n = MAX_SPLIT_PANES; g_app.paneCount = n; }

    g_app.docAreaLeft = docL;
    g_app.docAreaRight = docR;

    availW = (docR - docL) - (n - 1) * SPLITTER_W;
    if (availW < n) availW = n;
    minW = (availW >= n * PANE_MIN_W) ? PANE_MIN_W : (availW / n);
    if (minW < 1) minW = 1;

    for (i = 0; i < n; i++) {
        if (g_app.panes[i].widthRatio <= 0.0f) g_app.panes[i].widthRatio = 1.0f / (float)n;
        sumRatio += g_app.panes[i].widthRatio;
    }
    if (sumRatio <= 0.0f) sumRatio = 1.0f;
    for (i = 0; i < n; i++) {
        g_app.panes[i].widthRatio /= sumRatio;
    }

    {
        int used = 0;
        for (i = 0; i < n - 1; i++) {
            int remMin = (n - 1 - i) * minW;
            int w = (int)round(g_app.panes[i].widthRatio * (float)availW);
            if (w < minW) w = minW;
            if (used + w + remMin > availW) w = availW - used - remMin;
            widths[i] = w;
            used += w;
        }
        widths[n - 1] = availW - used;
    }

    x = docL;
    for (i = 0; i < n; i++) {
        DocPane *p = &g_app.panes[i];
        int pw = widths[i];
        SetRect(&p->rect, x, 0, x + pw, ch);
        SetRect(&p->sbTrackRect, x + pw - SCROLLBAR_W, 0, x + pw, ch);
        if (n > 1) {
            int cx = x + pw - SCROLLBAR_W - 25;
            if (cx < x + 4) cx = x + 4;
            SetRect(&p->closeRect, cx, 6, cx + 20, 26);
        } else {
            SetRectEmpty(&p->closeRect);
        }
        pane_update_thumb(p);
        x += pw + SPLITTER_W;
    }

    recalc_toc_rect();
}

static void update_search_panel_pos(int show)
{
    int px, py;
    if (!g_search.hPanel) return;
    px = g_app.docAreaRight - 365;
    if (px < g_app.docAreaLeft + 10) px = g_app.docAreaLeft + 10;
    py = (g_app.paneCount > 1) ? 28 : 10;
    SetWindowPos(g_search.hPanel, HWND_TOP, px, py, 350, 36,
                 (show ? SWP_SHOWWINDOW : (SWP_NOACTIVATE | (g_search.active ? SWP_SHOWWINDOW : SWP_HIDEWINDOW))));
}

static void update_view_menu(void)
{
    HMENU hMain, hView;
    if (!g_app.hWnd) return;
    hMain = GetMenu(g_app.hWnd);
    if (!hMain) return;
    hView = GetSubMenu(hMain, 2); /* 0:파일, 1:편집, 2:보기 */
    if (!hView) return;

    CheckMenuItem(hMain, IDM_VIEW_DARKMODE, MF_BYCOMMAND | (g_app.darkMode ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMain, IDM_VIEW_TOC, MF_BYCOMMAND | (g_app.tocVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMain, IDM_VIEW_TOC_LEFT, MF_BYCOMMAND | (g_app.tocSide == 0 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMain, IDM_VIEW_TOC_RIGHT, MF_BYCOMMAND | (g_app.tocSide == 1 ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(hMain, IDM_VIEW_SPLIT_CLOSE, MF_BYCOMMAND | (g_app.paneCount > 1 ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMain, IDM_VIEW_SPLIT_VERT, MF_BYCOMMAND | (g_app.paneCount < MAX_SPLIT_PANES ? MF_ENABLED : MF_GRAYED));
    DrawMenuBar(g_app.hWnd);
}

static void toc_rebuild_items(void)
{
    const Layout *L = g_app.panes[0].layout;
    int i, minLvl = 6;
    g_app.tocCount = 0;
    g_app.tocMinLevel = 1;
    if (!L) return;

    for (i = 0; i < L->nanchors; i++) {
        const Anchor *a = &L->anchors[i];
        if (a->level >= 1 && a->level <= 6 && a->title && a->title[0]) {
            if (g_app.tocCount >= g_app.tocCap) {
                int nc = g_app.tocCap ? g_app.tocCap * 2 : 32;
                TocEntry *nt = (TocEntry *)realloc(g_app.tocItems, (size_t)nc * sizeof(TocEntry));
                if (!nt) break;
                g_app.tocItems = nt;
                g_app.tocCap = nc;
            }
            {
                TocEntry *te = &g_app.tocItems[g_app.tocCount++];
                te->level = a->level;
                strncpy_s(te->anchorName, sizeof(te->anchorName), a->name ? a->name : "", _TRUNCATE);
                strncpy_s(te->title, sizeof(te->title), a->title, _TRUNCATE);
                if (a->level < minLvl) minLvl = a->level;
            }
        }
    }
    if (g_app.tocCount > 0) g_app.tocMinLevel = minLvl;
}

static int toc_active_heading(void)
{
    const DocPane *ap = active_pane();
    int best = -1, i;
    if (!ap->layout || g_app.tocCount <= 0) return -1;
    for (i = 0; i < g_app.tocCount; i++) {
        const Anchor *anc = layout_find_anchor(ap->layout, g_app.tocItems[i].anchorName);
        if (anc && (int)anc->y <= ap->scrollY + 48) {
            best = i;
        }
    }
    if (best < 0 && g_app.tocCount > 0) best = 0;
    return best;
}

static void toc_jump_to_index(int idx)
{
    DocPane *ap = active_pane();
    const Anchor *anc;
    if (idx < 0 || idx >= g_app.tocCount || !ap->layout) return;
    anc = layout_find_anchor(ap->layout, g_app.tocItems[idx].anchorName);
    if (anc) {
        pane_scroll(g_app.activePane, (int)anc->y - 16);
    }
}

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

static void rebuild_pane_layout(int idx, int preserveScroll)
{
    DocPane *p;
    LayoutOptions opt = {0};
    int paneW, oldMax;
    float rel = 0.0f;

    if (idx < 0 || idx >= g_app.paneCount) return;
    p = &g_app.panes[idx];
    oldMax = p->maxScrollY;
    if (preserveScroll && oldMax > 0 && p->scrollY > 0) {
        rel = (float)p->scrollY / (float)oldMax;
    }

    pane_free_layout(p);
    if (!g_app.doc) return;

    paneW = (p->rect.right - p->rect.left) - SCROLLBAR_W;
    if (paneW < 60) paneW = 60;

    opt.width = (float)paneW;
    opt.pageHeight = 0; /* Continuous mode */
    opt.marginL = 24.0f;
    opt.marginR = 24.0f;
    opt.marginT = 24.0f;
    opt.marginB = 32.0f;
    opt.maxContentWidth = 860.0f;
    opt.baseSize = g_app.baseSize;
    opt.dark = g_app.darkMode;

    p->layout = layout_build(g_app.doc, &opt);
    p->maxScrollY = p->layout ? ((int)p->layout->pageHeight - g_app.clientH) : 0;
    if (p->maxScrollY < 0) p->maxScrollY = 0;

    if (preserveScroll && oldMax > 0 && p->scrollY > 0) {
        p->scrollY = (int)round(rel * (float)p->maxScrollY);
    }
    if (p->scrollY < 0) p->scrollY = 0;
    if (p->scrollY > p->maxScrollY) p->scrollY = p->maxScrollY;
    pane_update_thumb(p);
}

static void rebuild_all_layouts(int preserveScroll)
{
    int i;
    recalc_geometry();
    for (i = 0; i < g_app.paneCount; i++) {
        rebuild_pane_layout(i, preserveScroll);
    }
    toc_rebuild_items();
    recalc_geometry();

    if (g_search.active && g_search.matchCount > 0) {
        const DocPane *ap = active_pane();
        if (ap->layout) {
            int m;
            for (m = 0; m < g_search.matchCount; m++) {
                float my = 0;
                layout_text_pos(ap->layout, g_search.matches[m].toffStart, NULL, NULL, &my);
                g_search.matches[m].y = my;
            }
        }
    }

    if (g_search.hPanel) {
        search_update_font();
        update_search_panel_pos(0);
    }
    ui_update_fonts();
    if (g_app.hWnd) InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void rebuild_layout(void)
{
    rebuild_all_layouts(1);
}

static void pane_split_active(void)
{
    int n = g_app.paneCount;
    int docW = g_app.docAreaRight - g_app.docAreaLeft;
    int ins, i, copyScroll;
    if (n >= MAX_SPLIT_PANES) return;
    if (docW > 0 && (docW - n * SPLITTER_W) / (n + 1) < 120) return;

    ins = g_app.activePane + 1;
    if (ins < 1) ins = 1;
    if (ins > n) ins = n;
    copyScroll = g_app.panes[g_app.activePane].scrollY;

    for (i = 0; i < n; i++) {
        g_app.panes[i].widthRatio *= (float)n / (float)(n + 1);
    }
    for (i = n; i > ins; i--) {
        g_app.panes[i] = g_app.panes[i - 1];
    }
    pane_init(&g_app.panes[ins], 1.0f / (float)(n + 1));
    g_app.panes[ins].scrollY = copyScroll;
    g_app.paneCount = n + 1;
    g_app.activePane = ins;

    recalc_geometry();
    for (i = 0; i < g_app.paneCount; i++) {
        rebuild_pane_layout(i, i != ins);
    }
    recalc_geometry();
    update_search_panel_pos(0);
    update_view_menu();
    if (g_app.hWnd) InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void pane_close(int idx)
{
    int i;
    float sum = 0.0f;
    if (g_app.paneCount <= 1 || idx < 0 || idx >= g_app.paneCount) return;

    pane_free_layout(&g_app.panes[idx]);
    for (i = idx; i < g_app.paneCount - 1; i++) {
        g_app.panes[i] = g_app.panes[i + 1];
    }
    pane_init(&g_app.panes[g_app.paneCount - 1], 0.0f);
    g_app.paneCount--;

    for (i = 0; i < g_app.paneCount; i++) sum += g_app.panes[i].widthRatio;
    if (sum > 0.0f) {
        for (i = 0; i < g_app.paneCount; i++) g_app.panes[i].widthRatio /= sum;
    }

    if (g_app.activePane == idx && g_app.activePane >= g_app.paneCount) {
        g_app.activePane = g_app.paneCount - 1;
    } else if (g_app.activePane > idx) {
        g_app.activePane--;
    }

    g_app.hoverClosePane = -1;
    g_app.hoverPaneSb = -1;
    g_app.hoverSplitter = SPLIT_HIT_NONE;
    if (g_app.dragMode == DRAG_SELECT || g_app.dragMode == DRAG_PANE_SB || g_app.dragMode == DRAG_PANE_SPLIT) {
        if (g_app.dragMode != DRAG_NONE) ReleaseCapture();
        g_app.dragMode = DRAG_NONE;
    }

    rebuild_all_layouts(1);
    update_view_menu();
}

static int pane_at_point(int x, int y)
{
    int i;
    POINT pt = {x, y};
    for (i = 0; i < g_app.paneCount; i++) {
        if (PtInRect(&g_app.panes[i].rect, pt)) return i;
    }
    return -1;
}

static void open_file(const char *path)
{
    Doc *newDoc = doc_load_file(path);
    int i;
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

    if (g_app.dragMode != DRAG_NONE) {
        ReleaseCapture();
        g_app.dragMode = DRAG_NONE;
        g_app.dragIdx = -1;
    }
    if (g_app.doc) doc_free(g_app.doc);
    g_app.doc = newDoc;
    strncpy_s(g_app.currentPath, LONG_PATH_MAX, path, _TRUNCATE);
    doc_resolve_images(g_app.doc, 0);

    recent_add(path);
    for (i = 0; i < g_app.paneCount; i++) {
        g_app.panes[i].selStart = -1;
        g_app.panes[i].selEnd = -1;
        g_app.panes[i].selAnchor = -1;
        g_app.panes[i].scrollY = 0;
        g_app.panes[i].pendingLink = -1;
    }
    g_app.tocScrollY = 0;
    update_window_title();
    rebuild_all_layouts(0);
    if (g_search.active) search_perform();
}

static void load_sample_content(void)
{
    int i;
    if (g_app.dragMode != DRAG_NONE) {
        ReleaseCapture();
        g_app.dragMode = DRAG_NONE;
        g_app.dragIdx = -1;
    }
    if (g_app.doc) doc_free(g_app.doc);
    g_app.doc = doc_parse(g_sampleMarkdown, strlen(g_sampleMarkdown), NULL);
    g_app.currentPath[0] = 0;
    for (i = 0; i < g_app.paneCount; i++) {
        g_app.panes[i].selStart = -1;
        g_app.panes[i].selEnd = -1;
        g_app.panes[i].selAnchor = -1;
        g_app.panes[i].scrollY = 0;
        g_app.panes[i].pendingLink = -1;
    }
    g_app.tocScrollY = 0;
    update_window_title();
    rebuild_all_layouts(0);
    if (g_search.active) search_perform();
}

/* ---------------- GDI Rendering ---------------- */

static void render_display_list(HDC hdc, const DocPane *pane, int viewW, int viewH)
{
    const Layout *L = pane->layout;
    int scrollY = pane->scrollY;
    RECT rc = {0, 0, viewW, viewH};
    HBRUSH bgBrush;
    int i, layer;
    if (!L) return;

    bgBrush = CreateSolidBrush(RGB((L->background >> 16) & 0xFF, (L->background >> 8) & 0xFF, L->background & 0xFF));
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
            if (itemBot < -50.0f || itemTop > (float)viewH + 100.0f) continue;

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
                    if (pane->selStart >= 0 && pane->selEnd >= 0 && pane->selStart != pane->selEnd && it->toff) {
                        int s1 = min(pane->selStart, pane->selEnd);
                        int s2 = max(pane->selStart, pane->selEnd);
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

static void draw_cross_icon(HDC hdc, const RECT *r, COLORREF col)
{
    int cx = (r->left + r->right) / 2;
    int cy = (r->top + r->bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ oldP = SelectObject(hdc, pen);
    MoveToEx(hdc, cx - 3, cy - 3, NULL);
    LineTo(hdc, cx + 4, cy + 4);
    MoveToEx(hdc, cx + 3, cy - 3, NULL);
    LineTo(hdc, cx - 4, cy + 4);
    SelectObject(hdc, oldP);
    DeleteObject(pen);
}

static void paint_pane(HDC hdc, int idx)
{
    const DocPane *p = &g_app.panes[idx];
    int viewW = p->rect.right - p->rect.left;
    int viewH = p->rect.bottom - p->rect.top;
    int saved;

    if (viewW <= 0 || viewH <= 0) return;

    saved = SaveDC(hdc);
    IntersectClipRect(hdc, p->rect.left, p->rect.top, p->rect.right, p->rect.bottom);
    SetViewportOrgEx(hdc, p->rect.left, p->rect.top, NULL);
    render_display_list(hdc, p, viewW, viewH);
    RestoreDC(hdc, saved);

    /* Subtle active-pane top indicator when split */
    if (g_app.paneCount > 1) {
        RECT topBar = {p->rect.left, p->rect.top, p->rect.right, p->rect.top + (idx == g_app.activePane ? 2 : 1)};
        COLORREF barCol = (idx == g_app.activePane)
            ? (g_app.darkMode ? RGB(88, 166, 255) : RGB(9, 105, 218))
            : (g_app.darkMode ? RGB(33, 38, 45) : RGB(234, 238, 242));
        HBRUSH br = CreateSolidBrush(barCol);
        FillRect(hdc, &topBar, br);
        DeleteObject(br);
    }

    draw_scrollbar(hdc, &p->sbTrackRect, &p->sbThumbRect,
                   (g_app.hoverPaneSb == idx || (g_app.dragMode == DRAG_PANE_SB && g_app.dragIdx == idx)),
                   g_app.darkMode);

    /* Achromatic close button with clear contrast at top-right of each split pane */
    if (g_app.paneCount > 1 && p->closeRect.right > p->closeRect.left) {
        int hovered = (g_app.hoverClosePane == idx);
        COLORREF fillCol = hovered
            ? (g_app.darkMode ? RGB(64, 64, 64) : RGB(222, 222, 222))
            : (g_app.darkMode ? RGB(36, 36, 36) : RGB(242, 242, 242));
        COLORREF rimCol = hovered
            ? (g_app.darkMode ? RGB(112, 112, 112) : RGB(158, 158, 158))
            : (g_app.darkMode ? RGB(72, 72, 72) : RGB(202, 202, 202));
        COLORREF xCol = hovered
            ? (g_app.darkMode ? RGB(242, 242, 242) : RGB(32, 32, 32))
            : (g_app.darkMode ? RGB(172, 172, 172) : RGB(105, 105, 105));
        HBRUSH br = CreateSolidBrush(fillCol);
        HPEN pen = CreatePen(PS_SOLID, 1, rimCol);
        HGDIOBJ oldP = SelectObject(hdc, pen);
        HGDIOBJ oldB = SelectObject(hdc, br);
        RoundRect(hdc, p->closeRect.left, p->closeRect.top, p->closeRect.right, p->closeRect.bottom, 5, 5);
        SelectObject(hdc, oldB);
        SelectObject(hdc, oldP);
        DeleteObject(pen);
        DeleteObject(br);
        draw_cross_icon(hdc, &p->closeRect, xCol);
    }
}

static void paint_splitter(HDC hdc, int leftX, int height, int highlighted)
{
    RECT sr = {leftX, 0, leftX + SPLITTER_W, height};
    RECT lineR = {leftX + SPLITTER_W / 2, 0, leftX + SPLITTER_W / 2 + 1, height};
    HBRUSH bgBr = CreateSolidBrush(g_app.darkMode ? RGB(22, 27, 34) : RGB(243, 244, 246));
    HBRUSH lineBr = CreateSolidBrush(highlighted
        ? (g_app.darkMode ? RGB(88, 166, 255) : RGB(9, 105, 218))
        : (g_app.darkMode ? RGB(48, 54, 61) : RGB(208, 215, 222)));
    FillRect(hdc, &sr, bgBr);
    FillRect(hdc, &lineR, lineBr);
    DeleteObject(lineBr);
    DeleteObject(bgBr);
}

static void paint_toc_edge_tab(HDC hdc)
{
    int cw = g_app.clientW;
    int ch = g_app.clientH;
    int barH = 48;
    int cy = (ch - barH) / 2;
    int x0;
    COLORREF tabCol = g_app.darkMode ? RGB(68, 76, 86) : RGB(196, 204, 212);
    HBRUSH br;
    HPEN nullPen;
    HGDIOBJ oP, oB;

    if (!g_app.tocVisible || get_effective_toc_width() <= 0 || ch < 80 || cw < 40) return;
    x0 = (g_app.tocSide == 0) ? 2 : (cw - 6);
    br = CreateSolidBrush(tabCol);
    nullPen = (HPEN)GetStockObject(NULL_PEN);
    oP = SelectObject(hdc, nullPen);
    oB = SelectObject(hdc, br);
    RoundRect(hdc, x0, cy, x0 + 4, cy + barH, 4, 4);
    SelectObject(hdc, oB);
    SelectObject(hdc, oP);
    DeleteObject(br);
}

typedef struct {
    Font *f;
    uint16_t gid;
    float adv;
} TocGlyph;

/* Render UTF-8 text using the specified FontFamily fallback chain with ellipsis truncation. */
static void paint_chain_text(HDC hdc, int family, int bold, float pxSize, const char *utf8,
                             const RECT *r, COLORREF col, int centerAlign)
{
    TocGlyph gs[256];
    int ng = 0, pos = 0, len;
    float totalW = 0.0f, availW;
    Font *pf;
    float asc, dsc, midY, x;
    int baseY, oldAlign, pixelSize, i;
    COLORREF oldCol;
    HGDIOBJ oldF;

    if (!utf8 || !utf8[0] || !r || r->right <= r->left || r->bottom <= r->top) return;
    if (pxSize < 8.0f) pxSize = 12.5f;
    availW = (float)(r->right - r->left);
    len = (int)strlen(utf8);

    while (pos < len && ng < 252) {
        uint32_t cp = utf8_next(utf8, len, &pos);
        int gid = 0;
        Font *f = font_resolve(family, bold, 0, cp, &gid);
        float adv = 0.0f;
        if (!f) break;
        if (f->upem > 0) adv = (float)font_adv(f, gid) * pxSize / (float)f->upem;
        gs[ng].f = f;
        gs[ng].gid = (uint16_t)gid;
        gs[ng].adv = adv;
        totalW += adv;
        ng++;
    }

    if (totalW > availW) {
        int dotGid = 0, d;
        Font *dotF = font_resolve(family, bold, 0, '.', &dotGid);
        float dotAdv = (dotF && dotF->upem > 0)
            ? ((float)font_adv(dotF, dotGid) * pxSize / (float)dotF->upem)
            : (pxSize * 0.25f);
        float dotsW = dotAdv * 3.0f;
        while (ng > 0 && (totalW + dotsW) > availW) {
            ng--;
            totalW -= gs[ng].adv;
        }
        if (dotF && dotsW <= availW) {
            for (d = 0; d < 3 && ng < 256; d++) {
                gs[ng].f = dotF;
                gs[ng].gid = (uint16_t)dotGid;
                gs[ng].adv = dotAdv;
                totalW += dotAdv;
                ng++;
            }
        }
    }

    pf = font_primary(family, bold, 0);
    asc = (pf && pf->upem > 0) ? ((float)pf->ascent * pxSize / (float)pf->upem) : (pxSize * 0.8f);
    dsc = (pf && pf->upem > 0) ? ((float)pf->descent * pxSize / (float)pf->upem) : (pxSize * 0.2f);
    midY = 0.5f * (float)(r->top + r->bottom);
    baseY = (int)round(midY + 0.5f * (asc - dsc));
    x = centerAlign ? ((float)r->left + 0.5f * (availW - totalW)) : (float)r->left;
    if (x < (float)r->left) x = (float)r->left;

    oldAlign = SetTextAlign(hdc, TA_BASELINE | TA_LEFT);
    oldCol = SetTextColor(hdc, col);
    oldF = GetCurrentObject(hdc, OBJ_FONT);
    pixelSize = max(8, (int)round(pxSize));

    i = 0;
    while (i < ng) {
        Font *runF = gs[i].f;
        HFONT hf = (HFONT)font_hfont(runF, pixelSize);
        uint16_t gids[256];
        int dxs[256];
        float runStartX = x;
        float curX = x;
        int runN = 0;
        while (i < ng && gs[i].f == runF && runN < 256) {
            float nextX = curX + gs[i].adv;
            gids[runN] = gs[i].gid;
            dxs[runN] = (int)round(nextX) - (int)round(curX);
            curX = nextX;
            runN++;
            i++;
        }
        if (hf) {
            SelectObject(hdc, hf);
            ExtTextOutW(hdc, (int)round(runStartX), baseY, ETO_GLYPH_INDEX, NULL,
                        (LPCWSTR)gids, runN, dxs);
        }
        x = curX;
    }

    SelectObject(hdc, oldF);
    SetTextColor(hdc, oldCol);
    SetTextAlign(hdc, oldAlign);
}

static void paint_toc_sidebar(HDC hdc)
{
    const RECT *tr = &g_app.tocRect;
    COLORREF bg = g_app.darkMode ? RGB(22, 27, 34) : RGB(246, 248, 250);
    COLORREF border = g_app.darkMode ? RGB(48, 54, 61) : RGB(208, 215, 222);
    COLORREF fg = g_app.darkMode ? RGB(230, 237, 243) : RGB(31, 35, 40);
    COLORREF muted = g_app.darkMode ? RGB(139, 148, 158) : RGB(101, 109, 118);
    COLORREF accent = g_app.darkMode ? RGB(88, 166, 255) : RGB(9, 105, 218);
    HBRUSH bgBr;
    RECT hdrLine;
    int saved, activeIdx, i;
    float uiPx = (g_app.uiFontPx > 0.0f) ? g_app.uiFontPx : 12.5f;
    HFONT uiFont = g_app.hUiFont ? g_app.hUiFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ oldF;

    if (tr->right <= tr->left || tr->bottom <= tr->top) return;

    /* Soft outer shadow along the floating edge */
    {
        int sx = (g_app.tocSide == 0) ? g_app.tocSplitRect.right : (g_app.tocSplitRect.left - 3);
        RECT shR = {sx, tr->top, sx + 3, tr->bottom};
        COLORREF shCol = g_app.darkMode ? RGB(8, 10, 14) : RGB(224, 229, 235);
        HBRUSH shBr = CreateSolidBrush(shCol);
        FillRect(hdc, &shR, shBr);
        DeleteObject(shBr);
    }

    bgBr = CreateSolidBrush(bg);
    FillRect(hdc, tr, bgBr);
    DeleteObject(bgBr);

    /* Header text rendered with the configured body font chain */
    SetBkMode(hdc, TRANSPARENT);
    oldF = SelectObject(hdc, uiFont);
    {
        RECT titleR = {tr->left + 12, 0, g_app.tocSwapBtnRect.left - 4, TOC_HEADER_H};
        paint_chain_text(hdc, FAM_BODY, 1, uiPx + 0.5f, "\xEB\xAA\xA9\xEC\xB0\xA8", &titleR, fg, 0);
    }

    /* Swap side button (Left <-> Right) */
    {
        const RECT *br = &g_app.tocSwapBtnRect;
        if (g_app.tocHoverBtn == 1) {
            HBRUSH hbr = CreateSolidBrush(g_app.darkMode ? RGB(48, 54, 61) : RGB(234, 238, 242));
            FillRect(hdc, br, hbr);
            DeleteObject(hbr);
        }
        SetTextAlign(hdc, TA_LEFT | TA_TOP);
        SelectObject(hdc, uiFont);
        SetTextColor(hdc, g_app.tocHoverBtn == 1 ? fg : muted);
        {
            RECT sr = *br;
            DrawTextW(hdc, g_app.tocSide == 0 ? L"\x2192" : L"\x2190", -1, &sr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    /* Close TOC button */
    {
        const RECT *br = &g_app.tocCloseBtnRect;
        if (g_app.tocHoverBtn == 2) {
            HBRUSH hbr = CreateSolidBrush(g_app.darkMode ? RGB(48, 54, 61) : RGB(234, 238, 242));
            FillRect(hdc, br, hbr);
            DeleteObject(hbr);
        }
        draw_cross_icon(hdc, br, g_app.tocHoverBtn == 2 ? fg : muted);
    }

    {
        HBRUSH bBr = CreateSolidBrush(border);
        SetRect(&hdrLine, tr->left, TOC_HEADER_H - 1, tr->right, TOC_HEADER_H);
        FillRect(hdc, &hdrLine, bBr);
        DeleteObject(bBr);
    }

    saved = SaveDC(hdc);
    IntersectClipRect(hdc, tr->left, TOC_HEADER_H, tr->right, tr->bottom);
    activeIdx = toc_active_heading();

    if (g_app.tocCount == 0) {
        RECT emp = {tr->left + 12, TOC_HEADER_H + 16, tr->right - 12, TOC_HEADER_H + 44};
        paint_chain_text(hdc, FAM_BODY, 0, uiPx, "# \xEC\xA0\x9C\xEB\xAA\xA9\xEC\x9D\xB4 \xEC\x97\x86\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4", &emp, muted, 1);
    } else {
        int listRight = tr->right - (g_app.tocMaxScrollY > 0 ? SCROLLBAR_W : 4);
        for (i = 0; i < g_app.tocCount; i++) {
            int y = TOC_HEADER_H + 6 + i * TOC_ITEM_H - g_app.tocScrollY;
            RECT rowR;
            int relLvl, indent, isBold;
            COLORREF txtCol;
            if (y + TOC_ITEM_H < TOC_HEADER_H || y > tr->bottom) continue;

            SetRect(&rowR, tr->left + 6, y, listRight - 4, y + TOC_ITEM_H - 2);
            if (i == activeIdx || i == g_app.tocHoverIdx) {
                COLORREF rowBg = (i == activeIdx)
                    ? (g_app.darkMode ? RGB(31, 41, 55) : RGB(221, 244, 255))
                    : (g_app.darkMode ? RGB(33, 38, 45) : RGB(234, 238, 242));
                HBRUSH rBr = CreateSolidBrush(rowBg);
                HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
                HGDIOBJ oP = SelectObject(hdc, nullPen);
                HGDIOBJ oB = SelectObject(hdc, rBr);
                RoundRect(hdc, rowR.left, rowR.top, rowR.right, rowR.bottom, 6, 6);
                SelectObject(hdc, oB);
                SelectObject(hdc, oP);
                DeleteObject(rBr);
            }
            if (i == activeIdx) {
                RECT accR = {rowR.left, rowR.top + 4, rowR.left + 3, rowR.bottom - 4};
                HBRUSH aBr = CreateSolidBrush(accent);
                FillRect(hdc, &accR, aBr);
                DeleteObject(aBr);
            }

            relLvl = g_app.tocItems[i].level - g_app.tocMinLevel;
            if (relLvl < 0) relLvl = 0;
            if (relLvl > 4) relLvl = 4;
            indent = 10 + relLvl * 11;

            isBold = (g_app.tocItems[i].level == g_app.tocMinLevel);
            txtCol = (i == activeIdx) ? accent : (g_app.tocItems[i].level <= g_app.tocMinLevel + 1 ? fg : muted);
            rowR.left += indent;
            rowR.right -= 6;
            paint_chain_text(hdc, FAM_BODY, isBold, uiPx, g_app.tocItems[i].title, &rowR, txtCol, 0);
        }
    }

    RestoreDC(hdc, saved);
    SelectObject(hdc, oldF);

    draw_scrollbar(hdc, &g_app.tocSbTrackRect, &g_app.tocSbThumbRect,
                   (g_app.hoverTocSb || g_app.dragMode == DRAG_TOC_SB),
                   g_app.darkMode);
}

/* ---------------- Window Procedure ---------------- */

static int is_over_text(const DocPane *pane, float x, float y)
{
    int i;
    if (!pane || !pane->layout) return 0;
    for (i = 0; i < pane->layout->nitems; i++) {
        const Item *it = &pane->layout->items[i];
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

static void pane_scroll(int paneIdx, int newPos)
{
    DocPane *p;
    if (paneIdx < 0 || paneIdx >= g_app.paneCount) return;
    p = &g_app.panes[paneIdx];
    if (newPos < 0) newPos = 0;
    if (newPos > p->maxScrollY) newPos = p->maxScrollY;
    if (newPos != p->scrollY) {
        p->scrollY = newPos;
        pane_update_thumb(p);
        InvalidateRect(g_app.hWnd, NULL, FALSE);
    }
}

static void do_scroll(int newPos)
{
    pane_scroll(g_app.activePane, newPos);
}

static void toc_scroll(int newPos)
{
    if (newPos < 0) newPos = 0;
    if (newPos > g_app.tocMaxScrollY) newPos = g_app.tocMaxScrollY;
    if (newPos != g_app.tocScrollY) {
        g_app.tocScrollY = newPos;
        toc_update_thumb();
        InvalidateRect(g_app.hWnd, NULL, FALSE);
    }
}

static int hit_test_splitter(int cx)
{
    int i;
    if (toc_is_interactive() && !IsRectEmpty(&g_app.tocSplitRect)) {
        if (cx >= g_app.tocSplitRect.left && cx < g_app.tocSplitRect.right) {
            return SPLIT_HIT_TOC;
        }
    }
    for (i = 0; i < g_app.paneCount - 1; i++) {
        if (cx >= g_app.panes[i].rect.right && cx < g_app.panes[i + 1].rect.left) {
            return i;
        }
    }
    return SPLIT_HIT_NONE;
}

static int toc_hit_row(int cx, int cy)
{
    POINT pt = {cx, cy};
    int row, rowTop;
    if (!toc_is_interactive() || !PtInRect(&g_app.tocRect, pt) || cy < TOC_HEADER_H + 6) return -1;
    if (g_app.tocMaxScrollY > 0 && PtInRect(&g_app.tocSbTrackRect, pt)) return -1;
    row = (cy - (TOC_HEADER_H + 6) + g_app.tocScrollY) / TOC_ITEM_H;
    if (row < 0 || row >= g_app.tocCount) return -1;
    rowTop = TOC_HEADER_H + 6 + row * TOC_ITEM_H - g_app.tocScrollY;
    if (cy < rowTop || cy >= rowTop + TOC_ITEM_H - 2) return -1;
    return row;
}

static void toggle_toc_visible(void)
{
    g_app.tocVisible = !g_app.tocVisible;
    g_app.tocHoverIdx = -1;
    g_app.tocHoverBtn = 0;
    settings_save();
    update_view_menu();
    toc_set_open(g_app.tocVisible);
    if (g_app.hWnd) InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void set_toc_side(int side)
{
    int newSide = side ? 1 : 0;
    int sideChanged = (g_app.tocSide != newSide);
    g_app.tocSide = newSide;
    g_app.tocVisible = 1;
    if (sideChanged) g_app.tocAnim = 0.0f;
    recalc_toc_rect();
    settings_save();
    update_view_menu();
    toc_set_open(1);
    if (g_app.hWnd) InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void toc_update_hover_state(int cx, int cy)
{
    int cw = g_app.clientW;
    int effW = get_effective_toc_width();
    if (!g_app.tocVisible || effW <= 0) return;

    if (!g_app.tocOpen) {
        if (g_app.tocSide == 0) {
            if (cx >= 0 && cx <= TOC_TRIGGER_PX) toc_set_open(1);
        } else {
            POINT pt = {cx, cy};
            int overClose = (g_app.paneCount > 1 && PtInRect(&g_app.panes[g_app.paneCount - 1].closeRect, pt));
            if (!overClose && cx <= cw && cx >= cw - TOC_TRIGGER_PX) toc_set_open(1);
        }
    } else {
        if (g_app.tocSide == 0) {
            if (cx > effW + SPLITTER_W + TOC_HIDE_MARGIN) toc_set_open(0);
        } else {
            if (cx < cw - effW - SPLITTER_W - TOC_HIDE_MARGIN) toc_set_open(0);
        }
    }
}

static HCURSOR desired_hover_cursor(int cx, int cy)
{
    int sp = hit_test_splitter(cx);
    POINT pt = {cx, cy};
    int pi;
    if (g_app.dragMode == DRAG_TOC_SPLIT || g_app.dragMode == DRAG_PANE_SPLIT || sp != SPLIT_HIT_NONE) {
        return g_app.hSizeWECursor;
    }
    if (g_app.dragMode == DRAG_SELECT) return g_app.hIBeamCursor;
    if (g_app.dragMode == DRAG_PANE_SB || g_app.dragMode == DRAG_TOC_SB) return g_app.hArrowCursor;

    if (toc_is_interactive() && PtInRect(&g_app.tocRect, pt)) {
        if (PtInRect(&g_app.tocSwapBtnRect, pt) || PtInRect(&g_app.tocCloseBtnRect, pt) || toc_hit_row(cx, cy) >= 0) {
            return g_app.hHandCursor;
        }
        return g_app.hArrowCursor;
    }

    pi = pane_at_point(cx, cy);
    if (pi >= 0) {
        const DocPane *p = &g_app.panes[pi];
        if (g_app.paneCount > 1 && PtInRect(&p->closeRect, pt)) return g_app.hArrowCursor;
        if (p->maxScrollY > 0 && PtInRect(&p->sbTrackRect, pt)) return g_app.hArrowCursor;
        if (p->hoverLink >= 0) return g_app.hHandCursor;
        if (p->hoverText) return g_app.hIBeamCursor;
    }
    return g_app.hArrowCursor;
}

static void paint_window_contents(HDC hdc)
{
    if (g_app.clientW <= 0 || g_app.clientH <= 0) return;
    {
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBm = CreateCompatibleBitmap(hdc, g_app.clientW, g_app.clientH);
        HGDIOBJ oldBm = SelectObject(memDC, memBm);
        COLORREF winBg = g_app.darkMode ? RGB(13, 17, 23) : RGB(255, 255, 255);
        RECT fullRc = {0, 0, g_app.clientW, g_app.clientH};
        HBRUSH fBr = CreateSolidBrush(winBg);
        int i;

        FillRect(memDC, &fullRc, fBr);
        DeleteObject(fBr);

        for (i = 0; i < g_app.paneCount; i++) {
            paint_pane(memDC, i);
            if (i < g_app.paneCount - 1) {
                paint_splitter(memDC, g_app.panes[i].rect.right, g_app.clientH,
                               (g_app.hoverSplitter == i || (g_app.dragMode == DRAG_PANE_SPLIT && g_app.dragIdx == i)));
            }
        }

        if (g_app.tocVisible) {
            if (g_app.tocAnim > 0.01f && !IsRectEmpty(&g_app.tocRect)) {
                paint_toc_sidebar(memDC);
                if (!IsRectEmpty(&g_app.tocSplitRect)) {
                    paint_splitter(memDC, g_app.tocSplitRect.left, g_app.clientH,
                                   (g_app.hoverSplitter == SPLIT_HIT_TOC || g_app.dragMode == DRAG_TOC_SPLIT));
                }
            } else {
                paint_toc_edge_tab(memDC);
            }
        }

        BitBlt(hdc, 0, 0, g_app.clientW, g_app.clientH, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBm);
        DeleteObject(memBm);
        DeleteDC(memDC);
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
        ui_update_fonts();
        update_recent_menu();
        update_view_menu();
        return 0;
    }

    case WM_SIZE: {
        g_app.clientW = LOWORD(lParam);
        g_app.clientH = HIWORD(lParam);
        rebuild_layout();
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        paint_window_contents(hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_PRINTCLIENT: {
        paint_window_contents((HDC)wParam);
        return 0;
    }

    case WM_TIMER:
        if (wParam == IDT_TOC_ANIM) {
            float target = (g_app.tocVisible && g_app.tocOpen) ? 1.0f : 0.0f;
            LARGE_INTEGER now, freq;
            float dt, step;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            dt = (freq.QuadPart > 0)
                ? (float)((double)(now.QuadPart - g_app.tocAnimLastQpc.QuadPart) / (double)freq.QuadPart)
                : 0.016f;
            g_app.tocAnimLastQpc = now;
            if (dt < 0.001f) dt = 0.001f;
            if (dt > 0.04f) dt = 0.04f;
            step = dt / TOC_ANIM_DUR;

            if (g_app.tocAnim < target) {
                g_app.tocAnim += step;
                if (g_app.tocAnim >= target) g_app.tocAnim = target;
            } else if (g_app.tocAnim > target) {
                g_app.tocAnim -= step;
                if (g_app.tocAnim <= target) g_app.tocAnim = target;
            }
            recalc_toc_rect();
            InvalidateRect(hWnd, NULL, FALSE);
            if (g_app.tocAnim == target) {
                g_app.tocAnimActive = 0;
                KillTimer(hWnd, IDT_TOC_ANIM);
            }
            return 0;
        }
        break;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        POINT pt;
        int pi;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ScreenToClient(hWnd, &pt);

        if (toc_is_interactive() && PtInRect(&g_app.tocRect, pt)) {
            toc_scroll(g_app.tocScrollY - (delta / WHEEL_DELTA) * 48);
            return 0;
        }
        pi = pane_at_point(pt.x, pt.y);
        if (pi < 0) pi = g_app.activePane;
        pane_scroll(pi, g_app.panes[pi].scrollY - (delta / WHEEL_DELTA) * 60);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_app.trackingMouse = 0;
        if (g_app.tocVisible && g_app.tocOpen &&
            g_app.dragMode != DRAG_TOC_SB && g_app.dragMode != DRAG_TOC_SPLIT) {
            toc_set_open(0);
        }
        if (g_app.hoverClosePane != -1 || g_app.hoverPaneSb != -1 || g_app.hoverTocSb ||
            g_app.hoverSplitter != SPLIT_HIT_NONE || g_app.tocHoverIdx != -1 || g_app.tocHoverBtn != 0) {
            g_app.hoverClosePane = -1;
            g_app.hoverPaneSb = -1;
            g_app.hoverTocSb = 0;
            g_app.hoverSplitter = SPLIT_HIT_NONE;
            g_app.tocHoverIdx = -1;
            g_app.tocHoverBtn = 0;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEMOVE: {
        int cx = GET_X_LPARAM(lParam);
        int cy = GET_Y_LPARAM(lParam);
        POINT pt = {cx, cy};
        int needRedraw = 0;

        if (!g_app.trackingMouse) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hWnd, 0};
            TrackMouseEvent(&tme);
            g_app.trackingMouse = 1;
        }

        if (g_app.dragMode == DRAG_SELECT) {
            DocPane *p = &g_app.panes[g_app.dragIdx];
            int localX = cx - p->rect.left;
            int localY = cy - p->rect.top;
            int hit;
            if (localY < 0) pane_scroll(g_app.dragIdx, p->scrollY + localY);
            else if (localY > (p->rect.bottom - p->rect.top)) pane_scroll(g_app.dragIdx, p->scrollY + (localY - (p->rect.bottom - p->rect.top)));

            hit = layout_hit_text(p->layout, 0, (float)localX, (float)(localY + p->scrollY));
            if (hit >= 0 && p->selAnchor >= 0 && hit != p->selEnd) {
                p->selStart = p->selAnchor;
                p->selEnd = hit;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            SetCursor(g_app.hIBeamCursor);
            return 0;
        }

        if (g_app.dragMode == DRAG_PANE_SB) {
            DocPane *p = &g_app.panes[g_app.dragIdx];
            int newPos = scrollbar_pos_from_y(&p->sbTrackRect, &p->sbThumbRect, p->maxScrollY, cy);
            pane_scroll(g_app.dragIdx, newPos);
            return 0;
        }

        if (g_app.dragMode == DRAG_TOC_SB) {
            int newPos = scrollbar_pos_from_y(&g_app.tocSbTrackRect, &g_app.tocSbThumbRect, g_app.tocMaxScrollY, cy);
            toc_scroll(newPos);
            return 0;
        }

        if (g_app.dragMode == DRAG_TOC_SPLIT) {
            int maxAllowed = min(TOC_MAX_W, g_app.clientW - 180);
            int nw = (g_app.tocSide == 0) ? cx : (g_app.clientW - cx - SPLITTER_W);
            if (maxAllowed < TOC_MIN_W) maxAllowed = TOC_MIN_W;
            if (nw < TOC_MIN_W) nw = TOC_MIN_W;
            if (nw > maxAllowed) nw = maxAllowed;
            if (nw != g_app.tocWidth) {
                g_app.tocWidth = nw;
                g_app.tocOpen = 1;
                g_app.tocAnim = 1.0f;
                recalc_toc_rect();
                InvalidateRect(hWnd, NULL, FALSE);
            }
            SetCursor(g_app.hSizeWECursor);
            return 0;
        }

        if (g_app.dragMode == DRAG_PANE_SPLIT) {
            int i = g_app.dragIdx;
            int availW = (g_app.docAreaRight - g_app.docAreaLeft) - (g_app.paneCount - 1) * SPLITTER_W;
            float totalPair = g_app.dragStartRatioA + g_app.dragStartRatioB;
            float deltaR = (availW > 0) ? ((float)(cx - g_app.dragStartCoord) / (float)availW) : 0.0f;
            float minR = (availW >= g_app.paneCount * PANE_MIN_W) ? ((float)PANE_MIN_W / (float)availW) : 0.08f;
            float newA = g_app.dragStartRatioA + deltaR;
            if (newA < minR) newA = minR;
            if (newA > totalPair - minR) newA = totalPair - minR;
            g_app.panes[i].widthRatio = newA;
            g_app.panes[i + 1].widthRatio = totalPair - newA;
            recalc_geometry();
            rebuild_pane_layout(i, 1);
            rebuild_pane_layout(i + 1, 1);
            recalc_geometry();
            update_search_panel_pos(0);
            InvalidateRect(hWnd, NULL, FALSE);
            SetCursor(g_app.hSizeWECursor);
            return 0;
        }

        toc_update_hover_state(cx, cy);

        {
            int newSplit = hit_test_splitter(cx);
            int newTocBtn = 0;
            int newTocIdx = -1;
            int newTocSb = 0;
            int newClosePane = -1;
            int newPaneSb = -1;
            int pi = pane_at_point(cx, cy);

            if (newSplit == SPLIT_HIT_NONE && toc_is_interactive() && PtInRect(&g_app.tocRect, pt)) {
                if (PtInRect(&g_app.tocSwapBtnRect, pt)) newTocBtn = 1;
                else if (PtInRect(&g_app.tocCloseBtnRect, pt)) newTocBtn = 2;
                else if (g_app.tocMaxScrollY > 0 && PtInRect(&g_app.tocSbTrackRect, pt)) newTocSb = 1;
                else newTocIdx = toc_hit_row(cx, cy);
            } else if (newSplit == SPLIT_HIT_NONE && pi >= 0) {
                DocPane *p = &g_app.panes[pi];
                if (g_app.paneCount > 1 && PtInRect(&p->closeRect, pt)) {
                    newClosePane = pi;
                } else if (p->maxScrollY > 0 && PtInRect(&p->sbTrackRect, pt)) {
                    newPaneSb = pi;
                } else {
                    int docX = cx - p->rect.left;
                    int docY = (cy - p->rect.top) + p->scrollY;
                    p->hoverLink = p->layout ? layout_hit_link(p->layout, 0, (float)docX, (float)docY) : -1;
                    p->hoverText = is_over_text(p, (float)docX, (float)docY);
                }
            }

            if (newSplit != g_app.hoverSplitter || newTocBtn != g_app.tocHoverBtn ||
                newTocIdx != g_app.tocHoverIdx || newTocSb != g_app.hoverTocSb ||
                newClosePane != g_app.hoverClosePane || newPaneSb != g_app.hoverPaneSb) {
                g_app.hoverSplitter = newSplit;
                g_app.tocHoverBtn = newTocBtn;
                g_app.tocHoverIdx = newTocIdx;
                g_app.hoverTocSb = newTocSb;
                g_app.hoverClosePane = newClosePane;
                g_app.hoverPaneSb = newPaneSb;
                needRedraw = 1;
            }
        }

        SetCursor(desired_hover_cursor(cx, cy));
        if (needRedraw) InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hWnd, &pt);
            SetCursor(desired_hover_cursor(pt.x, pt.y));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        int cx = GET_X_LPARAM(lParam);
        int cy = GET_Y_LPARAM(lParam);
        POINT pt = {cx, cy};
        int sp = hit_test_splitter(cx);
        int pi;

        if (sp == SPLIT_HIT_TOC) {
            g_app.dragMode = DRAG_TOC_SPLIT;
            g_app.dragStartCoord = cx;
            g_app.dragStartVal = g_app.tocWidth;
            SetCapture(hWnd);
            SetCursor(g_app.hSizeWECursor);
            return 0;
        }
        if (sp >= 0 && sp < g_app.paneCount - 1) {
            g_app.dragMode = DRAG_PANE_SPLIT;
            g_app.dragIdx = sp;
            g_app.dragStartCoord = cx;
            g_app.dragStartRatioA = g_app.panes[sp].widthRatio;
            g_app.dragStartRatioB = g_app.panes[sp + 1].widthRatio;
            SetCapture(hWnd);
            SetCursor(g_app.hSizeWECursor);
            return 0;
        }

        if (toc_is_interactive() && PtInRect(&g_app.tocRect, pt)) {
            if (PtInRect(&g_app.tocSwapBtnRect, pt)) {
                set_toc_side(!g_app.tocSide);
                return 0;
            }
            if (PtInRect(&g_app.tocCloseBtnRect, pt)) {
                toggle_toc_visible();
                return 0;
            }
            if (g_app.tocMaxScrollY > 0 && PtInRect(&g_app.tocSbTrackRect, pt)) {
                if (PtInRect(&g_app.tocSbThumbRect, pt)) {
                    g_app.dragMode = DRAG_TOC_SB;
                    g_app.dragStartCoord = cy;
                    SetCapture(hWnd);
                } else if (cy < g_app.tocSbThumbRect.top) {
                    toc_scroll(g_app.tocScrollY - (g_app.clientH - TOC_HEADER_H));
                } else {
                    toc_scroll(g_app.tocScrollY + (g_app.clientH - TOC_HEADER_H));
                }
                return 0;
            }
            {
                int row = toc_hit_row(cx, cy);
                if (row >= 0) {
                    toc_jump_to_index(row);
                    return 0;
                }
            }
            return 0;
        }

        pi = pane_at_point(cx, cy);
        if (pi >= 0) {
            DocPane *p = &g_app.panes[pi];
            if (g_app.paneCount > 1 && PtInRect(&p->closeRect, pt)) {
                pane_close(pi);
                return 0;
            }

            if (g_app.activePane != pi) {
                g_app.activePane = pi;
                update_view_menu();
            }

            if (p->maxScrollY > 0 && PtInRect(&p->sbTrackRect, pt)) {
                if (PtInRect(&p->sbThumbRect, pt)) {
                    g_app.dragMode = DRAG_PANE_SB;
                    g_app.dragIdx = pi;
                    g_app.dragStartCoord = cy;
                    SetCapture(hWnd);
                } else if (cy < p->sbThumbRect.top) {
                    pane_scroll(pi, p->scrollY - g_app.clientH);
                } else {
                    pane_scroll(pi, p->scrollY + g_app.clientH);
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }

            {
                int docX = cx - p->rect.left;
                int docY = (cy - p->rect.top) + p->scrollY;
                int link = p->layout ? layout_hit_link(p->layout, 0, (float)docX, (float)docY) : -1;
                int hit = p->layout ? layout_hit_text(p->layout, 0, (float)docX, (float)docY) : -1;
                p->pendingLink = link;
                g_app.downPt = pt;
                if (hit >= 0) {
                    if (!(GetKeyState(VK_SHIFT) & 0x8000)) {
                        p->selAnchor = hit;
                        p->selStart = hit;
                        p->selEnd = hit;
                    } else {
                        p->selEnd = hit;
                    }
                } else {
                    p->selAnchor = -1;
                    p->selStart = -1;
                    p->selEnd = -1;
                }
                g_app.dragMode = DRAG_SELECT;
                g_app.dragIdx = pi;
                SetCapture(hWnd);
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_app.dragMode != DRAG_NONE) {
            int mode = g_app.dragMode;
            int idx = g_app.dragIdx;
            ReleaseCapture();
            g_app.dragMode = DRAG_NONE;

            if (mode == DRAG_TOC_SPLIT || mode == DRAG_TOC_SB) {
                if (mode == DRAG_TOC_SPLIT) settings_save();
                toc_update_hover_state(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            }

            if (mode == DRAG_SELECT && idx >= 0 && idx < g_app.paneCount) {
                DocPane *p = &g_app.panes[idx];
                int dx = abs(GET_X_LPARAM(lParam) - g_app.downPt.x);
                int dy = abs(GET_Y_LPARAM(lParam) - g_app.downPt.y);

                if (dx < 4 && dy < 4 && p->pendingLink >= 0 && g_app.doc && p->pendingLink < g_app.doc->nlinks) {
                    const char *target = g_app.doc->links[p->pendingLink];
                    if (target[0] == '#') {
                        const Anchor *anc = layout_find_anchor(p->layout, target + 1);
                        if (anc) pane_scroll(idx, (int)anc->y - 20);
                    } else if (link_is_safe_external(target)) {
                        const char *trimmed = target;
                        while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\r' || *trimmed == '\n') trimmed++;
                        wchar_t *wtarget = utf8_to_utf16(trimmed, -1, NULL);
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
                    p->selStart = -1;
                    p->selEnd = -1;
                    InvalidateRect(hWnd, NULL, FALSE);
                } else if (p->selStart == p->selEnd) {
                    p->selStart = -1;
                    p->selEnd = -1;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                p->pendingLink = -1;
            }
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        g_app.dragMode = DRAG_NONE;
        g_app.dragIdx = -1;
        return 0;

    case WM_LBUTTONDBLCLK: {
        int cx = GET_X_LPARAM(lParam);
        int cy = GET_Y_LPARAM(lParam);
        int pi = pane_at_point(cx, cy);
        if (pi >= 0) {
            DocPane *p = &g_app.panes[pi];
            int docX = cx - p->rect.left;
            int docY = (cy - p->rect.top) + p->scrollY;
            int hit = p->layout ? layout_hit_text(p->layout, 0, (float)docX, (float)docY) : -1;
            g_app.activePane = pi;
            if (hit >= 0 && p->layout && p->layout->text && p->layout->textlen > 0) {
                const char *txt = p->layout->text;
                int len = p->layout->textlen;
                int s, e, tmp;
                uint32_t hitCp;
                if (hit >= len) hit = len - 1;
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
                p->selStart = s;
                p->selEnd = e;
                p->selAnchor = s;
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_CONTEXTMENU: {
        POINT pt, cli;
        HMENU hMenu;
        DocPane *p;
        int hasSel, pi;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        if (pt.x == -1 && pt.y == -1) {
            pt.x = 50;
            pt.y = 50;
            ClientToScreen(hWnd, &pt);
        } else {
            cli = pt;
            ScreenToClient(hWnd, &cli);
            pi = pane_at_point(cli.x, cli.y);
            if (pi >= 0) {
                g_app.activePane = pi;
                update_view_menu();
            }
        }
        p = active_pane();
        hMenu = CreatePopupMenu();
        hasSel = (p->selStart >= 0 && p->selEnd >= 0 && p->selStart != p->selEnd);
        AppendMenuW(hMenu, MF_STRING | (hasSel ? 0 : MF_GRAYED), IDM_EDIT_COPY, L"복사(&C)\tCtrl+C");
        AppendMenuW(hMenu, MF_STRING, IDM_EDIT_SELECT_ALL, L"모두 선택(&A)\tCtrl+A");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, IDM_EDIT_FIND, L"찾기(&F)...\tCtrl+F");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING | (g_app.tocVisible ? MF_CHECKED : 0), IDM_VIEW_TOC, L"목차 표시(&M)\tCtrl+M");
        AppendMenuW(hMenu, MF_STRING | (g_app.paneCount < MAX_SPLIT_PANES ? 0 : MF_GRAYED), IDM_VIEW_SPLIT_VERT, L"나누어 보기(&N)\tCtrl+\\");
        if (g_app.paneCount > 1) {
            AppendMenuW(hMenu, MF_STRING, IDM_VIEW_SPLIT_CLOSE, L"이 분할 닫기(&W)\tCtrl+W");
        }

        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
        DestroyMenu(hMenu);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT len = DragQueryFileW(hDrop, 0, NULL, 0);
        if (len > 0 && len < LONG_PATH_MAX) {
            wchar_t *dropped = (wchar_t *)malloc(((size_t)len + 1u) * sizeof(wchar_t));
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
            ofn.lpstrFilter = L"Markdown 파일 (*.md;*.markdown;*.txt)\0*.md;*.markdown;*.mdown;*.txt\0모든 파일 (*.*)\0*.*\0";
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

        case IDM_EDIT_SELECT_ALL: {
            DocPane *p = active_pane();
            if (p->layout && p->layout->textlen > 0) {
                p->selStart = 0;
                p->selEnd = p->layout->textlen;
                p->selAnchor = 0;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }

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
            ofn.lpstrFilter = L"PDF 파일 (*.pdf)\0*.pdf\0모든 파일 (*.*)\0*.*\0";
            ofn.lpstrFile = fileBuf;
            ofn.nMaxFile = LONG_PATH_MAX;
            ofn.lpstrDefExt = L"pdf";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                char *u8Out = utf16_to_utf8(fileBuf, -1, NULL);
                if (u8Out && export_pdf(g_app.doc, u8Out, err, sizeof(err)) == 0) {
                    MessageBoxW(hWnd, L"PDF 파일을 내보냈습니다.", L"완료", MB_OK | MB_ICONINFORMATION);
                } else {
                    wchar_t *werr = err[0] ? utf8_to_utf16(err, -1, NULL) : NULL;
                    MessageBoxW(hWnd, werr ? werr : L"PDF 내보내기에 실패했습니다.", L"오류", MB_OK | MB_ICONERROR);
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
            ofn.lpstrFilter = L"Word 문서 (*.docx)\0*.docx\0모든 파일 (*.*)\0*.*\0";
            ofn.lpstrFile = fileBuf;
            ofn.nMaxFile = LONG_PATH_MAX;
            ofn.lpstrDefExt = L"docx";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                char *u8Out = utf16_to_utf8(fileBuf, -1, NULL);
                if (u8Out && docx_write(g_app.doc, u8Out, err, sizeof(err)) == 0) {
                    MessageBoxW(hWnd, L"DOCX 파일을 내보냈습니다.", L"완료", MB_OK | MB_ICONINFORMATION);
                } else {
                    wchar_t *werr = err[0] ? utf8_to_utf16(err, -1, NULL) : NULL;
                    MessageBoxW(hWnd, werr ? werr : L"DOCX 내보내기에 실패했습니다.", L"오류", MB_OK | MB_ICONERROR);
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
            update_view_menu();
            rebuild_layout();
            return 0;

        case IDM_VIEW_TOC:
            toggle_toc_visible();
            return 0;

        case IDM_VIEW_TOC_LEFT:
            set_toc_side(0);
            return 0;

        case IDM_VIEW_TOC_RIGHT:
            set_toc_side(1);
            return 0;

        case IDM_VIEW_SPLIT_VERT:
            pane_split_active();
            return 0;

        case IDM_VIEW_SPLIT_CLOSE:
            pane_close(g_app.activePane);
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
            MessageBoxW(hWnd, L"글꼴 설정을 기본값으로 초기화했습니다.",
                        L"글꼴 초기화", MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        case IDM_HELP_ABOUT:
            MessageBoxW(hWnd,
                        L"Lilac the Markdown Viewer\n\n"
                        L"by ym2413-opll, Version 1.4.0.\n",
                        L"정보", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;

    case WM_KEYDOWN: {
        DocPane *ap = active_pane();
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
            if (ap->selStart != ap->selEnd) {
                ap->selStart = -1;
                ap->selEnd = -1;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            break;
        case VK_UP: do_scroll(ap->scrollY - 40); return 0;
        case VK_DOWN: do_scroll(ap->scrollY + 40); return 0;
        case VK_PRIOR: do_scroll(ap->scrollY - g_app.clientH); return 0;
        case VK_NEXT: do_scroll(ap->scrollY + g_app.clientH); return 0;
        case VK_HOME: do_scroll(0); return 0;
        case VK_END: do_scroll(ap->maxScrollY); return 0;
        }
        break;
    }

    case WM_DESTROY:
        if (g_search.hFont) DeleteObject(g_search.hFont);
        if (g_search.matches) free(g_search.matches);
        if (g_app.hUiFont) DeleteObject(g_app.hUiFont);
        if (g_app.hUiBoldFont) DeleteObject(g_app.hUiBoldFont);
        free(g_app.tocItems);
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
            MessageBoxW(NULL, L"Lilac이 *.md 파일 기본 프로그램으로 등록되었습니다.", L"Lilac", MB_OK | MB_ICONINFORMATION);
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
    g_app.paneCount = 1;
    g_app.activePane = 0;
    pane_init(&g_app.panes[0], 1.0f);
    g_app.tocVisible = 1;
    g_app.tocSide = 0;
    g_app.tocWidth = 240;
    g_app.tocHoverIdx = -1;
    g_app.hoverClosePane = -1;
    g_app.hoverPaneSb = -1;
    g_app.hoverSplitter = SPLIT_HIT_NONE;
    g_app.hHandCursor = LoadCursor(NULL, IDC_HAND);
    g_app.hArrowCursor = LoadCursor(NULL, IDC_ARROW);
    g_app.hIBeamCursor = LoadCursor(NULL, IDC_IBEAM);
    g_app.hSizeWECursor = LoadCursor(NULL, IDC_SIZEWE);

    font_init();
    settings_load();

    /* Register Window Class */
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
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
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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
    update_view_menu();

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

    for (i = 0; i < g_app.paneCount; i++) pane_free_layout(&g_app.panes[i]);
    if (g_app.doc) doc_free(g_app.doc);
    font_shutdown();

    return (int)msg.wParam;
}
