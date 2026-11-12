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
    float rackOffsetX = 0.0f;   /* user-dragged position along the rope */
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
