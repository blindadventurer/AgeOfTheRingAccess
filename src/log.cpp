#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <mutex>
#include "log.h"

static std::wstring g_logPath;
static std::mutex g_logMutex;

void LogInit(const std::wstring& logPath) { g_logPath = logPath; }

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    w.resize(n - 1);
    return w;
}

void LogLines(const std::vector<std::wstring>& lines) {
    if (lines.empty()) return;
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf_s(stamp, L"%04d-%02d-%02d %02d:%02d:%02d.%03d ", t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::string all;
    for (const auto& l : lines) all += Utf8(stamp + l) + "\r\n";
    std::lock_guard<std::mutex> lock(g_logMutex);
    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, all.data(), (DWORD)all.size(), &written, nullptr);
    CloseHandle(f);  // no flush: used for bulk research listings, not for crash evidence
}

// Appends one line and closes the file at once, so a crash right after keeps it.
void LogLine(const std::wstring& text) {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf_s(stamp, L"%04d-%02d-%02d %02d:%02d:%02d.%03d ", t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::string line = Utf8(stamp + text) + "\r\n";
    std::lock_guard<std::mutex> lock(g_logMutex);
    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, line.data(), (DWORD)line.size(), &written, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
}
