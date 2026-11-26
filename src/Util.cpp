#include "Util.h"

#include "MiniJson.h"

#include <filesystem>
#include <system_error>

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

namespace {

/* 当前进程是否为轻雪主程序：插件加载在 SnowAir.exe 内；测试宿主（HostSim）
   不应读写用户真实的缓存目录。 */
bool RunningInSnowAir()
{
    static const bool cached = []() -> bool {
        wchar_t exe[MAX_PATH * 2]{};
        if (GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe))) == 0) {
            return false;
        }
        std::wstring name = std::filesystem::path(exe).filename().wstring();
        std::wstring lower;
        lower.reserve(name.size());
        for (wchar_t c : name) {
            lower.push_back(static_cast<wchar_t>(towlower(c)));
        }
        return lower.find(L"snowair") != std::wstring::npos;
    }();
    return cached;
}

/* 轻雪的配置路径：先看 exe 同目录（便携模式，和宿主 initConfigPath 一致），
   否则 %AppData%\SnowAir\config.json。 */
std::wstring HostConfigPath()
{
    if (!RunningInSnowAir()) {
        return {};
    }
    wchar_t exe[MAX_PATH * 2]{};
    if (GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe))) > 0) {
        const std::filesystem::path portable =
            std::filesystem::path(exe).parent_path() / L"config.json";
        std::error_code ec;
        if (std::filesystem::exists(portable, ec)) {
            return portable.wstring();
        }
    }
    PWSTR roaming = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming) {
        path = (std::filesystem::path(roaming) / L"SnowAir" / L"config.json").wstring();
    }
    if (roaming) {
        CoTaskMemFree(roaming);
    }
    return path;
}

std::wstring RoamingSnowAirDir()
{
    PWSTR roaming = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming) {
        dir = (std::filesystem::path(roaming) / L"SnowAir").wstring();
    }
    if (roaming) {
        CoTaskMemFree(roaming);
    }
    return dir;
}

std::wstring DefaultSnowAirCacheRoot()
{
    PWSTR local = nullptr;
    std::wstring root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        root = (std::filesystem::path(local) / L"SnowAir" / L"Cache").wstring();
    }
    if (local) {
        CoTaskMemFree(local);
    }
    return root;
}

std::wstring TrimSpace(const std::wstring& text)
{
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

/* 把缓存文件从旧目录搬到新目录（同名时保留新目录里的那份）。 */
void MigrateThumbFiles(const std::wstring& from, const std::wstring& to)
{
    if (from.empty() || to.empty() || SamePath(from, to)) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(from, ec)) {
        return;
    }
    if (!EnsureDir(to)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(from, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        if (_wcsicmp(entry.path().extension().c_str(), L".png") != 0) {
            continue;
        }
        const std::filesystem::path target = std::filesystem::path(to) / entry.path().filename();
        std::error_code existsEc;
        if (std::filesystem::exists(target, existsEc)) {
            continue;
        }
        std::error_code moveEc;
        std::filesystem::rename(entry.path(), target, moveEc);
        if (moveEc) {
            moveEc.clear();
            std::filesystem::copy_file(entry.path(), target,
                                       std::filesystem::copy_options::overwrite_existing, moveEc);
            if (!moveEc) {
                std::error_code removeEc;
                std::filesystem::remove(entry.path(), removeEc);
            }
        }
    }
    std::error_code emptyEc;
    if (std::filesystem::is_directory(from, emptyEc) &&
        std::filesystem::is_empty(from, emptyEc)) {
        std::error_code removeEc;
        std::filesystem::remove(from, removeEc);
    }
}

std::wstring ResolveSnowAirCacheRoot()
{
    const std::wstring configPath = HostConfigPath();
    std::wstring root;
    if (!configPath.empty()) {
        std::vector<BYTE> bytes;
        if (ReadFileBytes(configPath, bytes) && !bytes.empty()) {
            std::string text(bytes.begin(), bytes.end());
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                static_cast<unsigned char>(text[1]) == 0xBB &&
                static_cast<unsigned char>(text[2]) == 0xBF) {
                text.erase(0, 3);
            }
            json::Value value;
            if (json::Parse(text, value)) {
                const json::Value* features = value.Get("features");
                const json::Value* custom = features ? features->Get("cacheDir") : nullptr;
                if (custom && custom->type == json::Type::String) {
                    const std::wstring stored = TrimSpace(Utf8ToWide(custom->text));
                    if (!stored.empty()) {
                        std::filesystem::path path(stored);
                        if (path.is_relative()) {
                            path = std::filesystem::path(RoamingSnowAirDir()) / path;
                        }
                        path = path.lexically_normal();
                        /* 宿主存的是最终缓存根（...\SnowAir\Cache）；兼容旧版只给了
                           「放在哪」的情况，和 Setting::setCacheDir 的规则保持一致。 */
                        const std::wstring fileName = path.filename().wstring();
                        const std::wstring parentName = path.parent_path().filename().wstring();
                        if (!(_wcsicmp(fileName.c_str(), L"Cache") == 0 &&
                              _wcsicmp(parentName.c_str(), L"SnowAir") == 0)) {
                            if (_wcsicmp(fileName.c_str(), L"SnowAir") == 0) {
                                path /= L"Cache";
                            } else {
                                path /= L"SnowAir";
                                path /= L"Cache";
                            }
                        }
                        root = path.wstring();
                    }
                }
            }
        }
    }
    if (root.empty()) {
        root = DefaultSnowAirCacheRoot();
    }
    return root;
}

} /* namespace */

std::wstring SnowAirCacheRoot()
{
    static std::wstring cached;
    static uint64_t cachedAt = 0;
    if (!RunningInSnowAir()) {
        return {};
    }
    /* 用户改「缓存位置」时宿主不会通知插件；隔几秒重读一次配置就能跟上，
       文件已经由宿主随 Misc 整棵搬走。 */
    const uint64_t now = GetTickCount64();
    if (cachedAt != 0 && now - cachedAt < 5000) {
        return cached;
    }
    cachedAt = now;
    cached = ResolveSnowAirCacheRoot();
    return cached;
}

std::wstring RackThumbDir()
{
    static std::wstring cached;
    static uint64_t cachedAt = 0;
    static std::wstring lastDir;
    const uint64_t now = GetTickCount64();
    if (!cached.empty() && now - cachedAt < 5000) {
        return cached;
    }
    cachedAt = now;
    std::wstring target;
    const std::wstring cacheRoot = SnowAirCacheRoot();
    if (!cacheRoot.empty()) {
        const std::wstring dir = JoinPath(JoinPath(cacheRoot, L"Misc"), L"Rack");
        if (EnsureDir(dir)) {
            target = dir;
        }
    }
    if (target.empty()) {
        target = JoinPath(GetDataDir(), L"thumbs");
        EnsureDir(target);
    }
    /* 主程序换了缓存位置（或从旧版插件升级）：把旧缩略图搬过来，卡片不用重缩。 */
    if (!lastDir.empty()) {
        MigrateThumbFiles(lastDir, target);
    }
    const std::wstring legacy = JoinPath(GetDataDir(), L"thumbs");
    if (!SamePath(legacy, target)) {
        MigrateThumbFiles(legacy, target);
    }
    lastDir = target;
    cached = target;
    return target;
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

uint64_t FileContentHash(const std::wstring& path)
{
    std::vector<BYTE> bytes;
    if (!ReadFileBytes(path, bytes) || bytes.empty()) {
        return 0;
    }
    constexpr size_t kChunk = 16u * 1024u * 1024u;
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](const BYTE* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
    };
    if (bytes.size() <= kChunk * 2) {
        mix(bytes.data(), bytes.size());
    } else {
        mix(bytes.data(), kChunk);
        mix(bytes.data() + bytes.size() - kChunk, kChunk);
    }
    hash ^= static_cast<uint64_t>(bytes.size());
    hash *= 1099511628211ull;
    return hash;
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

HGLOBAL MakeDropFilesHandle(const std::vector<std::wstring>& paths)
{
    if (paths.empty()) {
        return nullptr;
    }
    size_t chars = 0;
    for (const auto& path : paths) {
        chars += path.size() + 1;
    }
    const size_t bytes = sizeof(DROPFILES) + (chars + 1) * sizeof(wchar_t);
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
    auto* text = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
    for (const auto& path : paths) {
        std::memcpy(text, path.c_str(), path.size() * sizeof(wchar_t));
        text += path.size();
        *text++ = L'\0';
    }
    *text = L'\0';
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
