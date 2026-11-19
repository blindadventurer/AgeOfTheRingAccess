#pragma once
#include <string>
#include <vector>

// Mod log next to the .asi (AotRAccess.log). Each line is appended, flushed and closed.
void LogInit(const std::wstring& logPath);
void LogLine(const std::wstring& text);
// Several lines in one write, without flushing (bulk listings).
void LogLines(const std::vector<std::wstring>& lines);
std::string Utf8(const std::wstring& w);
std::wstring Widen(const char* utf8);
