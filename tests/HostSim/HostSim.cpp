/*
 * HostSim — a tiny stand-in for SnowAir's plugin host.
 *
 * It loads dist\rack.dll, implements the SnowAirHost callback table, feeds it
 * test captures, and pumps a normal Win32 message loop. This is a development
 * and visual-verification tool; it is NOT shipped in the plugin package.
 *
 * Keys:
 *   F8  add the next test capture
 *   F9  toggle light/dark theme
 *   F10 toggle rack.enabled
 *   Esc quit
 */
#include <windows.h>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "SnowAirPlugin.h"

namespace {

bool g_dark = false;
bool g_enabled = true;
int g_hoverDelay = 200;
int g_autoHide = 700;
int g_maxItems = 8;
int g_theme = 0;   /* rack.theme: 0 银灰, 1 奶白, 2 木棉, 3 深沉 */

std::vector<std::wstring> g_images;
size_t g_nextImage = 0;
const SnowAirPlugin* g_plugin = nullptr;
uint64_t g_started = 0;
uint64_t g_durationMs = 0;
bool g_running = true;
std::wstring g_logPath;

/* ARGB values are 0xRRGGBBAA. Roles follow SnowAirThemeRole. */
const uint32_t kLight[] = {
    0x1E2228FF, 0x59616DFF, 0x8A929EFF, 0xFFFFFFFF, 0xF4F6F9FF,
    0xD6DBE2FF, 0x2F6FEBFF, 0xFFFFFFFF, 0x1F9D55FF, 0xFFFFFFFF,
    0xF4F4F5FF, 0xF4F4F5FF, 0x171717FF, 0xFAFAFAFF, 0xEEEEEEFF,
};
const uint32_t kDark[] = {
    0xE9EAEEFF, 0xB9BCC4FF, 0x8A8E99FF, 0x2A2B2FFF, 0x1D1E21FF,
    0x474A52FF, 0x6EA8FFFF, 0x0E1116FF, 0x5BD18BFF, 0x1F1F1FFF,
    0x363635FF, 0x262626FF, 0xFAFAFAFF, 0x171717FF, 0x262626FF,
};

uint32_t HostThemeColor(void*, int role)
{
    if (role < 0 || role >= SA_THEME_COUNT) {
        return 0;
    }
    return g_dark ? kDark[role] : kLight[role];
}

int HostIsDark(void*)
{
    return g_dark ? 1 : 0;
}

/* 宿主没有临时预览主题时返回 -1，晾衣架才会去读 "rack.theme" 配置。 */
int HostThemeOverride(void*)
{
    return -1;
}

bool HostGetBool(void*, const wchar_t* key, bool fallback)
{
    if (key && _wcsicmp(key, L"rack.enabled") == 0) {
        return g_enabled;
    }
    return fallback;
}

void HostSetBool(void*, const wchar_t* key, bool value)
{
    if (key && _wcsicmp(key, L"rack.enabled") == 0) {
        g_enabled = value;
    }
}

int HostGetInt(void*, const wchar_t* key, int fallback)
{
    if (!key) {
        return fallback;
    }
    if (_wcsicmp(key, L"rack.hover_delay_ms") == 0) {
        return g_hoverDelay;
    }
    if (_wcsicmp(key, L"rack.auto_hide_ms") == 0) {
        return g_autoHide;
    }
    if (_wcsicmp(key, L"rack.max_items") == 0) {
        return g_maxItems;
    }
    if (_wcsicmp(key, L"rack.theme") == 0) {
        return g_theme;
    }
    return fallback;
}

void HostSetInt(void*, const wchar_t* key, int value)
{
    if (!key) {
        return;
    }
    if (_wcsicmp(key, L"rack.hover_delay_ms") == 0) {
        g_hoverDelay = value;
    } else if (_wcsicmp(key, L"rack.auto_hide_ms") == 0) {
        g_autoHide = value;
    } else if (_wcsicmp(key, L"rack.max_items") == 0) {
        g_maxItems = value;
    }
}

void HostOpenSettings(void*)
{
    wprintf(L"[HostSim] host open_settings()\n");
}

void HostLog(void*, const wchar_t* message)
{
    if (!message) {
        return;
    }
    wprintf(L"[rack] %ls\n", message);
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
    if (!g_logPath.empty()) {
        HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, message, static_cast<DWORD>(wcslen(message) * sizeof(wchar_t)),
                      &written, nullptr);
            const wchar_t newline[] = L"\r\n";
            WriteFile(file, newline, 2 * sizeof(wchar_t), &written, nullptr);
            CloseHandle(file);
        }
    }
}

/* ---- fake unified entries, for previewing the envelope panel ---- */
std::vector<SnowAirEntry> g_entries;
/* 与 g_entries 平行的 id 表（v4 便签接口要按 id 取条目） */
std::vector<std::wstring> g_entryIds;
unsigned long long g_noteSeq = 0;

long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

SnowAirEntry MakeEntry(int kind, int source, const std::wstring& label,
                       const std::wstring& path, const std::wstring& thumb,
                       long long createdAt, int width, int height, int duration,
                       const std::vector<std::wstring>& tags)
{
    SnowAirEntry entry{};
    entry.kind = kind;
    entry.source = source;
    entry.created_at = createdAt;
    entry.width = width;
    entry.height = height;
    entry.duration_sec = duration;
    wcsncpy_s(entry.label, SNOWAIR_ENTRY_LABEL_MAX, label.c_str(), _TRUNCATE);
    wcsncpy_s(entry.path, SNOWAIR_ENTRY_PATH_MAX, path.c_str(), _TRUNCATE);
    wcsncpy_s(entry.thumb, SNOWAIR_ENTRY_PATH_MAX, thumb.c_str(), _TRUNCATE);
    std::wstring joined;
    for (const auto& tag : tags) {
        if (!joined.empty()) joined.push_back(L'\n');
        joined += tag;
    }
    wcsncpy_s(entry.tags, SNOWAIR_ENTRY_TAGS_MAX, joined.c_str(), _TRUNCATE);
    return entry;
}

void BuildFakeEntries()
{
    g_entries.clear();
    const long long now = NowMs();
    const long long minute = 60 * 1000;
    auto nameOf = [](const std::wstring& path) {
        return std::filesystem::path(path).filename().wstring();
    };
    for (size_t i = 0; i < g_images.size(); ++i) {
        const std::wstring& image = g_images[i];
        std::vector<std::wstring> tags;
        if (i == 0) tags.push_back(L"界面");
        g_entries.push_back(MakeEntry(SNOWAIR_ENTRY_IMAGE, SNOWAIR_SOURCE_CAPTURE,
                                      nameOf(image), image, image,
                                      now - static_cast<long long>(i + 1) * 17 * minute,
                                      1920, 1080, 0, tags));
    }
    g_entries.push_back(MakeEntry(SNOWAIR_ENTRY_TEXT, SNOWAIR_SOURCE_CLIPBOARD,
                                  L"SnowAir 轻雪", L"", L"", now - 2 * minute, 0, 0, 0,
                                  {L"灵感", L"待办"}));
    g_entries.push_back(MakeEntry(
        SNOWAIR_ENTRY_TEXT, SNOWAIR_SOURCE_CLIPBOARD,
        L"这是一段比较长的文本，用来检查卡片在内容比较多的时候如何换行，以及底部标签和右上角按钮会不会挤在一起。",
        L"", L"", now - 3 * minute, 0, 0, 0, {}));
    if (!g_images.empty()) {
        g_entries.push_back(MakeEntry(SNOWAIR_ENTRY_IMAGE, SNOWAIR_SOURCE_CLIPBOARD,
                                      L"剪贴板图片.png", g_images.front(), g_images.front(),
                                      now - 4 * minute, 640, 480, 0, {}));
        g_entries.push_back(MakeEntry(SNOWAIR_ENTRY_FILES, SNOWAIR_SOURCE_CLIPBOARD,
                                      nameOf(g_images.back()) + L"  等 2 个", g_images.back(),
                                      L"", now - 5 * minute, 0, 0, 0, {L"素材"}));
        g_entries.push_back(MakeEntry(SNOWAIR_ENTRY_MEDIA, SNOWAIR_SOURCE_RECORD,
                                      L"录屏_2026-10-30_14-20.mp4", g_images.back(),
                                      g_images.back(), now - 6 * minute, 1920, 1080, 42, {}));
        g_entries.push_back(MakeEntry(SNOWAIR_ENTRY_IMAGE, SNOWAIR_SOURCE_OTHER,
                                      L"参考图.png", g_images.back(), g_images.back(),
                                      now - 400LL * 24 * 60 * minute, 1280, 720, 0,
                                      {L"参考"}));
    }
    std::stable_sort(g_entries.begin(), g_entries.end(),
                     [](const SnowAirEntry& a, const SnowAirEntry& b) {
                         return a.created_at > b.created_at;
                     });
    g_entryIds.clear();
    for (size_t i = 0; i < g_entries.size(); ++i) {
        g_entryIds.push_back(L"e" + std::to_wstring(i + 1));
    }
}

int HostClipboardCount(void*)
{
    int count = 0;
    for (const auto& entry : g_entries) {
        if (entry.source == SNOWAIR_SOURCE_CLIPBOARD) ++count;
    }
    return count;
}

int HostEntryCount(void*)
{
    return static_cast<int>(g_entries.size());
}

void HostEntryRefresh(void*) {}

int HostEntryGet(void*, int index, SnowAirEntry* out)
{
    if (!out || index < 0 || index >= static_cast<int>(g_entries.size())) return 0;
    *out = g_entries[static_cast<size_t>(index)];
    return 1;
}

void HostEntryCopy(void*, int index)
{
    wprintf(L"[HostSim] entry_copy(%d)\n", index);
}

int HostEntryHang(void*, int index)
{
    if (index < 0 || index >= static_cast<int>(g_entries.size()) || !g_plugin) return 0;
    const auto& entry = g_entries[static_cast<size_t>(index)];
    if (!entry.path[0]) return 0;
    g_plugin->on_capture(entry.path);
    wprintf(L"[HostSim] entry_hang(%d) -> on_capture\n", index);
    return 1;
}

void HostEntryDelete(void*, int index)
{
    if (index < 0 || index >= static_cast<int>(g_entries.size())) return;
    g_entries.erase(g_entries.begin() + index);
    if (index < static_cast<int>(g_entryIds.size())) {
        g_entryIds.erase(g_entryIds.begin() + index);
    }
    wprintf(L"[HostSim] entry_delete(%d)\n", index);
}

/* ---- v4：文本记录（便签） ---- */

int HostEntryId(void*, int index, wchar_t* out, int capacity)
{
    if (!out || capacity <= 0) return 0;
    out[0] = 0;
    if (index < 0 || index >= static_cast<int>(g_entryIds.size())) return 0;
    wcsncpy_s(out, static_cast<size_t>(capacity), g_entryIds[static_cast<size_t>(index)].c_str(),
              _TRUNCATE);
    return 1;
}

int HostEntryText(void*, const wchar_t* id, wchar_t* out, int capacity)
{
    if (!id || !*id) return -1;
    for (size_t i = 0; i < g_entries.size() && i < g_entryIds.size(); ++i) {
        if (g_entryIds[i] != id) continue;
        if (g_entries[i].kind != SNOWAIR_ENTRY_TEXT) return -1;
        const std::wstring text = g_entries[i].label;
        if (out && capacity > 0) {
            wcsncpy_s(out, static_cast<size_t>(capacity), text.c_str(), _TRUNCATE);
        }
        return static_cast<int>(text.size());
    }
    return -1;
}

int HostEntryCreateText(void*, const wchar_t* text, const wchar_t* tags, wchar_t* idOut,
                        int capacity)
{
    if (!text || !*text) return 0;
    SnowAirEntry entry =
        MakeEntry(SNOWAIR_ENTRY_TEXT, SNOWAIR_SOURCE_CLIPBOARD, text, L"", L"", NowMs(), 0, 0, 0,
                  {});
    if (tags && *tags) {
        wcsncpy_s(entry.tags, SNOWAIR_ENTRY_TAGS_MAX, tags, _TRUNCATE);
    }
    const std::wstring id = L"n" + std::to_wstring(++g_noteSeq);
    g_entries.insert(g_entries.begin(), entry);
    g_entryIds.insert(g_entryIds.begin(), id);
    if (idOut && capacity > 0) {
        wcsncpy_s(idOut, static_cast<size_t>(capacity), id.c_str(), _TRUNCATE);
    }
    HostLog(nullptr, (L"note_create " + id + L" '" + std::wstring(text) + L"'").c_str());
    return 1;
}

int HostEntryUpdateText(void*, const wchar_t* id, const wchar_t* text, const wchar_t* tags)
{
    if (!id || !*id || !text) return 0;
    for (size_t i = 0; i < g_entries.size() && i < g_entryIds.size(); ++i) {
        if (g_entryIds[i] != id) continue;
        wcsncpy_s(g_entries[i].label, SNOWAIR_ENTRY_LABEL_MAX, text, _TRUNCATE);
        if (tags && *tags) wcsncpy_s(g_entries[i].tags, SNOWAIR_ENTRY_TAGS_MAX, tags, _TRUNCATE);
        HostLog(nullptr, (L"note_update " + std::wstring(id) + L" '" + std::wstring(text) +
                          L"'").c_str());
        return 1;
    }
    return 0;
}

int HostEntryDeleteId(void*, const wchar_t* id)
{
    if (!id || !*id) return 0;
    for (size_t i = 0; i < g_entries.size() && i < g_entryIds.size(); ++i) {
        if (g_entryIds[i] != id) continue;
        g_entries.erase(g_entries.begin() + (std::ptrdiff_t)i);
        g_entryIds.erase(g_entryIds.begin() + (std::ptrdiff_t)i);
        HostLog(nullptr, (L"note_delete " + std::wstring(id)).c_str());
        return 1;
    }
    return 0;
}

void HostEntryReveal(void*, int index)
{
    wprintf(L"[HostSim] entry_reveal(%d)\n", index);
}

void HostEntryTagsSet(void*, int index, const wchar_t* tags)
{
    if (index < 0 || index >= static_cast<int>(g_entries.size())) return;
    wcsncpy_s(g_entries[static_cast<size_t>(index)].tags, SNOWAIR_ENTRY_TAGS_MAX,
              tags ? tags : L"", _TRUNCATE);
    wprintf(L"[HostSim] entry_tags_set(%d)\n", index);
}

int HostEntryAllTags(void*, wchar_t* out, int capacity)
{
    if (!out || capacity <= 0) return 0;
    std::vector<std::wstring> tags;
    for (const auto& entry : g_entries) {
        const std::wstring text = entry.tags;
        size_t start = 0;
        while (start <= text.size()) {
            size_t pos = text.find(L'\n', start);
            if (pos == std::wstring::npos) pos = text.size();
            std::wstring part = text.substr(start, pos - start);
            if (!part.empty()) {
                bool exists = false;
                for (const auto& known : tags) {
                    if (known == part) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) tags.push_back(std::move(part));
            }
            if (pos == text.size()) break;
            start = pos + 1;
        }
    }
    std::wstring joined;
    for (const auto& tag : tags) {
        if (!joined.empty()) joined.push_back(L'\n');
        joined += tag;
    }
    wcsncpy_s(out, static_cast<size_t>(capacity), joined.c_str(), _TRUNCATE);
    return static_cast<int>(tags.size());
}

void AddNextImage()
{
    if (!g_plugin || g_images.empty()) {
        wprintf(L"[HostSim] no test images configured\n");
        return;
    }
    const std::wstring& image = g_images[g_nextImage % g_images.size()];
    ++g_nextImage;
    if (GetFileAttributesW(image.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[HostSim] missing image: %ls\n", image.c_str());
        return;
    }
    g_plugin->on_capture(image.c_str());
    wprintf(L"[HostSim] on_capture(%ls)\n", image.c_str());
}

bool KeyPressedOnce(int vk, bool* previous)
{
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !*previous;
    *previous = down;
    return pressed;
}

int RunLoop()
{
    SetTimer(nullptr, 1, 50, nullptr);
    MSG msg{};
    bool f8 = false, f9 = false, f10 = false, esc = false;

    while (g_running && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == nullptr) {
            if (KeyPressedOnce(VK_F8, &f8)) {
                AddNextImage();
            }
            if (KeyPressedOnce(VK_F9, &f9)) {
                g_dark = !g_dark;
                if (g_plugin) {
                    g_plugin->on_setting_changed(L"theme");
                }
                wprintf(L"[HostSim] theme -> %ls\n", g_dark ? L"dark" : L"light");
            }
            if (KeyPressedOnce(VK_F10, &f10)) {
                g_enabled = !g_enabled;
                if (g_plugin) {
                    g_plugin->on_setting_changed(L"rack.enabled");
                }
                wprintf(L"[HostSim] enabled -> %d\n", g_enabled ? 1 : 0);
            }
            if (KeyPressedOnce(VK_ESCAPE, &esc)) {
                g_running = false;
            }
            if (g_durationMs > 0 && GetTickCount64() - g_started >= g_durationMs) {
                g_running = false;
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(nullptr, 1);
    return 0;
}

} /* namespace */

int wmain(int argc, wchar_t** argv)
{
    _wsetlocale(LC_ALL, L".UTF-8");
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring dllPath;
    bool addAll = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--dll" && i + 1 < argc) {
            dllPath = argv[++i];
        } else if (arg == L"--image" && i + 1 < argc) {
            g_images.push_back(argv[++i]);
        } else if (arg == L"--seconds" && i + 1 < argc) {
            g_durationMs = static_cast<uint64_t>(_wtoi(argv[++i]) * 1000);
        } else if (arg == L"--dark") {
            g_dark = true;
        } else if (arg == L"--light") {
            g_dark = false;
        } else if (arg == L"--all") {
            addAll = true;
        } else if (arg == L"--hover" && i + 1 < argc) {
            g_hoverDelay = _wtoi(argv[++i]);
        } else if (arg == L"--autohide" && i + 1 < argc) {
            g_autoHide = _wtoi(argv[++i]);
        } else if (arg == L"--max" && i + 1 < argc) {
            g_maxItems = _wtoi(argv[++i]);
        } else if (arg == L"--theme" && i + 1 < argc) {
            g_theme = _wtoi(argv[++i]);
        } else if (arg == L"--help" || arg == L"-h") {
            wprintf(L"HostSim: --dll <rack.dll> --image <png> [--image ...] "
                    L"[--seconds N] [--dark|--light] [--theme 0..3]\n");
            return 0;
        }
    }

    if (dllPath.empty()) {
        wchar_t module[MAX_PATH]{};
        GetModuleFileNameW(nullptr, module, MAX_PATH);
        std::wstring dir(module);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.resize(slash);
        }
        dllPath = dir + L"\\rack.dll";
    }
    if (g_images.empty()) {
        g_images.push_back(L"tests\\assets\\sample1.png");
    }

    g_logPath = L"HostSim.log";
    g_started = GetTickCount64();

    HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        wprintf(L"[HostSim] LoadLibrary(%ls) failed: %lu\n", dllPath.c_str(), GetLastError());
        return 1;
    }

    using EntryFn = const SnowAirPlugin* (*)();
    auto entry = reinterpret_cast<EntryFn>(GetProcAddress(module, "SnowAirPluginEntry"));
    if (!entry) {
        wprintf(L"[HostSim] SnowAirPluginEntry not found\n");
        FreeLibrary(module);
        return 1;
    }

    g_plugin = entry();
    if (!g_plugin || g_plugin->api_version != SNOWAIR_PLUGIN_API) {
        wprintf(L"[HostSim] API mismatch (plugin=%u host=%u)\n",
                g_plugin ? g_plugin->api_version : 0, SNOWAIR_PLUGIN_API);
        FreeLibrary(module);
        return 1;
    }

    SnowAirHost host{};
    host.api_version = SNOWAIR_PLUGIN_API;
    host.user = nullptr;
    host.theme_color = &HostThemeColor;
    host.is_dark = &HostIsDark;
    host.theme_override = &HostThemeOverride;
    host.config_get_bool = &HostGetBool;
    host.config_set_bool = &HostSetBool;
    host.config_get_int = &HostGetInt;
    host.config_set_int = &HostSetInt;
    host.open_settings = &HostOpenSettings;
    host.log = &HostLog;
    host.clipboard_count = &HostClipboardCount;
    host.entry_count = &HostEntryCount;
    host.entry_refresh = &HostEntryRefresh;
    host.entry_get = &HostEntryGet;
    host.entry_copy = &HostEntryCopy;
    host.entry_hang = &HostEntryHang;
    host.entry_delete = &HostEntryDelete;
    host.entry_reveal = &HostEntryReveal;
    host.entry_tags_set = &HostEntryTagsSet;
    host.entry_all_tags = &HostEntryAllTags;
    host.entry_id = &HostEntryId;
    host.entry_text = &HostEntryText;
    host.entry_create_text = &HostEntryCreateText;
    host.entry_update_text = &HostEntryUpdateText;
    host.entry_delete_id = &HostEntryDeleteId;

    BuildFakeEntries();

    if (!g_plugin->init(&host)) {
        wprintf(L"[HostSim] plugin init failed\n");
        FreeLibrary(module);
        return 1;
    }

    wprintf(L"[HostSim] loaded %ls %ls\n", g_plugin->name, g_plugin->version);
    wprintf(L"[HostSim] F8=add capture  F9=theme  F10=enabled  Esc=quit  rack.theme=%d\n", g_theme);
    if (g_durationMs > 0) {
        wprintf(L"[HostSim] auto-quit in %llu ms\n",
                static_cast<unsigned long long>(g_durationMs));
    }

    AddNextImage();
    if (addAll) {
        while (g_nextImage < g_images.size()) {
            AddNextImage();
        }
    }
    const int result = RunLoop();

    if (g_plugin) {
        g_plugin->shutdown();
        g_plugin = nullptr;
    }
    FreeLibrary(module);
    return result;
}

