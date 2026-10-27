/*
 * main.c - Lilac Win32 Markdown viewer application.
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
static const wchar_t g_sampleMarkdown[] =
    L"# Lilac - Markdown 뷰어\n\n"
    L"이 페이지는 파일을 열기 전 기본으로 표시되는 페이지입니다. 위쪽 메뉴에서 Markdown 파일을 열어 주십시오.\n";

typedef struct AppState {
    HWND    hWnd;
    Doc    *doc;
    Layout *layout;
    wchar_t currentPath[MAX_PATH];
    float   baseSize;
    int     darkMode;
    int     scrollY;
    int     maxScrollY;
    int     clientW, clientH;
    int     hoverLink;
    HCURSOR hHandCursor;
    HCURSOR hArrowCursor;
} AppState;

static AppState g_app;

/* ---------------- File Association ---------------- */

static int register_file_association(int reg)
{
    wchar_t exePath[MAX_PATH];
    wchar_t cmdBuf[MAX_PATH + 32];
    HKEY hKey;

    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    swprintf(cmdBuf, MAX_PATH + 32, L"\"%ls\" \"%%1\"", exePath);

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
            swprintf(iconBuf, MAX_PATH + 8, L"\"%ls\",0", exePath);
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

static void settings_load(void)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Lilac\\Settings", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[512];
        DWORD sz = sizeof(buf);
        if (RegQueryValueExW(hKey, L"BodyFontChain", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS && buf[0]) {
            font_set_family_chain(FAM_BODY, buf);
        } else {
            sz = sizeof(buf);
            if (RegQueryValueExW(hKey, L"BodyFont", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS && buf[0]) {
                font_set_family_face(FAM_BODY, buf);
            }
        }

        sz = sizeof(buf);
        if (RegQueryValueExW(hKey, L"MonoFontChain", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS && buf[0]) {
            font_set_family_chain(FAM_MONO, buf);
        } else {
            sz = sizeof(buf);
            if (RegQueryValueExW(hKey, L"MonoFont", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS && buf[0]) {
                font_set_family_face(FAM_MONO, buf);
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
        const wchar_t *b = font_get_family_face(FAM_BODY);
        const wchar_t *m = font_get_family_face(FAM_MONO);
        const wchar_t *bChain = font_get_family_chain(FAM_BODY);
        const wchar_t *mChain = font_get_family_chain(FAM_MONO);

        RegSetValueExW(hKey, L"BodyFont", 0, REG_SZ, (const BYTE *)b, (DWORD)(wcslen(b) + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"MonoFont", 0, REG_SZ, (const BYTE *)m, (DWORD)(wcslen(m) + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"BodyFontChain", 0, REG_SZ, (const BYTE *)bChain, (DWORD)(wcslen(bChain) + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"MonoFontChain", 0, REG_SZ, (const BYTE *)mChain, (DWORD)(wcslen(mChain) + 1) * sizeof(wchar_t));
        RegCloseKey(hKey);
    }
}

static int pick_font_face(HWND hWnd, wchar_t *outFace, int outCap, int fixedOnly)
{
    CHOOSEFONTW cf = {0};
    LOGFONTW lf = {0};
    HDC hdc = GetDC(hWnd);

    lf.lfHeight = -16;
    wcsncpy_s(lf.lfFaceName, LF_FACESIZE, outFace, _TRUNCATE);

    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = hWnd;
    cf.hDC = hdc;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_EFFECTS;
    if (fixedOnly) cf.Flags |= CF_FIXEDPITCHONLY;

    if (ChooseFontW(&cf)) {
        wcsncpy_s(outFace, outCap, lf.lfFaceName, _TRUNCATE);
        ReleaseDC(hWnd, hdc);
        return 1;
    }
    ReleaseDC(hWnd, hdc);
    return 0;
}

static wchar_t g_dlgBodyChain[512];
static wchar_t g_dlgMonoChain[512];

static void rebuild_layout(void);

static INT_PTR CALLBACK FontDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        wcsncpy_s(g_dlgBodyChain, 512, font_get_family_chain(FAM_BODY), _TRUNCATE);
        wcsncpy_s(g_dlgMonoChain, 512, font_get_family_chain(FAM_MONO), _TRUNCATE);
        SetDlgItemTextW(hDlg, IDC_EDIT_BODY_CHAIN, g_dlgBodyChain);
        SetDlgItemTextW(hDlg, IDC_EDIT_MONO_CHAIN, g_dlgMonoChain);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_CHANGE_BODY: {
            wchar_t picked[64] = {0};
            if (pick_font_face(hDlg, picked, 64, 0)) {
                wchar_t cur[512];
                GetDlgItemTextW(hDlg, IDC_EDIT_BODY_CHAIN, cur, 512);
                wchar_t updated[512];
                _snwprintf_s(updated, 512, _TRUNCATE, L"%ls, %ls", picked, cur);
                font_set_family_chain(FAM_BODY, updated);
                SetDlgItemTextW(hDlg, IDC_EDIT_BODY_CHAIN, font_get_family_chain(FAM_BODY));
            }
            return TRUE;
        }

        case IDC_BTN_CHANGE_MONO: {
            wchar_t picked[64] = {0};
            if (pick_font_face(hDlg, picked, 64, 1)) {
                wchar_t cur[512];
                GetDlgItemTextW(hDlg, IDC_EDIT_MONO_CHAIN, cur, 512);
                wchar_t updated[512];
                _snwprintf_s(updated, 512, _TRUNCATE, L"%ls, %ls", picked, cur);
                font_set_family_chain(FAM_MONO, updated);
                SetDlgItemTextW(hDlg, IDC_EDIT_MONO_CHAIN, font_get_family_chain(FAM_MONO));
            }
            return TRUE;
        }

        case IDC_BTN_RESET: {
            font_set_family_chain(FAM_BODY, NULL);
            font_set_family_chain(FAM_MONO, NULL);
            SetDlgItemTextW(hDlg, IDC_EDIT_BODY_CHAIN, font_get_family_chain(FAM_BODY));
            SetDlgItemTextW(hDlg, IDC_EDIT_MONO_CHAIN, font_get_family_chain(FAM_MONO));
            return TRUE;
        }

        case IDOK: {
            wchar_t bChain[512];
            wchar_t mChain[512];
            GetDlgItemTextW(hDlg, IDC_EDIT_BODY_CHAIN, bChain, 512);
            GetDlgItemTextW(hDlg, IDC_EDIT_MONO_CHAIN, mChain, 512);
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

/* ---------------- Document Loading & Layout ---------------- */

static void update_window_title(void)
{
    wchar_t title[512];
    if (g_app.currentPath[0]) {
        swprintf(title, 512, L"%ls - Lilac", path_name(g_app.currentPath));
    } else {
        swprintf(title, 512, L"%ls - Lilac", g_app.doc && g_app.doc->title ? g_app.doc->title : L"Lilac");
    }
    SetWindowTextW(g_app.hWnd, title);
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

    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (int)g_app.layout->pageHeight;
    si.nPage = g_app.clientH;
    si.nPos = g_app.scrollY;
    SetScrollInfo(g_app.hWnd, SB_VERT, &si, TRUE);

    InvalidateRect(g_app.hWnd, NULL, FALSE);
}

static void open_file(const wchar_t *path)
{
    Doc *newDoc = doc_load_file(path);
    if (!newDoc) {
        wchar_t msg[MAX_PATH + 64];
        swprintf(msg, MAX_PATH + 64, L"파일을 열 수 없습니다:\n%ls", path);
        MessageBoxW(g_app.hWnd, msg, L"오류", MB_OK | MB_ICONERROR);
        return;
    }

    if (g_app.doc) doc_free(g_app.doc);
    g_app.doc = newDoc;
    wcsncpy_s(g_app.currentPath, MAX_PATH, path, _TRUNCATE);
    doc_resolve_images(g_app.doc, 0);

    g_app.scrollY = 0;
    update_window_title();
    rebuild_layout();
}

static void load_sample_content(void)
{
    char *utf8 = utf16_to_utf8(g_sampleMarkdown, -1, NULL);
    if (g_app.doc) doc_free(g_app.doc);
    g_app.doc = doc_parse(utf8, strlen(utf8), NULL);
    free(utf8);
    g_app.currentPath[0] = 0;
    update_window_title();
    rebuild_layout();
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

            if (it->layer != layer) continue;
            if (drawY + it->h < 0 || drawY > (float)clientH + 100.0f) continue;

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
                Ellipse(hdc, (int)round(it->x), (int)round(drawY),
                        (int)round(it->x + it->w), (int)round(drawY + it->h));
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
                    int *dx = (it->n <= 256) ? stackDx : (int *)malloc(it->n * sizeof(int));
                    int g;

                    if (!hf) continue;
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
        HICON hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        HICON hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
        if (hIcon) SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        if (hIconSm) SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);
        DragAcceptFiles(hWnd, TRUE);
        return 0;
    }

    case WM_SIZE:
        g_app.clientW = LOWORD(lParam);
        g_app.clientH = HIWORD(lParam);
        rebuild_layout();
        return 0;

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
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam) + g_app.scrollY;
        int link = -1;
        if (g_app.layout) {
            link = layout_hit_link(g_app.layout, 0, (float)x, (float)y);
        }
        if (link != g_app.hoverLink) {
            g_app.hoverLink = link;
            SetCursor(link >= 0 ? g_app.hHandCursor : g_app.hArrowCursor);
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g_app.hoverLink >= 0) {
            SetCursor(g_app.hHandCursor);
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam) + g_app.scrollY;
        int link = -1;
        if (g_app.layout) {
            link = layout_hit_link(g_app.layout, 0, (float)x, (float)y);
        }
        if (link >= 0 && link < g_app.doc->nlinks) {
            const wchar_t *target = g_app.doc->links[link];
            if (target[0] == '#') {
                const Anchor *anc = layout_find_anchor(g_app.layout, target + 1);
                if (anc) do_scroll((int)anc->y - 20);
            } else if (!_wcsnicmp(target, L"http://", 7) || !_wcsnicmp(target, L"https://", 8) || !_wcsnicmp(target, L"mailto:", 7)) {
                ShellExecuteW(hWnd, L"open", target, NULL, NULL, SW_SHOWNORMAL);
            } else if (g_app.doc->dir) {
                wchar_t *joined = path_join(g_app.doc->dir, target);
                if (GetFileAttributesW(joined) != INVALID_FILE_ATTRIBUTES) {
                    open_file(joined);
                }
                free(joined);
            }
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        wchar_t dropped[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, dropped, MAX_PATH)) {
            open_file(dropped);
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FILE_OPEN: {
            OPENFILENAMEW ofn = {0};
            wchar_t fileBuf[MAX_PATH] = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"Markdown 문서 (*.md;*.markdown;*.txt)\0*.md;*.markdown;*.mdown;*.txt\0모든 파일 (*.*)\0*.*\0";
            ofn.lpstrFile = fileBuf;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                open_file(fileBuf);
            }
            return 0;
        }

        case IDM_FILE_EXPORT_PDF: {
            OPENFILENAMEW ofn = {0};
            wchar_t fileBuf[MAX_PATH] = {0};
            wchar_t err[256] = {0};
            if (g_app.currentPath[0]) {
                wchar_t *defName = path_with_ext(path_name(g_app.currentPath), L".pdf");
                wcsncpy_s(fileBuf, MAX_PATH, defName, _TRUNCATE);
                free(defName);
            } else {
                wcscpy_s(fileBuf, MAX_PATH, L"Document.pdf");
            }
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"PDF 문서 (*.pdf)\0*.pdf\0모든 파일 (*.*)\0*.*\0";
            ofn.lpstrFile = fileBuf;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"pdf";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                if (export_pdf(g_app.doc, fileBuf, err, 256) == 0) {
                    MessageBoxW(hWnd, L"PDF 문서를 성공적으로 내보냈습니다.", L"완료", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hWnd, err[0] ? err : L"PDF 내보내기 실패", L"오류", MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }

        case IDM_FILE_EXPORT_DOCX: {
            OPENFILENAMEW ofn = {0};
            wchar_t fileBuf[MAX_PATH] = {0};
            wchar_t err[256] = {0};
            if (g_app.currentPath[0]) {
                wchar_t *defName = path_with_ext(path_name(g_app.currentPath), L".docx");
                wcsncpy_s(fileBuf, MAX_PATH, defName, _TRUNCATE);
                free(defName);
            } else {
                wcscpy_s(fileBuf, MAX_PATH, L"Document.docx");
            }
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"Microsoft Word 문서 (*.docx)\0*.docx\0모든 파일 (*.*)\0*.*\0";
            ofn.lpstrFile = fileBuf;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"docx";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                if (docx_write(g_app.doc, fileBuf, err, 256) == 0) {
                    MessageBoxW(hWnd, L"Word (DOCX) 문서를 성공적으로 내보냈습니다.", L"완료", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hWnd, err[0] ? err : L"DOCX 내보내기 실패", L"오류", MB_OK | MB_ICONERROR);
                }
            }
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
            rebuild_layout();
            return 0;

        case IDM_VIEW_REFRESH:
            if (g_app.currentPath[0]) open_file(g_app.currentPath);
            else load_sample_content();
            return 0;

        case IDM_SETTINGS_FONT:
            DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_FONT_DIALOG), hWnd, FontDlgProc);
            return 0;

        case IDM_SETTINGS_BODY_FONT: {
            wchar_t face[64];
            wcsncpy_s(face, 64, font_get_family_face(FAM_BODY), _TRUNCATE);
            if (pick_font_face(hWnd, face, 64, 0)) {
                font_set_family_face(FAM_BODY, face);
                settings_save();
                rebuild_layout();
            }
            return 0;
        }

        case IDM_SETTINGS_MONO_FONT: {
            wchar_t face[64];
            wcsncpy_s(face, 64, font_get_family_face(FAM_MONO), _TRUNCATE);
            if (pick_font_face(hWnd, face, 64, 1)) {
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
                        L"by ym2413-opll, Version 1.0.0\n",
                        L"정보", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_UP: do_scroll(g_app.scrollY - 40); return 0;
        case VK_DOWN: do_scroll(g_app.scrollY + 40); return 0;
        case VK_PRIOR: do_scroll(g_app.scrollY - g_app.clientH); return 0;
        case VK_NEXT: do_scroll(g_app.scrollY + g_app.clientH); return 0;
        case VK_HOME: do_scroll(0); return 0;
        case VK_END: do_scroll(g_app.maxScrollY); return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

/* ---------------- Entry Point ---------------- */

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXW wc = {0};
    MSG msg;
    HACCEL hAccel;
    HWND hWnd;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    (void)hPrevInstance;
    (void)lpCmdLine;

    /* Handle command-line arguments */
    if (argc >= 2) {
        if (!_wcsicmp(argv[1], L"--register") || !_wcsicmp(argv[1], L"/register")) {
            register_file_association(1);
            MessageBoxW(NULL, L"Lilac이 *.md 파일 기본 프로그램으로 성공적으로 등록되었습니다.", L"Lilac", MB_OK | MB_ICONINFORMATION);
            LocalFree(argv);
            return 0;
        }
        if (!_wcsicmp(argv[1], L"--unregister") || !_wcsicmp(argv[1], L"/unregister")) {
            register_file_association(0);
            MessageBoxW(NULL, L"Lilac *.md 파일 연결이 해제되었습니다.", L"Lilac", MB_OK | MB_ICONINFORMATION);
            LocalFree(argv);
            return 0;
        }
        if (!_wcsicmp(argv[1], L"--export-pdf") && argc >= 4) {
            wchar_t err[256] = {0};
            Doc *doc;
            font_init();
            settings_load();
            doc = doc_load_file(argv[2]);
            if (!doc) {
                LocalFree(argv);
                return 2;
            }
            if (export_pdf(doc, argv[3], err, 256) != 0) {
                doc_free(doc);
                font_shutdown();
                LocalFree(argv);
                return 3;
            }
            doc_free(doc);
            font_shutdown();
            LocalFree(argv);
            return 0;
        }
        if (!_wcsicmp(argv[1], L"--export-docx") && argc >= 4) {
            wchar_t err[256] = {0};
            Doc *doc;
            font_init();
            settings_load();
            doc = doc_load_file(argv[2]);
            if (!doc) {
                LocalFree(argv);
                return 2;
            }
            if (docx_write(doc, argv[3], err, 256) != 0) {
                doc_free(doc);
                font_shutdown();
                LocalFree(argv);
                return 3;
            }
            doc_free(doc);
            font_shutdown();
            LocalFree(argv);
            return 0;
        }
    }

    /* Initialize application state */
    memset(&g_app, 0, sizeof g_app);
    g_app.baseSize = 15.0f;
    g_app.darkMode = 0;
    g_app.hoverLink = -1;
    g_app.hHandCursor = LoadCursor(NULL, IDC_HAND);
    g_app.hArrowCursor = LoadCursor(NULL, IDC_ARROW);

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
        LocalFree(argv);
        return 1;
    }

    hWnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        g_className,
        L"Lilac",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1000, 750,
        NULL, NULL, hInstance, NULL);

    if (!hWnd) {
        LocalFree(argv);
        return 1;
    }

    g_app.hWnd = hWnd;

    /* If a file was passed on the command line (e.g. double click), open it */
    if (argc >= 2 && argv[1][0] != '-' && argv[1][0] != '/') {
        open_file(argv[1]);
    } else {
        load_sample_content();
    }
    LocalFree(argv);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_ACCEL));

    while (GetMessageW(&msg, NULL, 0, 0)) {
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
