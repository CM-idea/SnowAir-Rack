#pragma once

#include "Common.h"

namespace config_keys {
inline constexpr wchar_t kEnabled[] = L"rack.enabled";
inline constexpr wchar_t kHoverDelay[] = L"rack.hover_delay_ms";
inline constexpr wchar_t kAutoHide[] = L"rack.auto_hide_ms";
inline constexpr wchar_t kAutoRetract[] = L"rack.auto_retract";
inline constexpr wchar_t kMaxItems[] = L"rack.max_items";
inline constexpr wchar_t kAutoCapture[] = L"rack.auto_capture";
inline constexpr wchar_t kSize[] = L"rack.size";
} /* namespace config_keys */

struct RackConfig {
    bool enabled = true;
    int hoverDelayMs = 250;
    int autoHideMs = 500;
    bool autoRetract = true;
    int maxItems = 8;
    bool autoCapture = true;
    int size = 1;   // 0 small, 1 medium, 2 large

    float CardScale() const;
    void Load(const SnowAirHost* host, void* user);
};
