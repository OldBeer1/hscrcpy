#ifndef HSCRCPY_LAYOUT_WINDOW_H
#define HSCRCPY_LAYOUT_WINDOW_H

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace hscrcpy {

struct LayoutNode {
    std::map<std::string, std::string> attrs;  // all attributes from JSON
    int bounds_left = 0, bounds_top = 0, bounds_right = 0, bounds_bottom = 0;
    int orig_left = 0, orig_top = 0, orig_right = 0, orig_bottom = 0;
    std::vector<LayoutNode> children;
    HTREEITEM tree_item = nullptr;

    const std::string &attr(const char *key) const {
        static const std::string empty;
        auto it = attrs.find(key);
        return it != attrs.end() ? it->second : empty;
    }
};

class LayoutWindow {
public:
    using OnNodeSelected = std::function<void(int left, int top, int right, int bottom)>;
    using OnRefresh = std::function<std::string()>;

    static void create(HINSTANCE inst, HWND parent, int width = 420);
    static void show(OnNodeSelected on_select, OnRefresh on_refresh);
    static void hide();
    static void reset();
    static void destroy();
    static bool is_visible();
    static HWND hwnd();
    static int panel_width();
    static void set_panel_width(int w);
    static void reposition(int x, int y, int w, int h);
    static bool get_highlight_bounds(int &left, int &top, int &right, int &bottom);
    static void clear_highlight();

private:
    static constexpr int TOOLBAR_H = 52;
    static constexpr int MARGIN = 4;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LayoutNode parse_json_to_node(const nlohmann::json &j);
    static void build_tree(HTREEITEM parent, LayoutNode &node);
    static void on_tree_select();
    static void show_node_props(const LayoutNode *node);
    static LayoutNode *find_node_by_tree_item(HTREEITEM item, LayoutNode *root);
    static void refresh_layout();
    static void search_tree();
    static void copy_to_clipboard(HWND hwnd, const std::wstring &text);
    static void copy_list_value(HWND hwnd, int item_idx);
    static void copy_list_property(HWND hwnd, int item_idx);
    static int get_selected_list_item();
    static DWORD WINAPI DumpThreadProc(LPVOID param);

    static HWND g_hwnd;
    static HWND g_tree;
    static HWND g_list;
    static HWND g_search_edit;
    static HWND g_search_btn;
    static HWND g_refresh_btn;
    static HWND g_expand_btn;
    static HWND g_status_label;
    static HINSTANCE g_inst;
    static HWND g_parent;
    static LayoutNode g_root;
    static OnNodeSelected g_on_select;
    static OnRefresh g_on_refresh;
    static std::string g_json;
    static int g_layout_w;

    static int g_hl_left, g_hl_top, g_hl_right, g_hl_bottom;
    static bool g_has_hl;
};

} // namespace hscrcpy

#endif
