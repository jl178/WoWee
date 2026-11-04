#include "rendering/spell_missile_rules.hpp"

namespace wowee {
namespace rendering {
namespace missilerules {

VisualColumns mergeRangedItemVisual(const VisualColumns* spell, const VisualColumns& item) {
    VisualColumns out{};
    if (spell) out = *spell;
    // 0x720F80: each of these the spell leaves 0 comes from the weapon's row.
    for (int c : {1, 2, 3, 14, 15, 16, 10, 17, 18, 19, 20, 21, 11, 12})
        if (out[static_cast<size_t>(c)] == 0) out[static_cast<size_t>(c)] = item[static_cast<size_t>(c)];
    // The missile goes as a pair, and only when the weapon's row flies one.
    if (out[7] == 0 && item[7] != 0) {
        out[7] = 1;
        out[8] = item[8];
    }
    return out;
}

MissileKind missileKind(int32_t missileModel, uint32_t ammoDisplayId) {
    switch (missileModel) {
        case -1: return MissileKind::MainHand;
        case -2: return MissileKind::OffHand;
        case -3: return MissileKind::Ranged;
        case -4:
        case -5: return MissileKind::Ammo;
        case 0: return ammoDisplayId != 0 ? MissileKind::AmmoModel : MissileKind::None;
        default: return missileModel > 0 ? MissileKind::Effect : MissileKind::None;
    }
}

int missileItemSlot(MissileKind kind) {
    switch (kind) {
        case MissileKind::MainHand: return 0;
        case MissileKind::OffHand: return 1;
        case MissileKind::Ranged: return 2;
        default: return -1;
    }
}

const char* itemComponentFolder(uint32_t inventoryType) {
    switch (inventoryType) {
        case 1: return "Item\\ObjectComponents\\Head\\";
        case 3: return "Item\\ObjectComponents\\Shoulder\\";
        case 14: return "Item\\ObjectComponents\\Shield\\";
        case 24: return "Item\\ObjectComponents\\Ammo\\";
        case 27: return "Item\\ObjectComponents\\Quiver\\";
        default: return "Item\\ObjectComponents\\Weapon\\";
    }
}

bool itemMissilePaths(const ItemDisplayNames& names, uint32_t inventoryType, ItemMissilePaths& out) {
    int i = -1;
    if (!names.model[0].empty()) i = 0;
    else if (!names.model[1].empty()) i = 1;
    if (i < 0 || inventoryType == 1) return false;
    const std::string folder = itemComponentFolder(inventoryType);
    out.model = folder + names.model[i];
    out.texture = names.texture[i].empty() ? std::string() : folder + names.texture[i] + ".blp";
    return true;
}

bool ammoMissilePaths(const ItemDisplayNames& names, uint32_t inventoryType, ItemMissilePaths& out) {
    if (inventoryType != 24 && inventoryType != 25) return false;
    if (names.model[1].empty() || names.texture[1].empty()) return false;
    const std::string folder = inventoryType == 25 ? "Item\\ObjectComponents\\Weapon\\"
                                                   : "Item\\ObjectComponents\\Ammo\\";
    out.model = folder + names.model[1];
    out.texture = folder + names.texture[1] + ".blp";
    return true;
}

}  // namespace missilerules
}  // namespace rendering
}  // namespace wowee
