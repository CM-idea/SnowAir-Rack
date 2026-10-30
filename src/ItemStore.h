#pragma once

#include "Common.h"
#include "Item.h"

/*
 * Persisted hanging history. The image files stay owned by SnowAir (or by the
 * quick-pin temp folder); this store only remembers which cards were on the
 * rope, their order and their per-card drag/lock state.
 */
class ItemStore {
public:
    bool Load(std::vector<Item>& items) const;
    bool Save(const std::vector<Item>& items) const;

private:
    std::wstring FilePath() const;
};
