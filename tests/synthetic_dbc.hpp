// Synthetic WDBC tables for the sound tests: a string block builder and a
// table builder that takes rows as field index -> value maps.
#pragma once

#include <catch_amalgamated.hpp>
#include "pipeline/dbc_loader.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace synthetic_dbc {

/// A string block: add() returns the offset a record stores.
struct Strings {
    std::string block{std::string(1, '\0')};
    uint32_t add(const std::string& s) {
        if (s.empty()) return 0;
        const auto off = static_cast<uint32_t>(block.size());
        block += s;
        block.push_back('\0');
        return off;
    }
};

inline uint32_t bits(float f) {
    uint32_t u = 0;
    static_assert(sizeof(u) == sizeof(f));
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline std::shared_ptr<wowee::pipeline::DBCFile> table(uint32_t fields,
                                                       const std::vector<std::vector<uint32_t>>& rows,
                                                       const std::string& strings) {
    std::vector<uint8_t> bytes;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    };
    bytes.insert(bytes.end(), {'W', 'D', 'B', 'C'});
    u32(static_cast<uint32_t>(rows.size()));
    u32(fields);
    u32(fields * 4);
    u32(static_cast<uint32_t>(strings.size()));
    for (const auto& row : rows) {
        REQUIRE(row.size() == fields);
        for (uint32_t v : row) u32(v);
    }
    bytes.insert(bytes.end(), strings.begin(), strings.end());
    auto dbc = std::make_shared<wowee::pipeline::DBCFile>();
    REQUIRE(dbc->load(bytes));
    return dbc;
}

inline std::vector<uint32_t> row(uint32_t fields, std::map<uint32_t, uint32_t> set) {
    std::vector<uint32_t> r(fields, 0);
    for (auto [i, v] : set) r[i] = v;
    return r;
}

} // namespace synthetic_dbc
