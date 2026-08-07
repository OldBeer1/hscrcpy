#include "cph_window.h"
#include "cph_i18n_data.h"
#include "msgbox_util.h"
#include "log.h"
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <commdlg.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <thread>
#include <atomic>

namespace hscrcpy {

// ── Static member definitions ──────────────────────────────

HWND CphWindow::g_hwnd = nullptr;
HINSTANCE CphWindow::g_inst = nullptr;
HWND CphWindow::g_parent = nullptr;
int CphWindow::g_panel_w = 460;

HWND CphWindow::g_tab = nullptr;

HWND CphWindow::g_gnss_lat_edit = nullptr;
HWND CphWindow::g_gnss_lng_edit = nullptr;
HWND CphWindow::g_gnss_alt_edit = nullptr;
HWND CphWindow::g_gnss_speed_edit = nullptr;
HWND CphWindow::g_gnss_bearing_edit = nullptr;
HWND CphWindow::g_gnss_hacc_edit = nullptr;
HWND CphWindow::g_gnss_vacc_edit = nullptr;
HWND CphWindow::g_gnss_sacc_edit = nullptr;
HWND CphWindow::g_gnss_bacc_edit = nullptr;
HWND CphWindow::g_gnss_inject_btn = nullptr;
HWND CphWindow::g_gnss_refresh_btn = nullptr;
HWND CphWindow::g_gnss_current_label = nullptr;

HWND CphWindow::g_gnss_traj_edit = nullptr;
HWND CphWindow::g_gnss_traj_browse_btn = nullptr;
HWND CphWindow::g_gnss_traj_start_btn = nullptr;
HWND CphWindow::g_gnss_traj_stop_btn = nullptr;

HWND CphWindow::g_bat_capacity_edit = nullptr;
HWND CphWindow::g_bat_temp_edit = nullptr;
HWND CphWindow::g_bat_voltage_edit = nullptr;
HWND CphWindow::g_bat_voltage_max_edit = nullptr;
HWND CphWindow::g_bat_current_edit = nullptr;
HWND CphWindow::g_bat_current_avg_edit = nullptr;
HWND CphWindow::g_bat_current_max_edit = nullptr;
HWND CphWindow::g_bat_health_combo = nullptr;
HWND CphWindow::g_bat_present_combo = nullptr;
HWND CphWindow::g_bat_total_energy_edit = nullptr;
HWND CphWindow::g_bat_technology_edit = nullptr;
HWND CphWindow::g_bat_plugged_combo = nullptr;
HWND CphWindow::g_bat_charger_combo = nullptr;
HWND CphWindow::g_bat_chargestate_combo = nullptr;
HWND CphWindow::g_bat_inject_btn = nullptr;
HWND CphWindow::g_bat_reset_btn = nullptr;
HWND CphWindow::g_bat_refresh_btn = nullptr;
HWND CphWindow::g_bat_current_label = nullptr;
HWND CphWindow::g_bat_scene_combo = nullptr;
HWND CphWindow::g_bat_scene_btn = nullptr;

HWND CphWindow::g_status_label = nullptr;

HWND CphWindow::g_haps_pkg_list = nullptr;
HWND CphWindow::g_haps_perm_list = nullptr;
HWND CphWindow::g_haps_refresh_btn = nullptr;
HWND CphWindow::g_haps_grant_all_btn = nullptr;
HWND CphWindow::g_haps_grant_btn = nullptr;
HWND CphWindow::g_haps_revoke_btn = nullptr;
HWND CphWindow::g_haps_revoke_all_btn = nullptr;
std::vector<std::string> CphWindow::g_haps_packages;
std::string CphWindow::g_haps_selected_pkg;
std::string CphWindow::g_haps_user_id;
bool CphWindow::g_haps_loaded = false;
bool CphWindow::g_bat_inject_filled = false;
bool CphWindow::g_gnss_inject_filled = false;
bool CphWindow::g_haps_bm_unsupported = false;

// I18n controls
HWND CphWindow::g_i18n_lang_combo = nullptr;
HWND CphWindow::g_i18n_region_combo = nullptr;
HWND CphWindow::g_i18n_tz_combo = nullptr;
HWND CphWindow::g_i18n_24hour_check = nullptr;
HWND CphWindow::g_i18n_apply_btn = nullptr;
HWND CphWindow::g_i18n_refresh_btn = nullptr;
HWND CphWindow::g_i18n_date_edit = nullptr;
HWND CphWindow::g_i18n_time_edit = nullptr;
HWND CphWindow::g_i18n_set_datetime_btn = nullptr;
HWND CphWindow::g_i18n_autotime_btn = nullptr;
HWND CphWindow::g_i18n_current_label = nullptr;
bool CphWindow::g_i18n_loaded = false;

// Sensor controls
HWND CphWindow::g_sensor_degree_label = nullptr;
HWND CphWindow::g_sensor_shaking_label = nullptr;
HWND CphWindow::g_sensor_source_label = nullptr;
HWND CphWindow::g_sensor_rotate_combo = nullptr;
HWND CphWindow::g_sensor_rotate_btn = nullptr;
HWND CphWindow::g_sensor_rotate_next_btn = nullptr;
HWND CphWindow::g_sensor_reset_btn = nullptr;
HWND CphWindow::g_sensor_shake_edit = nullptr;
HWND CphWindow::g_sensor_shake_btn = nullptr;
HWND CphWindow::g_sensor_refresh_btn = nullptr;
bool CphWindow::g_sensor_loaded = false;

HWND CphWindow::g_tab_pages[TAB_COUNT] = {};

CphWindow::OnShellCommand CphWindow::g_on_shell;
CphWindow::OnPushFile CphWindow::g_on_push;
CphWindow::OnBusy CphWindow::g_on_busy;
int CphWindow::g_current_tab = TAB_GNSS;
std::atomic<bool> CphWindow::g_gnss_busy{false};
std::atomic<bool> CphWindow::g_battery_busy{false};
std::atomic<bool> CphWindow::g_haps_busy{false};
std::atomic<bool> CphWindow::g_i18n_busy{false};
std::atomic<bool> CphWindow::g_sensor_busy{false};
UINT_PTR CphWindow::g_refresh_timer = 0;

// Timer ID for auto-refresh
static const int CPH_REFRESH_TIMER_ID = 2001;
static const int CPH_REFRESH_INTERVAL_MS = 5000;

// Flag: Refresh button should fill edit controls
static bool s_refresh_fill_edits = false;

// Consecutive refresh failure counter - stop auto-refresh after 3 failures
static int s_refresh_fail_count = 0;
static const int MAX_REFRESH_FAILS = 3;

// Custom message for async operation completion
static const UINT WM_CPH_ASYNC_DONE = RegisterWindowMessageW(L"HScrcpyCphAsyncDone");
// Custom message for retrying a busy async operation
static const UINT WM_CPH_RETRY_ASYNC = RegisterWindowMessageW(L"HScrcpyCphRetryAsync");

HWND CphWindow::g_toast_hwnd = nullptr;

static const wchar_t CPH_CLASS_W[] = L"HScrcpyCphClass";
static const wchar_t CPH_PAGE_W[] = L"HScrcpyCphPageClass";

// I18n reference data (454 locales, 473 timezones, 245 regions) now in
// cph_i18n_data.h. The old hardcoded arrays here were incomplete (83 langs,
// 24 tzs, 249 regions) and used CBS_DROPDOWNLIST — replaced by CBS_DROPDOWN
// with live filtering in i18n_filter_combo().

// Tab control subclass: fill white background instead of system COLOR_BTNFACE gray
static WNDPROC g_tab_orig_proc = nullptr;
static HFONT g_tab_bold_font = nullptr;  // cached bold font for selected tab button
static LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    return CallWindowProcW(g_tab_orig_proc, hwnd, msg, wp, lp);
}

// Trajectory edit hint: gray placeholder text (no manifest → no EM_SETCUEBANNER)
static HWND s_traj_edit_hwnd = nullptr;
static bool s_traj_hint_active = true;

static LRESULT CALLBACK TrajEditSubProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    if (msg == WM_SETFOCUS) {
        if (s_traj_hint_active) {
            s_traj_hint_active = false;
            SetWindowTextW(hwnd, L"");
            InvalidateRect(hwnd, nullptr, TRUE);
        }
    } else if (msg == WM_KILLFOCUS) {
        wchar_t buf[256] = {};
        GetWindowTextW(hwnd, buf, 256);
        if (buf[0] == 0) {
            s_traj_hint_active = true;
            SetWindowTextW(hwnd, L"/data/local/tmp/gpx.xml");
            InvalidateRect(hwnd, nullptr, TRUE);
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, TrajEditSubProc, 0);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Helper: set scroll range for a tab page
static void set_page_scroll(HWND page, int content_height, int page_height) {
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE };
    si.nMin = 0;
    si.nMax = content_height - 1;
    si.nPage = page_height;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
}

// Helper: trim trailing newlines from string
static std::string trim_trailing_newlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// Page WndProc: forward WM_COMMAND and WM_NOTIFY to the CPH main window
static LRESULT CALLBACK CphPageWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_VSCROLL) {
        SCROLLINFO si = { sizeof(si), SIF_POS | SIF_RANGE | SIF_PAGE };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int old_pos = si.nPos;
        int new_pos = si.nPos;
        switch (LOWORD(wp)) {
            case SB_TOP: new_pos = si.nMin; break;
            case SB_BOTTOM: new_pos = si.nMax; break;
            case SB_LINEUP: new_pos -= 20; break;
            case SB_LINEDOWN: new_pos += 20; break;
            case SB_PAGEUP: new_pos -= si.nPage; break;
            case SB_PAGEDOWN: new_pos += si.nPage; break;
            case SB_THUMBTRACK: new_pos = si.nTrackPos; break;
        }
        new_pos = std::max(si.nMin, std::min(new_pos, si.nMax - (int)si.nPage + 1));
        if (new_pos != old_pos) {
            ScrollWindow(hwnd, 0, old_pos - new_pos, NULL, NULL);
            si.nPos = new_pos;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        SCROLLINFO si = { sizeof(si), SIF_POS | SIF_RANGE | SIF_PAGE };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int lines = delta / WHEEL_DELTA * 3;
        int new_pos = si.nPos - lines * 20;
        new_pos = std::max(si.nMin, std::min(new_pos, si.nMax - (int)si.nPage + 1));
        if (new_pos != si.nPos) {
            ScrollWindow(hwnd, 0, si.nPos - new_pos, NULL, NULL);
            si.nPos = new_pos;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        HWND cph_hwnd = GetParent(GetParent(hwnd));  // page -> tab -> cph window
        if (cph_hwnd) SendMessageW(cph_hwnd, WM_COMMAND, wp, lp);
        return 0;
    }
    if (msg == WM_NOTIFY) {
        HWND cph_hwnd = GetParent(GetParent(hwnd));
        if (cph_hwnd) SendMessageW(cph_hwnd, WM_NOTIFY, wp, lp);
        return 0;
    }
    if (msg == WM_CONTEXTMENU) {
        HWND cph_hwnd = GetParent(GetParent(hwnd));
        if (cph_hwnd) SendMessageW(cph_hwnd, WM_CONTEXTMENU, wp, lp);
        return 0;
    }
    if (msg == WM_ERASEBKGND) {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    if (msg == WM_CTLCOLOREDIT) {
        HDC hdc = (HDC)wp;
        HWND hEdit = (HWND)lp;
        SetBkColor(hdc, RGB(255, 255, 255));
        if (hEdit == s_traj_edit_hwnd && s_traj_hint_active) {
            SetTextColor(hdc, RGB(160, 160, 160));  // gray hint
        } else {
            SetTextColor(hdc, RGB(0, 0, 0));  // normal black
        }
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    if (msg == WM_CTLCOLORSTATIC) {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    if (msg == WM_CTLCOLORBTN) {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, RGB(255, 255, 255));
        SetTextColor(hdc, RGB(0, 0, 0));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    if (msg == WM_DRAWITEM) {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        if (dis->CtlType == ODT_BUTTON) {
            FillRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(WHITE_BRUSH));
            wchar_t buf[32] = {};
            GetWindowTextW(dis->hwndItem, buf, 32);
            int ox = 0, oy = 0;
            if (dis->itemState & ODS_SELECTED) { ox = 1; oy = 1; }
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, (dis->itemState & ODS_DISABLED) ? RGB(160, 160, 160) : RGB(0, 0, 0));
            // Draw thin gray border for text buttons
            if (!SendMessageW(dis->hwndItem, BM_GETIMAGE, IMAGE_ICON, 0)) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
                HPEN old = (HPEN)SelectObject(dis->hDC, pen);
                HBRUSH oldb = (HBRUSH)SelectObject(dis->hDC, GetStockObject(WHITE_BRUSH));
                Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
                SelectObject(dis->hDC, old);
                SelectObject(dis->hDC, oldb);
                DeleteObject(pen);
            }
            RECT tr = dis->rcItem;
            tr.left += ox; tr.top += oy;
            DrawTextW(dis->hDC, buf, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Register the page class once (with background brush to prevent overlap bleed)
static void ensure_page_class_registered(HINSTANCE inst) {
    static bool reg = false;
    if (reg) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CphPageWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = CPH_PAGE_W;
    RegisterClassExW(&wc);
    reg = true;
}

// ── Cloud phone detection ──────────────────────────────────

bool CphWindow::is_cloud_phone(OnShellCommand on_shell) {
    auto shell = on_shell ? on_shell : g_on_shell;
    if (!shell) { LOGE("CPH is_cloud_phone: no shell callback"); return false; }
    std::string output;
    // Cloud phone detection: /system/bin/cmd exists (even if CPH libraries are missing)
    bool ok = shell("cmd", &output, 5);
    LOGE("CPH is_cloud_phone: shell ok=%d output=[%.300s]", ok, output.c_str());
    if (!ok) return false;
    // If cmd is not found at all, it's not a cloud phone
    if (output.find("not found") != std::string::npos ||
        output.find("No such file") != std::string::npos) return false;
    // If cmd produced any output (including CPH library errors), it's a cloud phone
    // /system/bin/cmd exists only on cloud phone devices
    return !output.empty();
}

// ── Toast notification ─────────────────────────────────────

struct ToastData {
    wchar_t *text;
    bool success;
};

static LRESULT CALLBACK CphToastWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
        auto *data = reinterpret_cast<ToastData *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        SetTimer(hwnd, 1, 2000, nullptr);
        return 0;
    }
    if (msg == WM_TIMER) {
        KillTimer(hwnd, 1);
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        auto *data = reinterpret_cast<ToastData *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (data) {
            delete[] data->text;
            delete data;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        auto *data = reinterpret_cast<ToastData *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!data) {
            EndPaint(hwnd, &ps);
            return 0;
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        // Fill background with success/failure color
        COLORREF bg_color = data->success ? RGB(102, 187, 106) : RGB(239, 154, 154);
        HBRUSH bg_brush = CreateSolidBrush(bg_color);
        FillRect(hdc, &rc, bg_brush);
        DeleteObject(bg_brush);
        SetBkMode(hdc, TRANSPARENT);
        HFONT hf = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(hdc, hf);
        DrawTextW(hdc, data->text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        DeleteObject(hf);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CphWindow::show_toast(const char *text, bool success) {
    if (!g_hwnd) return;
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CphToastWndProc;
        wc.hInstance = g_inst;
        wc.hbrBackground = nullptr;  // Background drawn in WM_PAINT
        wc.lpszClassName = L"HScrcpyCphToastClass";
        RegisterClassExW(&wc);
        reg = true;
    }
    if (g_toast_hwnd && IsWindow(g_toast_hwnd)) DestroyWindow(g_toast_hwnd);
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    auto *wtext = new wchar_t[len];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, len);
    auto *data = new ToastData{wtext, success};
    // Calculate width based on text length
    HDC dc = GetDC(g_hwnd);
    HFONT hf = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(dc, hf);
    SIZE sz;
    GetTextExtentPoint32W(dc, wtext, len - 1, &sz);
    SelectObject(dc, old);
    DeleteObject(hf);
    ReleaseDC(g_hwnd, dc);
    int tw = sz.cx + 24;
    if (tw < 160) tw = 160;
    if (tw > 420) tw = 420;
    // Center toast on the entire main hscrcpy window (g_parent) - use screen coordinates for popup
    RECT prc;
    GetWindowRect(g_parent, &prc);
    int screen_x = (prc.left + prc.right) / 2 - tw / 2;
    int screen_y = (prc.top + prc.bottom) / 2 - 15;
    g_toast_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                    L"HScrcpyCphToastClass", L"",
                                    WS_POPUP | WS_VISIBLE,
                                    screen_x, screen_y,
                                    tw, 30,
                                    g_parent, nullptr, g_inst, data);
}

void CphWindow::msg_box(const char *title, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    HWND parent = g_parent ? g_parent : g_hwnd;
    hscrcpy::centered_msgbox_a(parent, buf, title, MB_OK);
}

void CphWindow::set_status(const char *text) {
    if (g_status_label) SetWindowTextA(g_status_label, text);
}

// ── UI helpers ─────────────────────────────────────────────

static HFONT g_cph_font = nullptr;

static HWND make_label(HWND parent, const wchar_t *text, int x, int y, int w, int h) {
    HWND hw = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                             x, y, w, h, parent, nullptr, nullptr, nullptr);
    if (g_cph_font) SendMessageW(hw, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
    return hw;
}

static HWND make_label_left(HWND parent, const wchar_t *text, int x, int y, int w, int h) {
    HWND hw = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             x, y, w, h, parent, nullptr, nullptr, nullptr);
    if (g_cph_font) SendMessageW(hw, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
    return hw;
}

static HWND make_edit(HWND parent, int id, const wchar_t *def, int x, int y, int w, int h) {
    HWND hw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", def ? def : L"",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    if (g_cph_font) SendMessageW(hw, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
    return hw;
}

static HWND make_btn(HWND parent, const wchar_t *text, int id, int x, int y, int w, int h) {
    HWND hw = CreateWindowW(L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    if (g_cph_font) SendMessageW(hw, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
    return hw;
}

static HWND make_combo(HWND parent, int id, int x, int y, int w, int h) {
    HWND hw = CreateWindowW(L"COMBOBOX", L"",
                             WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    if (g_cph_font) SendMessageW(hw, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
    return hw;
}

// Editable combobox (CBS_DROPDOWN): user can type arbitrary text, and the
// list is filtered live by i18n_filter_combo() on CBN_EDITCHANGE. Used for
// the i18n Language / Region / Timezone pickers where the full list has
// hundreds of entries and a plain CBS_DROPDOWNLIST would be unusable.
static HWND make_editable_combo(HWND parent, int id, int x, int y, int w, int h) {
    HWND hw = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | WS_VSCROLL | CBS_AUTOHSCROLL,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    if (g_cph_font) SendMessageW(hw, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
    SendMessageW(hw, CB_SETMINVISIBLE, 30, 0);  // cap dropdown height for 473-tz list
    return hw;
}

static HWND make_group(HWND parent, const wchar_t *text, int x, int y, int w, int h) {
    HWND hw = CreateWindowW(L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                             x, y, w, h, parent, nullptr, nullptr, nullptr);
    if (g_cph_font) SendMessageW(hw, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
    return hw;
}

// ── Haps: Parse helpers ─────────────────────────────────────

// Parse "bm dump -a" output → package names
// Output format: "ID: 100:" header + indented package names
static std::vector<std::string> parse_packages(const std::string &output) {
    std::vector<std::string> pkgs;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);
        if (trimmed.rfind("ID:", 0) == 0) continue;  // skip "ID: 100:" header
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);
        if (!trimmed.empty()) pkgs.push_back(trimmed);
    }
    return pkgs;
}

// Parse "bm dump-permissions -n <pkg>" output → (name, granted, system_fixed) triples
// Output format: header line + "  <perm>  [GRANTED|DENIED]  <- system-fixed"
static std::vector<HapsPermission> parse_permissions(const std::string &output) {
    std::vector<HapsPermission> perms;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        auto bracket = line.find('[');
        if (bracket == std::string::npos) continue;
        std::string name = line.substr(0, bracket);
        size_t s = name.find_first_not_of(" \t");
        size_t e = name.find_last_not_of(" \t");
        if (s == std::string::npos) continue;
        name = name.substr(s, e - s + 1);
        bool is_granted = (line.find("[GRANTED]", bracket) != std::string::npos);
        bool is_fixed = (line.find("system-fixed", bracket) != std::string::npos);
        perms.push_back({name, is_granted, is_fixed});
    }
    return perms;
}

// ── Haps: Populate ListViews ────────────────────────────────

void CphWindow::haps_populate_packages() {
    if (!g_haps_pkg_list) return;
    SendMessageW(g_haps_pkg_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_haps_pkg_list, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < (int)g_haps_packages.size(); i++) {
        int len = MultiByteToWideChar(CP_UTF8, 0, g_haps_packages[i].c_str(), -1, nullptr, 0);
        std::vector<wchar_t> wname(len);
        MultiByteToWideChar(CP_UTF8, 0, g_haps_packages[i].c_str(), -1, wname.data(), len);
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = wname.data();
        SendMessageW(g_haps_pkg_list, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
    }
    SendMessageW(g_haps_pkg_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_haps_pkg_list, nullptr, TRUE);
    UpdateWindow(g_haps_pkg_list);
    // Auto-select first item → triggers LVN_ITEMCHANGED → loads permissions
    if (!g_haps_packages.empty()) {
        LVITEMW lvi = {};
        lvi.state = LVIS_SELECTED;
        lvi.stateMask = LVIS_SELECTED;
        SendMessageW(g_haps_pkg_list, LVM_SETITEMSTATE, 0, (LPARAM)&lvi);
    }
}

void CphWindow::haps_populate_permissions(const std::vector<HapsPermission> &perms) {
    if (!g_haps_perm_list) return;
    SendMessageW(g_haps_perm_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_haps_perm_list, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < (int)perms.size(); i++) {
        int len = MultiByteToWideChar(CP_UTF8, 0, perms[i].name.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> wname(len);
        MultiByteToWideChar(CP_UTF8, 0, perms[i].name.c_str(), -1, wname.data(), len);
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = wname.data();
        SendMessageW(g_haps_perm_list, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

        const char *status = perms[i].granted ? "Granted" : "Denied";
        int slen = MultiByteToWideChar(CP_UTF8, 0, status, -1, nullptr, 0);
        std::vector<wchar_t> wstatus(slen);
        MultiByteToWideChar(CP_UTF8, 0, status, -1, wstatus.data(), slen);
        lvi.iSubItem = 1;
        lvi.pszText = wstatus.data();
        SendMessageW(g_haps_perm_list, LVM_SETITEMTEXTW, i, (LPARAM)&lvi);

        const char *fixed = perms[i].system_fixed ? "Yes" : "";
        int flen = MultiByteToWideChar(CP_UTF8, 0, fixed, -1, nullptr, 0);
        std::vector<wchar_t> wfixed(flen);
        MultiByteToWideChar(CP_UTF8, 0, fixed, -1, wfixed.data(), flen);
        lvi.iSubItem = 2;
        lvi.pszText = wfixed.data();
        SendMessageW(g_haps_perm_list, LVM_SETITEMTEXTW, i, (LPARAM)&lvi);
    }
    SendMessageW(g_haps_perm_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_haps_perm_list, nullptr, TRUE);
    UpdateWindow(g_haps_perm_list);
}

// Collect selected permission names from the permissions ListView as CSV
std::string CphWindow::haps_get_selected_perms_csv() {
    if (!g_haps_perm_list) return "";
    std::string csv;
    int idx = -1;
    for (;;) {
        idx = (int)SendMessageW(g_haps_perm_list, LVM_GETNEXTITEM, idx, LVNI_SELECTED);
        if (idx == -1) break;
        wchar_t wname[256] = {};
        LVITEMW lvi = {};
        lvi.iSubItem = 0;
        lvi.pszText = wname;
        lvi.cchTextMax = 256;
        SendMessageW(g_haps_perm_list, LVM_GETITEMTEXTW, idx, (LPARAM)&lvi);
        if (!csv.empty()) csv += ",";
        int len = WideCharToMultiByte(CP_UTF8, 0, wname, -1, nullptr, 0, nullptr, nullptr);
        std::string name(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, &name[0], len, nullptr, nullptr);
        if (!name.empty() && name.back() == '\0') name.pop_back();
        csv += name;
    }
    return csv;
}

// Copy text to Windows clipboard. Returns true on success.
static bool copy_text_to_clipboard(HWND hwnd, const std::string &text) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    bool ok = false;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (hg) {
        char *p = (char *)GlobalLock(hg);
        if (p) {
            memcpy(p, text.data(), text.size());
            p[text.size()] = 0;
            GlobalUnlock(hg);
            // SetClipboardData transfers ownership to clipboard on success
            ok = SetClipboardData(CF_TEXT, hg) != NULL;
            if (!ok) GlobalFree(hg);
        } else {
            GlobalFree(hg);
        }
    }
    CloseClipboard();
    return ok;
}

// Detect "bm: '<cmd>' is not a valid bm command" (low-version HarmonyOS
// where grant/revoke/dump-permissions subcommands don't exist yet)
static bool bm_perm_unsupported(const std::string &output) {
    return output.find("is not a valid bm command") != std::string::npos;
}

// Enable/disable Haps permission action buttons
void CphWindow::haps_enable_perm_buttons(bool enable) {
    if (g_haps_grant_btn) EnableWindow(g_haps_grant_btn, enable);
    if (g_haps_revoke_btn) EnableWindow(g_haps_revoke_btn, enable);
    if (g_haps_grant_all_btn) EnableWindow(g_haps_grant_all_btn, enable);
    if (g_haps_revoke_all_btn) EnableWindow(g_haps_revoke_all_btn, enable);
}

// ── Haps: Async operations ──────────────────────────────────

void CphWindow::haps_list_packages() {
    set_status("Loading packages...");
    std::string cmd = "bm dump -a";
    if (!g_haps_user_id.empty()) cmd += " -u " + g_haps_user_id;
    run_async(ASYNC_HAPS_LIST_PACKAGES, cmd);
}

void CphWindow::haps_list_permissions() {
    if (g_haps_selected_pkg.empty()) return;
    std::string cmd = "bm dump-permissions -n " + g_haps_selected_pkg;
    if (!g_haps_user_id.empty()) cmd += " -u " + g_haps_user_id;
    run_async(ASYNC_HAPS_LIST_PERMISSIONS, cmd);
}

void CphWindow::haps_grant(const std::string &perms_csv) {
    if (g_haps_selected_pkg.empty()) return;
    std::string cmd = "bm grant -n " + g_haps_selected_pkg + " -p " + perms_csv;
    if (!g_haps_user_id.empty()) cmd += " -u " + g_haps_user_id;
    run_async(ASYNC_HAPS_GRANT, cmd);
}

void CphWindow::haps_revoke(const std::string &perms_csv) {
    if (g_haps_selected_pkg.empty()) return;
    std::string cmd = "bm revoke -n " + g_haps_selected_pkg + " -p " + perms_csv;
    if (!g_haps_user_id.empty()) cmd += " -u " + g_haps_user_id;
    run_async(ASYNC_HAPS_REVOKE, cmd);
}

void CphWindow::haps_grant_all() {
    if (g_haps_selected_pkg.empty()) return;
    std::string cmd = "bm grant -n " + g_haps_selected_pkg + " -a";
    if (!g_haps_user_id.empty()) cmd += " -u " + g_haps_user_id;
    run_async(ASYNC_HAPS_GRANT_ALL, cmd);
}

void CphWindow::haps_revoke_all() {
    if (g_haps_selected_pkg.empty()) return;
    std::string cmd = "bm revoke -n " + g_haps_selected_pkg + " -a";
    if (!g_haps_user_id.empty()) cmd += " -u " + g_haps_user_id;
    run_async(ASYNC_HAPS_REVOKE_ALL, cmd);
}

// ── Public API ─────────────────────────────────────────────

void CphWindow::create(HINSTANCE inst, HWND parent, int width) {
    g_inst = inst;
    g_parent = parent;
    g_panel_w = width;

    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX icex = {sizeof(icex), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&icex);

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = CPH_CLASS_W;
        RegisterClassExW(&wc);
        registered = true;
    }
}

void CphWindow::show(OnShellCommand on_shell, OnPushFile on_push, OnBusy on_busy) {
    g_on_shell = on_shell;
    g_on_push = on_push;
    g_on_busy = on_busy;

    if (!g_hwnd) {
        g_hwnd = CreateWindowExW(0, CPH_CLASS_W, L"",
                                  WS_CHILD | WS_BORDER | WS_CLIPCHILDREN,
                                  0, 0, g_panel_w, 100, g_parent, nullptr, g_inst, nullptr);
    }
    ShowWindow(g_hwnd, SW_SHOW);
    InvalidateRect(g_hwnd, nullptr, TRUE);
    // Start auto-refresh timer (1 second)
    if (g_refresh_timer) KillTimer(g_hwnd, CPH_REFRESH_TIMER_ID);
    g_refresh_timer = SetTimer(g_hwnd, CPH_REFRESH_TIMER_ID, CPH_REFRESH_INTERVAL_MS, nullptr);
    // Initial refresh
    if (g_current_tab == TAB_GNSS) { s_refresh_fill_edits = true; refresh_gnss(); }
    else if (g_current_tab == TAB_BATTERY) { s_refresh_fill_edits = true; refresh_battery(); }
    else if (g_current_tab == TAB_HAPS) { if (!g_haps_loaded) haps_list_packages(); }
    else if (g_current_tab == TAB_I18N) { if (!g_i18n_loaded) i18n_refresh(); }
    else if (g_current_tab == TAB_SENSOR) { if (!g_sensor_loaded) sensor_refresh(); }
}

void CphWindow::hide() {
    if (g_hwnd) {
        if (g_refresh_timer) { KillTimer(g_hwnd, CPH_REFRESH_TIMER_ID); g_refresh_timer = 0; }
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

void CphWindow::reset() {
    // GNSS — clear edit boxes and current location label
    // (traj edit keeps its hint — restored below, not cleared)
    HWND gnss_edits[] = {g_gnss_lat_edit, g_gnss_lng_edit, g_gnss_alt_edit,
        g_gnss_speed_edit, g_gnss_bearing_edit, g_gnss_hacc_edit,
        g_gnss_vacc_edit, g_gnss_sacc_edit, g_gnss_bacc_edit};
    for (HWND h : gnss_edits) { if (h) SetWindowTextW(h, L""); }
    if (g_gnss_traj_edit) {
        s_traj_hint_active = true;
        SetWindowTextW(g_gnss_traj_edit, L"/data/local/tmp/gpx.xml");
    }
    if (g_gnss_current_label) SetWindowTextW(g_gnss_current_label, L"—");

    // Battery — clear edit boxes, reset combos to first item, clear label
    HWND bat_edits[] = {g_bat_capacity_edit, g_bat_temp_edit, g_bat_voltage_edit,
        g_bat_voltage_max_edit, g_bat_current_edit, g_bat_current_avg_edit,
        g_bat_current_max_edit, g_bat_total_energy_edit, g_bat_technology_edit};
    for (HWND h : bat_edits) { if (h) SetWindowTextW(h, L""); }
    HWND bat_combos[] = {g_bat_health_combo, g_bat_present_combo, g_bat_plugged_combo,
        g_bat_charger_combo, g_bat_chargestate_combo, g_bat_scene_combo};
    for (HWND h : bat_combos) { if (h) SendMessageW(h, CB_SETCURSEL, 0, 0); }
    if (g_bat_current_label) SetWindowTextW(g_bat_current_label, L"—");

    // Haps — clear ListViews and cached data
    if (g_haps_pkg_list) SendMessageW(g_haps_pkg_list, LVM_DELETEALLITEMS, 0, 0);
    if (g_haps_perm_list) SendMessageW(g_haps_perm_list, LVM_DELETEALLITEMS, 0, 0);
    g_haps_packages.clear();
    g_haps_selected_pkg.clear();
    g_haps_user_id.clear();
    g_haps_loaded = false;
    g_haps_bm_unsupported = false;
    haps_enable_perm_buttons(true);

    // I18n — reset combos, checkboxes, edits, current label
    HWND i18n_combos[] = {g_i18n_lang_combo, g_i18n_region_combo, g_i18n_tz_combo};
    for (HWND h : i18n_combos) { if (h) SendMessageW(h, CB_SETCURSEL, (WPARAM)-1, 0); }
    if (g_i18n_24hour_check) SendMessageW(g_i18n_24hour_check, BM_SETCHECK, BST_UNCHECKED, 0);
    if (g_i18n_autotime_btn) SendMessageW(g_i18n_autotime_btn, BM_SETCHECK, BST_UNCHECKED, 0);
    // Re-enable mutex controls (AutoTime unchecked → all enabled)
    HWND mutex_ctrls[] = {g_i18n_tz_combo, g_i18n_date_edit, g_i18n_time_edit, g_i18n_set_datetime_btn};
    for (HWND h : mutex_ctrls) { if (h) EnableWindow(h, TRUE); }
    if (g_i18n_date_edit) SetWindowTextW(g_i18n_date_edit, L"");
    if (g_i18n_time_edit) SetWindowTextW(g_i18n_time_edit, L"");
    if (g_i18n_current_label) SetWindowTextW(g_i18n_current_label, L"--");
    g_i18n_loaded = false;

    // Sensor — clear state labels, reset combo/edit
    if (g_sensor_degree_label) SetWindowTextW(g_sensor_degree_label, L"?");
    if (g_sensor_shaking_label) SetWindowTextW(g_sensor_shaking_label, L"?");
    if (g_sensor_source_label) SetWindowTextW(g_sensor_source_label, L"?");
    if (g_sensor_rotate_combo) SendMessageW(g_sensor_rotate_combo, CB_SETCURSEL, 0, 0);
    if (g_sensor_shake_edit) SetWindowTextW(g_sensor_shake_edit, L"1000");
    g_sensor_loaded = false;

    // Reset refresh state
    s_refresh_fill_edits = false;
    s_refresh_fail_count = 0;
    g_gnss_inject_filled = false;
    g_bat_inject_filled = false;
    if (g_status_label) SetWindowTextW(g_status_label, L"");
}

void CphWindow::destroy() {
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
}

bool CphWindow::is_visible() {
    return g_hwnd && IsWindowVisible(g_hwnd);
}

HWND CphWindow::hwnd() { return g_hwnd; }
int CphWindow::panel_width() { return g_panel_w; }
void CphWindow::set_panel_width(int w) { g_panel_w = w; }

void CphWindow::reposition(int x, int y, int w, int h) {
    if (!g_hwnd) return;
    MoveWindow(g_hwnd, x, y, w, h, TRUE);
    // Resize tab control to fill the panel
    if (g_tab) {
        int tw = w - MARGIN * 2;
        MoveWindow(g_tab, MARGIN, HEADER_H, tw, 800, TRUE);
        // Resize tab pages
        RECT tab_rc = {0, 0, tw, 800};
        TabCtrl_AdjustRect(g_tab, FALSE, &tab_rc);
        int pw = tab_rc.right - tab_rc.left;
        int ph = tab_rc.bottom - tab_rc.top;
        for (int i = 0; i < TAB_COUNT; i++) {
            if (g_tab_pages[i])
                MoveWindow(g_tab_pages[i], tab_rc.left, tab_rc.top, pw, ph, TRUE);
        }
    }
}

// ── Tab page switching ─────────────────────────────────────

void CphWindow::on_tab_change() {
    if (!g_tab) return;
    int sel = TabCtrl_GetCurSel(g_tab);
    if (sel >= 0 && sel < TAB_COUNT) {
        show_tab_page(sel);
        if (sel == TAB_GNSS) refresh_gnss();
        else if (sel == TAB_BATTERY) refresh_battery();
        else if (sel == TAB_HAPS) { if (!g_haps_loaded) haps_list_packages(); }
        else if (sel == TAB_I18N) { i18n_refresh(); }
        else if (sel == TAB_SENSOR) { if (!g_sensor_loaded) sensor_refresh(); }
    }
}

void CphWindow::show_tab_page(int page) {
    g_current_tab = page;
    for (int i = 0; i < TAB_COUNT; i++) {
        if (g_tab_pages[i])
            ShowWindow(g_tab_pages[i], i == page ? SW_SHOW : SW_HIDE);
    }
    if (page >= 0 && page < TAB_COUNT && g_tab_pages[page]) {
        InvalidateRect(g_tab_pages[page], nullptr, TRUE);
        UpdateWindow(g_tab_pages[page]);
    }
}

// ── Async shell runner ─────────────────────────────────────

// Per-tab busy flags: same-tab ops are serialized, cross-tab ops run in parallel.
std::atomic<bool>* CphWindow::busy_flag_for(AsyncOp op) {
    switch (op) {
    case ASYNC_REFRESH_GNSS:
    case ASYNC_INJECT_GNSS:
    case ASYNC_START_TRAJ:
    case ASYNC_STOP_TRAJ:
    case ASYNC_INJECT_SCENE:
        return &g_gnss_busy;
    case ASYNC_REFRESH_BATTERY:
    case ASYNC_INJECT_BATTERY:
    case ASYNC_RESET_BATTERY:
        return &g_battery_busy;
    case ASYNC_I18N_GET:
    case ASYNC_I18N_APPLY:
    case ASYNC_I18N_SET_DATETIME:
    case ASYNC_I18N_SET_AUTOTIME:
    case ASYNC_I18N_DISABLE_AUTOTIME:
    case ASYNC_I18N_SET_24HOUR:
        return &g_i18n_busy;
    case ASYNC_SENSOR_REFRESH:
    case ASYNC_SENSOR_ROTATE:
    case ASYNC_SENSOR_ROTATE_RESET:
    case ASYNC_SENSOR_SHAKE:
        return &g_sensor_busy;
    default:
        return &g_haps_busy;
    }
}

void CphWindow::run_async(AsyncOp op, const std::string &cmd) {
    if (!g_on_shell || !g_hwnd) return;
    // Refresh ops are silently dropped if the same tab is busy;
    // user-initiated ops (inject/grant/revoke) are retried.
    bool is_refresh = (op == ASYNC_REFRESH_GNSS || op == ASYNC_REFRESH_BATTERY
        || op == ASYNC_HAPS_LIST_PACKAGES || op == ASYNC_HAPS_LIST_PERMISSIONS
        || op == ASYNC_I18N_GET || op == ASYNC_SENSOR_REFRESH);
    auto *busy = busy_flag_for(op);
    if (busy->exchange(true)) {
        if (!is_refresh) {
            LOGE("CPH run_async: op=%d queued (busy), will retry", (int)op);
            std::string *cmd_copy = new std::string(cmd);
            int op_int = (int)op;
            std::thread([hwnd = g_hwnd, op_int, cmd_copy]() {
                Sleep(500);
                PostMessageW(hwnd, WM_CPH_RETRY_ASYNC, (WPARAM)op_int, (LPARAM)cmd_copy);
            }).detach();
        }
        return;
    }

    std::string shell_cmd = cmd;
    auto on_shell = g_on_shell;
    auto hwnd = g_hwnd;

    std::thread([hwnd, op, shell_cmd, on_shell]() {
        AsyncResult *r = new AsyncResult{op, false, "", shell_cmd};
        int timeout = (op == ASYNC_REFRESH_GNSS || op == ASYNC_REFRESH_BATTERY) ? 5 : 10;
        if (op == ASYNC_INJECT_SCENE) timeout = 3;
        if (op == ASYNC_HAPS_LIST_PERMISSIONS || op == ASYNC_HAPS_GRANT
            || op == ASYNC_HAPS_REVOKE || op == ASYNC_HAPS_GRANT_ALL
            || op == ASYNC_HAPS_REVOKE_ALL) timeout = 5;
        if (op == ASYNC_SENSOR_REFRESH) timeout = 5;
        if (op == ASYNC_SENSOR_ROTATE || op == ASYNC_SENSOR_ROTATE_RESET) timeout = 10;
        if (op == ASYNC_SENSOR_SHAKE) timeout = 15;
        r->ok = on_shell(shell_cmd, &r->output, timeout);
        LOGE("CPH async result: op=%d ok=%d cmd=[%s] output=[%s]", (int)op, r->ok, shell_cmd.c_str(), r->output.c_str());
        PostMessageW(hwnd, WM_CPH_ASYNC_DONE, 0, reinterpret_cast<LPARAM>(r));
    }).detach();
}

void CphWindow::handle_async_done(AsyncResult *r) {
    busy_flag_for(r->op)->store(false);

    switch (r->op) {
    case ASYNC_REFRESH_GNSS: {
        // Case-insensitive error detection
        std::string lower_out = r->output;
        for (auto &c : lower_out) c = (char)tolower((unsigned char)c);
        if (r->ok && lower_out.find("error") == std::string::npos &&
            lower_out.find("failed") == std::string::npos &&
            lower_out.find("not found") == std::string::npos) {
            s_refresh_fail_count = 0;
            parse_gnss_location(r->output, s_refresh_fill_edits);
            s_refresh_fill_edits = false;
            set_status("GNSS location refreshed");
        } else {
            s_refresh_fail_count++;
            if (lower_out.find("error") != std::string::npos ||
                lower_out.find("failed") != std::string::npos ||
                lower_out.find("not found") != std::string::npos) {
                // Error output - show in current label
                SetWindowTextA(g_gnss_current_label, r->output.c_str());
                set_status(r->output.c_str());
            } else if (r->output.find("position switch") != std::string::npos ||
                       r->output.find("gnss server") != std::string::npos) {
                // GNSS server unavailable - could be switch off or server crashed
                set_status("GNSS server unavailable (check location switch in settings)");
            }
        }
        break;
    }

    case ASYNC_INJECT_GNSS:
        if (r->ok && r->output.find("Success") != std::string::npos) {
            show_toast("GNSS location injected", true);
            set_status("GNSS location injected");
            refresh_gnss();
        } else {
            // cmd returned error (e.g. missing latitude/longitude, position switch off)
            std::string err = r->output.empty() ? "Unknown error" : r->output;
            while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
            msg_box("GNSS Inject", "Inject failed:\n%s", err.c_str());
            set_status(err.c_str());
        }
        // Resume auto-refresh timer after inject
        if (g_hwnd && !g_refresh_timer) {
            g_refresh_timer = SetTimer(g_hwnd, CPH_REFRESH_TIMER_ID, CPH_REFRESH_INTERVAL_MS, nullptr);
        }
        break;

    case ASYNC_START_TRAJ:
        if (r->ok && r->output.find("error:") == std::string::npos) {
            show_toast("Trajectory playback started", true);
            set_status(r->cmd.c_str());
        } else {
            std::string err = r->output.empty() ? "Unknown error" : r->output;
            while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
            msg_box("Trajectory", "Trajectory start failed:\n%s", err.c_str());
            set_status(err.c_str());
        }
        break;

    case ASYNC_STOP_TRAJ:
        // Stop may return "gnss server response timeout" even when successfully stopped
        if (r->ok && (r->output.find("error:") == std::string::npos ||
                      r->output.find("timeout") != std::string::npos)) {
            show_toast("Trajectory playback stopped", true);
            set_status("Trajectory stopped");
        } else {
            std::string err = r->output.empty() ? "Unknown error" : r->output;
            while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
            msg_box("Trajectory", "Trajectory stop failed:\n%s", err.c_str());
        }
        break;

    case ASYNC_REFRESH_BATTERY:
        if (r->ok) {
            s_refresh_fail_count = 0;
            parse_battery_info(r->output, s_refresh_fill_edits);
            s_refresh_fill_edits = false;
            set_status("Battery info refreshed");
        } else {
            s_refresh_fail_count++;
        }
        break;

    case ASYNC_INJECT_BATTERY:
        if (r->ok) {
            show_toast("Battery info injected", true);
            set_status(r->cmd.c_str());
            refresh_battery();
        } else {
            if (r->output.find("invalid parameter") != std::string::npos) {
                show_toast("Invalid parameter - check values", false);
            } else {
                show_toast("Battery injection failed", false);
            }
            set_status(r->output.c_str());
        }
        break;

    case ASYNC_RESET_BATTERY:
        if (r->ok) {
            show_toast("Battery reset to default", true);
            set_status("cmd battery info -r");
            s_refresh_fill_edits = true;
            refresh_battery();
        } else {
            show_toast("Battery reset failed", false);
        }
        break;

    case ASYNC_INJECT_SCENE:
        if (r->output.find("Start Charge Scene") != std::string::npos ||
            r->output.find("Success") != std::string::npos) {
            show_toast("Charge scene started", true);
            set_status(r->cmd.c_str());
        } else {
            show_toast("Charge scene failed", false);
            set_status(r->output.empty() ? "No response" : r->output.c_str());
        }
        break;

    case ASYNC_HAPS_LIST_PACKAGES: {
        // hdc returns exit 0 even on failure; check output for bm error
        if (r->ok && r->output.find("error:") == std::string::npos) {
            // Parse user_id from "ID: <number>:" header line
            size_t id_pos = r->output.find("ID:");
            if (id_pos != std::string::npos) {
                size_t num_start = r->output.find_first_not_of(" \t", id_pos + 3);
                if (num_start != std::string::npos) {
                    size_t colon = r->output.find(':', num_start);
                    if (colon != std::string::npos) {
                        g_haps_user_id = r->output.substr(num_start, colon - num_start);
                        while (!g_haps_user_id.empty() && (g_haps_user_id.back() == ' ' || g_haps_user_id.back() == '\t'))
                            g_haps_user_id.pop_back();
                    }
                }
            }
            g_haps_packages = parse_packages(r->output);
            haps_populate_packages();
            g_haps_loaded = true;
            set_status("Packages loaded");
        } else {
            show_toast("Failed to list packages", false);
            set_status(r->output.empty() ? "List packages failed" : r->output.c_str());
        }
        break;
    }

    case ASYNC_HAPS_LIST_PERMISSIONS: {
        // hdc returns exit 0 even on failure; check output for bm error.
        // "no user-grant permissions found" is not an error — the bundle
        // simply has no user-grantable permissions (system apps, uninstalled, etc.)
        if (r->ok && (r->output.find("error:") == std::string::npos
                      || r->output.find("no user-grant permissions found") != std::string::npos)) {
            auto perms = parse_permissions(r->output);
            haps_populate_permissions(perms);
            set_status(perms.empty() ? "No user-grantable permissions" : "Permissions loaded");
        } else if (bm_perm_unsupported(r->output)) {
            // Low-version HarmonyOS: bm grant/revoke/dump-permissions don't exist
            g_haps_bm_unsupported = true;
            haps_populate_permissions({});
            haps_enable_perm_buttons(false);
            set_status("bm permission commands not supported on this HarmonyOS");
            show_toast("bm permission commands not supported on this HarmonyOS", false);
        } else {
            haps_populate_permissions({});  // clear stale list
            show_toast("Failed to load permissions", false);
            set_status(r->output.empty() ? "Permissions load failed" : r->output.c_str());
        }
        break;
    }

    case ASYNC_HAPS_GRANT:
    case ASYNC_HAPS_REVOKE:
    case ASYNC_HAPS_GRANT_ALL:
    case ASYNC_HAPS_REVOKE_ALL: {
        bool succeeded = r->output.find("succeeded") != std::string::npos;
        // Detect system-fixed warning: "warning: the following permissions are fixed
        // by system and cannot be modified" followed by "  - <perm>" lines.
        std::string fixed_perms;
        auto wpos = r->output.find("warning:");
        if (wpos != std::string::npos) {
            std::istringstream iss(r->output.substr(wpos));
            std::string line;
            while (std::getline(iss, line)) {
                // Lines like "  - ohos.permission.XXX"
                auto dash = line.find("- ");
                if (dash == std::string::npos) continue;
                // Stop at non-indented lines (e.g. "grant permission succeeded")
                if (!line.empty() && line[0] != ' ') break;
                std::string perm = line.substr(dash + 2);
                // trim trailing whitespace
                while (!perm.empty() && (perm.back() == ' ' || perm.back() == '\r'
                       || perm.back() == '\t')) perm.pop_back();
                if (perm.empty()) continue;
                if (!fixed_perms.empty()) fixed_perms += ", ";
                fixed_perms += perm;
            }
        }
        if (succeeded) {
            const char *action = (r->op == ASYNC_HAPS_GRANT_ALL) ? "All permissions granted"
                : (r->op == ASYNC_HAPS_REVOKE_ALL) ? "All permissions revoked"
                : (r->op == ASYNC_HAPS_GRANT) ? "Permissions granted" : "Permissions revoked";
            show_toast(action, true);
            set_status(action);
            if (!fixed_perms.empty()) {
                msg_box("Haps", "The following permissions are fixed by system "
                        "and cannot be modified:\n%s", fixed_perms.c_str());
            }
            haps_list_permissions();
        } else if (!fixed_perms.empty()) {
            // All selected permissions were system-fixed → operation failed
            msg_box("Haps", "The following permissions are fixed by system "
                    "and cannot be modified:\n%s", fixed_perms.c_str());
        } else if (bm_perm_unsupported(r->output)) {
            g_haps_bm_unsupported = true;
            haps_enable_perm_buttons(false);
            set_status("bm permission commands not supported on this HarmonyOS");
            msg_box("Haps", "This HarmonyOS version does not support "
                    "bm permission commands (grant/revoke/dump-permissions).");
        } else {
            std::string err = r->output.empty() ? "Unknown error" : r->output;
            while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
            msg_box("Haps", "Operation failed:\n%s", err.c_str());
        }
        break;
    }

    case ASYNC_I18N_GET: {
        if (r->ok && r->output.find("Current locale settings") != std::string::npos) {
            i18n_parse_and_populate(r->output);
            g_i18n_loaded = true;
            set_status("i18n settings loaded");
        } else {
            show_toast("Failed to load i18n settings", false);
            set_status(r->output.empty() ? "i18n load failed" : r->output.c_str());
        }
        break;
    }

    case ASYNC_I18N_APPLY: {
        // i18n_apply only sends -l/-r/-t (not -z/-a — those go through their
        // own checkboxes), so only check those three success markers.
        int success_count = 0;
        if (r->output.find("language succeeded") != std::string::npos) success_count++;
        if (r->output.find("region succeeded") != std::string::npos) success_count++;
        if (r->output.find("timezone succeeded") != std::string::npos) success_count++;
        std::string errors = i18n_extract_errors(r->output);
        if (success_count > 0 && errors.empty()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "i18n: %d setting(s) applied", success_count);
            show_toast(buf, true);
            set_status(buf);
        } else if (success_count > 0 && !errors.empty()) {
            // Partial success: some fields applied, some failed. Show both the
            // count and the verbatim server error lines in the toast.
            std::string toast = "i18n: " + std::to_string(success_count) + " applied, some failed";
            show_toast(toast.c_str(), false);
            set_status(errors.c_str());
        } else {
            // All failed — surface server error verbatim.
            std::string toast = "i18n apply failed";
            if (!errors.empty()) toast += ": " + errors;
            show_toast(toast.c_str(), false);
            set_status(errors.empty() ? "i18n apply failed" : errors.c_str());
        }
        // Refresh in all cases so combos reflect the server's true state.
        i18n_refresh();
        break;
    }

    case ASYNC_I18N_SET_DATETIME: {
        if (r->output.find("set datetime succeeded") != std::string::npos) {
            show_toast("Datetime set", true);
            set_status("i18n datetime set");
        } else {
            std::string errors = i18n_extract_errors(r->output);
            std::string toast = "Set datetime failed";
            if (!errors.empty()) toast += ": " + errors;
            show_toast(toast.c_str(), false);
            set_status(errors.empty() ? "i18n datetime failed" : errors.c_str());
        }
        i18n_refresh();
        break;
    }

    case ASYNC_I18N_SET_AUTOTIME: {
        if (r->output.find("set autotime succeeded") != std::string::npos) {
            show_toast("Auto-time enabled", true);
            set_status("i18n autotime enabled");
        } else {
            std::string errors = i18n_extract_errors(r->output);
            std::string toast = "Enable autotime failed";
            if (!errors.empty()) toast += ": " + errors;
            show_toast(toast.c_str(), false);
            set_status(errors.empty() ? "i18n autotime failed" : errors.c_str());
        }
        // Refresh to restore checkbox/combo to the server's true state on
        // failure (instead of manually reverting just the checkbox).
        i18n_refresh();
        break;
    }

    case ASYNC_I18N_DISABLE_AUTOTIME: {
        // Triggered by unchecking AutoTime (sends -t tz -d datetime, which
        // implicitly disables autotime per design 4.1). Distinct from
        // ASYNC_I18N_SET_DATETIME so the toast reflects user intent.
        if (r->output.find("set datetime succeeded") != std::string::npos) {
            show_toast("Auto-time disabled", true);
            set_status("i18n autotime disabled");
        } else {
            std::string errors = i18n_extract_errors(r->output);
            std::string toast = "Disable autotime failed";
            if (!errors.empty()) toast += ": " + errors;
            show_toast(toast.c_str(), false);
            set_status(errors.empty() ? "i18n disable autotime failed" : errors.c_str());
        }
        i18n_refresh();
        break;
    }

    case ASYNC_I18N_SET_24HOUR: {
        if (r->output.find("24-hour clock succeeded") != std::string::npos) {
            show_toast("24-hour clock updated", true);
            set_status("i18n 24-hour clock set");
        } else {
            std::string errors = i18n_extract_errors(r->output);
            std::string toast = "Set 24-hour clock failed";
            if (!errors.empty()) toast += ": " + errors;
            show_toast(toast.c_str(), false);
            set_status(errors.empty() ? "i18n 24-hour failed" : errors.c_str());
        }
        i18n_refresh();
        break;
    }

    case ASYNC_SENSOR_REFRESH: {
        std::string lower = r->output;
        for (auto &c : lower) c = (char)tolower((unsigned char)c);
        if (r->ok && lower.find("error") == std::string::npos &&
            lower.find("not found") == std::string::npos &&
            lower.find("no such") == std::string::npos) {
            s_refresh_fail_count = 0;
            g_sensor_loaded = true;
            sensor_parse_info(r->output);
            set_status("Sensor state refreshed");
        } else {
            s_refresh_fail_count++;
            if (g_sensor_degree_label) SetWindowTextA(g_sensor_degree_label, "?");
            if (g_sensor_shaking_label) SetWindowTextA(g_sensor_shaking_label, "?");
            if (g_sensor_source_label) SetWindowTextA(g_sensor_source_label, "?");
            set_status("cmd sensor unavailable on this device");
        }
        break;
    }

    case ASYNC_SENSOR_ROTATE: {
        std::string lower = r->output;
        for (auto &c : lower) c = (char)tolower((unsigned char)c);
        if (lower.find("conflict") != std::string::npos ||
            lower.find("shaking") != std::string::npos) {
            show_toast("Shake running - rotation blocked", false);
            set_status("Rotation blocked: shake in progress");
        } else if (lower.find("succeeded") != std::string::npos ||
                   lower.find("success") != std::string::npos) {
            show_toast("Screen rotated", true);
            set_status("Screen rotation applied");
            sensor_refresh();
        } else {
            std::string err = trim_trailing_newlines(r->output.empty() ? "Rotation failed" : r->output);
            show_toast(err.c_str(), false);
            set_status(err.c_str());
        }
        break;
    }

    case ASYNC_SENSOR_ROTATE_RESET: {
        std::string lower = r->output;
        for (auto &c : lower) c = (char)tolower((unsigned char)c);
        if (lower.find("conflict") != std::string::npos ||
            lower.find("shaking") != std::string::npos) {
            show_toast("Shake running - reset blocked", false);
            set_status("Reset blocked: shake in progress");
        } else if (lower.find("succeeded") != std::string::npos ||
                   lower.find("success") != std::string::npos) {
            show_toast("Rotation reset to 0", true);
            set_status("Screen rotation reset");
            sensor_refresh();
        } else {
            std::string err = trim_trailing_newlines(r->output.empty() ? "Reset failed" : r->output);
            show_toast(err.c_str(), false);
            set_status(err.c_str());
        }
        break;
    }

    case ASYNC_SENSOR_SHAKE: {
        std::string lower = r->output;
        for (auto &c : lower) c = (char)tolower((unsigned char)c);
        if (lower.find("conflict") != std::string::npos ||
            lower.find("already") != std::string::npos) {
            show_toast("Shake already running", false);
            set_status("Shake blocked: already in progress");
        } else if (lower.find("succeeded") != std::string::npos ||
                   lower.find("success") != std::string::npos) {
            show_toast("Shake triggered", true);
            set_status("Shake simulation triggered");
            sensor_refresh();
        } else {
            std::string err = trim_trailing_newlines(r->output.empty() ? "Shake failed" : r->output);
            show_toast(err.c_str(), false);
            set_status(err.c_str());
        }
        break;
    }
    }

    delete r;
}

// ── I18n: Language/region/timezone/datetime/24hour/autotime ─

void CphWindow::i18n_refresh() {
    run_async(ASYNC_I18N_GET, "cmd i18n get");
}

void CphWindow::i18n_apply() {
    // Apply button handles Language/Region/Timezone only.
    // AutoTime and 24-hour are applied immediately by their checkboxes.
    std::string cmd = "cmd i18n set";
    bool has_opt = false;

    // Language combo — CBS_DROPDOWN: user may type a value not in the list,
    // so fall back to GetWindowText when CB_GETCURSEL returns CB_ERR.
    if (g_i18n_lang_combo) {
        wchar_t wbuf[32] = {};
        int sel = (int)SendMessageW(g_i18n_lang_combo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            SendMessageW(g_i18n_lang_combo, CB_GETLBTEXT, sel, (LPARAM)wbuf);
        } else {
            GetWindowTextW(g_i18n_lang_combo, wbuf, 32);
        }
        char buf[32] = {};
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), nullptr, nullptr);
        if (buf[0]) { cmd += " -l "; cmd += buf; has_opt = true; }
    }

    // Region combo
    if (g_i18n_region_combo) {
        wchar_t wbuf[8] = {};
        int sel = (int)SendMessageW(g_i18n_region_combo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            SendMessageW(g_i18n_region_combo, CB_GETLBTEXT, sel, (LPARAM)wbuf);
        } else {
            GetWindowTextW(g_i18n_region_combo, wbuf, 8);
        }
        char buf[8] = {};
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), nullptr, nullptr);
        if (buf[0]) { cmd += " -r "; cmd += buf; has_opt = true; }
    }

    // Timezone combo — only when AutoTime is not checked (mutex)
    bool autotime = g_i18n_autotime_btn &&
        (SendMessageW(g_i18n_autotime_btn, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!autotime && g_i18n_tz_combo) {
        wchar_t wbuf[64] = {};
        int sel = (int)SendMessageW(g_i18n_tz_combo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            SendMessageW(g_i18n_tz_combo, CB_GETLBTEXT, sel, (LPARAM)wbuf);
        } else {
            GetWindowTextW(g_i18n_tz_combo, wbuf, 64);
        }
        char buf[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), nullptr, nullptr);
        if (buf[0]) { cmd += " -t "; cmd += buf; has_opt = true; }
    }

    if (!has_opt) {
        show_toast("Select at least one setting", false);
        return;
    }

    run_async(ASYNC_I18N_APPLY, cmd);
}

void CphWindow::i18n_set_datetime() {
    wchar_t wdate[16] = {};
    wchar_t wtime[16] = {};
    if (g_i18n_date_edit) GetWindowTextW(g_i18n_date_edit, wdate, 16);
    if (g_i18n_time_edit) GetWindowTextW(g_i18n_time_edit, wtime, 16);

    if (!wdate[0] || !wtime[0]) {
        show_toast("Please enter date and time", false);
        return;
    }

    char date[16] = {};
    char time[16] = {};
    WideCharToMultiByte(CP_UTF8, 0, wdate, -1, date, sizeof(date), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, wtime, -1, time, sizeof(time), nullptr, nullptr);

    // Include timezone if set (user expectation: Apply sets both datetime and timezone)
    std::string cmd = "cmd i18n set";
    if (g_i18n_tz_combo) {
        wchar_t wtz[64] = {};
        int sel = (int)SendMessageW(g_i18n_tz_combo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            SendMessageW(g_i18n_tz_combo, CB_GETLBTEXT, sel, (LPARAM)wtz);
        } else {
            GetWindowTextW(g_i18n_tz_combo, wtz, 64);
        }
        if (wtz[0]) {
            char tz[64] = {};
            WideCharToMultiByte(CP_UTF8, 0, wtz, -1, tz, sizeof(tz), nullptr, nullptr);
            cmd += " -t ";
            cmd += tz;
        }
    }
    cmd += " -d \"";
    cmd += date;
    cmd += "T";
    cmd += time;
    cmd += "\"";

    run_async(ASYNC_I18N_SET_DATETIME, cmd);
}

void CphWindow::i18n_set_autotime() {
    run_async(ASYNC_I18N_SET_AUTOTIME, "cmd i18n set -a");
}

void CphWindow::i18n_disable_autotime() {
    // Disable autotime by setting timezone and datetime
    // Read current timezone from combo (CBS_DROPDOWN: fall back to edit text
    // when the user typed a value not present in the filtered list).
    wchar_t wtz[64] = {};
    int tz_sel = (int)SendMessageW(g_i18n_tz_combo, CB_GETCURSEL, 0, 0);
    if (tz_sel != CB_ERR) {
        SendMessageW(g_i18n_tz_combo, CB_GETLBTEXT, tz_sel, (LPARAM)wtz);
    } else {
        GetWindowTextW(g_i18n_tz_combo, wtz, 64);
    }
    if (!wtz[0]) {
        show_toast("Please select a timezone first", false);
        // BS_AUTOCHECKBOX already toggled the checkbox off; refresh to restore
        // the server's true state (AutoTime still ON) instead of leaving the
        // UI desynced.
        i18n_refresh();
        return;
    }
    char tz[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, wtz, -1, tz, sizeof(tz), nullptr, nullptr);

    // Read current date and time from edits
    wchar_t wdate[16] = {};
    wchar_t wtime[16] = {};
    if (g_i18n_date_edit) GetWindowTextW(g_i18n_date_edit, wdate, 16);
    if (g_i18n_time_edit) GetWindowTextW(g_i18n_time_edit, wtime, 16);

    if (!wdate[0] || !wtime[0]) {
        show_toast("Please enter date and time", false);
        i18n_refresh();
        return;
    }

    char date[16] = {};
    char time[16] = {};
    WideCharToMultiByte(CP_UTF8, 0, wdate, -1, date, sizeof(date), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, wtime, -1, time, sizeof(time), nullptr, nullptr);

    // Build combined command: -t timezone -d "datetime"
    std::string cmd = "cmd i18n set -t ";
    cmd += tz;
    cmd += " -d \"";
    cmd += date;
    cmd += "T";
    cmd += time;
    cmd += "\"";

    run_async(ASYNC_I18N_DISABLE_AUTOTIME, cmd);
}

void CphWindow::i18n_set_24hour(bool enabled) {
    std::string cmd = "cmd i18n set -z ";
    cmd += enabled ? "true" : "false";
    run_async(ASYNC_I18N_SET_24HOUR, cmd);
}

// Live-filter an editable combobox by the current edit text.
// Called on CBN_EDITCHANGE for the i18n Language / Region / Timezone combos.
// Rebuilds the dropdown list with only entries that case-insensitively
// contain the current edit text. Reentry-guarded because SetWindowTextW
// below would retrigger CBN_EDITCHANGE and recurse.
void CphWindow::i18n_filter_combo(int combo_id) {
    HWND combo = nullptr;
    const wchar_t *const *wide_list = nullptr;
    int wide_count = 0;
    const char *const *narrow_list = nullptr;
    int narrow_count = 0;

    if (combo_id == IDC_I18N_LANG_COMBO) {
        combo = g_i18n_lang_combo;
        wide_list = I18N_LOCALES;
        wide_count = I18N_LOCALES_COUNT;
    } else if (combo_id == IDC_I18N_TZ_COMBO) {
        combo = g_i18n_tz_combo;
        wide_list = I18N_TIMEZONES;
        wide_count = I18N_TIMEZONES_COUNT;
    } else if (combo_id == IDC_I18N_REGION_COMBO) {
        combo = g_i18n_region_combo;
        narrow_list = I18N_REGIONS;
        narrow_count = I18N_REGIONS_COUNT;
    }
    if (!combo) return;

    // Reentry guard: SetWindowTextW below would retrigger CBN_EDITCHANGE.
    static int s_filtering = 0;
    if (s_filtering) return;
    s_filtering = 1;

    // Read current edit text and lowercase it for case-insensitive matching.
    wchar_t wbuf[128] = {};
    GetWindowTextW(combo, wbuf, 128);
    std::wstring needle;
    for (int i = 0; wbuf[i]; i++) needle.push_back((wchar_t)towlower(wbuf[i]));

    // Preserve edit selection so the caret does not jump to 0 on rebuild.
    DWORD sel = (DWORD)SendMessageW(combo, CB_GETEDITSEL, 0, 0);
    int sel_start = LOWORD(sel);
    int sel_end = HIWORD(sel);

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    const int MAX_MATCH = 200;  // cap list size to avoid lag on 473-entry tz list
    int added = 0;

    if (wide_list) {
        for (int i = 0; i < wide_count && added < MAX_MATCH; i++) {
            std::wstring hay;
            for (const wchar_t *p = wide_list[i]; *p; p++) hay.push_back((wchar_t)towlower(*p));
            if (needle.empty() || hay.find(needle) != std::wstring::npos) {
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)wide_list[i]);
                added++;
            }
        }
    } else if (narrow_list) {
        for (int i = 0; i < narrow_count && added < MAX_MATCH; i++) {
            wchar_t w[16] = {};
            MultiByteToWideChar(CP_UTF8, 0, narrow_list[i], -1, w, 16);
            std::wstring hay;
            for (const wchar_t *p = w; *p; p++) hay.push_back((wchar_t)towlower(*p));
            if (needle.empty() || hay.find(needle) != std::wstring::npos) {
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)w);
                added++;
            }
        }
    }

    // Restore edit text + selection (CB_RESETCONTENT cleared it).
    SetWindowTextW(combo, wbuf);
    SendMessageW(combo, CB_SETEDITSEL, 0, MAKELPARAM(sel_start, sel_end));
    if (added > 0) SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);

    s_filtering = 0;
}

// Extract all "error: ..." lines from a cmd i18n output blob so the UI
// can surface the server-side error verbatim instead of a generic toast.
std::string CphWindow::i18n_extract_errors(const std::string &output) {
    std::string result;
    size_t pos = 0;
    while ((pos = output.find("error:", pos)) != std::string::npos) {
        size_t line_end = output.find('\n', pos);
        if (line_end == std::string::npos) line_end = output.size();
        std::string line = output.substr(pos, line_end - pos);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (!result.empty()) result += "\n";
        result += line;
        pos = line_end;
    }
    return result;
}

void CphWindow::i18n_parse_and_populate(const std::string &output) {
    // Parse "cmd i18n get" output line-by-by (v2.1 format):
    //   Language:  zh-Hans
    //   Region:    CN
    //   Timezone:  Asia/Shanghai
    //   Datetime:  2026-07-20 12:53:21
    //   Is24Hour:  true
    //   AutoTime:  ON
    auto extract_val = [&](const char *key) -> std::string {
        std::string search = std::string(key) + ":";
        auto pos = output.find(search);
        if (pos == std::string::npos) return "";
        // Skip whitespace after colon
        auto start = pos + search.size();
        while (start < output.size() && (output[start] == ' ' || output[start] == '\t')) start++;
        auto end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();
        std::string val = output.substr(start, end - start);
        // Trim trailing whitespace/CR
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();
        return val;
    };

    std::string lang = extract_val("Language");
    std::string region = extract_val("Region");
    std::string tz = extract_val("Timezone");
    std::string dt = extract_val("Datetime");
    std::string hour24 = extract_val("Is24Hour");
    std::string autotime = extract_val("AutoTime");

    // Populate language combo. CBS_DROPDOWN allows values outside the preset
    // list, so when CB_SELECTSTRING returns CB_ERR we SetWindowTextW to show
    // the server value verbatim — the user can still edit it before Apply.
    if (g_i18n_lang_combo && !lang.empty()) {
        wchar_t wbuf[32] = {};
        MultiByteToWideChar(CP_UTF8, 0, lang.c_str(), -1, wbuf, 32);
        LRESULT idx = SendMessageW(g_i18n_lang_combo, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)wbuf);
        if (idx == CB_ERR) SetWindowTextW(g_i18n_lang_combo, wbuf);
    }

    // Populate region combo — read directly from Region field (v2.0+)
    if (g_i18n_region_combo && !region.empty()) {
        wchar_t wbuf[8] = {};
        MultiByteToWideChar(CP_UTF8, 0, region.c_str(), -1, wbuf, 8);
        LRESULT idx = SendMessageW(g_i18n_region_combo, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)wbuf);
        if (idx == CB_ERR) SetWindowTextW(g_i18n_region_combo, wbuf);
    }

    // Populate timezone combo
    if (g_i18n_tz_combo && !tz.empty()) {
        wchar_t wbuf[64] = {};
        MultiByteToWideChar(CP_UTF8, 0, tz.c_str(), -1, wbuf, 64);
        LRESULT idx = SendMessageW(g_i18n_tz_combo, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)wbuf);
        if (idx == CB_ERR) SetWindowTextW(g_i18n_tz_combo, wbuf);
    }

    // Populate date/time edits from datetime string (e.g. "2026-07-20 20:42:25")
    if (!dt.empty()) {
        // Split "YYYY-MM-DD hh:mm:ss" or "YYYY-MM-DDThh:mm:ss"
        std::string date_str, time_str;
        auto sep = dt.find('T');
        if (sep == std::string::npos) sep = dt.find(' ');
        if (sep != std::string::npos) {
            date_str = dt.substr(0, sep);
            time_str = dt.substr(sep + 1);
        } else {
            date_str = dt;
        }
        if (g_i18n_date_edit && !date_str.empty()) {
            wchar_t wbuf[16] = {};
            MultiByteToWideChar(CP_UTF8, 0, date_str.c_str(), -1, wbuf, 16);
            SetWindowTextW(g_i18n_date_edit, wbuf);
        }
        if (g_i18n_time_edit && !time_str.empty()) {
            wchar_t wbuf[16] = {};
            MultiByteToWideChar(CP_UTF8, 0, time_str.c_str(), -1, wbuf, 16);
            SetWindowTextW(g_i18n_time_edit, wbuf);
        }
    }

    // 24-hour checkbox
    if (g_i18n_24hour_check) {
        SendMessageW(g_i18n_24hour_check, BM_SETCHECK,
                      (hour24 == "true") ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    // AutoTime checkbox + mutex: ON → checked, gray out manual controls
    if (g_i18n_autotime_btn) {
        bool at_on = (autotime == "ON");
        SendMessageW(g_i18n_autotime_btn, BM_SETCHECK,
                      at_on ? BST_CHECKED : BST_UNCHECKED, 0);
        HWND mutex_ctrls[] = {g_i18n_tz_combo, g_i18n_date_edit, g_i18n_time_edit,
                              g_i18n_set_datetime_btn};
        for (HWND h : mutex_ctrls) { if (h) EnableWindow(h, !at_on); }
    }

    // Update current settings label
    if (g_i18n_current_label) {
        std::string text;
        text += "Language:  " + (lang.empty() ? "-" : lang) + "\r\n";
        text += "Region:    " + (region.empty() ? "-" : region) + "\r\n";
        text += "Timezone:  " + (tz.empty() ? "-" : tz) + "\r\n";
        text += "Datetime:  " + (dt.empty() ? "-" : dt) + "\r\n";
        text += "Is24Hour:  " + (hour24.empty() ? "-" : hour24) + "\r\n";
        text += "AutoTime:  " + (autotime.empty() ? "-" : autotime);
        SetWindowTextA(g_i18n_current_label, text.c_str());
    }
}

// ── Sensor: Screen rotation & shake simulation ─────────────

void CphWindow::sensor_refresh() {
    run_async(ASYNC_SENSOR_REFRESH, "cmd sensor info -l");
}

void CphWindow::sensor_rotate() {
    if (!g_sensor_rotate_combo) return;
    int sel = (int)SendMessageW(g_sensor_rotate_combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        show_toast("Select a rotation degree", false);
        return;
    }
    // Degrees are 0/90/180/270 indexed by position
    int degrees[] = {0, 90, 180, 270};
    int deg = (sel >= 0 && sel < 4) ? degrees[sel] : 0;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "cmd sensor rotate -d %d", deg);
    run_async(ASYNC_SENSOR_ROTATE, cmd);
}

void CphWindow::sensor_rotate_next() {
    // Rotate without specifying degree: cycles through 90→180→270→0→90...
    run_async(ASYNC_SENSOR_ROTATE, "cmd sensor rotate");
}

void CphWindow::sensor_reset_rotation() {
    run_async(ASYNC_SENSOR_ROTATE_RESET, "cmd sensor rotate -r");
}

void CphWindow::sensor_trigger_shake() {
    if (!g_sensor_shake_edit) return;
    wchar_t wbuf[16] = {};
    GetWindowTextW(g_sensor_shake_edit, wbuf, 16);
    if (!wbuf[0]) {
        msg_box("Shake", "Enter duration in milliseconds (100-10000)");
        return;
    }
    char buf[16] = {};
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), nullptr, nullptr);
    // Validate integer
    for (const char *p = buf; *p; p++) {
        if (*p < '0' || *p > '9') {
            msg_box("Shake", "Duration must be 100-10000 ms");
            return;
        }
    }
    int ms = atoi(buf);
    if (ms < 100 || ms > 10000) {
        msg_box("Shake", "Duration must be 100-10000 ms");
        return;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "cmd sensor shake -d %d", ms);
    run_async(ASYNC_SENSOR_SHAKE, cmd);
}

void CphWindow::sensor_parse_info(const std::string &output) {
    // Parse "cmd sensor info -l" output:
    //   degree: 0
    //   shaking: false
    //   accel source: NONE(DB)
    auto extract_val = [&](const char *key) -> std::string {
        std::string search = std::string(key) + ":";
        auto pos = output.find(search);
        if (pos == std::string::npos) return "";
        auto start = pos + search.size();
        while (start < output.size() && (output[start] == ' ' || output[start] == '\t')) start++;
        auto end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();
        std::string val = output.substr(start, end - start);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();
        return val;
    };

    std::string degree = extract_val("degree");
    std::string shaking = extract_val("shaking");
    std::string source = extract_val("accel source");

    if (g_sensor_degree_label) SetWindowTextA(g_sensor_degree_label, degree.empty() ? "?" : degree.c_str());
    if (g_sensor_shaking_label) SetWindowTextA(g_sensor_shaking_label, shaking.empty() ? "?" : shaking.c_str());
    if (g_sensor_source_label) SetWindowTextA(g_sensor_source_label, source.empty() ? "?" : source.c_str());

    // Sync the combo selection with the current degree
    if (g_sensor_rotate_combo && !degree.empty()) {
        int deg = atoi(degree.c_str());
        int idx = deg / 90;  // 0→0, 90→1, 180→2, 270→3
        if (idx >= 0 && idx < 4) {
            SendMessageW(g_sensor_rotate_combo, CB_SETCURSEL, idx, 0);
        }
    }
}

// ── GNSS: Parse current location from -l output ────────────

void CphWindow::parse_gnss_location(const std::string &output, bool fill_edits) {
    // Parse all key=value pairs from "  key=value" lines
    auto parse_val = [&](const char *key) -> std::string {
        std::string search = std::string("  ") + key + "=";
        auto pos = output.find(search);
        if (pos == std::string::npos) return "";
        auto start = pos + search.size();
        auto end = output.find('\n', start);
        return output.substr(start, end == std::string::npos ? end : end - start);
    };

    // All GNSS fields from cmd gnss location -l
    struct Field { const char *key; const char *display; };
    Field fields[] = {
        {"latitude", "latitude"}, {"longitude", "longitude"}, {"altitude", "altitude"},
        {"speed", "speed"}, {"bearing", "bearing"},
        {"horizontalAccuracy", "horizontalAccuracy"}, {"verticalAccuracy", "verticalAccuracy"},
        {"speedAccuracy", "speedAccuracy"}, {"bearingAccuracy", "bearingAccuracy"},
        {"timeForFix", "timeForFix"}, {"timeSinceBoot", "timeSinceBoot"},
        {"timeUncertainty", "timeUncertainty"}, {"fieldValidity", "fieldValidity"},
    };

    // Fill inject edit controls on first parse or when Refresh button clicked
    if (!g_gnss_inject_filled || fill_edits) {
        const char *edit_ids[] = {"latitude", "longitude", "altitude", "speed", "bearing",
                                   "horizontalAccuracy", "verticalAccuracy", "speedAccuracy", "bearingAccuracy"};
        HWND edit_hwnds[] = {g_gnss_lat_edit, g_gnss_lng_edit, g_gnss_alt_edit,
                              g_gnss_speed_edit, g_gnss_bearing_edit, g_gnss_hacc_edit, g_gnss_vacc_edit,
                              g_gnss_sacc_edit, g_gnss_bacc_edit};
        bool has_data = false;
        for (int i = 0; i < 9; i++) {
            std::string val = parse_val(edit_ids[i]);
            if (!val.empty()) { SetWindowTextA(edit_hwnds[i], val.c_str()); has_data = true; }
        }
        if (has_data) g_gnss_inject_filled = true;
    }

    // Build current display - all 13 fields, one per line, left-aligned
    std::string cur;
    for (int i = 0; i < 13; i++) {
        std::string val = parse_val(fields[i].key);
        // Trim trailing whitespace from value
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' '))
            val.pop_back();
        if (i > 0) cur += "\n";
        cur += fields[i].display;
        cur += "=";
        cur += val.empty() ? "?" : val;
    }
    SetWindowTextA(g_gnss_current_label, cur.c_str());
}

void CphWindow::refresh_gnss() {
    s_refresh_fail_count = 0;  // Reset on manual refresh
    run_async(ASYNC_REFRESH_GNSS, "cmd gnss location -l");
}

void CphWindow::refresh_gnss_fill_edits() {
    s_refresh_fill_edits = true;
    s_refresh_fail_count = 0;  // Reset on manual refresh
    run_async(ASYNC_REFRESH_GNSS, "cmd gnss location -l");
}

// ── GNSS: Inject location ──────────────────────────────────
// cmd gnss location -p 'latitude=XX:longitude=XX:altitude=XX:speed=XX:bearing=XX:...'

void CphWindow::inject_gnss() {
    if (!g_on_shell) { msg_box("Error", "Shell command not available"); return; }

    char lat[64], lng[64], alt[64], spd[64], brg[64], hacc[64], vacc[64], sacc[64], bacc[64];
    GetWindowTextA(g_gnss_lat_edit, lat, sizeof(lat));
    GetWindowTextA(g_gnss_lng_edit, lng, sizeof(lng));
    GetWindowTextA(g_gnss_alt_edit, alt, sizeof(alt));
    GetWindowTextA(g_gnss_speed_edit, spd, sizeof(spd));
    GetWindowTextA(g_gnss_bearing_edit, brg, sizeof(brg));
    GetWindowTextA(g_gnss_hacc_edit, hacc, sizeof(hacc));
    GetWindowTextA(g_gnss_vacc_edit, vacc, sizeof(vacc));
    GetWindowTextA(g_gnss_sacc_edit, sacc, sizeof(sacc));
    GetWindowTextA(g_gnss_bacc_edit, bacc, sizeof(bacc));

    if (!lat[0] || !lng[0]) {
        msg_box("GNSS", "Latitude and Longitude are required");
        return;
    }

    // Validate ranges per API spec
    double d_lat = atof(lat), d_lng = atof(lng);
    if (d_lat < -90 || d_lat > 90) { msg_box("GNSS", "Latitude must be -90 to 90"); return; }
    if (d_lng < -180 || d_lng > 180) { msg_box("GNSS", "Longitude must be -180 to 180"); return; }
    if (brg[0]) { double d = atof(brg); if (d < 0 || d > 360) { msg_box("GNSS", "Bearing must be 0 to 360"); return; } }

    // Build param string: latitude and longitude are required, others optional
    // Only include non-zero optional params to avoid "invalid location coordinate data" error
    std::string params;
    params += "latitude=";
    params += lat;
    params += ":longitude=";
    params += lng;

    auto append_if_nonzero = [&](const char *key, const char *val) {
        if (!val[0]) return;
        // Skip default value 0 for optional params
        double d = atof(val);
        if (d == 0.0) return;
        params += ":";
        params += key;
        params += "=";
        params += val;
    };
    append_if_nonzero("altitude", alt);
    append_if_nonzero("speed", spd);
    append_if_nonzero("bearing", brg);
    append_if_nonzero("horizontalAccuracy", hacc);
    append_if_nonzero("verticalAccuracy", vacc);
    append_if_nonzero("speedAccuracy", sacc);
    append_if_nonzero("bearingAccuracy", bacc);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cmd gnss location -p %s", params.c_str());
    LOGE("CPH inject_gnss: cmd=[%s]", cmd);
    // Pause auto-refresh during inject to avoid interference
    if (g_refresh_timer) { KillTimer(g_hwnd, CPH_REFRESH_TIMER_ID); g_refresh_timer = 0; }
    run_async(ASYNC_INJECT_GNSS, cmd);
}

// ── GNSS: Trajectory ───────────────────────────────────────
// cmd gnss trajectory -p /data/local/tmp/test.gpx  (start)
// cmd gnss trajectory -d  (stop)

void CphWindow::start_trajectory() {
    if (!g_on_shell) { msg_box("Error", "Shell command not available"); return; }

    char path[512];
    GetWindowTextA(g_gnss_traj_edit, path, sizeof(path));

    if (s_traj_hint_active || !path[0]) {
        msg_box("Trajectory", "Please browse a GPX trajectory file first");
        return;
    }

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "cmd gnss trajectory -p '%s'", path);
    run_async(ASYNC_START_TRAJ, cmd);
}

void CphWindow::stop_trajectory() {
    run_async(ASYNC_STOP_TRAJ, "cmd gnss trajectory -d");
}

// ── Battery: Parse current info from -l output ─────────────

void CphWindow::parse_battery_info(const std::string &output, bool fill_edits) {
    auto parse_val = [&](const char *key) -> std::string {
        // Try "[A] key=" first, then "    key="
        std::string search1 = std::string("[A] ") + key + "=";
        std::string search2 = std::string("    ") + key + "=";
        auto pos = output.find(search1);
        if (pos == std::string::npos) pos = output.find(search2);
        if (pos == std::string::npos) return "";
        auto eq = output.find('=', pos);
        if (eq == std::string::npos) return "";
        auto start = eq + 1;
        auto end = output.find('\n', start);
        return output.substr(start, end == std::string::npos ? end : end - start);
    };

    // All battery fields from cmd battery info -l
    const char *all_fields[] = {
        "capacity", "pluggedType", "chargerType", "chargeState",
        "voltage", "voltageMax", "current", "currentAverage",
        "currentMax", "temperature", "healthState", "present",
        "totalEnergy", "technology",
    };

    // Fill inject controls only on first parse or when Refresh button clicked
    if (!g_bat_inject_filled || fill_edits) {
        std::string cap = parse_val("capacity");
        std::string temp = parse_val("temperature");
        std::string plugged = parse_val("pluggedType");
        std::string charger = parse_val("chargerType");
        std::string state = parse_val("chargeState");
        std::string voltage = parse_val("voltage");
        std::string voltage_max = parse_val("voltageMax");
        std::string current = parse_val("current");
        std::string current_avg = parse_val("currentAverage");
        std::string current_max = parse_val("currentMax");
        std::string health = parse_val("healthState");
        std::string present = parse_val("present");
        std::string total_energy = parse_val("totalEnergy");
        std::string tech = parse_val("technology");

        if (!cap.empty()) SetWindowTextA(g_bat_capacity_edit, cap.c_str());
        if (!temp.empty()) SetWindowTextA(g_bat_temp_edit, temp.c_str());
        if (!voltage.empty()) SetWindowTextA(g_bat_voltage_edit, voltage.c_str());
        if (!voltage_max.empty()) SetWindowTextA(g_bat_voltage_max_edit, voltage_max.c_str());
        if (!current.empty()) SetWindowTextA(g_bat_current_edit, current.c_str());
        if (!current_avg.empty()) SetWindowTextA(g_bat_current_avg_edit, current_avg.c_str());
        if (!current_max.empty()) SetWindowTextA(g_bat_current_max_edit, current_max.c_str());
        if (!total_energy.empty()) SetWindowTextA(g_bat_total_energy_edit, total_energy.c_str());
        if (!tech.empty()) SetWindowTextA(g_bat_technology_edit, tech.c_str());

        auto set_combo_by_val = [](HWND combo, const std::string &val, const int *map, int map_size) {
            for (int i = 0; i < map_size; i++) {
                if (val == std::to_string(map[i])) {
                    SendMessageW(combo, CB_SETCURSEL, i, 0);
                    return;
                }
            }
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
        };
        int plugged_map[] = {0, 1, 2, 3};
        set_combo_by_val(g_bat_plugged_combo, plugged, plugged_map, 4);
        int charger_map[] = {0, 1, 2, 3, 4, 5, 6};
        set_combo_by_val(g_bat_charger_combo, charger, charger_map, 7);
        int state_map[] = {0, 1, 2, 3};
        set_combo_by_val(g_bat_chargestate_combo, state, state_map, 4);
        int health_map[] = {0, 1, 2, 3, 4, 5};
        set_combo_by_val(g_bat_health_combo, health, health_map, 6);
        int present_map[] = {0, 1};
        set_combo_by_val(g_bat_present_combo, present, present_map, 2);

        bool has_data = !cap.empty();
        if (has_data) g_bat_inject_filled = true;
    }

    // Build current display - all 14 fields, one per line, left-aligned
    std::string cur;
    for (int i = 0; i < 14; i++) {
        std::string val = parse_val(all_fields[i]);
        // Trim trailing whitespace from value
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' '))
            val.pop_back();
        if (i > 0) cur += "\n";
        cur += all_fields[i];
        cur += "=";
        cur += val.empty() ? "?" : val;
    }
    SetWindowTextA(g_bat_current_label, cur.c_str());
}

void CphWindow::refresh_battery() {
    run_async(ASYNC_REFRESH_BATTERY, "cmd battery info -l");
}

// ── Battery: Inject ────────────────────────────────────────
// cmd battery info -p 'capacity=90:temperature=222:pluggedType=1:chargerType=1:chargeState=1'

void CphWindow::inject_battery() {
    if (!g_on_shell) { msg_box("Error", "Shell command not available"); return; }

    char capacity[32], temp[32], voltage[32], voltage_max[32];
    char current_val[32], current_avg[32], current_max[32];
    char total_energy[32], technology[32];
    GetWindowTextA(g_bat_capacity_edit, capacity, sizeof(capacity));
    GetWindowTextA(g_bat_temp_edit, temp, sizeof(temp));
    GetWindowTextA(g_bat_voltage_edit, voltage, sizeof(voltage));
    GetWindowTextA(g_bat_voltage_max_edit, voltage_max, sizeof(voltage_max));
    GetWindowTextA(g_bat_current_edit, current_val, sizeof(current_val));
    GetWindowTextA(g_bat_current_avg_edit, current_avg, sizeof(current_avg));
    GetWindowTextA(g_bat_current_max_edit, current_max, sizeof(current_max));
    GetWindowTextA(g_bat_total_energy_edit, total_energy, sizeof(total_energy));
    GetWindowTextA(g_bat_technology_edit, technology, sizeof(technology));

    // Association constraint: pluggedType, chargerType, chargeState, capacity must all be present or all absent
    // capacity is the key indicator - if set, the other 3 must also be set
    bool has_assoc = capacity[0] != '\0';
    if (has_assoc) {
        int cap_val = atoi(capacity);
        if (cap_val < 0 || cap_val > 100) {
            msg_box("Battery", "Capacity must be 0 to 100");
            return;
        }
    }

    // Get combo values
    int plugged_sel = (int)SendMessageW(g_bat_plugged_combo, CB_GETCURSEL, 0, 0);
    int charger_sel = (int)SendMessageW(g_bat_charger_combo, CB_GETCURSEL, 0, 0);
    int state_sel = (int)SendMessageW(g_bat_chargestate_combo, CB_GETCURSEL, 0, 0);
    int health_sel = (int)SendMessageW(g_bat_health_combo, CB_GETCURSEL, 0, 0);
    int present_sel = (int)SendMessageW(g_bat_present_combo, CB_GETCURSEL, 0, 0);

    // Map combo selections to values
    int plugged_map[] = {0, 1, 2, 3};
    int charger_map[] = {0, 1, 2, 3, 4, 5, 6};
    int state_map[] = {0, 1, 2, 3};
    int health_map[] = {0, 1, 2, 3, 4, 5};
    int present_map[] = {0, 1};

    std::string params;

    // Association group: capacity + pluggedType + chargerType + chargeState (all or none)
    if (has_assoc) {
        params += "capacity=";
        params += capacity;
        params += ":pluggedType=";
        params += std::to_string(plugged_map[plugged_sel]);
        params += ":chargerType=";
        params += std::to_string(charger_map[charger_sel]);
        params += ":chargeState=";
        params += std::to_string(state_map[state_sel]);
    }

    // Independent optional params
    auto append_if = [&](const char *key, const char *val) {
        if (!val[0]) return;
        params += ":";
        params += key;
        params += "=";
        params += val;
    };
    auto append_int = [&](const char *key, int val) {
        params += ":";
        params += key;
        params += "=";
        params += std::to_string(val);
    };
    append_if("temperature", temp);
    append_if("voltage", voltage);
    append_if("voltageMax", voltage_max);
    append_if("current", current_val);
    append_if("currentAverage", current_avg);
    append_if("currentMax", current_max);
    append_int("healthState", health_map[health_sel]);
    append_int("present", present_map[present_sel]);
    append_if("totalEnergy", total_energy);
    append_if("technology", technology);

    if (params.empty()) { msg_box("Battery", "No parameters to inject"); return; }

    // Remove leading ':' if association group was empty
    if (params[0] == ':') params = params.substr(1);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cmd battery info -p '%s'", params.c_str());
    run_async(ASYNC_INJECT_BATTERY, cmd);
}

void CphWindow::reset_battery() {
    run_async(ASYNC_RESET_BATTERY, "cmd battery info -r");
}

void CphWindow::inject_charge_scene() {
    if (!g_on_shell) { msg_box("Error", "Shell command not available"); return; }

    int sel = (int)SendMessageW(g_bat_scene_combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        msg_box("Charge Scene", "Please select a charge scene");
        return;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "cmd battery charge-scene -i %d", sel);
    run_async(ASYNC_INJECT_SCENE, cmd);
}

// ── WndProc ────────────────────────────────────────────────

LRESULT CALLBACK CphWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CPH_ASYNC_DONE) {
        auto *r = reinterpret_cast<AsyncResult *>(lp);
        handle_async_done(r);
        return 0;
    }
    if (msg == WM_CPH_RETRY_ASYNC) {
        auto *cmd = reinterpret_cast<std::string *>(lp);
        AsyncOp op = static_cast<AsyncOp>((int)wp);
        std::string cmd_str = *cmd;
        delete cmd;
        // Retry the async operation
        run_async(op, cmd_str);
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        g_cph_font = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        // Header
        HWND title = CreateWindowW(L"STATIC", L"Cloud Phone (CPH) Simulation",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    MARGIN, 6, 300, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(title, WM_SETFONT, (WPARAM)g_cph_font, TRUE);

        // Tab control
        g_tab = CreateWindowW(WC_TABCONTROLW, L"",
                               WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | TCS_FIXEDWIDTH | TCS_OWNERDRAWFIXED,
                               MARGIN, HEADER_H, g_panel_w - MARGIN * 2, 800,
                               hwnd, (HMENU)IDC_TAB, nullptr, nullptr);
        SendMessageW(g_tab, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
        g_tab_orig_proc = (WNDPROC)SetWindowLongPtrW(g_tab, GWLP_WNDPROC, (LONG_PTR)TabSubclassProc);

        TCITEMW tie = {};
        tie.mask = TCIF_TEXT;
        tie.pszText = const_cast<LPWSTR>(L"Battery");
        TabCtrl_InsertItem(g_tab, TAB_BATTERY, &tie);
        tie.pszText = const_cast<LPWSTR>(L"GNSS");
        TabCtrl_InsertItem(g_tab, TAB_GNSS, &tie);
        tie.pszText = const_cast<LPWSTR>(L"Haps");
        TabCtrl_InsertItem(g_tab, TAB_HAPS, &tie);
        tie.pszText = const_cast<LPWSTR>(L"I18n");
        TabCtrl_InsertItem(g_tab, TAB_I18N, &tie);
        tie.pszText = const_cast<LPWSTR>(L"Sensor");
        TabCtrl_InsertItem(g_tab, TAB_SENSOR, &tie);
        // Force tab item width to fit text
        SendMessageW(g_tab, TCM_SETITEMSIZE, 0, MAKELPARAM(80, 24));

        // Get tab display area to position child pages correctly
        RECT tab_rc = {0, 0, g_panel_w - MARGIN * 2, 800};
        TabCtrl_AdjustRect(g_tab, FALSE, &tab_rc);

        // Page position relative to the tab control
        int px = tab_rc.left;
        int py = tab_rc.top;
        int pw = tab_rc.right - tab_rc.left;
        int ph = tab_rc.bottom - tab_rc.top;
        int row_h = EDIT_H + ROW_GAP;
        int label_x = MARGIN + 4;          // label left edge
        int edit_x = label_x + LABEL_W + 6; // edit left edge (after label + gap)
        int edit_w = pw - edit_x - MARGIN - 4; // edit width (fill to right margin)
        int grp_x = MARGIN;
        int grp_w = pw - MARGIN * 2;

        // ── GNSS page ─────────────────────────────────────
        {
            ensure_page_class_registered(g_inst);
            HWND page = CreateWindowW(CPH_PAGE_W, L"",
                                       WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL,
                                       px, py, pw, ph,
                                       g_tab, nullptr, nullptr, nullptr);
            g_tab_pages[TAB_GNSS] = page;
            int y = 6;

            // Current location display - multi-line (13 fields)
            make_group(page, L"Current Location", grp_x, y, grp_w, 215);
            g_gnss_current_label = CreateWindowW(L"STATIC", L"--",
                                                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL,
                                                   grp_x + 10, y + 16, grp_w - 20, 195,
                                                   page, (HMENU)IDC_GNSS_CURRENT_LABEL, nullptr, nullptr);
            SendMessageW(g_gnss_current_label, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
            y += 221;

            // Location inject group
            make_group(page, L"Inject Location (WGS84)", grp_x, y, grp_w, 319);
            int iy = y + 20;

            make_label(page, L"Latitude *:", label_x, iy + 3, LABEL_W, 18);
            g_gnss_lat_edit = make_edit(page, IDC_GNSS_LAT_EDIT, L"39.9042", edit_x, iy, edit_w, EDIT_H);
            iy += row_h;
            make_label(page, L"Longitude *:", label_x, iy + 3, LABEL_W, 18);
            g_gnss_lng_edit = make_edit(page, IDC_GNSS_LNG_EDIT, L"116.4074", edit_x, iy, edit_w, EDIT_H);
            iy += row_h;
            make_label(page, L"Altitude:", label_x, iy + 3, LABEL_W, 18);
            g_gnss_alt_edit = make_edit(page, IDC_GNSS_ALT_EDIT, L"", edit_x, iy, edit_w, EDIT_H);
            iy += row_h;
            make_label(page, L"Speed (m/s):", label_x, iy + 3, LABEL_W, 18);
            g_gnss_speed_edit = make_edit(page, IDC_GNSS_SPEED_EDIT, L"", edit_x, iy, edit_w, EDIT_H);
            iy += row_h;
            make_label(page, L"Bearing:", label_x, iy + 3, LABEL_W, 18);
            g_gnss_bearing_edit = make_edit(page, IDC_GNSS_BEARING_EDIT, L"", edit_x, iy, edit_w, EDIT_H);
            iy += row_h;
            make_label(page, L"H-Accuracy:", label_x, iy + 3, LABEL_W, 18);
            g_gnss_hacc_edit = make_edit(page, IDC_GNSS_HACC_EDIT, L"", edit_x, iy, edit_w, EDIT_H);
            iy += row_h;
            make_label(page, L"V-Accuracy:", label_x, iy + 3, LABEL_W, 18);
            g_gnss_vacc_edit = make_edit(page, IDC_GNSS_VACC_EDIT, L"", edit_x, iy, edit_w, EDIT_H);
            iy += row_h;
            make_label(page, L"S-Accuracy:", label_x, iy + 3, LABEL_W, 18);
            g_gnss_sacc_edit = make_edit(page, IDC_GNSS_SACC_EDIT, L"", edit_x, iy, edit_w, EDIT_H);
            iy += row_h;
            make_label(page, L"B-Accuracy:", label_x, iy + 3, LABEL_W, 18);
            g_gnss_bacc_edit = make_edit(page, IDC_GNSS_BACC_EDIT, L"", edit_x, iy, edit_w, EDIT_H);
            iy += row_h + 6;

            // Inject + Refresh buttons
            g_gnss_inject_btn = make_btn(page, L"Inject", IDC_GNSS_INJECT_BTN,
                                          edit_x, iy, 90, BTN_H);
            g_gnss_refresh_btn = make_btn(page, L"Refresh", IDC_GNSS_REFRESH_BTN,
                                           edit_x + 98, iy, 90, BTN_H);
            y += 325;

            // Trajectory group
            make_group(page, L"Trajectory Playback", grp_x, y, grp_w, 95);
            int ty = y + 20;
            make_label(page, L"GPX/XML:", label_x, ty + 3, LABEL_W, 18);
            int browse_w = 50;
            g_gnss_traj_edit = make_edit(page, IDC_GNSS_TRAJ_EDIT, L"/data/local/tmp/gpx.xml",
                                          edit_x, ty, edit_w - browse_w - 4, EDIT_H);
            // Gray placeholder hint for traj edit (no manifest → manual cue banner)
            s_traj_edit_hwnd = g_gnss_traj_edit;
            s_traj_hint_active = true;
            SetWindowSubclass(g_gnss_traj_edit, TrajEditSubProc, 0, 0);
            g_gnss_traj_browse_btn = make_btn(page, L"...", IDC_GNSS_TRAJ_BROWSE_BTN,
                                               edit_x + edit_w - browse_w, ty, browse_w, EDIT_H);
            ty += row_h + 8;
            g_gnss_traj_start_btn = make_btn(page, L"Start", IDC_GNSS_TRAJ_START_BTN,
                                              edit_x, ty, 90, BTN_H);
            g_gnss_traj_stop_btn = make_btn(page, L"Stop", IDC_GNSS_TRAJ_STOP_BTN,
                                             edit_x + 98, ty, 90, BTN_H);
            set_page_scroll(g_tab_pages[TAB_GNSS], y + 95, ph);
        }

        // ── Battery page ──────────────────────────────────
        {
            ensure_page_class_registered(g_inst);
            HWND page = CreateWindowW(CPH_PAGE_W, L"",
                                       WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL,
                                       px, py, pw, ph,
                                       g_tab, nullptr, nullptr, nullptr);
            g_tab_pages[TAB_BATTERY] = page;
            int y = 6;

            // Current battery display - multi-line (14 fields)
            make_group(page, L"Current Battery Info", grp_x, y, grp_w, 231);
            g_bat_current_label = CreateWindowW(L"STATIC", L"--",
                                                 WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL,
                                                 grp_x + 10, y + 16, grp_w - 20, 210,
                                                 page, (HMENU)IDC_BAT_CURRENT_LABEL, nullptr, nullptr);
            SendMessageW(g_bat_current_label, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
            y += 237;

            // Inject group
            make_group(page, L"Inject Battery Info", grp_x, y, grp_w, 315);
            int iy = y + 20;
            // Two-column layout: each column = label + edit, equal edit widths
            int col2_label_w = 42;       // right-side label width
            int col2_x = edit_x + edit_w / 2 + 4;  // right column start
            int col2_edit_x = col2_x + col2_label_w + 4;  // right edit start
            int col1_w = edit_w / 2 - 2;             // left edit width
            int col2_w = edit_w - (col2_edit_x - edit_x) - 2;  // right edit width (match left)

            // Row 1: Capacity + Temp
            make_label(page, L"Capacity %:", label_x, iy + 3, LABEL_W, 18);
            g_bat_capacity_edit = make_edit(page, IDC_BAT_CAPACITY_EDIT, L"100", edit_x, iy, col1_w, EDIT_H);
            make_label(page, L"Temp:", col2_x, iy + 3, col2_label_w, 18);
            g_bat_temp_edit = make_edit(page, IDC_BAT_TEMP_EDIT, L"222",
                                         col2_edit_x, iy, col2_w, EDIT_H);
            iy += row_h;

            // Row 2: Voltage + VoltageMax
            make_label(page, L"Voltage:", label_x, iy + 3, LABEL_W, 18);
            g_bat_voltage_edit = make_edit(page, IDC_BAT_VOLTAGE_EDIT, L"", edit_x, iy, col1_w, EDIT_H);
            make_label(page, L"V-Max:", col2_x, iy + 3, col2_label_w, 18);
            g_bat_voltage_max_edit = make_edit(page, IDC_BAT_VOLTAGE_MAX_EDIT, L"-1",
                                                col2_edit_x, iy, col2_w, EDIT_H);
            iy += row_h;

            // Row 3: Current + CurrentAvg
            make_label(page, L"Current:", label_x, iy + 3, LABEL_W, 18);
            g_bat_current_edit = make_edit(page, IDC_BAT_CURRENT_EDIT, L"", edit_x, iy, col1_w, EDIT_H);
            make_label(page, L"I-Avg:", col2_x, iy + 3, col2_label_w, 18);
            g_bat_current_avg_edit = make_edit(page, IDC_BAT_CURRENT_AVG_EDIT, L"",
                                                col2_edit_x, iy, col2_w, EDIT_H);
            iy += row_h;

            // Row 4: CurrentMax + TotalEnergy
            make_label(page, L"I-Max:", label_x, iy + 3, LABEL_W, 18);
            g_bat_current_max_edit = make_edit(page, IDC_BAT_CURRENT_MAX_EDIT, L"-1",
                                                edit_x, iy, col1_w, EDIT_H);
            make_label(page, L"TotalE:", col2_x, iy + 3, col2_label_w, 18);
            g_bat_total_energy_edit = make_edit(page, IDC_BAT_TOTAL_ENERGY_EDIT, L"",
                                                 col2_edit_x, iy, col2_w, EDIT_H);
            iy += row_h;

            // Row 5: Technology
            make_label(page, L"Technology:", label_x, iy + 3, LABEL_W, 18);
            g_bat_technology_edit = make_edit(page, IDC_BAT_TECHNOLOGY_EDIT, L"Li-poly",
                                               edit_x, iy, edit_w, EDIT_H);
            iy += row_h;

            // Row 6: Plugged combo
            make_label(page, L"Plugged:", label_x, iy + 3, LABEL_W, 18);
            g_bat_plugged_combo = make_combo(page, IDC_BAT_PLUGGED_COMBO,
                                              edit_x, iy, edit_w, 200);
            SendMessageW(g_bat_plugged_combo, CB_ADDSTRING, 0, (LPARAM)L"0: None");
            SendMessageW(g_bat_plugged_combo, CB_ADDSTRING, 0, (LPARAM)L"1: AC");
            SendMessageW(g_bat_plugged_combo, CB_ADDSTRING, 0, (LPARAM)L"2: USB");
            SendMessageW(g_bat_plugged_combo, CB_ADDSTRING, 0, (LPARAM)L"3: Wireless");
            SendMessageW(g_bat_plugged_combo, CB_SETCURSEL, 0, 0);
            iy += row_h;

            // Row 7: Charger combo (0-6)
            make_label(page, L"Charger:", label_x, iy + 3, LABEL_W, 18);
            g_bat_charger_combo = make_combo(page, IDC_BAT_CHARGER_COMBO,
                                              edit_x, iy, edit_w, 200);
            SendMessageW(g_bat_charger_combo, CB_ADDSTRING, 0, (LPARAM)L"0: None");
            SendMessageW(g_bat_charger_combo, CB_ADDSTRING, 0, (LPARAM)L"1: AC slow");
            SendMessageW(g_bat_charger_combo, CB_ADDSTRING, 0, (LPARAM)L"2: AC quick");
            SendMessageW(g_bat_charger_combo, CB_ADDSTRING, 0, (LPARAM)L"3: AC super");
            SendMessageW(g_bat_charger_combo, CB_ADDSTRING, 0, (LPARAM)L"4: AC turbo");
            SendMessageW(g_bat_charger_combo, CB_ADDSTRING, 0, (LPARAM)L"5: Wireless slow");
            SendMessageW(g_bat_charger_combo, CB_ADDSTRING, 0, (LPARAM)L"6: Wireless quick");
            SendMessageW(g_bat_charger_combo, CB_SETCURSEL, 0, 0);
            iy += row_h;

            // Row 8: ChargeState combo
            make_label(page, L"Chg State:", label_x, iy + 3, LABEL_W, 18);
            g_bat_chargestate_combo = make_combo(page, IDC_BAT_CHARGESTATE_COMBO,
                                                  edit_x, iy, edit_w, 200);
            SendMessageW(g_bat_chargestate_combo, CB_ADDSTRING, 0, (LPARAM)L"0: Discharging");
            SendMessageW(g_bat_chargestate_combo, CB_ADDSTRING, 0, (LPARAM)L"1: Charging");
            SendMessageW(g_bat_chargestate_combo, CB_ADDSTRING, 0, (LPARAM)L"2: Not charging");
            SendMessageW(g_bat_chargestate_combo, CB_ADDSTRING, 0, (LPARAM)L"3: Full");
            SendMessageW(g_bat_chargestate_combo, CB_SETCURSEL, 0, 0);
            iy += row_h;

            // Row 9: Health + Present (side by side)
            make_label(page, L"Health:", label_x, iy + 3, LABEL_W, 18);
            g_bat_health_combo = make_combo(page, IDC_BAT_HEALTH_COMBO,
                                             edit_x, iy, col1_w, 200);
            SendMessageW(g_bat_health_combo, CB_ADDSTRING, 0, (LPARAM)L"0: Unknown");
            SendMessageW(g_bat_health_combo, CB_ADDSTRING, 0, (LPARAM)L"1: Good");
            SendMessageW(g_bat_health_combo, CB_ADDSTRING, 0, (LPARAM)L"2: Overheat");
            SendMessageW(g_bat_health_combo, CB_ADDSTRING, 0, (LPARAM)L"3: Dead");
            SendMessageW(g_bat_health_combo, CB_ADDSTRING, 0, (LPARAM)L"4: Over voltage");
            SendMessageW(g_bat_health_combo, CB_ADDSTRING, 0, (LPARAM)L"5: Unspecified");
            SendMessageW(g_bat_health_combo, CB_SETCURSEL, 1, 0);
            make_label(page, L"Present:", col2_x, iy + 3, col2_label_w, 18);
            g_bat_present_combo = make_combo(page, IDC_BAT_PRESENT_COMBO,
                                              col2_edit_x, iy, col2_w, 200);
            SendMessageW(g_bat_present_combo, CB_ADDSTRING, 0, (LPARAM)L"0: No");
            SendMessageW(g_bat_present_combo, CB_ADDSTRING, 0, (LPARAM)L"1: Yes");
            SendMessageW(g_bat_present_combo, CB_SETCURSEL, 1, 0);
            iy += row_h + 8;

            // Buttons
            g_bat_inject_btn = make_btn(page, L"Inject", IDC_BAT_INJECT_BTN,
                                         edit_x, iy, 80, BTN_H);
            g_bat_reset_btn = make_btn(page, L"Reset", IDC_BAT_RESET_BTN,
                                        edit_x + 88, iy, 80, BTN_H);
            g_bat_refresh_btn = make_btn(page, L"Refresh", IDC_BAT_REFRESH_BTN,
                                          edit_x + 176, iy, 80, BTN_H);
            y += 323;

            // Charge scene group
            make_group(page, L"Charge Scene Simulation", grp_x, y, grp_w, 60);
            int sy = y + 20;
            g_bat_scene_combo = make_combo(page, IDC_BAT_SCENE_COMBO,
                                            label_x, sy, edit_w - 90, 200);
            SendMessageW(g_bat_scene_combo, CB_ADDSTRING, 0, (LPARAM)L"0: AC normal 27W");
            SendMessageW(g_bat_scene_combo, CB_ADDSTRING, 0, (LPARAM)L"1: AC quick 40W");
            SendMessageW(g_bat_scene_combo, CB_ADDSTRING, 0, (LPARAM)L"2: AC super 40W");
            SendMessageW(g_bat_scene_combo, CB_ADDSTRING, 0, (LPARAM)L"3: AC super 66W");
            SendMessageW(g_bat_scene_combo, CB_ADDSTRING, 0, (LPARAM)L"4: AC turbo 66W");
            SendMessageW(g_bat_scene_combo, CB_ADDSTRING, 0, (LPARAM)L"5: Wireless 27W");
            SendMessageW(g_bat_scene_combo, CB_ADDSTRING, 0, (LPARAM)L"6: Wireless 40W");
            SendMessageW(g_bat_scene_combo, CB_ADDSTRING, 0, (LPARAM)L"7: Wireless 40W+");
            SendMessageW(g_bat_scene_combo, CB_ADDSTRING, 0, (LPARAM)L"8: Wireless 50W");
            SendMessageW(g_bat_scene_combo, CB_SETCURSEL, 0, 0);
            g_bat_scene_btn = make_btn(page, L"Start", IDC_BAT_SCENE_BTN,
                                        edit_x + edit_w - 82, sy, 80, BTN_H);
            set_page_scroll(g_tab_pages[TAB_BATTERY], y + 60, ph);
        }

        // ── Haps page ─────────────────────────────────────
        {
            ensure_page_class_registered(g_inst);
            HWND page = CreateWindowW(CPH_PAGE_W, L"",
                                       WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL,
                                       px, py, pw, ph,
                                       g_tab, nullptr, nullptr, nullptr);
            g_tab_pages[TAB_HAPS] = page;

            // Packages group (compact height, top gap like GNSS)
            int pkg_grp_y = 6;
            int pkg_grp_h = 350;
            make_group(page, L"Packages", grp_x, pkg_grp_y, grp_w, pkg_grp_h);
            int pkg_list_h = pkg_grp_h - 20 - BTN_H - 12;
            g_haps_pkg_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                               grp_x + 4, pkg_grp_y + 20, grp_w - 8, pkg_list_h,
                                               page, (HMENU)IDC_HAPS_PKG_LIST, nullptr, nullptr);
            SendMessageW(g_haps_pkg_list, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
            SendMessageW(g_haps_pkg_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                         (LPARAM)(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER));
            {
                LVCOLUMNW col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.pszText = const_cast<LPWSTR>(L"Bundle Name");
                col.cx = grp_w - 28;
                SendMessageW(g_haps_pkg_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
            }
            // Refresh button at bottom of packages group (horizontally centered)
            int refresh_x = grp_x + (grp_w - 80) / 2;
            g_haps_refresh_btn = make_btn(page, L"Refresh", IDC_HAPS_REFRESH_BTN,
                                           refresh_x, pkg_grp_y + pkg_grp_h - BTN_H - 6, 80, BTN_H);

            // Permissions group (compact, same height as packages)
            int perm_grp_y = pkg_grp_y + pkg_grp_h + ROW_GAP;
            int perm_grp_h = 300;
            make_group(page, L"Permissions", grp_x, perm_grp_y, grp_w, perm_grp_h);
            int perm_list_h = perm_grp_h - 20 - BTN_H - 12;
            g_haps_perm_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                                grp_x + 4, perm_grp_y + 20, grp_w - 8, perm_list_h,
                                                page, (HMENU)IDC_HAPS_PERM_LIST, nullptr, nullptr);
            SendMessageW(g_haps_perm_list, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
            SendMessageW(g_haps_perm_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                         (LPARAM)(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER));
            {
                int perm_col_w = grp_w - 12;
                int col0_w = perm_col_w * 3 / 5;
                int col1_w = perm_col_w * 1 / 5;
                int col2_w = perm_col_w - col0_w - col1_w;  // absorb remainder
                LVCOLUMNW col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.pszText = const_cast<LPWSTR>(L"Permission");
                col.cx = col0_w;
                SendMessageW(g_haps_perm_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
                col.pszText = const_cast<LPWSTR>(L"Status");
                col.cx = col1_w;
                SendMessageW(g_haps_perm_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
                col.pszText = const_cast<LPWSTR>(L"SystemFixed");
                col.cx = col2_w;
                SendMessageW(g_haps_perm_list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
            }

            // Action buttons at bottom of permissions group (horizontally centered)
            int btn_y = perm_grp_y + perm_grp_h - BTN_H - 6;
            int hbtn_w = 80;
            int hbtn_gap = 4;
            int hbtn_total = hbtn_w * 4 + hbtn_gap * 3;
            int hbtn_x = grp_x + (grp_w - hbtn_total) / 2;
            g_haps_grant_all_btn = make_btn(page, L"Grant All", IDC_HAPS_GRANT_ALL_BTN,
                                             hbtn_x, btn_y, hbtn_w, BTN_H);
            g_haps_grant_btn = make_btn(page, L"Grant", IDC_HAPS_GRANT_BTN,
                                         hbtn_x + hbtn_w + hbtn_gap, btn_y, hbtn_w, BTN_H);
            g_haps_revoke_all_btn = make_btn(page, L"Revoke All", IDC_HAPS_REVOKE_ALL_BTN,
                                               hbtn_x + (hbtn_w + hbtn_gap) * 2, btn_y, hbtn_w, BTN_H);
            g_haps_revoke_btn = make_btn(page, L"Revoke", IDC_HAPS_REVOKE_BTN,
                                           hbtn_x + (hbtn_w + hbtn_gap) * 3, btn_y, hbtn_w, BTN_H);
            set_page_scroll(g_tab_pages[TAB_HAPS], perm_grp_y + perm_grp_h, ph);
        }

        // ── I18n page ──────────────────────────────────────
        {
            ensure_page_class_registered(g_inst);
            HWND page = CreateWindowW(CPH_PAGE_W, L"",
                                       WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL,
                                       px, py, pw, ph,
                                       g_tab, nullptr, nullptr, nullptr);
            g_tab_pages[TAB_I18N] = page;
            int y = 6;

            // ── Current Settings group (top, like other tabs) ──
            int cur_grp_h = 6 * 18 + 24;
            make_group(page, L"Current Settings", grp_x, y, grp_w, cur_grp_h);
            g_i18n_current_label = CreateWindowW(L"STATIC", L"--",
                                                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL,
                                                   grp_x + 10, y + 18, grp_w - 20, cur_grp_h - 24,
                                                   page, (HMENU)IDC_I18N_CURRENT_LABEL, nullptr, nullptr);
            SendMessageW(g_i18n_current_label, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
            y += cur_grp_h + 4;

            // ── Locale group (Language + Region) ──
            int locale_grp_h = 2 * row_h + BTN_H + 32;
            make_group(page, L"Locale", grp_x, y, grp_w, locale_grp_h);
            int sy = y + 20;

            make_label(page, L"Language:", label_x, sy + 3, LABEL_W, 18);
            g_i18n_lang_combo = make_editable_combo(page, IDC_I18N_LANG_COMBO,
                                            edit_x, sy, edit_w, 200);
            for (int i = 0; i < I18N_LOCALES_COUNT; i++)
                SendMessageW(g_i18n_lang_combo, CB_ADDSTRING, 0, (LPARAM)I18N_LOCALES[i]);
            sy += row_h;

            make_label(page, L"Region:", label_x, sy + 3, LABEL_W, 18);
            g_i18n_region_combo = make_editable_combo(page, IDC_I18N_REGION_COMBO,
                                              edit_x, sy, edit_w, 300);
            for (int i = 0; i < I18N_REGIONS_COUNT; i++) {
                wchar_t wbuf[8];
                MultiByteToWideChar(CP_UTF8, 0, I18N_REGIONS[i], -1, wbuf, 8);
                SendMessageW(g_i18n_region_combo, CB_ADDSTRING, 0, (LPARAM)wbuf);
            }
            sy += row_h + 4;

            // Apply button (horizontally centered)
            g_i18n_apply_btn = make_btn(page, L"Apply", IDC_I18N_APPLY_BTN,
                                         grp_x + (grp_w - 80) / 2, sy, 80, BTN_H);
            y += locale_grp_h + 4;

            // ── Date Time group ──
            // Top: manual settings (Date/Time/Timezone/SetDateTime) — grayed when AutoTime on
            // Bottom: global switches (AutoTime + 24-hour) separated by thin line
            int dt_grp_h = 5 * row_h + 30;
            make_group(page, L"Date Time", grp_x, y, grp_w, dt_grp_h);
            int dy = y + 20;

            // Row 1: Date + Time edits (manual)
            int date_w = edit_w / 2 - 4;
            int time_x = edit_x + date_w + 8;
            int time_w = edit_w - date_w - 8;
            make_label(page, L"Date:", label_x, dy + 3, LABEL_W, 18);
            g_i18n_date_edit = make_edit(page, IDC_I18N_DATE_EDIT, L"2026-07-15",
                                          edit_x, dy, date_w, EDIT_H);
            make_label(page, L"Time:", time_x, dy + 3, 40, 18);
            g_i18n_time_edit = make_edit(page, IDC_I18N_TIME_EDIT, L"10:30:00",
                                          time_x + 44, dy, time_w - 44, EDIT_H);
            dy += row_h;

            // Row 2: Timezone combo (manual, grayed when AutoTime checked)
            make_label(page, L"Timezone:", label_x, dy + 3, LABEL_W, 18);
            g_i18n_tz_combo = make_editable_combo(page, IDC_I18N_TZ_COMBO,
                                           edit_x, dy, edit_w, 300);
            for (int i = 0; i < I18N_TIMEZONES_COUNT; i++)
                SendMessageW(g_i18n_tz_combo, CB_ADDSTRING, 0, (LPARAM)I18N_TIMEZONES[i]);
            dy += row_h;

            // Row 3: Set DateTime button, centered (manual, grayed when AutoTime checked)
            g_i18n_set_datetime_btn = make_btn(page, L"Apply", IDC_I18N_SET_DATETIME_BTN,
                                                 grp_x + (grp_w - 80) / 2, dy, 80, BTN_H);
            dy += row_h + 4;

            // Separator line between manual and global sections
            CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                           grp_x + 4, dy + 2, grp_w - 8, 2,
                           page, nullptr, nullptr, nullptr);
            dy += 8;

            // Row 4: Enable AutoTime checkbox (global master toggle)
            g_i18n_autotime_btn = CreateWindowW(L"BUTTON", L"Enable AutoTime",
                                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                 edit_x, dy, edit_w, EDIT_H,
                                                 page, (HMENU)IDC_I18N_AUTOTIME_BTN, nullptr, nullptr);
            if (g_cph_font) SendMessageW(g_i18n_autotime_btn, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
            dy += row_h;

            // Row 5: 24-hour checkbox (independent global toggle)
            g_i18n_24hour_check = CreateWindowW(L"BUTTON", L"Enable 24-hour clock",
                                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                 edit_x, dy, edit_w, EDIT_H,
                                                 page, (HMENU)IDC_I18N_24HOUR_CHECK, nullptr, nullptr);
            if (g_cph_font) SendMessageW(g_i18n_24hour_check, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
            y += dt_grp_h + 4;

            // Refresh button (bottom, horizontally centered)
            g_i18n_refresh_btn = make_btn(page, L"Refresh", IDC_I18N_REFRESH_BTN,
                                           grp_x + (grp_w - 80) / 2, y, 80, BTN_H);
            set_page_scroll(g_tab_pages[TAB_I18N], y + 30, ph);
        }

        // ── Sensor page ───────────────────────────────────
        {
            ensure_page_class_registered(g_inst);
            HWND page = CreateWindowW(CPH_PAGE_W, L"",
                                       WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL,
                                       px, py, pw, ph,
                                       g_tab, nullptr, nullptr, nullptr);
            g_tab_pages[TAB_SENSOR] = page;
            int y = 6;

            // ── Current Sensor State group ──
            int cur_grp_h = 3 * 18 + 50 + BTN_H;  // labels(54) + title(20) + gap(8) + padding(22) + btn(28)
            make_group(page, L"Current Sensor State", grp_x, y, grp_w, cur_grp_h);
            int ly = y + 20;
            make_label(page, L"Degree:", label_x, ly, LABEL_W, 18);
            g_sensor_degree_label = CreateWindowW(L"STATIC", L"?",
                                                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                                    edit_x, ly, edit_w, 18,
                                                    page, (HMENU)IDC_SENSOR_DEGREE_LABEL, nullptr, nullptr);
            if (g_cph_font) SendMessageW(g_sensor_degree_label, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
            ly += 18;
            make_label(page, L"Shaking:", label_x, ly, LABEL_W, 18);
            g_sensor_shaking_label = CreateWindowW(L"STATIC", L"?",
                                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                                     edit_x, ly, edit_w, 18,
                                                     page, (HMENU)IDC_SENSOR_SHAKING_LABEL, nullptr, nullptr);
            if (g_cph_font) SendMessageW(g_sensor_shaking_label, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
            ly += 18;
            make_label(page, L"Source:", label_x, ly, LABEL_W, 18);
            g_sensor_source_label = CreateWindowW(L"STATIC", L"?",
                                                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                                    edit_x, ly, edit_w, 18,
                                                    page, (HMENU)IDC_SENSOR_SOURCE_LABEL, nullptr, nullptr);
            if (g_cph_font) SendMessageW(g_sensor_source_label, WM_SETFONT, (WPARAM)g_cph_font, TRUE);
            ly += 18 + 8;
            // Refresh button (inside group, at bottom)
            g_sensor_refresh_btn = make_btn(page, L"Refresh", IDC_SENSOR_REFRESH_BTN,
                                             grp_x + (grp_w - 80) / 2, ly, 80, BTN_H);
            y += cur_grp_h + 4;

            // ── Screen Rotation group ──
            int rot_grp_h = 2 * row_h + 36;
            make_group(page, L"Screen Rotation", grp_x, y, grp_w, rot_grp_h);
            int ry = y + 20;
            make_label(page, L"Degree:", label_x, ry + 3, LABEL_W, 18);
            g_sensor_rotate_combo = make_combo(page, IDC_SENSOR_ROTATE_COMBO,
                                                edit_x, ry, edit_w, 100);
            SendMessageW(g_sensor_rotate_combo, CB_ADDSTRING, 0, (LPARAM)L"0");
            SendMessageW(g_sensor_rotate_combo, CB_ADDSTRING, 0, (LPARAM)L"90");
            SendMessageW(g_sensor_rotate_combo, CB_ADDSTRING, 0, (LPARAM)L"180");
            SendMessageW(g_sensor_rotate_combo, CB_ADDSTRING, 0, (LPARAM)L"270");
            SendMessageW(g_sensor_rotate_combo, CB_SETCURSEL, 0, 0);
            ry += row_h;
            // Rotate + Rotate Next + Reset buttons (3 buttons centered)
            int rot_btn_w = 80;
            int rot_btn_gap = 8;
            int rot_btn_total = rot_btn_w * 3 + rot_btn_gap * 2;
            int rot_btn_x = grp_x + (grp_w - rot_btn_total) / 2;
            g_sensor_rotate_btn = make_btn(page, L"Rotate", IDC_SENSOR_ROTATE_BTN,
                                            rot_btn_x, ry, rot_btn_w, BTN_H);
            g_sensor_rotate_next_btn = make_btn(page, L"Rotate Next", IDC_SENSOR_ROTATE_NEXT_BTN,
                                            rot_btn_x + rot_btn_w + rot_btn_gap, ry, rot_btn_w, BTN_H);
            g_sensor_reset_btn = make_btn(page, L"Reset", IDC_SENSOR_RESET_BTN,
                                            rot_btn_x + (rot_btn_w + rot_btn_gap) * 2, ry, rot_btn_w, BTN_H);
            y += rot_grp_h + 4;

            // ── Shake Simulation group ──
            int shake_grp_h = 2 * row_h + 36;
            make_group(page, L"Shake Simulation", grp_x, y, grp_w, shake_grp_h);
            int shy = y + 20;
            make_label(page, L"Duration (ms):", label_x, shy + 3, LABEL_W + 30, 18);
            g_sensor_shake_edit = make_edit(page, IDC_SENSOR_SHAKE_EDIT, L"1000",
                                             edit_x, shy, edit_w, EDIT_H);
            shy += row_h;
            // Trigger Shake button (centered)
            g_sensor_shake_btn = make_btn(page, L"Trigger Shake", IDC_SENSOR_SHAKE_BTN,
                                            grp_x + (grp_w - 100) / 2, shy, 100, BTN_H);
            y += shake_grp_h + 4;
            set_page_scroll(g_tab_pages[TAB_SENSOR], y, ph);
        }

        // Status label at bottom
        g_status_label = CreateWindowW(L"STATIC", L"Ready",
                                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                        MARGIN, 575, g_panel_w - MARGIN * 2, 18,
                                        hwnd, (HMENU)IDC_STATUS_LABEL, nullptr, nullptr);
        SendMessageW(g_status_label, WM_SETFONT, (WPARAM)g_cph_font, TRUE);

        // Sync tab control selection to the default page (BS_AUTOCHECKBOX
        // defaults to 0, but g_current_tab=TAB_GNSS=1 after tab reordering).
        TabCtrl_SetCurSel(g_tab, TAB_GNSS);
        show_tab_page(TAB_GNSS);

        SetPropW(hwnd, L"cph_font", (HANDLE)g_cph_font);
        return 0;
    }

    case WM_NOTIFY: {
        auto *nm = reinterpret_cast<NMHDR *>(lp);
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) {
            on_tab_change();
        }
        if (nm->idFrom == IDC_HAPS_PKG_LIST && nm->code == LVN_ITEMCHANGED) {
            auto *pn = reinterpret_cast<LPNMLISTVIEW>(lp);
            if ((pn->uNewState & LVIS_SELECTED) && !(pn->uOldState & LVIS_SELECTED)) {
                if (pn->iItem >= 0 && pn->iItem < (int)g_haps_packages.size()) {
                    g_haps_selected_pkg = g_haps_packages[pn->iItem];
                    if (!g_haps_bm_unsupported) haps_list_permissions();
                }
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        int code = HIWORD(wp);
        int id = LOWORD(wp);
        // Editable combobox live filter: rebuild dropdown list to match the
        // current edit text for the i18n Language / Region / Timezone combos.
        if (code == CBN_EDITCHANGE &&
            (id == IDC_I18N_LANG_COMBO || id == IDC_I18N_REGION_COMBO || id == IDC_I18N_TZ_COMBO)) {
            i18n_filter_combo(id);
            return 0;
        }
        switch (id) {
        case IDC_GNSS_INJECT_BTN:
            inject_gnss();
            break;
        case IDC_GNSS_REFRESH_BTN:
            refresh_gnss_fill_edits();
            break;
        case IDC_GNSS_TRAJ_BROWSE_BTN: {
            char file[512] = {};
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_parent ? g_parent : hwnd;
            ofn.lpstrFilter = "GPX/XML Files\0*.gpx;*.xml\0All Files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = sizeof(file);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ENABLEHOOK;
            ofn.lpfnHook = [](HWND hDlg, UINT msg, WPARAM, LPARAM) -> UINT_PTR {
                if (msg == WM_INITDIALOG && g_parent) {
                    RECT orr, mr;
                    if (GetWindowRect(g_parent, &orr) && GetWindowRect(hDlg, &mr)) {
                        int cx = orr.left + ((orr.right - orr.left) - (mr.right - mr.left)) / 2;
                        int cy = orr.top + ((orr.bottom - orr.top) - (mr.bottom - mr.top)) / 2;
                        SetWindowPos(hDlg, NULL, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                    }
                }
                return 0;
            };
            if (GetOpenFileNameA(&ofn)) {
                // Check file extension (.gpx or .xml, case-insensitive)
                std::string local_path = file;
                auto dot = local_path.find_last_of('.');
                if (dot == std::string::npos) {
                    msg_box("Trajectory", "Only .gpx and .xml files are supported");
                    break;
                }
                std::string ext = local_path.substr(dot);
                for (auto &c : ext) c = tolower(c);
                if (ext != ".gpx" && ext != ".xml") {
                    msg_box("Trajectory", "Only .gpx and .xml files are supported");
                    break;
                }

                auto sep = local_path.find_last_of("\\/");
                std::string filename = (sep != std::string::npos) ? local_path.substr(sep + 1) : local_path;
                std::string remote_path = std::string("/data/local/tmp/") + filename;

                // Push file to device
                if (g_on_push && g_on_push(local_path, remote_path)) {
                    // Set read permission on pushed file
                    std::string chmod_cmd = "chmod a+r '" + remote_path + "'";
                    std::string chmod_out;
                    if (g_on_shell) g_on_shell(chmod_cmd, &chmod_out, 5);
                    s_traj_hint_active = false;
                    SetWindowTextA(g_gnss_traj_edit, remote_path.c_str());
                    show_toast("File pushed to device", true);
                    set_status(remote_path.c_str());
                } else {
                    msg_box("Trajectory", "Failed to push file to device");
                }
            }
            break;
        }
        case IDC_GNSS_TRAJ_START_BTN:
            start_trajectory();
            break;
        case IDC_GNSS_TRAJ_STOP_BTN:
            stop_trajectory();
            break;
        case IDC_BAT_INJECT_BTN:
            inject_battery();
            break;
        case IDC_BAT_RESET_BTN:
            reset_battery();
            break;
        case IDC_BAT_REFRESH_BTN:
            s_refresh_fill_edits = true;
            refresh_battery();
            break;
        case IDC_BAT_SCENE_BTN:
            inject_charge_scene();
            break;
        case IDC_HAPS_REFRESH_BTN:
            g_haps_loaded = false;
            g_haps_bm_unsupported = false;
            haps_enable_perm_buttons(true);
            haps_list_packages();
            break;
        case IDC_HAPS_GRANT_ALL_BTN:
            haps_grant_all();
            break;
        case IDC_HAPS_REVOKE_ALL_BTN:
            haps_revoke_all();
            break;
        case IDC_HAPS_GRANT_BTN: {
            std::string csv = haps_get_selected_perms_csv();
            if (!csv.empty()) haps_grant(csv);
            break;
        }
        case IDC_HAPS_REVOKE_BTN: {
            std::string csv = haps_get_selected_perms_csv();
            if (!csv.empty()) haps_revoke(csv);
            break;
        }
        case IDC_HAPS_COPY_PERM_BTN: {
            // Collect selected permission names (one per line) → clipboard
            std::string csv = haps_get_selected_perms_csv();
            if (csv.empty()) break;
            // Convert comma-separated to newline-separated for clipboard
            std::string text;
            for (size_t i = 0; i < csv.size(); ++i) {
                if (csv[i] == ',') text += "\r\n";
                else text += csv[i];
            }
            text += "\r\n";
            if (copy_text_to_clipboard(hwnd, text))
                show_toast("Permission name copied", true);
            break;
        }
        case IDC_HAPS_COPY_PKG_BTN: {
            // Collect selected package names from g_haps_packages → clipboard
            if (!g_haps_pkg_list) break;
            std::string text;
            int idx = -1;
            for (;;) {
                idx = (int)SendMessageW(g_haps_pkg_list, LVM_GETNEXTITEM, idx, LVNI_SELECTED);
                if (idx == -1) break;
                if (idx >= 0 && idx < (int)g_haps_packages.size()) {
                    if (!text.empty()) text += "\r\n";
                    text += g_haps_packages[idx];
                }
            }
            if (text.empty()) break;
            text += "\r\n";
            if (copy_text_to_clipboard(hwnd, text))
                show_toast("Bundle name copied", true);
            break;
        }
        case IDC_I18N_REFRESH_BTN:
            g_i18n_loaded = false;
            i18n_refresh();
            break;
        case IDC_I18N_APPLY_BTN:
            i18n_apply();
            break;
        case IDC_I18N_SET_DATETIME_BTN:
            i18n_set_datetime();
            break;
        case IDC_I18N_AUTOTIME_BTN: {
            // AutoTime checkbox toggled — gray out Timezone/Date/Time/SetDateTime (mutex)
            bool autotime = (SendMessageW(g_i18n_autotime_btn, BM_GETCHECK, 0, 0) == BST_CHECKED);
            HWND mutex_ctrls[] = {g_i18n_tz_combo, g_i18n_date_edit, g_i18n_time_edit,
                                  g_i18n_set_datetime_btn};
            for (HWND h : mutex_ctrls) { if (h) EnableWindow(h, !autotime); }
            if (autotime) {
                // Enable AutoTime immediately when checked
                i18n_set_autotime();
            } else {
                // Uncheck: send current timezone + Date/Time to indirectly turn off AutoTime
                // (design 4.1: set -d automatically disables autotime)
                i18n_disable_autotime();
            }
            break;
        }
        case IDC_I18N_24HOUR_CHECK: {
            // 24-hour checkbox toggled — apply immediately
            bool checked = (SendMessageW(g_i18n_24hour_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
            i18n_set_24hour(checked);
            break;
        }
        case IDC_SENSOR_REFRESH_BTN:
            g_sensor_loaded = false;
            sensor_refresh();
            break;
        case IDC_SENSOR_ROTATE_BTN:
            sensor_rotate();
            break;
        case IDC_SENSOR_ROTATE_NEXT_BTN:
            sensor_rotate_next();
            break;
        case IDC_SENSOR_RESET_BTN:
            sensor_reset_rotation();
            break;
        case IDC_SENSOR_SHAKE_BTN:
            sensor_trigger_shake();
            break;
        }
        return 0;
    }

    case WM_CONTEXTMENU: {
        HWND ctrl = (HWND)wp;
        if (ctrl && GetDlgCtrlID(ctrl) == IDC_HAPS_PERM_LIST) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            // Use screen-to-list hit test if right-click via keyboard (-1,-1)
            if (pt.x == -1 && pt.y == -1) {
                RECT rc;
                GetWindowRect(ctrl, &rc);
                pt.x = rc.left + 20;
                pt.y = rc.top + 20;
            }
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDC_HAPS_GRANT_BTN, L"Grant");
            AppendMenuW(menu, MF_STRING, IDC_HAPS_REVOKE_BTN, L"Revoke");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDC_HAPS_COPY_PERM_BTN, L"Copy Permission Name");
            TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
        }
        if (ctrl && GetDlgCtrlID(ctrl) == IDC_HAPS_PKG_LIST) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) {
                RECT rc;
                GetWindowRect(ctrl, &rc);
                pt.x = rc.left + 20;
                pt.y = rc.top + 20;
            }
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDC_HAPS_COPY_PKG_BTN, L"Copy Bundle Name");
            TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
        }
        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (g_tab) MoveWindow(g_tab, MARGIN, HEADER_H, w - MARGIN * 2, h - HEADER_H - 26, TRUE);
        if (g_status_label) MoveWindow(g_status_label, MARGIN, h - 22, w - MARGIN * 2, 18, TRUE);
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        if (dis->CtlType == ODT_TAB) {
            // Owner-draw tab buttons: white background + centered text
            FillRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(WHITE_BRUSH));
            wchar_t buf[32] = {};
            TCITEMW ti = {};
            ti.mask = TCIF_TEXT;
            ti.pszText = buf;
            ti.cchTextMax = 32;
            TabCtrl_GetItem(g_tab, dis->itemID, &ti);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, RGB(0, 0, 0));
            // Draw thin gray border for unselected tabs
            if (!(dis->itemState & ODS_SELECTED)) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
                HPEN old = (HPEN)SelectObject(dis->hDC, pen);
                HBRUSH oldb = (HBRUSH)SelectObject(dis->hDC, GetStockObject(WHITE_BRUSH));
                Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
                          dis->rcItem.right, dis->rcItem.bottom);
                SelectObject(dis->hDC, old);
                SelectObject(dis->hDC, oldb);
                DeleteObject(pen);
            }
            // Bold text for selected tab, normal for others
            HFONT draw_font = g_cph_font;
            if (dis->itemState & ODS_SELECTED) {
                if (!g_tab_bold_font)
                    g_tab_bold_font = CreateFontW(14, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                                  DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                if (g_tab_bold_font) draw_font = g_tab_bold_font;
            }
            HFONT old_font = (HFONT)SelectObject(dis->hDC, draw_font);
            DrawTextW(dis->hDC, buf, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dis->hDC, old_font);
            return TRUE;
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == CPH_REFRESH_TIMER_ID) {
            // Stop auto-refresh after consecutive failures to avoid overloading gnss HAL
            if (s_refresh_fail_count >= MAX_REFRESH_FAILS) return 0;
            // Auto-refresh current tab data (async, non-blocking)
            if (g_current_tab == TAB_GNSS) refresh_gnss();
            else if (g_current_tab == TAB_BATTERY) refresh_battery();
            // Sensor data is static - only refresh after user actions, not on timer
            return 0;
        }
        break;

    case WM_DESTROY: {
        if (g_refresh_timer) { KillTimer(hwnd, CPH_REFRESH_TIMER_ID); g_refresh_timer = 0; }
        if (g_cph_font) { DeleteObject(g_cph_font); g_cph_font = nullptr; }
        if (g_tab_bold_font) { DeleteObject(g_tab_bold_font); g_tab_bold_font = nullptr; }
        RemovePropW(hwnd, L"cph_font");
        g_hwnd = nullptr;
        g_tab = nullptr;
        g_gnss_lat_edit = g_gnss_lng_edit = g_gnss_alt_edit = nullptr;
        g_gnss_speed_edit = g_gnss_bearing_edit = nullptr;
        g_gnss_hacc_edit = g_gnss_vacc_edit = g_gnss_sacc_edit = g_gnss_bacc_edit = nullptr;
        g_gnss_inject_btn = g_gnss_refresh_btn = nullptr;
        g_gnss_current_label = nullptr;
        g_gnss_traj_edit = nullptr;
        s_traj_edit_hwnd = nullptr;
        s_traj_hint_active = true;
        g_gnss_traj_browse_btn = g_gnss_traj_start_btn = g_gnss_traj_stop_btn = nullptr;
        g_bat_capacity_edit = g_bat_temp_edit = g_bat_voltage_edit = nullptr;
        g_bat_voltage_max_edit = g_bat_current_edit = g_bat_current_avg_edit = nullptr;
        g_bat_current_max_edit = g_bat_total_energy_edit = g_bat_technology_edit = nullptr;
        g_bat_health_combo = g_bat_present_combo = nullptr;
        g_bat_plugged_combo = g_bat_charger_combo = g_bat_chargestate_combo = nullptr;
        g_bat_inject_btn = g_bat_reset_btn = g_bat_refresh_btn = nullptr;
        g_bat_current_label = nullptr;
        g_bat_scene_combo = g_bat_scene_btn = nullptr;
        g_haps_pkg_list = g_haps_perm_list = nullptr;
        g_haps_refresh_btn = g_haps_grant_all_btn = g_haps_grant_btn = g_haps_revoke_btn = g_haps_revoke_all_btn = nullptr;
        g_haps_packages.clear();
        g_haps_selected_pkg.clear();
        g_haps_user_id.clear();
        g_haps_loaded = false;
        g_haps_bm_unsupported = false;
        g_i18n_lang_combo = g_i18n_region_combo = g_i18n_tz_combo = nullptr;
        g_i18n_24hour_check = nullptr;
        g_i18n_apply_btn = g_i18n_refresh_btn = nullptr;
        g_i18n_date_edit = g_i18n_time_edit = nullptr;
        g_i18n_set_datetime_btn = g_i18n_autotime_btn = nullptr;
        g_i18n_current_label = nullptr;
        g_i18n_loaded = false;
        g_sensor_degree_label = g_sensor_shaking_label = g_sensor_source_label = nullptr;
        g_sensor_rotate_combo = g_sensor_rotate_btn = g_sensor_rotate_next_btn = g_sensor_reset_btn = nullptr;
        g_sensor_shake_edit = g_sensor_shake_btn = g_sensor_refresh_btn = nullptr;
        g_sensor_loaded = false;
        g_status_label = nullptr;
        for (auto &p : g_tab_pages) p = nullptr;
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace hscrcpy
