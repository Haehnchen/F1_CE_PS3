// Launcher of the recompiled game: picks the options that are environment variables (README, "Debugging aids"), keeps
// them in f1ce_launcher.ini next to the executable and starts f1ce.exe. The game runs from the folder above build-win,
// the current folder of the old launch script: log and saves (ps3_data/) are relative to it.
#include <windows.h>
#include <shlobj.h>

#include <string>
#include <vector>

namespace {
struct Choice { const wchar_t* label; const wchar_t* value; };   // value nullptr: the variable stays unset
struct Combo {
    const wchar_t* label;
    const wchar_t* env;
    std::vector<Choice> choices;
    int def;
    HWND h = nullptr;
};
struct Check {
    const wchar_t* label;
    const wchar_t* env;
    HWND h = nullptr;
};

Combo g_combos[] = {
    {L"Renderer", L"F1CE_RENDERER", {{L"OpenGL", nullptr}, {L"Vulkan", L"vulkan"}}, 0},
    {L"Internal resolution", L"F1CE_SCALE",
     {{L"1280x720 (original)", nullptr}, {L"2560x1440", L"2"}, {L"3840x2160", L"3"}, {L"5120x2880", L"4"}}, 0},
    {L"Anti-aliasing (MSAA)", L"F1CE_MSAA", {{L"2x", L"2"}, {L"4x (original)", nullptr}, {L"8x", L"8"}}, 1},
    {L"Vertical sync", L"F1CE_VSYNC", {{L"On", nullptr}, {L"Off", L"0"}, {L"Adaptive", L"-1"}}, 0},
    {L"Window size", L"F1CE_WINDOW",
     {{L"1280x720", nullptr}, {L"1600x900", L"1600x900"}, {L"1920x1080", L"1920x1080"}, {L"2560x1440", L"2560x1440"}}, 0},
};
Check g_checks[] = {
    {L"Up to 60 fps (game speed unchanged)", L"F1CE_60FPS"},
    {L"FXAA on the final image", L"F1CE_FXAA"},
    {L"Start in full screen (F11 toggles it)", L"F1CE_FULLSCREEN"},
    {L"Stretch to the window instead of keeping 16:9", L"F1CE_STRETCH"},
};

enum { ID_START = 100, ID_BROWSE, ID_QUIT, WM_GAME_EXIT = WM_APP + 1 };
HWND g_path, g_main;
HFONT g_font;
std::wstring g_exe_dir, g_run_dir, g_ini;

std::wstring parent(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(0, s);
}
std::wstring full_path(const std::wstring& p) {
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, b, nullptr);
    return n ? std::wstring(b, n) : p;
}
bool is_file(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring window_text(HWND h) {
    wchar_t b[MAX_PATH * 2];
    GetWindowTextW(h, b, MAX_PATH * 2);
    return b;
}

void load_settings() {
    wchar_t b[MAX_PATH * 2];
    std::wstring def = full_path(g_run_dir + L"\\..\\F1_CE_PS3_og\\PS3_GAME");
    GetPrivateProfileStringW(L"game", L"path", def.c_str(), b, MAX_PATH * 2, g_ini.c_str());
    SetWindowTextW(g_path, b);
    for (Combo& c : g_combos) {
        int i = GetPrivateProfileIntW(L"options", c.env, c.def, g_ini.c_str());
        if (i < 0 || i >= (int)c.choices.size()) i = c.def;
        SendMessageW(c.h, CB_SETCURSEL, i, 0);
    }
    for (Check& c : g_checks)
        SendMessageW(c.h, BM_SETCHECK, GetPrivateProfileIntW(L"options", c.env, 0, g_ini.c_str()) ? BST_CHECKED : BST_UNCHECKED, 0);
}

void save_settings() {
    WritePrivateProfileStringW(L"game", L"path", window_text(g_path).c_str(), g_ini.c_str());
    for (Combo& c : g_combos) {
        int i = (int)SendMessageW(c.h, CB_GETCURSEL, 0, 0);
        WritePrivateProfileStringW(L"options", c.env, std::to_wstring(i).c_str(), g_ini.c_str());
    }
    for (Check& c : g_checks) {
        bool on = SendMessageW(c.h, BM_GETCHECK, 0, 0) == BST_CHECKED;
        WritePrivateProfileStringW(L"options", c.env, on ? L"1" : L"0", g_ini.c_str());
    }
}

DWORD WINAPI wait_game(void* process) {
    WaitForSingleObject(process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    PostMessageW(g_main, WM_GAME_EXIT, code, 0);
    return 0;
}

void start_game() {
    std::wstring game = window_text(g_path), exe = g_exe_dir + L"\\f1ce.exe";
    if (!is_file(game + L"\\USRDIR\\EBOOT.BIN") && !is_file(game + L"\\PARAM.SFO")) {
        MessageBoxW(g_main, L"This folder does not look like the game's PS3_GAME folder.", L"F1 CE", MB_ICONWARNING);
        return;
    }
    save_settings();
    // the variable is cleared when unset here: some options act as soon as it exists, whatever its value
    for (Combo& c : g_combos)
        SetEnvironmentVariableW(c.env, c.choices[SendMessageW(c.h, CB_GETCURSEL, 0, 0)].value);
    for (Check& c : g_checks)
        SetEnvironmentVariableW(c.env, SendMessageW(c.h, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : nullptr);
    std::wstring cmd = L"\"" + exe + L"\" \"" + game + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_run_dir.c_str(), &si, &pi)) {
        std::wstring msg = L"Could not start " + exe + L" (error " + std::to_wstring(GetLastError()) + L").";
        MessageBoxW(g_main, msg.c_str(), L"F1 CE", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    ShowWindow(g_main, SW_HIDE);
    CloseHandle(CreateThread(nullptr, 0, wait_game, pi.hProcess, 0, nullptr));
}

void browse() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_main;
    bi.lpszTitle = L"Select the PS3_GAME folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi)) {
        wchar_t b[MAX_PATH];
        if (SHGetPathFromIDListW(id, b)) SetWindowTextW(g_path, b);
        CoTaskMemFree(id);
    }
}

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
    const int dpi = GetDpiForSystem();
    auto px = [&](int v) { return MulDiv(v, dpi, 96); };
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), g_main,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

constexpr int WIN_W = 470, MARGIN = 14, LABEL_W = 150, ROW = 30;

void build_controls() {
    int y = MARGIN;
    add(L"STATIC", L"Game folder (PS3_GAME)", 0, MARGIN, y, 300, 18);
    y += 20;
    g_path = add(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, MARGIN, y, WIN_W - 2 * MARGIN - 86, 24);
    add(L"BUTTON", L"Browse...", WS_TABSTOP, WIN_W - MARGIN - 80, y, 80, 24, ID_BROWSE);
    y += 38;
    for (Combo& c : g_combos) {
        add(L"STATIC", c.label, 0, MARGIN, y + 3, LABEL_W, 18);
        c.h = add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, MARGIN + LABEL_W, y, WIN_W - 2 * MARGIN - LABEL_W, 160);
        for (const Choice& ch : c.choices) SendMessageW(c.h, CB_ADDSTRING, 0, (LPARAM)ch.label);
        y += ROW;
    }
    y += 4;
    for (Check& c : g_checks) {
        c.h = add(L"BUTTON", c.label, WS_TABSTOP | BS_AUTOCHECKBOX, MARGIN, y, WIN_W - 2 * MARGIN, 22);
        y += 24;
    }
    y += 12;
    add(L"BUTTON", L"Start", WS_TABSTOP | BS_DEFPUSHBUTTON, WIN_W - 2 * MARGIN - 180, y, 90, 28, ID_START);
    add(L"BUTTON", L"Quit", WS_TABSTOP, WIN_W - MARGIN - 90, y, 90, 28, ID_QUIT);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == ID_START) start_game();
            else if (LOWORD(wp) == ID_BROWSE) browse();
            else if (LOWORD(wp) == ID_QUIT) DestroyWindow(h);
            return 0;
        case WM_GAME_EXIT:
            ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
            if (wp) {
                std::wstring msg2 = L"The game stopped with code " + std::to_wstring((unsigned)wp) + L".\nSee " + g_run_dir + L"\\f1ce_recomp.log";
                MessageBoxW(h, msg2.c_str(), L"F1 CE", MB_ICONERROR);
            }
            return 0;
        case WM_DESTROY:
            save_settings();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    wchar_t b[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    g_exe_dir = parent(b);
    g_run_dir = parent(g_exe_dir);
    g_ini = g_exe_dir + L"\\f1ce_launcher.ini";

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"F1CELauncher";
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const int dpi = GetDpiForSystem(), height = MulDiv(MARGIN + 58 + 5 * ROW + 4 + 4 * 24 + 12 + 28 + MARGIN, dpi, 96);
    RECT r{0, 0, MulDiv(WIN_W, dpi, 96), height};
    AdjustWindowRect(&r, style, FALSE);
    g_main = CreateWindowW(wc.lpszClassName, L"Formula One: Championship Edition", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    build_controls();
    load_settings();
    ShowWindow(g_main, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
