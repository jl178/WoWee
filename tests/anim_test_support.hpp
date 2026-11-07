#pragma once

// Helpers the animation tests share.

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace anim_test {

/// A model's `has(id)` for the fallback walks: true for the ids given.
inline auto hasIds(std::initializer_list<uint32_t> ids) {
    std::vector<uint32_t> v(ids);
    return [v](uint32_t id) {
        for (uint32_t x : v)
            if (x == id) return true;
        return false;
    };
}

}  // namespace anim_test
