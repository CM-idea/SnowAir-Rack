#pragma once

#include "CardType.h"
#include "Common.h"
#include "Image.h"

struct Item {
    int id = 0;
    /* 0=图片, 1=文字类(便签/文本/链接/颜色), 2=录屏, 3=文件类 */
    int kind = 0;
    /* 卡片实际形态；旧数据由 kind/path/thumb 推断。 */
    card::Type cardType = card::Type::Image;
    std::wstring path;        /* file used for copy/open/drag (first file) */
    /* 多文件条目：全部文件路径，path 始终等于 paths.front()。 */
    std::vector<std::wstring> paths;
    std::wstring sourcePath;  /* original SnowAir capture */
    std::wstring thumbPath;   /* 录屏封面（可为空） */
    /* SVG 卡片：path 是轻雪渲染的 PNG 预览，svgId 是轻雪剪切板记录的 id；
       复制/拖拽时按 id 读回 SVG 代码，不另存 .svg 文件。 */
    std::wstring svgId;
    std::wstring label;       /* 录屏/文件卡片上显示的文字 */
    /*
     * 便签属于轻雪的剪切板历史，插件只记条目 id；note 只是本次运行用的缓存，
     * 不会写进 rack_items.json（见 ItemStore::Save）。
     */
    std::wstring noteId;
    std::wstring note;
    int noteColor = 0;
    /* 卡片创建时间（毫秒），头部第二行显示月-日 时:分。 */
    long long createdAt = 0;
    /* 每张卡自己的强调色；accentSet=false 时用 cardType 的默认色。 */
    uint32_t accent = 0;
    bool accentSet = false;
    /* 颜色卡滚轮位置（运行时状态，不持久化）。 */
    float colorScroll = 0.0f;
    float aspect = 1.0f;
    float rackOffsetX = 0.0f;   /* user-dragged position along the rope (transient) */
    /* 自定义位置：true 时这张卡不占槽位，按 customX 画；customX 是内容坐标。 */
    bool custom = false;
    float customX = 0.0f;
    bool locked = false;        /* locked cards can be moved but not dragged out */
    int captureX = 0;
    int captureY = 0;
    int captureW = 0;
    int captureH = 0;
    uint64_t shakeStartMs = 0;  /* physical wobble after a spacing adjustment */
    float shakeAmp = 0.0f;
    ImageBitmap thumb;
    ImageBitmap preview;
    bool thumbAttempted = false;
    bool previewAttempted = false;
    bool ownsFile = false;    /* true for RackQuick temp copies */
    uint64_t addedMs = 0;
    uint64_t mtime = 0;
    /* 文件内容指纹：路径变了也能认出是同一个文件；0 = 还没算/算不出。 */
    uint64_t contentHash = 0;
};

/* 缩略图缓存文件名：优先内容指纹，跨改名/跨会话稳定；没有指纹时退回路径+mtime。 */
inline std::wstring ThumbCacheKey(const Item& item)
{
    uint64_t hash = item.contentHash;
    if (hash == 0) {
        hash = 1469598103934665603ull;
        for (const wchar_t c : item.path) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ull;
        }
        hash ^= item.mtime;
        hash *= 1099511628211ull;
    }
    wchar_t buffer[24]{};
    swprintf_s(buffer, L"%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

/* 布局约定：占槽位的吸附组在前面，自定义位置的卡片全排在后面。
   新卡要插到吸附组末尾（组和自定义卡之间），否则它会按内容坐标 0 落到绳子最左边。 */
inline void NormalizeRackOrder(std::vector<Item>& items)
{
    std::stable_partition(items.begin(), items.end(), [](const Item& item) { return !item.custom; });
}
