// AotRAccess: accessibility mod for Age of the Ring (BFME2 RotWK, SAGE, 32-bit).
// Loaded by Ultimate ASI Loader (winmm.dll proxy) from rotwk\scripts\AotRAccess.asi.
// DllMain only starts a worker thread; everything else runs there.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstdio>
#include "commands.h"
#include "game.h"
#include "gamethread.h"
#include "hints.h"
#include <shellapi.h>
#include "log.h"
#include "menu.h"
#include "nocatch.h"
#include "objects_db.h"
#include "savefix.h"
#include "scanner.h"
#include "screens.h"
#include "screentext.h"
#include "speech.h"
#include "tactics.h"
#include "training.h"
#include "waves.h"
#include "wotr.h"
#include "wotrsetup.h"
#include "spells.h"
#include "strings.h"

#pragma comment(lib, "version.lib")
#pragma comment(lib, "shell32.lib")

static std::wstring g_modDir;  // rotwk\scripts\AotRAccess (prism.dll lives here)

static std::wstring ModulePath(HMODULE m) {
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(m, p, MAX_PATH);
    return p;
}

static std::wstring FileVersion(const std::wstring& path) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return L"none";
    std::string buf(size, '\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, size, &buf[0])) return L"none";
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(&buf[0], L"\\", (void**)&fi, &len) || !fi) return L"none";
    wchar_t v[64];
    swprintf_s(v, L"%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
               HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    return v;
}

static std::wstring LoadedModuleInfo(const wchar_t* name) {
    HMODULE m = GetModuleHandleW(name);
    if (!m) return std::wstring(name) + L"=not loaded";
    std::wstring p = ModulePath(m);
    return std::wstring(name) + L"=" + p + L" (" + FileVersion(p) + L")";
}

// The folder after "-mod" on the command line (the AotR folder), else the game folder.
static std::wstring ModFolderFromCommandLine(const std::wstring& fallback) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring result = fallback;
    for (int i = 0; argv && i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], L"-mod") == 0) result = argv[i + 1];
    if (argv) LocalFree(argv);
    while (!result.empty() && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result;
}

static BOOL CALLBACK FindVisibleWindow(HWND h, LPARAM found) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
        *(HWND*)found = h;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI Worker(LPVOID) {
    std::wstring exe = ModulePath(nullptr);
    LogLine(L"AotRAccess loaded | bits=" + std::to_wstring(sizeof(void*) * 8) +
            L" | build=" + std::wstring(_CRT_WIDE(__DATE__) L" " _CRT_WIDE(__TIME__)) +
            L" | pid=" + std::to_wstring(GetCurrentProcessId()) +
            L" | exe=" + exe + L" (" + FileVersion(exe) + L")");
    LogLine(L"cmdline: " + std::wstring(GetCommandLineW()));
    LogLine(L"loader: " + LoadedModuleInfo(L"winmm.dll") + L" | " + LoadedModuleInfo(L"dinput8.dll"));

    SpeechStart(g_modDir);
    Speak(L"AotR Access загружен.", true);

    // Readiness signal for later stages: the game's first visible window.
    HWND wnd = nullptr;
    for (int i = 0; i < 1200 && !wnd; ++i) {  // up to 2 minutes
        EnumWindows(FindVisibleWindow, (LPARAM)&wnd);
        if (!wnd) Sleep(100);
    }
    LogLine(wnd ? L"window visible | " + LoadedModuleInfo(L"d3d9.dll")
                : L"no visible window after 2 minutes");

    if (!game::VersionGate()) {
        LogLine(L"version gate: unknown game.dat build, menu reading stays off");
        Speak(L"Неизвестная версия игры, чтение меню выключено.", false);
        return 0;
    }
    LogLine(L"version gate: game.dat 2.01 code matches");
    ScreensInit(g_modDir);
    std::wstring gameFolder = exe.substr(0, exe.find_last_of(L'\\'));
    std::wstring modFolder = ModFolderFromCommandLine(gameFolder);
    StringsLoad(modFolder, gameFolder);
    ObjectsDbLoad(modFolder);
    SpellsLoad(modFolder);
    MenuInit(wnd);
    ScannerInit(wnd, g_modDir);
    WavesInit(g_modDir);
    HintsInit(g_modDir);
    NoCatchInit(g_modDir);
    WotrInit(modFolder);
    WotrSetupInit(modFolder);
    GameThreadInit(wnd);
    RunOnGameThread(HintsInstallHook);  // on the game thread: executeAction cannot be mid-run
    RunOnGameThread(NoCatchInstallHook);
    RunOnGameThread(SaveFixInstallHook);

    // Mod loop: screen and menu watchers every 100 ms, keys checked every 20 ms.
    bool dumpHeld = false;
    for (int tick = 0;; ++tick) {
        Sleep(20);
        if (tick % 5 == 0) { ScreensTick(); MenuTick(); ScannerTick(); TacticsTick(); }
        if (tick % 25 == 0) ScreenTextTick();
        bool focused = false;
        if (HWND fg = GetForegroundWindow()) {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            focused = pid == GetCurrentProcessId();
        }
        WotrKeys(focused);
        ScreenTextKeys(focused);
        if (!WotrActive()) MenuKeys(focused);
        TrainingKeys(focused);
        CommandsKeys(focused);
        ScannerKeys(focused);
        ScannerClickKeys(focused);
        TacticsKeys(focused);
        CameraKeys(focused);
        ScannerTestKeys(focused);
        HintsKeys(focused);
        bool dumpKey = focused && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                       (GetAsyncKeyState(VK_F12) & 0x8000);
        if (dumpKey && !dumpHeld) ScreensDump();
        dumpHeld = dumpKey;
    }
}

// Ultimate ASI Loader calls this after LoadLibrary, outside the loader lock.
extern "C" __declspec(dllexport) void InitializeASI() {}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        std::wstring self = ModulePath(module);           // ...\scripts\AotRAccess.asi
        std::wstring base = self.substr(0, self.rfind(L'.'));
        LogInit(base + L".log");
        g_modDir = base;                                    // ...\scripts\AotRAccess
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
