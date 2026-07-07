#ifndef HSCRCPY_FILE_MANAGER_WINDOW_H
#define HSCRCPY_FILE_MANAGER_WINDOW_H

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <functional>

namespace hscrcpy {

struct FileEntry {
    std::string perms;       // e.g. "drwxr-xr-x"
    int hard_links = 0;
    std::string owner;       // e.g. "root"
    std::string group;       // e.g. "root"
    long long size = 0;      // bytes
    std::string date;        // e.g. "2024-01-15 10:30"
    std::string name;        // filename
    std::string link_target; // symlink target (empty if not symlink)

    bool is_dir() const { return !perms.empty() && perms[0] == 'd'; }
    bool is_link() const { return !perms.empty() && perms[0] == 'l'; }
    bool is_file() const { return !perms.empty() && perms[0] == '-'; }
    bool can_read() const { return perms.size() >= 2 && perms[1] == 'r'; }
    bool can_write() const { return perms.size() >= 3 && perms[2] == 'w'; }
    bool can_execute() const { return perms.size() >= 4 && perms[3] == 'x'; }
};

// Clipboard action types
enum class FileClipAction { None, Copy, Move };

struct FileClipboard {
    std::vector<FileEntry> entries;
    std::string source_dir;
    std::string dest_dir;
    FileClipAction action = FileClipAction::None;
};

class FileManagerWindow {
public:
    using OnShellCommand = std::function<bool(const std::string &cmd, std::string *output, int timeout_sec)>;
    using OnPushFile = std::function<bool(const std::string &local, const std::string &remote, std::string *output)>;
    using OnPullFile = std::function<bool(const std::string &remote, const std::string &local, std::string *output)>;
    using OnBusy = std::function<void(bool busy)>;

    static void create(HINSTANCE inst, HWND parent, int width = 420);
    static void show(OnShellCommand on_shell, OnPushFile on_push, OnPullFile on_pull, OnBusy on_busy);
    static void hide();
    static void destroy();
    static bool is_visible();
    static HWND hwnd();
    static int panel_width();
    static void set_panel_width(int w);
    static void reposition(int x, int y, int w, int h);
    static void show_toast(const char *text, bool success = true, HWND parent = nullptr);

private:
    static constexpr int TOOLBAR_H = 58;
    static constexpr int MARGIN = 4;
    static constexpr int BTN_SIZE = 28;

    // Control IDs
    enum CtrlId : int {
        IDC_ADDRESS = 2001,
        IDC_GO_BTN,
        IDC_UP_BTN,
        IDC_BACK_BTN,
        IDC_SEARCH_EDIT,
        IDC_SEARCH_BTN,
        IDC_IMPORT_BTN,
        IDC_EXPORT_BTN,
        IDC_NEW_BTN,
        IDC_DELETE_BTN,
        IDC_COPY_BTN,
        IDC_PASTE_BTN,
        IDC_RENAME_BTN,
        IDC_SELECTALL_BTN,
        IDC_FILE_LIST,
        IDC_STATUS_LABEL,
        // Context menu commands
        IDM_OPEN = 2101,
        IDM_PUSH,
        IDM_PUSH_FOLDER,
        IDM_OPEN_CONTAINING_FOLDER,
        IDM_PULL,
        IDM_COPY,
        IDM_PASTE,
        IDM_MOVE_HERE,
        IDM_RENAME,
        IDM_DELETE,
        IDM_NEWFOLDER,
        IDM_NEWFILE,
        IDM_REFRESH,
        IDM_PROPERTIES,
        IDM_SELECTALL,
        IDM_DESELECTALL,
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static DWORD WINAPI LsThreadProc(LPVOID param);
    static DWORD WINAPI ActionThreadProc(LPVOID param);

    static void navigate(const std::string &path);
    static void go_back();
    static void enter_selected();
    static void refresh_list();
    static void search_files();
    static void import_files();
    static void import_folder();
    static void export_files();
    static void new_item();
    static void delete_selected();
    static void copy_selected();
    static void paste_clipboard();
    static void rename_selected();
    static void enter_select_mode();
    static void exit_select_mode();
    static void toggle_select_all();
    static std::vector<int> get_selected_indices();
    static void show_context_menu(POINT pt);
    static void show_properties();
    static void update_status();
    static void msg_box(const char *title, const char *fmt, ...);
    static bool confirm(const char *title, const char *fmt, ...);
    static std::string build_remote_path(const std::string &dir, const std::string &name);
    static bool parse_ls_line(const std::string &line, FileEntry &entry);
    static void populate_list();
    static bool show_input_dialog(const wchar_t *title, const wchar_t *prompt, std::wstring &out_value);
    static LRESULT CALLBACK ToastWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    static HWND g_hwnd;
    static HWND g_address_edit;
    static HWND g_folder_icon;
    static HWND g_go_btn;
    static HWND g_back_btn;
    static HWND g_search_edit;
    static HWND g_search_btn;
    static HWND g_import_btn;
    static HWND g_export_btn;
    static HWND g_new_btn;
    static HWND g_delete_btn;
    static HWND g_copy_btn;
    static HWND g_paste_btn;
    static HWND g_rename_btn;
    static HWND g_selectall_btn;
    static HWND g_file_list;
    static HWND g_status_label;
    static HINSTANCE g_inst;
    static HWND g_parent;
    static int g_panel_w;

    static std::string g_current_dir;
    static std::vector<FileEntry> g_entries;
    static std::vector<std::string> g_history;  // navigation history
    static FileClipboard g_clipboard;
    static std::string g_select_after_refresh;
    static bool g_check_mode;

    static OnShellCommand g_on_shell;
    static OnPushFile g_on_push;
    static OnPullFile g_on_pull;
    static OnBusy g_on_busy;
    static bool g_busy;

    // Icon handles
    static HIMAGELIST g_img_list;
    static HWND g_toast_hwnd;
};

// Get user's Downloads directory (with trailing backslash). Falls back to Desktop.
// Defined in file_manager_window.cpp; shared by main.cpp for screenshot/recording saves.
std::string get_download_dir();

} // namespace hscrcpy

#endif
