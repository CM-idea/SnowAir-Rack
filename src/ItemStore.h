#pragma once

#include "Common.h"
#include "Item.h"

/*
 * 挂在绳子上的历史记录（持久化用）。图片文件本身归轻雪所有（快速贴图那
 * 一份在临时目录里），这里只记住有哪些卡片在绳上、它们的顺序，以及
 * 每张卡的拖动与锁定状态。
 */
class ItemStore {
public:
    bool Load(std::vector<Item>& items) const;
    bool Save(const std::vector<Item>& items) const;

private:
    std::wstring FilePath() const;
};
