#include "layout_window.h"
#include "log.h"
#include <windowsx.h>
#include <nlohmann/json.hpp>
#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace hscrcpy {

using json = nlohmann::json;

// Helper: UTF-8 string to wide string
static std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (len <= 0) return L"";
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

// Helper: wide string to UTF-8
static std::string wide_to_utf8(const std::wstring &ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    if (len <= 0) return "";
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, NULL, NULL);
    return s;
}

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
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = LAYOUT_CLASS_W;
        RegisterClassExW(&wc);
        registered = true;
    }
}

void LayoutWindow::show(const std::string &json_str, OnNodeSelected on_select, OnRefresh on_refresh) {
    g_json = json_str;
    g_on_select = on_select;
    g_on_refresh = on_refresh;

    if (!g_hwnd) {
        g_hwnd = CreateWindowExW(0, LAYOUT_CLASS_W, L"",
                                  WS_CHILD | WS_BORDER | WS_CLIPCHILDREN,
                                  0, 0, g_layout_w, 100, g_parent, NULL, g_inst, NULL);
    }

    try {
        json j = json::parse(g_json);
        g_root = parse_json_to_node(j);
    } catch (const std::exception &e) {
        LOGE("Failed to parse layout JSON: %s", e.what());
        if (g_status_label) SetWindowTextW(g_status_label, L"Parse error");
        return;
    }

    TreeView_DeleteAllItems(g_tree);
    ListView_DeleteAllItems(g_list);

    if (!g_root.type.empty()) {
        build_tree(TVI_ROOT, g_root);
    }

    if (g_status_label) SetWindowTextW(g_status_label, L"UI Layout");
    ShowWindow(g_hwnd, SW_SHOW);
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

HWND LayoutWindow::hwnd() {
    return g_hwnd;
}

int LayoutWindow::panel_width() {
    return g_layout_w;
}

void LayoutWindow::set_panel_width(int w) {
    g_layout_w = w;
}

void LayoutWindow::reposition(int x, int y, int w, int h) {
    if (g_hwnd) {
        MoveWindow(g_hwnd, x, y, w, h, TRUE);
    }
}

bool LayoutWindow::get_highlight_bounds(int &left, int &top, int &right, int &bottom) {
    if (!g_has_hl) return false;
    left = g_hl_left;
    top = g_hl_top;
    right = g_hl_right;
    bottom = g_hl_bottom;
    return true;
}

void LayoutWindow::clear_highlight() {
    g_has_hl = false;
}

LayoutNode LayoutWindow::parse_json_to_node(const nlohmann::json &j) {
    LayoutNode node;

    if (j.contains("attributes") && j["attributes"].is_object()) {
        auto &attr = j["attributes"];
        if (attr.contains("type") && attr["type"].is_string()) node.type = attr["type"].get<std::string>();
        if (attr.contains("text") && attr["text"].is_string()) node.text = attr["text"].get<std::string>();
        if (attr.contains("id") && attr["id"].is_string()) node.id = attr["id"].get<std::string>();
        if (attr.contains("key") && attr["key"].is_string()) node.key = attr["key"].get<std::string>();
        if (attr.contains("description") && attr["description"].is_string()) node.desc = attr["description"].get<std::string>();
        if (attr.contains("bounds") && attr["bounds"].is_string()) {
            node.bounds = attr["bounds"].get<std::string>();
            sscanf(node.bounds.c_str(), "[%d,%d][%d,%d]",
                   &node.bounds_left, &node.bounds_top, &node.bounds_right, &node.bounds_bottom);
        }
        if (attr.contains("origBounds") && attr["origBounds"].is_string()) {
            node.orig_bounds = attr["origBounds"].get<std::string>();
            sscanf(node.orig_bounds.c_str(), "[%d,%d][%d,%d]",
                   &node.orig_left, &node.orig_top, &node.orig_right, &node.orig_bottom);
        }
    }

    if (node.type.empty()) node.type = "Root";

    if (j.contains("children") && j["children"].is_array()) {
        for (auto &child : j["children"]) {
            node.children.push_back(parse_json_to_node(child));
        }
    }

    return node;
}

void LayoutWindow::build_tree(HTREEITEM parent, LayoutNode &node) {
    TVINSERTSTRUCTW ins = {};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    ins.item.cChildren = node.children.empty() ? 0 : 1;

    std::wstring wlabel;
    if (!node.text.empty()) {
        wlabel = utf8_to_wide(node.type) + L" \"" + utf8_to_wide(node.text) + L"\"";
    } else {
        wlabel = utf8_to_wide(node.type);
    }
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

void LayoutWindow::on_tree_select() {
    HTREEITEM sel = TreeView_GetSelection(g_tree);
    if (!sel) return;

    LayoutNode *node = find_node_by_tree_item(sel, &g_root);
    if (!node) return;

    show_node_props(node);

    // Use bounds (not origBounds) for highlight — bounds matches the visible screen area
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

void LayoutWindow::show_node_props(const LayoutNode *node) {
    ListView_DeleteAllItems(g_list);

    auto add_prop = [&](const wchar_t *name, const std::string &val) {
        std::wstring wval = utf8_to_wide(val);
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = ListView_GetItemCount(g_list);
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<LPWSTR>(name);
        int idx = (int)SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
        LVITEMW lvi2 = {};
        lvi2.iSubItem = 1;
        lvi2.pszText = const_cast<LPWSTR>(wval.c_str());
        SendMessageW(g_list, LVM_SETITEMTEXTW, idx, (LPARAM)&lvi2);
    };

    add_prop(L"type", node->type);
    add_prop(L"text", node->text);
    add_prop(L"id", node->id);
    add_prop(L"key", node->key);
    add_prop(L"description", node->desc);
    add_prop(L"bounds", node->bounds);
    if (!node->orig_bounds.empty()) add_prop(L"origBounds", node->orig_bounds);
    if (node->bounds_right > node->bounds_left) {
        char pos[64];
        snprintf(pos, sizeof(pos), "%d, %d",
                 (node->bounds_left + node->bounds_right) / 2,
                 (node->bounds_top + node->bounds_bottom) / 2);
        add_prop(L"center", std::string(pos));
    }
    add_prop(L"children", std::to_string(node->children.size()));
}

void LayoutWindow::refresh_layout() {
    if (!g_on_refresh) return;
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

// Flatten tree nodes in depth-first order for search
static void flatten_nodes(LayoutNode &root, std::vector<LayoutNode*> &out) {
    out.push_back(&root);
    for (auto &child : root.children) {
        flatten_nodes(child, out);
    }
}

void LayoutWindow::search_tree() {
    wchar_t wbuf[256];
    GetWindowTextW(g_search_edit, wbuf, 256);
    std::string query = wide_to_utf8(wbuf);
    if (query.empty()) return;

    // Flatten all nodes
    std::vector<LayoutNode*> all;
    flatten_nodes(g_root, all);
    if (all.empty()) return;

    // Find current selection index
    HTREEITEM sel = TreeView_GetSelection(g_tree);
    LayoutNode *start_node = sel ? find_node_by_tree_item(sel, &g_root) : nullptr;
    int start_idx = 0;
    if (start_node) {
        for (int i = 0; i < (int)all.size(); i++) {
            if (all[i] == start_node) { start_idx = i; break; }
        }
    }

    // Search from next node, wrapping around
    for (int i = 1; i <= (int)all.size(); i++) {
        int idx = (start_idx + i) % (int)all.size();
        LayoutNode *node = all[idx];
        if (node->type.find(query) != std::string::npos ||
            node->text.find(query) != std::string::npos ||
            node->id.find(query) != std::string::npos ||
            node->key.find(query) != std::string::npos ||
            node->desc.find(query) != std::string::npos) {
            if (node->tree_item) {
                TreeView_SelectItem(g_tree, node->tree_item);
                TreeView_EnsureVisible(g_tree, node->tree_item);
            }
            break;
        }
    }
}

LRESULT CALLBACK LayoutWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_status_label = CreateWindowW(L"STATIC", L"UI Layout",
                                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        4, 4, 200, 18, hwnd, (HMENU)20, g_inst, NULL);

        g_search_edit = CreateWindowExW(0, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                         4, 26, 200, 22, hwnd, (HMENU)10, g_inst, NULL);

        g_search_btn = CreateWindowW(L"BUTTON", L"Search",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      210, 26, 60, 22, hwnd, (HMENU)11, g_inst, NULL);

        g_refresh_btn = CreateWindowW(L"BUTTON", L"Refresh",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       278, 26, 60, 22, hwnd, (HMENU)12, g_inst, NULL);

        g_expand_btn = CreateWindowW(L"BUTTON", L"Expand",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      346, 26, 60, 22, hwnd, (HMENU)13, g_inst, NULL);

        g_tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT,
                                  4, 52, 100, 100, hwnd, (HMENU)1, g_inst, NULL);

        g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
                                  4, 160, 100, 100, hwnd, (HMENU)2, g_inst, NULL);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 90;
        col.pszText = const_cast<LPWSTR>(L"Property");
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.cx = 300;
        col.pszText = const_cast<LPWSTR>(L"Value");
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);

        HFONT hf = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
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
        int toolbar_h = 52;  // status label + search row
        int tree_h = (h - toolbar_h) * 3 / 5;
        int list_h = h - toolbar_h - tree_h - 4;
        if (list_h < 20) list_h = 20;
        MoveWindow(g_tree, 4, toolbar_h, w - 8, tree_h, TRUE);
        MoveWindow(g_list, 4, toolbar_h + tree_h + 4, w - 8, list_h, TRUE);
        break;
    }
    case WM_NOTIFY: {
        NMHDR *nm = reinterpret_cast<NMHDR *>(lp);
        if (nm->idFrom == 1 && nm->code == TVN_SELCHANGEDW) {
            on_tree_select();
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 11: search_tree(); break;
        case 12: refresh_layout(); break;
        case 13: {
            static bool expanded = false;
            if (expanded) {
                HTREEITEM root = TreeView_GetRoot(g_tree);
                while (root) {
                    TreeView_Expand(g_tree, root, TVE_COLLAPSE);
                    root = TreeView_GetNextSibling(g_tree, root);
                }
                SetWindowTextW(g_expand_btn, L"Expand");
            } else {
                HTREEITEM root = TreeView_GetRoot(g_tree);
                while (root) {
                    TreeView_Expand(g_tree, root, TVE_EXPAND);
                    root = TreeView_GetNextSibling(g_tree, root);
                }
                SetWindowTextW(g_expand_btn, L"Collapse");
            }
            expanded = !expanded;
            break;
        }
        }
        break;
    case WM_USER + 100:
        if (wp == 0 && lp == 0) {
            try {
                json j = json::parse(g_json);
                g_root = parse_json_to_node(j);
            } catch (...) {
                if (g_status_label) SetWindowTextW(g_status_label, L"Parse error");
                return 0;
            }
            TreeView_DeleteAllItems(g_tree);
            ListView_DeleteAllItems(g_list);
            g_has_hl = false;
            if (!g_root.type.empty()) build_tree(TVI_ROOT, g_root);
            if (g_status_label) SetWindowTextW(g_status_label, L"UI Layout");
        } else {
            if (g_status_label) SetWindowTextW(g_status_label, L"Failed to load layout");
        }
        break;
    case WM_DESTROY:
        g_hwnd = nullptr;
        g_tree = nullptr;
        g_list = nullptr;
        g_status_label = nullptr;
        g_has_hl = false;
        if (g_on_select) g_on_select(0, 0, 0, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace hscrcpy
