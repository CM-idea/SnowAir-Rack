#include "Config.h"

void RackConfig::Load(const SnowAirHost* host, void* user)
{
    if (!host) {
        return;
    }
    if (host->config_get_bool) {
        enabled = host->config_get_bool(user, config_keys::kEnabled, true);
        autoCapture = host->config_get_bool(user, config_keys::kAutoCapture, true);
        autoRetract = host->config_get_bool(user, config_keys::kAutoRetract, true);
    }
    if (host->config_get_int) {
        hoverDelayMs = host->config_get_int(user, config_keys::kHoverDelay, 250);
        size = host->config_get_int(user, config_keys::kSize, 1);
        autoHideMs = host->config_get_int(user, config_keys::kAutoHide, 500);
        maxItems = host->config_get_int(user, config_keys::kMaxItems, 8);
    }
    hoverDelayMs = rack::ClampInt(hoverDelayMs, 0, 2000);
    autoHideMs = rack::ClampInt(autoHideMs, 0, 5000);
    maxItems = rack::ClampInt(maxItems, 1, 30);
    size = rack::ClampInt(size, 0, 2);
}

float RackConfig::CardScale() const
{
    switch (size) {
    case 0: return 0.8f;
    case 2: return 1.35f;
    default: return 1.0f;
    }
}
