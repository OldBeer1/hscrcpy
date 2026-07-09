#include "layout_window.h"
#include "log.h"
#include <windowsx.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <string>

namespace hscrcpy {

using json = nlohmann::json;

// ── Helpers ──────────────────────────────────────────────

static std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (len <= 0) return L"";
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

static std::string wide_to_utf8(const std::wstring &ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    if (len <= 0) return "";
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, NULL, NULL);
    return s;
}

static void parse_bounds(const std::string &s, int &l, int &t, int &r, int &b) {
    if (!s.empty()) sscanf(s.c_str(), "[%d,%d][%d,%d]", &l, &t, &r, &b);
}

// ── Statics ──────────────────────────────────────────────

HWND LayoutWindow::g_hwnd = nullptr;
HWND LayoutWindow::g_tree = nullptr;
HWND LayoutWindow::g_list = nullptr;
HWND LayoutWindow::g_search_edit = nullptr;
HWND LayoutWindow::g_search_btn = nullptr;
HWND LayoutWindow::g_refresh_btn = nullptr;
HWND LayoutWindow::g_expand_btn = nullptr;
HWND LayoutWindow::g_status_label = nullptr;
HINSTANCE LayoutWindow::g_inst = nullptr;
HWND LayoutWindow::g_parent = nullptr;
LayoutNode LayoutWindow::g_root;
LayoutWindow::OnNodeSelected LayoutWindow::g_on_select;
LayoutWindow::OnRefresh LayoutWindow::g_on_refresh;
std::string LayoutWindow::g_json;
int LayoutWindow::g_layout_w = 420;
int LayoutWindow::g_hl_left = 0, LayoutWindow::g_hl_top = 0;
int LayoutWindow::g_hl_right = 0, LayoutWindow::g_hl_bottom = 0;
bool LayoutWindow::g_has_hl = false;

static const wchar_t *LAYOUT_CLASS_W = L"HScrcpyLayoutClass";

// ── Public API ───────────────────────────────────────────

void LayoutWindow::create(HINSTANCE inst, HWND parent, int width) {
    g_inst = inst;
    g_parent = parent;
    g_layout_w = width;

    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX icex = {sizeof(icex), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&icex);

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = LAYOUT_CLASS_W;
        RegisterClassExW(&wc);
        registered = true;
    }
}

void LayoutWindow::show(OnNodeSelected on_select, OnRefresh on_refresh) {
    g_on_select = on_select;
    g_on_refresh = on_refresh;

    if (!g_hwnd) {
        g_hwnd = CreateWindowExW(0, LAYOUT_CLASS_W, L"",
                                  WS_CHILD | WS_BORDER | WS_CLIPCHILDREN,
                                  0, 0, g_layout_w, 100, g_parent, NULL, g_inst, NULL);
    }

    ShowWindow(g_hwnd, SW_SHOW);
    refresh_layout();
}

void LayoutWindow::hide() {
    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_HIDE);
        g_has_hl = false;
        if (g_on_select) g_on_select(0, 0, 0, 0);
    }
}

void LayoutWindow::destroy() {
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
}

bool LayoutWindow::is_visible() {
    return g_hwnd && IsWindowVisible(g_hwnd);
}

HWND LayoutWindow::hwnd() { return g_hwnd; }
int LayoutWindow::panel_width() { return g_layout_w; }
void LayoutWindow::set_panel_width(int w) { g_layout_w = w; }

void LayoutWindow::reposition(int x, int y, int w, int h) {
    if (g_hwnd) MoveWindow(g_hwnd, x, y, w, h, TRUE);
}

bool LayoutWindow::get_highlight_bounds(int &left, int &top, int &right, int &bottom) {
    if (!g_has_hl) return false;
    left = g_hl_left; top = g_hl_top;
    right = g_hl_right; bottom = g_hl_bottom;
    return true;
}

void LayoutWindow::clear_highlight() { g_has_hl = false; }

// ── Data model ───────────────────────────────────────────

LayoutNode LayoutWindow::parse_json_to_node(const nlohmann::json &j) {
    LayoutNode node;

    if (j.contains("attributes") && j["attributes"].is_object()) {
        for (auto it = j["attributes"].begin(); it != j["attributes"].end(); ++it) {
            if (it.value().is_string()) {
                node.attrs[it.key()] = it.value().get<std::string>();
            }
        }
    }

    // Parse bounds coordinates for highlight
    parse_bounds(node.attr("bounds"), node.bounds_left, node.bounds_top, node.bounds_right, node.bounds_bottom);
    parse_bounds(node.attr("origBounds"), node.orig_left, node.orig_top, node.orig_right, node.orig_bottom);

    if (node.attr("type").empty()) node.attrs["type"] = "Root";

    if (j.contains("children") && j["children"].is_array()) {
        for (auto &child : j["children"]) {
            node.children.push_back(parse_json_to_node(child));
        }
    }

    return node;
}

// ── Tree building ────────────────────────────────────────

void LayoutWindow::build_tree(HTREEITEM parent, LayoutNode &node) {
    TVINSERTSTRUCTW ins = {};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    ins.item.cChildren = node.children.empty() ? 0 : 1;

    const auto &type = node.attr("type");
    const auto &text = node.attr("text");
    std::wstring wlabel = text.empty() ? utf8_to_wide(type)
                                       : utf8_to_wide(type) + L" \"" + utf8_to_wide(text) + L"\"";
    ins.item.pszText = const_cast<LPWSTR>(wlabel.c_str());
    ins.item.lParam = reinterpret_cast<LPARAM>(&node);

    HTREEITEM item = (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
    node.tree_item = item;

    for (auto &child : node.children) {
        build_tree(item, child);
    }
}

LayoutNode *LayoutWindow::find_node_by_tree_item(HTREEITEM item, LayoutNode *root) {
    if (root->tree_item == item) return root;
    for (auto &child : root->children) {
        LayoutNode *found = find_node_by_tree_item(item, &child);
        if (found) return found;
    }
    return nullptr;
}

// ── Node selection ───────────────────────────────────────

void LayoutWindow::on_tree_select() {
    HTREEITEM sel = TreeView_GetSelection(g_tree);
    if (!sel) return;

    LayoutNode *node = find_node_by_tree_item(sel, &g_root);
    if (!node) return;

    show_node_props(node);

    int left = node->bounds_left, top = node->bounds_top;
    int right = node->bounds_right, bottom = node->bounds_bottom;

    if (g_on_select && right > left) {
        g_hl_left = left; g_hl_top = top;
        g_hl_right = right; g_hl_bottom = bottom;
        g_has_hl = true;
        g_on_select(left, top, right, bottom);
    } else {
        g_has_hl = false;
        if (g_on_select) g_on_select(0, 0, 0, 0);
    }
}

// ── Property display ─────────────────────────────────────

void LayoutWindow::show_node_props(const LayoutNode *node) {
    ListView_DeleteAllItems(g_list);

    auto add_prop = [&](const wchar_t *name, const std::string &val) {
        if (val.empty()) return;
        std::wstring wval = utf8_to_wide(val);
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = ListView_GetItemCount(g_list);
        lvi.pszText = const_cast<LPWSTR>(name);
        int idx = (int)SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
        LVITEMW lvi2 = {};
        lvi2.iSubItem = 1;
        lvi2.pszText = const_cast<LPWSTR>(wval.c_str());
        SendMessageW(g_list, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi2);
    };

    // Fixed-order priority attributes
    static const char *priority[] = {
        "type", "text", "id", "key", "description", "bounds", "origBounds", nullptr
    };
    for (int i = 0; priority[i]; i++) {
        add_prop(utf8_to_wide(priority[i]).c_str(), node->attr(priority[i]));
    }

    // Computed: center
    if (node->bounds_right > node->bounds_left) {
        char pos[64];
        snprintf(pos, sizeof(pos), "%d, %d",
                 (node->bounds_left + node->bounds_right) / 2,
                 (node->bounds_top + node->bounds_bottom) / 2);
        add_prop(L"center", std::string(pos));
    }

    // Computed: children count
    add_prop(L"children", std::to_string(node->children.size()));

    // Remaining attributes not shown above
    for (auto &kv : node->attrs) {
        bool is_priority = false;
        for (int i = 0; priority[i]; i++) {
            if (kv.first == priority[i]) { is_priority = true; break; }
        }
        if (!is_priority) {
            add_prop(utf8_to_wide(kv.first).c_str(), kv.second);
        }
    }
}

// ── Clipboard helpers ────────────────────────────────────

void LayoutWindow::copy_to_clipboard(HWND hwnd, const std::wstring &text) {
    if (text.empty()) return;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        memcpy(GlobalLock(h), text.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

int LayoutWindow::get_selected_list_item() {
    return ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
}

void LayoutWindow::copy_list_value(HWND hwnd, int item_idx) {
    if (item_idx < 0) return;
    wchar_t wval[512] = {};
    LVITEMW lvi = {};
    lvi.iSubItem = 1;
    lvi.pszText = wval;
    lvi.cchTextMax = 512;
    SendMessageW(g_list, LVM_GETITEMTEXTW, item_idx, (LPARAM)&lvi);
    copy_to_clipboard(hwnd, wval);
}

void LayoutWindow::copy_list_property(HWND hwnd, int item_idx) {
    if (item_idx < 0) return;
    wchar_t wname[256] = {}, wval[512] = {};
    LVITEMW lvi = {};
    lvi.iSubItem = 0; lvi.pszText = wname; lvi.cchTextMax = 256;
    SendMessageW(g_list, LVM_GETITEMTEXTW, item_idx, (LPARAM)&lvi);
    lvi.iSubItem = 1; lvi.pszText = wval; lvi.cchTextMax = 512;
    SendMessageW(g_list, LVM_GETITEMTEXTW, item_idx, (LPARAM)&lvi);
    copy_to_clipboard(hwnd, std::wstring(wname) + L": " + wval);
}

// ── Refresh / search ─────────────────────────────────────

void LayoutWindow::refresh_layout() {
    if (!g_on_refresh) return;
    g_has_hl = false;
    if (g_on_select) g_on_select(0, 0, 0, 0);
    if (g_status_label) SetWindowTextW(g_status_label, L"Loading...");
    DWORD tid;
    CreateThread(NULL, 0, DumpThreadProc, NULL, 0, &tid);
}

DWORD WINAPI LayoutWindow::DumpThreadProc(LPVOID) {
    if (!g_on_refresh) return 0;
    std::string json_str = g_on_refresh();
    if (json_str.empty()) {
        PostMessageW(g_hwnd, WM_USER + 100, 0, 1);
        return 0;
    }
    g_json = json_str;
    PostMessageW(g_hwnd, WM_USER + 100, 0, 0);
    return 0;
}

static void flatten_nodes(LayoutNode &root, std::vector<LayoutNode*> &out) {
    out.push_back(&root);
    for (auto &child : root.children) flatten_nodes(child, out);
}

static bool node_matches(const LayoutNode &node, const std::string &query) {
    for (auto &kv : node.attrs) {
        if (kv.first.find(query) != std::string::npos ||
            kv.second.find(query) != std::string::npos)
            return true;
    }
    return false;
}

void LayoutWindow::search_tree() {
    wchar_t wbuf[256];
    GetWindowTextW(g_search_edit, wbuf, 256);
    std::string query = wide_to_utf8(wbuf);
    if (query.empty()) return;

    std::vector<LayoutNode*> all;
    flatten_nodes(g_root, all);
    if (all.empty()) return;

    // Find start index from current selection
    HTREEITEM sel = TreeView_GetSelection(g_tree);
    LayoutNode *start_node = sel ? find_node_by_tree_item(sel, &g_root) : nullptr;
    int start_idx = 0;
    if (start_node) {
        for (int i = 0; i < (int)all.size(); i++) {
            if (all[i] == start_node) { start_idx = i; break; }
        }
    }

    // Wrap-around search
    for (int i = 1; i <= (int)all.size(); i++) {
        int idx = (start_idx + i) % (int)all.size();
        if (node_matches(*all[idx], query)) {
            if (all[idx]->tree_item) {
                TreeView_SelectItem(g_tree, all[idx]->tree_item);
                TreeView_EnsureVisible(g_tree, all[idx]->tree_item);
            }
            break;
        }
    }
}

// ── WndProc ──────────────────────────────────────────────

// Owner-draw text button: white face + 1px gray border + black text.
static void draw_od_text_button(DRAWITEMSTRUCT *dis) {
    RECT r = dis->rcItem;
    FillRect(dis->hDC, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
    HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
    HBRUSH oldBr = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    if (dis->itemState & ODS_SELECTED) InflateRect(&r, -1, -1);
    Rectangle(dis->hDC, r.left, r.top, r.right, r.bottom);
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBr);
    DeleteObject(pen);

    wchar_t buf[64] = {};
    GetWindowTextW(dis->hwndItem, buf, 64);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, (dis->itemState & ODS_DISABLED) ? RGB(160, 160, 160) : RGB(0, 0, 0));
    HFONT f = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
    HFONT oldF = f ? (HFONT)SelectObject(dis->hDC, f) : nullptr;
    RECT tr = dis->rcItem;
    if (dis->itemState & ODS_SELECTED) { tr.left++; tr.top++; }
    DrawTextW(dis->hDC, buf, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldF) SelectObject(dis->hDC, oldF);

    if (dis->itemState & ODS_FOCUS) {
        RECT fr = dis->rcItem;
        InflateRect(&fr, -3, -3);
        DrawFocusRect(dis->hDC, &fr);
    }
}

LRESULT CALLBACK LayoutWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_status_label = CreateWindowW(L"STATIC", L"UI Layout",
                                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        MARGIN, MARGIN, 200, 18, hwnd, NULL, g_inst, NULL);

        g_search_edit = CreateWindowExW(0, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                         MARGIN, 26, 200, 22, hwnd, (HMENU)10, g_inst, NULL);

        g_search_btn = CreateWindowW(L"BUTTON", L"Search",
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      210, 26, 60, 22, hwnd, (HMENU)11, g_inst, NULL);

        g_refresh_btn = CreateWindowW(L"BUTTON", L"Refresh",
                                       WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                       278, 26, 60, 22, hwnd, (HMENU)12, g_inst, NULL);

        g_expand_btn = CreateWindowW(L"BUTTON", L"Expand",
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      346, 26, 60, 22, hwnd, (HMENU)13, g_inst, NULL);

        g_tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT,
                                  MARGIN, TOOLBAR_H, 100, 100, hwnd, (HMENU)1, g_inst, NULL);
        TreeView_SetBkColor(g_tree, RGB(255, 255, 255));

        g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
                                  MARGIN, 160, 100, 100, hwnd, (HMENU)2, g_inst, NULL);
        SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                     LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 90;
        col.pszText = const_cast<LPWSTR>(L"Property");
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.cx = 300;
        col.pszText = const_cast<LPWSTR>(L"Value");
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

        HFONT hf = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SetPropW(hwnd, L"LayoutFont", hf);
        SendMessageW(g_tree, WM_SETFONT, (WPARAM)hf, TRUE);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)hf, TRUE);
        SendMessageW(g_search_edit, WM_SETFONT, (WPARAM)hf, TRUE);
        SendMessageW(g_status_label, WM_SETFONT, (WPARAM)hf, TRUE);
        break;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        int content_w = w - MARGIN * 2;
        int tree_h = (h - TOOLBAR_H) * 3 / 5;
        int list_h = h - TOOLBAR_H - tree_h - MARGIN;
        if (list_h < 20) list_h = 20;

        MoveWindow(g_tree, MARGIN, TOOLBAR_H, content_w, tree_h, FALSE);
        MoveWindow(g_list, MARGIN, TOOLBAR_H + tree_h + MARGIN, content_w, list_h, FALSE);
        if (g_list && content_w > 0) {
            SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 0, content_w / 4);
            SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 1, (LPARAM)-2);
        }
        RedrawWindow(hwnd, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        break;
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
        if (dis->CtlType == ODT_BUTTON) {
            draw_od_text_button(dis);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR *nm = reinterpret_cast<NMHDR *>(lp);
        if (nm->idFrom == 1 && nm->code == TVN_SELCHANGEDW) {
            on_tree_select();
            // Update Expand/Collapse button text based on selected node's state
            HTREEITEM sel = TreeView_GetSelection(g_tree);
            if (sel && g_expand_btn) {
                UINT state = TreeView_GetItemState(g_tree, sel, TVIS_EXPANDED);
                bool has_children = TreeView_GetChild(g_tree, sel) != nullptr;
                if (has_children)
                    SetWindowTextW(g_expand_btn, (state & TVIS_EXPANDED) ? L"Collapse" : L"Expand");
            }
        } else if (nm->idFrom == 2) {
            if (nm->code == LVN_KEYDOWN) {
                auto *lvkd = reinterpret_cast<NMLVKEYDOWN *>(lp);
                if (lvkd->wVKey == 'C' && (GetKeyState(VK_CONTROL) & 0x8000))
                    copy_list_value(hwnd, get_selected_list_item());
            } else if (nm->code == NM_RCLICK) {
                int sel = get_selected_list_item();
                if (sel >= 0) {
                    POINT pt;
                    GetCursorPos(&pt);
                    HMENU hMenu = CreatePopupMenu();
                    AppendMenuW(hMenu, MF_STRING, 22, L"Copy Property");
                    AppendMenuW(hMenu, MF_STRING, 21, L"Copy Value");
                    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                    DestroyMenu(hMenu);
                }
            } else if (nm->code == NM_DBLCLK) {
                auto *nmitem = reinterpret_cast<NMITEMACTIVATE *>(lp);
                if (nmitem && nmitem->iItem >= 0)
                    copy_list_value(hwnd, nmitem->iItem);
            }
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 11: search_tree(); break;
        case 12: refresh_layout(); break;
        case 21: copy_list_value(hwnd, get_selected_list_item()); break;
        case 22: copy_list_property(hwnd, get_selected_list_item()); break;
        case 13: {
            // Toggle expand/collapse on the currently selected node (or root if none selected)
            HTREEITEM target = TreeView_GetSelection(g_tree);
            if (!target) target = TreeView_GetRoot(g_tree);
            if (target) {
                UINT state = TreeView_GetItemState(g_tree, target, TVIS_EXPANDED);
                bool is_expanded = (state & TVIS_EXPANDED) != 0;
                TreeView_Expand(g_tree, target, is_expanded ? TVE_COLLAPSE : TVE_EXPAND);
                SetWindowTextW(g_expand_btn, is_expanded ? L"Expand" : L"Collapse");
            }
            break;
        }
        }
        break;
    case WM_USER + 100:
        if (wp == 0 && lp == 0) {
            try {
                g_root = parse_json_to_node(json::parse(g_json));
            } catch (...) {
                if (g_status_label) SetWindowTextW(g_status_label, L"Parse error");
                return 0;
            }
            TreeView_DeleteAllItems(g_tree);
            ListView_DeleteAllItems(g_list);
            g_has_hl = false;
            if (!g_root.attr("type").empty()) {
                build_tree(TVI_ROOT, g_root);
                HTREEITEM root = TreeView_GetRoot(g_tree);
                if (root) TreeView_Expand(g_tree, root, TVE_EXPAND);
                if (g_expand_btn) SetWindowTextW(g_expand_btn, L"Collapse");
            }
            if (g_status_label) SetWindowTextW(g_status_label, L"UI Layout");
        } else {
            if (g_status_label) SetWindowTextW(g_status_label, L"Failed to load layout");
        }
        break;
    case WM_DESTROY: {
        HFONT hf = (HFONT)GetPropW(hwnd, L"LayoutFont");
        if (hf) { DeleteObject(hf); RemovePropW(hwnd, L"LayoutFont"); }
        g_hwnd = nullptr;
        g_tree = nullptr;
        g_list = nullptr;
        g_status_label = nullptr;
        g_has_hl = false;
        if (g_on_select) g_on_select(0, 0, 0, 0);
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace hscrcpy
