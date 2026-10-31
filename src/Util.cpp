#include "Util.h"

#include <shellscalingapi.h>
#include <cstdarg>

#pragma comment(lib, "shcore.lib")

namespace util {

std::wstring Format(const wchar_t* format, ...)
{
    wchar_t stack[1024];
    va_list args;
    va_start(args, format);
    int written = _vsnwprintf_s(stack, _TRUNCATE, format, args);
    va_end(args);
    if (written >= 0) {
        return std::wstring(stack, static_cast<size_t>(written));
    }

    std::vector<wchar_t> heap(4096);
    va_start(args, format);
    written = _vsnwprintf_s(heap.data(), heap.size(), _TRUNCATE, format, args);
    va_end(args);
    if (written < 0) {
        return std::wstring();
    }
    return std::wstring(heap.data(), static_cast<size_t>(written));
}

HMODULE GetDllModule()
{
    static HMODULE cached = []() -> HMODULE {
        HMODULE module = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetDllModule), &module);
        return module;
    }();
    return cached;
}

std::wstring GetDllDir()
{
    static std::wstring cached = []() -> std::wstring {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetDllDir), &module) ||
            !module) {
            return std::wstring();
        }
        wchar_t path[MAX_PATH * 2]{};
        DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path)) {
            return std::wstring();
        }
        std::wstring full(path, length);
        size_t slash = full.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
    }();
    return cached;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') {
        return a + b;
    }
    return a + L"\\" + b;
}

bool FileExists(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    if (DirectoryExists(path)) {
        return true;
    }
    int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS ||
           result == ERROR_FILE_EXISTS || DirectoryExists(path);
}

bool IsWritableDir(const std::wstring& path)
{
    std::wstring probe = JoinPath(path, L".rack-write-test");
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    return true;
}

std::wstring GetDataDir()
{
    static std::wstring cached = []() -> std::wstring {
        std::wstring dllDir = GetDllDir();
        if (!dllDir.empty()) {
            std::wstring preferred = JoinPath(dllDir, L"data");
            if (EnsureDir(preferred) && IsWritableDir(preferred)) {
                return preferred;
            }
        }

        PWSTR roaming = nullptr;
        std::wstring fallback;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming) {
            fallback = JoinPath(roaming, L"SnowAir\\plugin\\rack\\data");
        }
        if (roaming) {
            CoTaskMemFree(roaming);
        }
        if (fallback.empty()) {
            wchar_t cwd[MAX_PATH]{};
            if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0) {
                fallback = JoinPath(cwd, L"rack-data");
            } else {
                fallback = L"rack-data";
            }
        }
        EnsureDir(fallback);
        return fallback;
    }();
    return cached;
}

bool CopyFileTo(const std::wstring& from, const std::wstring& to)
{
    if (from.empty() || to.empty()) {
        return false;
    }
    std::wstring targetDir = to;
    size_t slash = targetDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        EnsureDir(targetDir.substr(0, slash));
    }
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
}

bool DeleteFileIfExists(const std::wstring& path)
{
    if (path.empty()) {
        return true;
    }
    if (DeleteFileW(path.c_str())) {
        return true;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
}

bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>& out)
{
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (64LL << 20)) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool ok = out.empty() ||
              (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
               read == out.size());
    CloseHandle(file);
    if (!ok) {
        out.clear();
    }
    return ok;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size)
{
    std::wstring dir = path;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        EnsureDir(dir.substr(0, slash));
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD wrote = 0;
    bool ok = size == 0 ||
              (WriteFile(file, data, static_cast<DWORD>(size), &wrote, nullptr) && wrote == size);
    CloseHandle(file);
    return ok;
}

uint64_t FileWriteTime(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
           data.ftLastWriteTime.dwLowDateTime;
}

std::wstring TimestampName()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return Format(L"%04u%02u%02u-%02u%02u%02u-%03u", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

std::wstring FullPath(const std::wstring& path)
{
    if (path.empty()) {
        return std::wstring();
    }
    wchar_t buffer[MAX_PATH * 2]{};
    DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)),
                                    buffer, nullptr);
    if (length == 0 || length >= std::size(buffer)) {
        return path;
    }
    return std::wstring(buffer, length);
}

bool SamePath(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool OpenInShell(const std::wstring& path)
{
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                                     SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool ShowInExplorer(const std::wstring& path)
{
    std::wstring args = L"/select,\"" + path + L"\"";
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                                     SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool OpenInPaint(const std::wstring& path)
{
    const std::wstring args = L"\"" + path + L"\"";

    /*
     * Windows 11 的「画图」是商店应用，挂在执行别名后面；直接写
     * "mspaint.exe" 交给 ShellExecute 不一定找得到。所以先试别名和常见
     * 安装位置，最后再退回 .png 的 "edit" 动词。
     */
    std::vector<std::wstring> candidates;
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) &&
        localAppData) {
        candidates.push_back(JoinPath(localAppData, L"Microsoft\\WindowsApps\\mspaint.exe"));
        CoTaskMemFree(localAppData);
    }
    wchar_t windowsDir[MAX_PATH]{};
    if (GetWindowsDirectoryW(windowsDir, MAX_PATH) > 0) {
        candidates.push_back(JoinPath(windowsDir, L"System32\\mspaint.exe"));
        candidates.push_back(JoinPath(windowsDir, L"SysWOW64\\mspaint.exe"));
    }
    candidates.push_back(L"mspaint.exe");

    for (const auto& candidate : candidates) {
        if (candidate != L"mspaint.exe" && !FileExists(candidate)) {
            continue;
        }
        HINSTANCE result = ShellExecuteW(nullptr, L"open", candidate.c_str(), args.c_str(),
                                         nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32) {
            return true;
        }
    }

    HINSTANCE result = ShellExecuteW(nullptr, L"edit", path.c_str(), nullptr, nullptr,
                                     SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return std::string();
    }
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        length, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) {
        return std::wstring();
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                     nullptr, 0);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        length);
    return out;
}

UINT DpiForMonitor(HMONITOR monitor)
{
    UINT x = 96, y = 96;
    if (monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) && x > 0) {
        return x;
    }
    return 96;
}

float ScaleForMonitor(HMONITOR monitor)
{
    return static_cast<float>(DpiForMonitor(monitor)) / 96.0f;
}

HGLOBAL MakeDropFilesHandle(const std::wstring& path)
{
    const size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!handle) {
        return nullptr;
    }
    auto* drop = static_cast<DROPFILES*>(GlobalLock(handle));
    if (!drop) {
        GlobalFree(handle);
        return nullptr;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    wchar_t* text =
        reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
    std::memcpy(text, path.c_str(), path.size() * sizeof(wchar_t));
    text[path.size()] = L'\0';
    text[path.size() + 1] = L'\0';
    GlobalUnlock(handle);
    return handle;
}

D2D1_COLOR_F Rgb(uint32_t rgb)
{
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f);
}

D2D1_COLOR_F ColorFromArgb(uint32_t argb, D2D1_COLOR_F fallback)
{
    if (argb == 0) {
        return fallback;
    }
    const float r = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
    const float g = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    const float b = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    float a = static_cast<float>(argb & 0xFF) / 255.0f;
    if (a == 0.0f) {
        a = 1.0f; /* hosts commonly return opaque colours without an alpha byte */
    }
    return D2D1::ColorF(r, g, b, a);
}

D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& color, float alpha)
{
    return D2D1::ColorF(color.r, color.g, color.b, alpha);
}

} /* namespace util */
