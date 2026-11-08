#pragma once

#include "Common.h"

namespace config_keys {
inline constexpr wchar_t kEnabled[] = L"rack.enabled";
inline constexpr wchar_t kHoverDelay[] = L"rack.hover_delay_ms";
inline constexpr wchar_t kAutoHide[] = L"rack.auto_hide_ms";
inline constexpr wchar_t kAutoRetract[] = L"rack.auto_retract";
inline constexpr wchar_t kDisplayMode[] = L"rack.display_mode";
inline constexpr wchar_t kMaxItems[] = L"rack.max_items";
inline constexpr wchar_t kAutoCapture[] = L"rack.auto_capture";
inline constexpr wchar_t kSize[] = L"rack.size";
} /* namespace config_keys */

/* 0=悬停展开，1=快捷键切换（默认隐藏），2=常驻显示（默认显示）。 */
inline constexpr int kDisplayHover = 0;
inline constexpr int kDisplayHotkey = 1;
inline constexpr int kDisplayAlways = 2;

struct RackConfig {
    bool enabled = true;
    int hoverDelayMs = 250;
    int autoHideMs = 500;
    /* 兼容旧字段：只有悬停模式才自动收回。 */
    bool autoRetract = true;
    int displayMode = kDisplayHover;
    int maxItems = 8;
    bool autoCapture = true;
    int size = 1;   // 0 small, 1 medium, 2 large

    float CardScale() const;
    void Load(const SnowAirHost* host, void* user);
};
