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
/* 文件内容指纹（FNV-1a 64，超过 32MB 只取首尾各 16MB）：
   用来判断「是不是同一个文件」，不依赖路径。 */
uint64_t FileContentHash(const std::wstring& path);
std::wstring TimestampName();
std::wstring FullPath(const std::wstring& path);
bool SamePath(const std::wstring& a, const std::wstring& b);

bool OpenInShell(const std::wstring& path);
bool ShowInExplorer(const std::wstring& path);

std::string WideToUtf8(const std::wstring& text);
std::wstring Utf8ToWide(const std::string& text);

UINT DpiForMonitor(HMONITOR monitor);
float ScaleForMonitor(HMONITOR monitor);

/* 单个文件路径的 CF_HDROP 数据块（调用方负责 GlobalFree / 交给剪贴板）。 */
HGLOBAL MakeDropFilesHandle(const std::wstring& path);
/* 多文件 CF_HDROP；paths 为空返回 nullptr。 */
HGLOBAL MakeDropFilesHandle(const std::vector<std::wstring>& paths);

/* 0xRRGGBB -> 不透明颜色。 */
D2D1_COLOR_F Rgb(uint32_t rgb);
D2D1_COLOR_F ColorFromArgb(uint32_t argb, D2D1_COLOR_F fallback);
D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& color, float alpha);

} /* namespace util */
