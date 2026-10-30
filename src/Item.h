#pragma once

#include "Common.h"
#include "Image.h"

struct Item {
    int id = 0;
    std::wstring path;        /* file used for copy/open/drag */
    std::wstring sourcePath;  /* original SnowAir capture */
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
