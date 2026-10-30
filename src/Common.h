#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "SnowAirPlugin.h"
#include "Version.h"

using Microsoft::WRL::ComPtr;

/* One global log sink, installed by Plugin::Init. Safe to call before init. */
void RackLog(const wchar_t* format, ...);

namespace rack {

inline float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int ClampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline uint64_t NowMs()
{
    return GetTickCount64();
}

inline float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

inline float EaseOutCubic(float t)
{
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

inline D2D1_RECT_F RectF(float l, float t, float r, float b)
{
    return D2D1::RectF(l, t, r, b);
}

inline D2D1_RECT_F Inset(const D2D1_RECT_F& r, float dx, float dy)
{
    return D2D1::RectF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy);
}

inline bool PointIn(const D2D1_RECT_F& r, float x, float y)
{
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

} /* namespace rack */
