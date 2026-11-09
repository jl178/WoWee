// The .hdlook table: parsing it and reading a character's look out of it.
// No asset access here (hd_characters.cpp has that), so it is tested alone.
#include "pipeline/hd_characters.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <unordered_set>

namespace wowee {
namespace pipeline {

namespace {

/// The character's index for each option, in HdCharacterLook::Option order.
std::array<int, HdCharacterLook::kOptionCount> indicesOf(const CharacterAppearance& who) {
    return {who.skinId, who.faceId, who.hairStyleId, who.hairColorId, who.facialHairId};
}

}  // namespace

HdCharacterLook parseHdCharacterLook(const std::string& text) {
    HdCharacterLook look;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;
        if (kind == "order") {
            int t;
            while (ls >> t) look.bodyOrder.push_back(t);
        } else if (kind == "hairtex") {
            ls >> look.hairTarget;
        } else if (kind == "choice") {
            // The file numbers the options 1-5, as ChrCustomizationConversion does.
            HdCharacterLook::Choice c;
            int option = 0, dependsOn = 0;
            if (!(ls >> option >> c.index >> c.choice >> c.enhanced)) continue;
            if (ls >> dependsOn >> c.dependsIndex) c.dependsOn = dependsOn - 1;
            c.option = option - 1;
            if (c.option < 0 || c.option >= HdCharacterLook::kOptionCount) continue;
            if (c.dependsOn >= HdCharacterLook::kOptionCount) c.dependsOn = -1;
            look.choices.push_back(c);
        } else if (kind == "default") {
            HdCharacterLook::Default d;
            if (ls >> d.choice >> d.enhanced) look.defaults.push_back(d);
        } else if (kind == "tex") {
            HdCharacterLook::Texture t;
            if (!(ls >> t.choice >> t.related >> t.target)) continue;
            std::getline(ls >> std::ws, t.path);
            if (!t.path.empty()) look.textures.push_back(std::move(t));
        } else if (kind == "geo") {
            HdCharacterLook::Geoset g;
            int id = -1;
            if (!(ls >> g.choice >> id) || id < 0 || id > 0xFFFF) continue;
            g.id = static_cast<uint16_t>(id);
            look.geosets.push_back(g);
        }
    }
    return look;
}

std::vector<uint32_t> selectHdCharacterChoices(const HdCharacterLook& look,
                                               const CharacterAppearance& who) {
    const auto idx = indicesOf(who);
    std::vector<uint32_t> picked;
    std::unordered_set<uint32_t> covered;  // Enhanced options a 3.3.5 one reached
    for (int o = 0; o < HdCharacterLook::kOptionCount; o++) {
        // Exact, then ignoring what it depends on, then the option's index 0:
        // a face number the table has no row for is still a face.
        for (int pass = 0; pass < 3; pass++) {
            bool any = false;
            for (const auto& c : look.choices) {
                if (c.option != o) continue;
                const int want = pass < 2 ? idx[o] : 0;
                if (c.index != want) continue;
                if (pass == 0 && c.dependsOn >= 0 && c.dependsIndex != idx[c.dependsOn]) continue;
                picked.push_back(c.choice);
                covered.insert(c.enhanced);
                any = true;
            }
            if (any) break;
        }
    }
    for (const auto& d : look.defaults) {
        if (covered.count(d.enhanced) == 0) picked.push_back(d.choice);
    }
    return picked;
}

CharacterSectionTextures resolveHdCharacterTextures(const HdCharacterLook& look,
                                                    const CharacterAppearance& who) {
    const auto picked = selectHdCharacterChoices(look, who);
    const std::unordered_set<uint32_t> on(picked.begin(), picked.end());
    // target -> path; the first matching row wins.
    std::map<int, std::string> byTarget;
    for (const auto& t : look.textures) {
        if (on.count(t.choice) == 0) continue;
        if (t.related != 0 && on.count(t.related) == 0) continue;
        byTarget.emplace(t.target, t.path);
    }
    CharacterSectionTextures out;
    bool first = true;
    for (int target : look.bodyOrder) {
        auto it = byTarget.find(target);
        if (first) {
            first = false;
            if (it != byTarget.end()) out.bodySkin = it->second;
            continue;
        }
        if (it != byTarget.end()) out.underwear.push_back(it->second);
    }
    if (auto it = byTarget.find(look.hairTarget); it != byTarget.end()) out.hair = it->second;
    out.exactFace = out.haveFace = true;
    out.haveHair = !out.hair.empty();
    return out;
}

HdCharacterGeosets resolveHdCharacterGeosets(const HdCharacterLook& look,
                                             const CharacterAppearance& who) {
    const auto picked = selectHdCharacterChoices(look, who);
    const std::unordered_set<uint32_t> on(picked.begin(), picked.end());
    HdCharacterGeosets out;
    // Group -> id for the groups above 20: the defaults, then what the picked
    // choices say, which wins (variant 0 being "none of this group").
    std::map<uint16_t, uint16_t> extra;
    for (const auto& g : look.geosets) {
        if (g.choice == 0 && g.id / 100 > 20) extra[static_cast<uint16_t>(g.id / 100)] = g.id;
    }
    bool hairSet = false;
    for (const auto& g : look.geosets) {
        if (g.choice == 0 || on.count(g.choice) == 0) continue;
        const uint16_t group = static_cast<uint16_t>(g.id / 100);
        const uint16_t variant = static_cast<uint16_t>(g.id % 100);
        switch (group) {
            case 0:
                if (!hairSet) out.hairScalp = g.id != 0 ? g.id : kHdNoHairGeoset;
                hairSet = true;
                break;
            case 1: out.facial100 = variant; break;
            case 2: out.facial200 = variant; break;
            case 3: out.facial300 = variant; break;
            default:
                if (group > 20) extra[group] = g.id;
                break;
        }
    }
    for (const auto& [group, id] : extra) {
        if (id % 100 != 0) out.extra.push_back(id);
    }
    return out;
}

}  // namespace pipeline
}  // namespace wowee
