#pragma once

/// The LiquidType a WMO group's MLIQ is drawn with, as the stock 3.3.5a client
/// (12340) resolves it. Pure functions; docs/re/wmo-rendering.md (Liquids) has
/// the addresses.

#include <cstdint>
#include <span>

namespace wowee::pipeline::stock_wmo_liquid {

/// 0x7c8d80: an old-style MLIQ takes its type from the first tile that has
/// liquid, (flags & 0xf) + 1; 0 when every tile is empty (0xf).
inline uint32_t liquidFromTiles(std::span<const uint8_t> tileFlags) {
    for (uint8_t f : tileFlags) {
        if ((f & 0xf) != 0xf) return static_cast<uint32_t>(f & 0xf) + 1u;
    }
    return 0;
}

/// 0x7d7310: a basic type (1..20, (t - 1) & 3 = water, ocean, magma, slime)
/// becomes its WMO LiquidType: 13 "WMO Water" (14 when the group has flag
/// 0x80000, "is ocean"), 14 "WMO Ocean", 19 "WMO Magma", 20 "WMO Slime".
/// Anything else is a LiquidType id already.
inline uint32_t wmoLiquidType(uint32_t t, uint32_t groupFlags) {
    if (t == 0 || t >= 21) return t;
    switch ((t - 1) & 3) {
        case 0: return (groupFlags & 0x80000u) ? 14u : 13u;
        case 1: return 14u;
        case 2: return 19u;
        default: return 20u;
    }
}

/// 0x7d82e0 + 0x7d7c30: the group's liquid type. With MOHD flag 0x4 the MOGP
/// groupLiquid is a LiquidType id; without it, 15 means none and anything
/// else is a basic type, one less. A group with MLIQ and no type takes it
/// from its tiles. Both go through wmoLiquidType.
inline uint32_t groupLiquidType(uint32_t mogpLiquid, uint32_t mohdFlags, uint32_t groupFlags,
                                std::span<const uint8_t> tileFlags) {
    uint32_t t = (mohdFlags & 0x4u) ? mogpLiquid : (mogpLiquid == 15u ? 0u : mogpLiquid + 1u);
    t = wmoLiquidType(t, groupFlags);
    if (t == 0) t = wmoLiquidType(liquidFromTiles(tileFlags), groupFlags);
    return t;
}

/// 0x793d20 at draw: water (a basic-water id below 21) in a group that is not
/// exterior (MOGP flags & 0x48 == 0) is LiquidType 17, "WMO Water - Interior",
/// unless the type has LiquidType flag 0x200. `typeFlags` is that DBC field.
inline uint32_t drawnLiquidType(uint32_t t, uint32_t groupFlags, uint32_t typeFlags) {
    const bool interior = (groupFlags & 0x48u) == 0;
    if (interior && (typeFlags & 0x200u) == 0 && t != 0 && t < 21 && ((t - 1) & 3) == 0) {
        return 17u;
    }
    return t;
}

/// MLIQ vertex spacing (0xa403a0): a 1/8 of an ADT chunk, 25/6 yards.
inline constexpr float kLiquidTileSize = 25.0f / 6.0f;

}  // namespace wowee::pipeline::stock_wmo_liquid
