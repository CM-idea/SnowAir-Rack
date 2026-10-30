#pragma once

#include "Common.h"

namespace util {

std::wstring Format(const wchar_t* format, ...);

HMODULE GetDllModule();
std::wstring GetDllDir();
std::wstring GetDataDir();
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);

bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
bool EnsureDir(const std::wstring& path);
bool IsWritableDir(const std::wstring& path);
bool CopyFileTo(const std::wstring& from, const std::wstring& to);
bool DeleteFileIfExists(const std::wstring& path);
bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>& out);
bool WriteFileBytes(const std::wstring& path, const void* data, size_t size);

uint64_t FileWriteTime(const std::wstring& path);
std::wstring TimestampName();
std::wstring FullPath(const std::wstring& path);
bool SamePath(const std::wstring& a, const std::wstring& b);

bool OpenInShell(const std::wstring& path);
bool ShowInExplorer(const std::wstring& path);
bool OpenInPaint(const std::wstring& path);

std::string WideToUtf8(const std::wstring& text);
std::wstring Utf8ToWide(const std::string& text);

UINT DpiForMonitor(HMONITOR monitor);
float ScaleForMonitor(HMONITOR monitor);

D2D1_COLOR_F ColorFromArgb(uint32_t argb, D2D1_COLOR_F fallback);
D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& color, float alpha);

} /* namespace util */
