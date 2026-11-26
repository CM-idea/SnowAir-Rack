#pragma once

#include "Common.h"

namespace rack {

/*
 * 拖到两张卡中间松手时的让位计算。
 *
 * naturalLefts / widths 是吸附组按顺序排好的最终屏幕位置；split 是落点左边的卡数
 * （[0, split) 往左让，[split, n) 往右让）。obstacle 是自定义卡占的屏幕区间。
 * 返回每张卡需要加的偏移量（加到 naturalLefts 上），保证同侧相邻卡至少隔 gap，
 * 并且都不压住自定义卡。只往外推，不会把卡拉回来。
 */
inline std::vector<float> PushApartOffsets(const std::vector<float>& naturalLefts,
                                           const std::vector<float>& widths, int split,
                                           float obstacleLeft, float obstacleRight, float gap)
{
    const int total = static_cast<int>(std::min(naturalLefts.size(), widths.size()));
    if (total <= 0) {
        return {};
    }
    const int cut = ClampInt(split, 0, total);
    std::vector<float> finalLeft(naturalLefts.begin(), naturalLefts.begin() + total);

    float limitRight = obstacleLeft - gap;
    for (int g = cut - 1; g >= 0; --g) {
        float left = finalLeft[static_cast<size_t>(g)];
        if (left + widths[static_cast<size_t>(g)] > limitRight) {
            left = limitRight - widths[static_cast<size_t>(g)];
        }
        finalLeft[static_cast<size_t>(g)] = left;
        limitRight = left - gap;
    }

    float limitLeft = obstacleRight + gap;
    for (int g = cut; g < total; ++g) {
        float left = finalLeft[static_cast<size_t>(g)];
        if (left < limitLeft) {
            left = limitLeft;
        }
        finalLeft[static_cast<size_t>(g)] = left;
        limitLeft = left + widths[static_cast<size_t>(g)] + gap;
    }

    std::vector<float> offsets(static_cast<size_t>(total), 0.0f);
    for (int g = 0; g < total; ++g) {
        offsets[static_cast<size_t>(g)] =
            finalLeft[static_cast<size_t>(g)] - naturalLefts[static_cast<size_t>(g)];
    }
    return offsets;
}

} /* namespace rack */
