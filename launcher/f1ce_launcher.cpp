// Launcher of the recompiled game: picks the options that are environment variables (README, "Debugging aids"), keeps
// them in f1ce_launcher.ini next to the executable and starts f1ce.exe. The game runs from the folder above build-win,
// the current folder of the old launch script: log and saves (ps3_data/) are relative to it.
// Two tabs: General (display and speed) and Advanced (Quick Race settings that replace the game menu's, F1CE_QR_*).
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shlobj.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {
struct Choice { const wchar_t* label; const wchar_t* value; };   // value nullptr: the variable stays unset
struct Combo {
    const wchar_t* label;
    const wchar_t* env;
    std::vector<Choice> choices;
    int def;
    int page = 0;   // tab: 0 General, 1 Advanced
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
     {{L"720x480 (4:3, PS3 480p)", L"480p:4x3"}, {L"720x480 (16:9, PS3 480p)", L"480p"},
      {L"720x576 (4:3, PS3 576p)", L"576p:4x3"}, {L"720x576 (16:9, PS3 576p)", L"576p"}, {L"904x565 (16:10)", L"904x565"},
      {L"1280x720 (16:9, original)", nullptr}, {L"1280x800 (16:10)", L"1280x800"}, {L"1440x900 (16:10)", L"1440x900"},
      {L"1920x1080 (16:9)", L"1.5"}, {L"2560x1080 (21:9)", L"2560x1080"}, {L"2560x1440 (16:9)", L"2"},
      {L"3440x1440 (21:9)", L"3440x1440"}, {L"3840x2160 (16:9)", L"3"}, {L"5120x2880 (16:9)", L"4"}}, 5},
    {L"Anti-aliasing (MSAA)", L"F1CE_MSAA", {{L"2x", L"2"}, {L"4x (original)", nullptr}, {L"8x", L"8"}}, 1},
    {L"Reflection faces per frame", L"F1CE_CUBE_FACES", {{L"6 (original)", nullptr}, {L"3", L"3"}, {L"2 (faster)", L"2"}, {L"1 (fastest)", L"1"}}, 0},
    {L"Vertical sync", L"F1CE_VSYNC", {{L"On", nullptr}, {L"Off", L"0"}, {L"Adaptive", L"-1"}}, 0},
    {L"Window size", L"F1CE_WINDOW",
     {{L"1280x720", nullptr}, {L"1600x900", L"1600x900"}, {L"1920x1080", L"1920x1080"}, {L"2560x1440", L"2560x1440"}}, 0},
    // Advanced tab, "Quick Race Settings" (runtime/hle_quickrace.cpp): "Game menu" leaves the variable unset
    {L"Circuit", L"F1CE_QR_TRACK",
     {{L"Game menu", nullptr}, {L"Bahrain (Sakhir)", L"bahrain"}, {L"Malaysia (Sepang)", L"malaysia"},
      {L"Australia (Melbourne)", L"melbourne"}, {L"San Marino (Imola)", L"sanmarino"}, {L"Europe (Nürburgring)", L"nurburgring"},
      {L"Spain (Barcelona)", L"spain"}, {L"Monaco", L"monaco"}, {L"Great Britain (Silverstone)", L"silverstone"},
      {L"Canada (Montréal)", L"canada"}, {L"USA (Indianapolis)", L"indy"}, {L"France (Magny-Cours)", L"france"},
      {L"Germany (Hockenheim)", L"hockenheim"}, {L"Hungary (Budapest)", L"hungary"}, {L"Turkey (Istanbul)", L"istanbul"},
      {L"Italy (Monza)", L"monza"}, {L"China (Shanghai)", L"shanghai"}, {L"Japan (Suzuka)", L"suzuka"},
      {L"Brazil (Interlagos)", L"brazil"}}, 0, 1},
    {L"Driver", L"F1CE_QR_DRIVER",
     {{L"Game menu", nullptr}, {L"F. Alonso (Renault)", L"falonso"}, {L"G. Fisichella (Renault)", L"gfisichella"},
      {L"K. Räikkönen (McLaren)", L"kraikkonen"}, {L"J. Montoya (McLaren)", L"jmontoya"},
      {L"M. Schumacher (Ferrari)", L"mschumacher"}, {L"F. Massa (Ferrari)", L"fmassa"},
      {L"R. Schumacher (Toyota)", L"rschumacher"}, {L"J. Trulli (Toyota)", L"jtrulli"}, {L"M. Webber (Williams)", L"mwebber"},
      {L"N. Rosberg (Williams)", L"nrosberg"}, {L"R. Barrichello (Honda)", L"rbarrichello"}, {L"J. Button (Honda)", L"jbutton"},
      {L"D. Coulthard (Red Bull)", L"dcoulthard"}, {L"C. Klien (Red Bull)", L"cklien"},
      {L"N. Heidfeld (BMW Sauber)", L"nheidfeld"}, {L"J. Villeneuve (BMW Sauber)", L"jvilleneuve"},
      {L"T. Monteiro (MF1)", L"tmonteiro"}, {L"C. Albers (MF1)", L"calbers"}, {L"V. Liuzzi (Toro Rosso)", L"vliuzzi"},
      {L"S. Speed (Toro Rosso)", L"sspeed"}, {L"T. Sato (Super Aguri)", L"tsato"}, {L"Y. Ide (Super Aguri)", L"yide"}}, 0, 1},
    {L"Race distance", L"F1CE_QR_DISTANCE",
     {{L"Game menu", nullptr}, {L"10%", L"10"}, {L"20%", L"20"}, {L"30%", L"30"}, {L"50%", L"50"}, {L"75%", L"75"},
      {L"100%", L"100"}}, 0, 1},
    {L"Weather", L"F1CE_QR_WEATHER",
     {{L"Game menu", nullptr}, {L"Variable", L"variable"}, {L"Sunny", L"sunny"}, {L"Cloudy", L"cloudy"},
      {L"Light rain", L"lightrain"}, {L"Heavy rain", L"heavyrain"}}, 0, 1},
    {L"Opponent AI", L"F1CE_QR_AI", {{L"Game menu", nullptr}, {L"Easy", L"easy"}, {L"Medium", L"medium"}, {L"Hard", L"hard"}}, 0, 1},
    {L"Fuel use && tyre wear", L"F1CE_QR_FUEL",
     {{L"Game menu", nullptr}, {L"Off", L"off"}, {L"Scaled", L"scaled"}, {L"Realistic", L"realistic"}}, 0, 1},
    {L"Crash damage", L"F1CE_QR_DAMAGE", {{L"Game menu", nullptr}, {L"Off", L"0"}, {L"On", L"1"}}, 0, 1},
    {L"Component failure", L"F1CE_QR_FAILURES", {{L"Game menu", nullptr}, {L"Off", L"0"}, {L"On", L"1"}}, 0, 1},
    {L"Penalties", L"F1CE_QR_PENALTIES", {{L"Game menu", nullptr}, {L"Off", L"0"}, {L"On", L"1"}}, 0, 1},
};
Check g_checks[] = {
    {L"Up to 60 fps (game speed unchanged)", L"F1CE_60FPS"},
    {L"FXAA on the final image", L"F1CE_FXAA"},
    {L"Start in full screen (F11 toggles it)", L"F1CE_FULLSCREEN"},
    {L"Stretch to the window instead of keeping the aspect ratio", L"F1CE_STRETCH"},
};

enum { ID_START = 100, ID_BROWSE, ID_QUIT, ID_TAB, WM_GAME_EXIT = WM_APP + 1 };
HWND g_path, g_main, g_tab;
std::vector<HWND> g_page[2];   // the controls of each tab
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
        GetPrivateProfileStringW(L"options", c.env, L"", b, MAX_PATH * 2, g_ini.c_str());
        int sel = c.def;
        for (size_t i = 0; i < c.choices.size(); i++)
            if (c.choices[i].value && !wcscmp(c.choices[i].value, b)) sel = (int)i;
        SendMessageW(c.h, CB_SETCURSEL, sel, 0);
    }
    for (Check& c : g_checks)
        SendMessageW(c.h, BM_SETCHECK, GetPrivateProfileIntW(L"options", c.env, 0, g_ini.c_str()) ? BST_CHECKED : BST_UNCHECKED, 0);
}

void save_settings() {
    WritePrivateProfileStringW(L"game", L"path", window_text(g_path).c_str(), g_ini.c_str());
    for (Combo& c : g_combos) {
        const wchar_t* v = c.choices[SendMessageW(c.h, CB_GETCURSEL, 0, 0)].value;   // (nullptr deletes the key: default)
        WritePrivateProfileStringW(L"options", c.env, v, g_ini.c_str());
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

constexpr int TAB_X = 8, TAB_TOP = 8, TAB_HEAD = 26;

// page: the tab the control belongs to (-1: always shown). Page controls are children of the tab control, so they
// draw on its themed body; x and y are in window coordinates either way.
HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, int page = -1) {
    const int dpi = GetDpiForSystem();
    auto px = [&](int v) { return MulDiv(v, dpi, 96); };
    if (page >= 0) { x -= TAB_X; y -= TAB_TOP; }
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | (page > 0 ? 0 : WS_VISIBLE) | style, px(x), px(y), px(w), px(h),
                             page >= 0 ? g_tab : g_main, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (page >= 0) g_page[page].push_back(c);
    return c;
}

void show_page(int page) {
    for (int p = 0; p < 2; p++)
        for (HWND c : g_page[p]) ShowWindow(c, p == page ? SW_SHOW : SW_HIDE);
}

constexpr int WIN_W = 470, MARGIN = 14, LABEL_W = 150, ROW = 30;

// the tab control passes its children's notifications and colour requests on to the main window
LRESULT CALLBACK tab_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_COMMAND || msg == WM_CTLCOLORSTATIC) return SendMessageW(g_main, msg, wp, lp);
    return DefSubclassProc(h, msg, wp, lp);
}

// returns the height of the window's client area
int build_controls() {
    const int top = TAB_TOP + TAB_HEAD + 10, inner = MARGIN + 6, inner_w = WIN_W - 2 * inner;
    g_tab = add(WC_TABCONTROLW, L"", WS_CLIPCHILDREN | WS_TABSTOP, TAB_X, TAB_TOP, WIN_W - 2 * TAB_X, 100, ID_TAB);
    SetWindowLongPtrW(g_tab, GWL_EXSTYLE, GetWindowLongPtrW(g_tab, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);   // (Tab key)
    SetWindowSubclass(g_tab, tab_proc, 1, 0);
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"General";
    SendMessageW(g_tab, TCM_INSERTITEMW, 0, (LPARAM)&ti);
    ti.pszText = (LPWSTR)L"Advanced";
    SendMessageW(g_tab, TCM_INSERTITEMW, 1, (LPARAM)&ti);

    int y = top;   // General
    add(L"STATIC", L"Game folder (PS3_GAME)", 0, inner, y, 300, 18, 0, 0);
    y += 20;
    g_path = add(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, inner, y, inner_w - 86, 24, 0, 0);
    add(L"BUTTON", L"Browse...", WS_TABSTOP, inner + inner_w - 80, y, 80, 24, ID_BROWSE, 0);
    y += 38;
    int y_adv = top;   // Advanced
    HWND group = add(L"BUTTON", L"Quick Race Settings", BS_GROUPBOX, inner, y_adv, inner_w, 100, 0, 1);
    y_adv += 24;
    for (Combo& c : g_combos) {
        int& row = c.page ? y_adv : y;
        const int x = c.page ? inner + 10 : inner, w = c.page ? inner_w - 20 : inner_w;
        add(L"STATIC", c.label, 0, x, row + 3, LABEL_W, 18, 0, c.page);
        c.h = add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, x + LABEL_W, row, w - LABEL_W, 240, 0, c.page);
        for (const Choice& ch : c.choices) SendMessageW(c.h, CB_ADDSTRING, 0, (LPARAM)ch.label);
        row += ROW;
    }
    add(L"STATIC", L"Used by the next Quick Race in place of the game menu's choices (the menu still shows its own).",
        0, inner + 10, y_adv + 2, inner_w - 20, 34, 0, 1);
    y_adv += 42;
    const int dpi = GetDpiForSystem();
    SetWindowPos(group, nullptr, 0, 0, MulDiv(inner_w, dpi, 96), MulDiv(y_adv - top, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
    y += 4;
    for (Check& c : g_checks) {
        c.h = add(L"BUTTON", c.label, WS_TABSTOP | BS_AUTOCHECKBOX, inner, y, inner_w, 22, 0, 0);
        y += 24;
    }
    y = std::max(y, y_adv) + 10;
    SetWindowPos(g_tab, nullptr, 0, 0, MulDiv(WIN_W - 2 * TAB_X, dpi, 96), MulDiv(y - TAB_TOP, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
    y += 12;
    add(L"BUTTON", L"Start", WS_TABSTOP | BS_DEFPUSHBUTTON, WIN_W - 2 * MARGIN - 180, y, 90, 28, ID_START);
    add(L"BUTTON", L"Quit", WS_TABSTOP, WIN_W - MARGIN - 90, y, 90, 28, ID_QUIT);
    return y + 28 + MARGIN;
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == ID_START) start_game();
            else if (LOWORD(wp) == ID_BROWSE) browse();
            else if (LOWORD(wp) == ID_QUIT) DestroyWindow(h);
            return 0;
        case WM_CTLCOLORSTATIC:   // labels and check boxes on the tab's body: its background under the text
            DrawThemeParentBackground((HWND)lp, (HDC)wp, nullptr);
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        case WM_NOTIFY:
            if (((NMHDR*)lp)->hwndFrom == g_tab && ((NMHDR*)lp)->code == TCN_SELCHANGE)
                show_page((int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0));
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

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const int dpi = GetDpiForSystem();
    g_main = CreateWindowW(wc.lpszClassName, L"Formula One: Championship Edition", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           100, 100, nullptr, nullptr, inst, nullptr);
    RECT r{0, 0, MulDiv(WIN_W, dpi, 96), MulDiv(build_controls(), dpi, 96)};   // (the size follows the controls)
    AdjustWindowRect(&r, style, FALSE);
    SetWindowPos(g_main, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
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
