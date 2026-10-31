#pragma once

#include "Common.h"
#include "Image.h"

struct Item {
    int id = 0;
    /* 0=图片, 1=便签(纯文字), 2=录屏, 3=其他文件 */
    int kind = 0;
    std::wstring path;        /* file used for copy/open/drag */
    std::wstring sourcePath;  /* original SnowAir capture */
    std::wstring thumbPath;   /* 录屏封面（可为空） */
    std::wstring label;       /* 录屏/文件卡片上显示的文字 */
    /*
     * 便签属于轻雪的剪切板历史，插件只记条目 id；note 只是本次运行用的缓存，
     * 不会写进 rack_items.json（见 ItemStore::Save）。
     */
    std::wstring noteId;
    std::wstring note;
    int noteColor = 0;
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
    bool ownsFile = false;    /* true for edited cache copies */
    uint64_t addedMs = 0;
    uint64_t mtime = 0;
    bool editing = false;
    uint64_t editStartedMs = 0;
    std::wstring editPath;
};
