#pragma once

// The stock trade skill window's model, as Wow.exe 3.3.5a (12340) keeps it in
// TradeSkillFrame.cpp. Header-only and free of the game state so the rules can
// be pinned by tests; SpellHandler feeds it and the Lua bindings read it.
//
// Addresses (see docs/re/professions.md):
//   open / toggle            0x5DE120   rebuild           0x5DD5A0
//   filters + sort           0x5DCC40   row order         0x5DAE60
//   header order             0x5DA6B0   name/level match  0x5DC110
//   link text (trade:)       0x5DB060   link open         0x5DE300
//   repeat: DoTradeSkill     0x5DAC00   pending->active   0x5DA0F0 (cast start)
//           next craft       0x5DA070   (cast go)         clear on fail 0x5DA020

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace game {
namespace stock_trade_skill {

/// GetTradeSkillInfo's second value, indexed by the row's difficulty
/// ("optimal" + i * 0x20 in Wow.exe).
inline const char* difficultyName(int d) {
    static const char* const kNames[4] = {"optimal", "medium", "easy", "trivial"};
    return (d >= 0 && d < 4) ? kNames[d] : kNames[3];
}

/// 0x5DD5A0: the colour of a recipe from the player's skill rank (the base
/// value, no bonus) and its SkillLineAbility TrivialSkillLineRankHigh (+0x28)
/// and TrivialSkillLineRankLow (+0x2C). A Low of zero means High - 25. The same
/// bands AzerothCore's Player::UpdateCraftSkill rolls skill-ups with: orange
/// below Low, yellow below (Low + High) / 2, green below High, grey from High.
/// A recipe with neither threshold is grey.
inline int difficulty(int rank, int trivialHigh, int trivialLow) {
    int low = trivialLow;
    if (low == 0) low = trivialHigh - 25;
    if (rank < low) return 0;
    if (rank < (low + trivialHigh) / 2) return 1;
    if (rank < trivialHigh) return 2;
    return 3;
}

/// The inventory-slot bits the slot filter works in. Bit n is the n-th name in
/// the GlobalStrings keys below (0xAD0E10).
inline constexpr uint32_t kNonEquipSlotBit = 0x4000000;
inline constexpr int kNumSlotBits = 27;
inline const char* slotBitKey(int bit) {
    static const char* const kKeys[kNumSlotBits] = {
        "HEADSLOT", "NECKSLOT", "SHOULDERSLOT", "SHIRTSLOT", "CHESTSLOT", "WAISTSLOT",
        "LEGSSLOT", "FEETSLOT", "WRISTSLOT", "HANDSSLOT", "FINGER0SLOT", "FINGER1SLOT",
        "TRINKET0SLOT", "TRINKET1SLOT", "BACKSLOT", "MAINHANDSLOT", "SECONDARYHANDSLOT",
        "RANGEDSLOT", "TABARDSLOT", "BAGSLOT", "BAGSLOT", "BAGSLOT", "BAGSLOT",
        "ENCHSLOT_WEAPON", "ENCHSLOT_2HWEAPON", "SHIELDSLOT", "NONEQUIPSLOT"};
    return (bit >= 0 && bit < kNumSlotBits) ? kKeys[bit] : "";
}

/// 0xA2D288: InventoryType -> slot bits.
inline uint32_t rawSlotBitsForInventoryType(uint32_t invType) {
    static const uint32_t kBits[29] = {
        0x0, 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0xC00, 0x3000,
        0x18000, 0x10000, 0x20000, 0x4000, 0x18000, 0x780000, 0x40000, 0x10, 0x18000,
        0x18000, 0x10000, 0x0, 0x20000, 0x20000, 0x0, 0x20000};
    return invType < 29 ? kBits[invType] : 0;
}

/// The slot bits of a recipe that makes an item (0x5DD5A0): the table, with a
/// bag, a ring and a trinket narrowed to their first slot, and "not equipped"
/// for anything the table has nothing for.
inline uint32_t slotBitsForCreatedItem(uint32_t invType) {
    uint32_t bits = rawSlotBitsForInventoryType(invType);
    if (invType == 18) bits = 0x80000;
    else if (invType == 11) bits = 0x400;
    else if (invType == 12) bits = 0x1000;
    return bits ? bits : kNonEquipSlotBit;
}

/// The slot bits of a recipe that makes nothing - an enchant - from the
/// spell's equipped-item requirement (0x5DD5A0): a weapon enchant is one-hand
/// when the subclass mask has bit 0 (one-handed axes) and two-hand otherwise;
/// an armour enchant takes its first inventory type; a shield enchant names no
/// inventory type and has subclass bit 6.
inline uint32_t slotBitsForEnchant(int32_t equippedItemClass, uint32_t subClassMask,
                                   uint32_t invTypeMask) {
    if (equippedItemClass == 2)
        return (subClassMask & 1u) ? 0x800000u : 0x1000000u;
    if (equippedItemClass != 4) return kNonEquipSlotBit;
    if (invTypeMask == 0)
        return (subClassMask & 0x40u) ? 0x2000000u : kNonEquipSlotBit;
    for (uint32_t t = 0; t < 29; ++t) {
        if (!(invTypeMask & (1u << t))) continue;
        uint32_t bits = rawSlotBitsForInventoryType(t);
        if (bits & 0x80000u) bits &= 0xFF8FFFFFu;
        else if (bits & 0x400u) bits &= ~0x800u;
        else if (bits & 0x1000u) bits &= ~0x2000u;
        return bits ? bits : kNonEquipSlotBit;
    }
    return kNonEquipSlotBit;
}

/// Spell effects that make an item and so file a recipe under the item's
/// class and subclass: CREATE_ITEM, CREATE_RANDOM_ITEM, CREATE_ITEM_2.
inline bool isCreateItemEffect(uint32_t effect) {
    return effect == 24 || effect == 59 || effect == 157;
}

/// Spell.dbc Attributes bit that puts a spell in a trade skill list
/// (SPELL_ATTR0_TRADESPELL). The opener (effect 47) and the passive ranks do
/// not have it.
inline constexpr uint32_t kAttrTradeSpell = 0x20;

/// One recipe, as the rebuild needs it.
struct Recipe {
    int32_t spellId = 0;
    std::string name;            ///< the spell's name - the last sort key
    int trivialHigh = 0;
    int trivialLow = 0;
    /// Effect 0 makes an item: the recipe is filed under that item's class
    /// and subclass, which needs the item's cache record.
    bool createsItem = false;
    bool itemKnown = false;      ///< the item cache has it (createsItem only)
    int itemClass = -1;
    int itemSubClass = -1;
    int itemLevel = 0;
    int requiredLevel = -1;      ///< -1: no item to test (the level filter passes)
    uint32_t slotBits = kNonEquipSlotBit;
    int numAvailable = -1;       ///< -1: no reagents; else how many the bags make
    /// Lower-cased text the search box matches (0x5DC110): the item's stat and
    /// spell lines and description, the spell's name, the reagents, the tools,
    /// the spell focus, the spell's description.
    std::vector<std::string> searchText;
};

struct Header {
    int itemClass = -1;
    int itemSubClass = -1;
    bool filterOn = true;    ///< subclass filter (+8)
    bool collapsed = false;  ///< (+0xC)
    uint32_t slotBits = 0;   ///< what its recipes make (+0x10)
};

struct Row {
    int32_t spellId = -1;    ///< -1 for a header
    int difficulty = 3;
    int itemClass = -1;
    int itemSubClass = -1;
    uint32_t slotBits = 0;
    int itemLevel = 0;
    int trivialHigh = 0;
    int numAvailable = 0;
    bool visible = true;
    int recipe = -1;         ///< index into recipes(), -1 for a header
    bool isHeader() const { return spellId < 0; }
};

inline std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Base64 alphabet of the trade link's recipe bits (0x5DB060). Six bits a
/// character, the first recipe in the lowest bit.
inline const char* tradeLinkAlphabet() {
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}
inline std::string encodeTradeLinkBits(const std::vector<bool>& bits) {
    std::string out;
    uint8_t acc = 0, n = 0;
    for (bool b : bits) {
        if (b) acc |= static_cast<uint8_t>(1u << n);
        if (++n > 5) { out += tradeLinkAlphabet()[acc]; acc = 0; n = 0; }
    }
    if (n != 0) out += tradeLinkAlphabet()[acc];
    return out;
}
/// The decode the list builder uses (0x5DD5A0 over the table at 0xA1B688):
/// bit i of the i-th row. Characters outside the alphabet read as zero.
inline std::vector<bool> decodeTradeLinkBits(const std::string& data, size_t count) {
    std::vector<bool> bits(count, false);
    const std::string alpha = tradeLinkAlphabet();
    for (size_t i = 0; i < count; ++i) {
        const size_t c = i / 6;
        if (c >= data.size()) break;
        const size_t v = alpha.find(data[c]);
        if (v == std::string::npos) continue;
        bits[i] = (v >> (i % 6)) & 1u;
    }
    return bits;
}
/// How many characters a list of `count` rows takes - what the link opener
/// demands before it opens anything (0x5DE120).
inline size_t tradeLinkDataLength(size_t count) { return (count + 5) / 6; }

/// The parts of a "trade:" hyperlink: trade:spellId:rank:maxRank:GUIDHEX:data.
struct TradeLink {
    uint32_t spellId = 0;
    int rank = 0;
    int maxRank = 0;
    uint64_t guid = 0;
    std::string data;
};
inline bool parseTradeLink(std::string link, TradeLink& out) {
    if (const size_t at = link.find("|H"); at != std::string::npos) {
        link = link.substr(at + 2);
        if (const size_t end = link.find("|h"); end != std::string::npos) link = link.substr(0, end);
    }
    if (link.rfind("trade:", 0) != 0) return false;
    std::vector<std::string> parts;
    size_t from = 6;
    for (int i = 0; i < 4; ++i) {
        const size_t c = link.find(':', from);
        if (c == std::string::npos) return false;
        parts.push_back(link.substr(from, c - from));
        from = c + 1;
    }
    out.data = link.substr(from);
    try {
        out.spellId = static_cast<uint32_t>(std::stoul(parts[0]));
        out.rank = std::stoi(parts[1]);
        out.maxRank = std::stoi(parts[2]);
        out.guid = std::stoull(parts[3], nullptr, 16);
    } catch (...) {
        return false;
    }
    return out.spellId != 0 && out.guid != 0;
}

/// The window's state. Indices given to and returned by the Lua functions are
/// 1-based; everything here is 0-based.
class Model {
public:
    // ---- what is open ----------------------------------------------------
    uint32_t openerSpell = 0;   ///< 0xC235A0
    uint32_t skillLine = 0;     ///< 0xC235A4 (0: closed)
    uint32_t lastSkillLine = 0; ///< 0xC235F0: the filters survive reopening it
    bool linked = false;        ///< 0xC235E4: someone else's list from a link
    uint64_t linkedGuid = 0;
    int linkRank = 0, linkMaxRank = 0;
    std::string linkData;

    // ---- filters (0xAD0E00..0E0C, 0xC235BC..CC) --------------------------
    uint32_t subClassFilter = 0xFFFFFFFFu;
    int currentSubClass = -1;
    uint32_t invSlotFilter = 0xFFFFFFFFu;
    uint32_t expandMask = 0xFFFFFFFFu;
    bool onlyMakeable = false;
    bool onlySkillUps = false;
    std::string nameFilter;     ///< lower-cased; empty is none
    int minLevel = 0, maxLevel = 0;
    uint32_t allSlotBits = 0;   ///< 0xC235D0: every row's bits since the line opened

    int32_t selectedSpell = 0;  ///< 0xC235A8

    // ---- repeat (0xC235D4..E0) -------------------------------------------
    int32_t repeatSpell = 0, repeatLeft = 0;     ///< the craft being repeated
    int32_t pendingSpell = 0, pendingCount = 0;  ///< asked for, not started yet

    bool isOpen() const { return skillLine != 0; }
    const std::vector<Row>& rows() const { return rows_; }
    const std::vector<Header>& headers() const { return headers_; }
    const std::vector<Recipe>& recipes() const { return recipes_; }
    size_t numVisible() const { return visible_; }
    bool pending() const { return pending_; }

    /// Opening a line (0x5DE120 after its toggle test): a line other than the
    /// last one opened starts with no filters and no selection.
    void beginOpen(uint32_t opener, uint32_t line) {
        openerSpell = opener;
        skillLine = line;
        if (lastSkillLine != line) {
            nameFilter.clear();
            minLevel = maxLevel = 0;
            onlyMakeable = false;
            subClassFilter = 0xFFFFFFFFu;
            invSlotFilter = 0xFFFFFFFFu;
            expandMask = 0xFFFFFFFFu;
            allSlotBits = 0;
            selectedSpell = 0;
        }
    }
    void finishOpen() { lastSkillLine = skillLine; }
    void close() {
        openerSpell = 0;
        skillLine = 0;
    }

    /// 0x5DD5A0 with its argument set: the list made again from `recipes`,
    /// the headers' filter and folding kept by item class and subclass.
    /// Returns false while an item a recipe makes is not in the cache yet - the
    /// stock client then shows no rows and waits for the last reply, which
    /// rebuilds again; true when the list is complete and TRADE_SKILL_UPDATE
    /// is due.
    bool rebuild(std::vector<Recipe> recipes, int rank) {
        recipes_ = std::move(recipes);
        struct Kept { int cls, sub; bool filterOn, collapsed; };
        std::vector<Kept> kept;
        for (const auto& h : headers_) kept.push_back({h.itemClass, h.itemSubClass, h.filterOn, h.collapsed});
        headers_.clear();
        subClassFilter = 0xFFFFFFFFu;
        expandMask = 0xFFFFFFFFu;
        rows_.clear();
        pending_ = false;
        for (size_t i = 0; i < recipes_.size(); ++i) {
            const Recipe& r = recipes_[i];
            Row row;
            row.spellId = r.spellId;
            row.recipe = static_cast<int>(i);
            row.trivialHigh = r.trivialHigh;
            row.difficulty = difficulty(rank, r.trivialHigh, r.trivialLow);
            row.numAvailable = r.numAvailable;
            if (r.createsItem && !r.itemKnown) pending_ = true;
            if (r.createsItem) {
                row.itemClass = r.itemClass;
                row.itemSubClass = r.itemSubClass;
                row.itemLevel = r.itemLevel;
            }
            row.slotBits = r.slotBits;
            rows_.push_back(row);
        }
        if (pending_) {
            visible_ = 0;
            return false;
        }
        for (const auto& row : rows_) {
            allSlotBits |= row.slotBits;
            int h = headerIndex(row.itemClass, row.itemSubClass);
            if (h < 0) {
                headers_.push_back({row.itemClass, row.itemSubClass, true, false, 0});
                h = static_cast<int>(headers_.size()) - 1;
            }
            headers_[static_cast<size_t>(h)].slotBits |= row.slotBits;
        }
        for (const auto& k : kept) {
            const int h = headerIndex(k.cls, k.sub);
            if (h < 0) continue;
            headers_[static_cast<size_t>(h)].filterOn = k.filterOn;
            headers_[static_cast<size_t>(h)].collapsed = k.collapsed;
        }
        for (const auto& h : headers_) {
            Row row;
            row.spellId = -1;
            row.itemClass = h.itemClass;
            row.itemSubClass = h.itemSubClass;
            rows_.push_back(row);
        }
        applyFilters(false);
        for (size_t i = 0; i < headers_.size() && i < 32; ++i) {
            if (!headers_[i].filterOn) subClassFilter &= ~(1u << i);
            if (headers_[i].collapsed) expandMask &= ~(1u << i);
        }
        return true;
    }

    /// 0x5DCC40: which rows show, then both lists sorted. With `fromMasks`
    /// the headers' flags are first read back from the two masks, which is
    /// how every filter setter reaches it.
    void applyFilters(bool fromMasks) {
        if (fromMasks) {
            for (size_t i = 0; i < headers_.size(); ++i) {
                const uint32_t bit = i < 32 ? (1u << i) : 0u;
                headers_[i].filterOn = (bit & subClassFilter) != 0;
                headers_[i].collapsed = (bit & expandMask) == 0;
            }
        }
        for (auto& row : rows_) {
            row.visible = true;
            if (row.isHeader()) continue;
            if (!headers_.empty()) {
                int h = headerIndex(row.itemClass, row.itemSubClass);
                if (h < 0) h = 0;
                if (!headers_[static_cast<size_t>(h)].filterOn) { row.visible = false; continue; }
            }
            if ((row.slotBits & invSlotFilter) == 0) row.visible = false;
            else if (onlyMakeable && row.numAvailable == 0) row.visible = false;
            else if (onlySkillUps && row.difficulty >= 3) row.visible = false;
            else if ((!nameFilter.empty() || minLevel != 0 || maxLevel != 0) && !matches(row))
                row.visible = false;
        }
        if (!headers_.empty()) {
            for (auto& hr : rows_) {
                if (!hr.isHeader()) continue;
                const int h = headerIndex(hr.itemClass, hr.itemSubClass);
                const bool collapsed = h >= 0 && headers_[static_cast<size_t>(h)].collapsed;
                bool any = false;
                for (auto& r : rows_) {
                    if (&r == &hr || r.itemClass != hr.itemClass || r.itemSubClass != hr.itemSubClass ||
                        !r.visible)
                        continue;
                    any = true;
                    if (collapsed) r.visible = false;
                }
                if (!any) hr.visible = false;
            }
        }
        sortHeaders();
        sortRows();
        visible_ = 0;
        for (const auto& r : rows_) if (r.visible) ++visible_;
    }

    /// The header list's names, for the header order (0x5DA6B0) and the
    /// subclass dropdown. Set by the owner before a rebuild.
    std::vector<std::pair<std::pair<int, int>, std::string>> headerNames;
    std::string headerName(int cls, int sub) const {
        for (const auto& [key, name] : headerNames)
            if (key.first == cls && key.second == sub) return name;
        return {};
    }

    int headerIndex(int cls, int sub) const {
        for (size_t i = 0; i < headers_.size(); ++i)
            if (headers_[i].itemClass == cls && headers_[i].itemSubClass == sub) return static_cast<int>(i);
        return -1;
    }
    /// 0x5DA7D0: the header index behind a header row, -1 for anything else.
    int headerIndexOfRow(size_t row) const {
        if (row >= rows_.size() || !rows_[row].isHeader()) return -1;
        return headerIndex(rows_[row].itemClass, rows_[row].itemSubClass);
    }
    /// 0x5DA830, negated: a header row whose header is open.
    bool rowExpanded(size_t row) const {
        const int h = headerIndexOfRow(row);
        return h >= 0 && h < 32 && (expandMask & (1u << h)) != 0;
    }

    // ---- selection ---------------------------------------------------------
    /// SelectTradeSkill (0x5DA900): the selection is the recipe, not the row,
    /// so it follows the recipe through a re-sort; a header selects nothing.
    void select(int index1) {
        const size_t i = static_cast<size_t>(index1 - 1);
        if (index1 < 1 || i >= rows_.size() || rows_[i].spellId < 1) { selectedSpell = 0; return; }
        selectedSpell = rows_[i].spellId;
    }
    /// GetTradeSkillSelectionIndex (0x5DA670 + 1): 0 when nothing.
    int selectionIndex() const {
        if (selectedSpell == 0) return 0;
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].spellId == selectedSpell) return static_cast<int>(i) + 1;
        return 0;
    }
    /// GetFirstTradeSkill (0x5DAB70): the first shown recipe, 0 when none.
    int firstRecipe() const {
        for (size_t i = 0; i < visible_ && i < rows_.size(); ++i)
            if (rows_[i].spellId > 0) return static_cast<int>(i) + 1;
        return 0;
    }
    const Row* rowAt(int index1) const {
        if (index1 < 1 || static_cast<size_t>(index1) > rows_.size()) return nullptr;
        return &rows_[static_cast<size_t>(index1) - 1];
    }
    const Recipe* recipeAt(int index1) const {
        const Row* r = rowAt(index1);
        if (!r || r->recipe < 0 || static_cast<size_t>(r->recipe) >= recipes_.size()) return nullptr;
        return &recipes_[static_cast<size_t>(r->recipe)];
    }

    // ---- filter setters (each answers whether TRADE_SKILL_UPDATE is due) ----
    /// The slot bits the slot dropdown lists: every row's, narrowed to the
    /// picked subclass when there is one (0x5DACE0).
    uint32_t listedSlotBits() const {
        if (currentSubClass >= 0 && static_cast<size_t>(currentSubClass) < headers_.size())
            return allSlotBits & headers_[static_cast<size_t>(currentSubClass)].slotBits;
        return allSlotBits;
    }
    /// The n-th (0-based) listed slot's bit number, -1 when there is none.
    int listedSlotBit(int n) const {
        const uint32_t bits = listedSlotBits();
        int seen = 0;
        for (int b = 0; b < kNumSlotBits; ++b) {
            if (!(bits & (1u << b))) continue;
            if (seen++ == n) return b;
        }
        return -1;
    }
    /// SetTradeSkillSubClassFilter (0x5DD0F0); index1 0 is "all".
    void setSubClassFilter(int index1, bool on, bool exclusive) {
        const int i = index1 - 1;
        if (i < 0) {
            subClassFilter = 0xFFFFFFFFu;
            currentSubClass = -1;
        } else {
            const uint32_t bit = i < 32 ? (1u << i) : 0u;
            subClassFilter = !on ? (subClassFilter & ~bit) : exclusive ? bit : (subClassFilter | bit);
            currentSubClass = i;
        }
        applyFilters(true);
    }
    bool subClassFilterOn(int index1) const {
        const int i = index1 - 1;
        if (i < 0) {
            for (size_t h = 0; h < headers_.size() && h < 32; ++h)
                if (!(subClassFilter & (1u << h))) return false;
            return true;
        }
        return i < 32 && (subClassFilter & (1u << i)) != 0;
    }
    /// SetTradeSkillInvSlotFilter (0x5DD230); false for a slot not listed.
    bool setInvSlotFilter(int index1, bool on, bool exclusive) {
        if (index1 - 1 < 0) {
            invSlotFilter = 0xFFFFFFFFu;
        } else {
            const int b = listedSlotBit(index1 - 1);
            if (b < 0) return false;
            const uint32_t bit = 1u << b;
            invSlotFilter = !on ? (invSlotFilter & ~bit) : exclusive ? bit : (invSlotFilter | bit);
        }
        applyFilters(true);
        return true;
    }
    /// GetTradeSkillInvSlotFilter (0x5DBF90): "all" is the whole mask; a
    /// single slot reads nil while nothing is narrowed.
    bool invSlotFilterOn(int index1) const {
        if (index1 - 1 < 0) return invSlotFilter == 0xFFFFFFFFu;
        if (invSlotFilter == 0xFFFFFFFFu) return false;
        const int b = listedSlotBit(index1 - 1);
        return b >= 0 && (invSlotFilter & (1u << b)) != 0;
    }
    /// Collapse/ExpandTradeSkillSubClass (0x5DD460 / 0x5DD500): by row, 0 for
    /// all. False when the row is not a header (the stock call raises).
    bool setExpanded(int index1, bool expanded) {
        if (index1 - 1 < 0) {
            expandMask = expanded ? 0xFFFFFFFFu : 0u;
        } else {
            const int h = headerIndexOfRow(static_cast<size_t>(index1 - 1));
            if (h < 0) return false;
            const uint32_t bit = h < 32 ? (1u << h) : 0u;
            expandMask = expanded ? (expandMask | bit) : (expandMask & ~bit);
        }
        applyFilters(true);
        return true;
    }
    /// SetTradeSkillItemNameFilter (0x5DCF60): true when it changed, which
    /// fires TRADE_SKILL_FILTER_UPDATE.
    bool setNameFilter(const std::string& text) {
        const std::string want = lowered(text);
        if (want == nameFilter) return false;
        nameFilter = want;
        applyFilters(true);
        return true;
    }
    bool setLevelFilter(int lo, int hi) {
        if (lo == minLevel && hi == maxLevel) return false;
        minLevel = lo;
        maxLevel = hi;
        applyFilters(true);
        return true;
    }
    bool setOnlyMakeable(bool on) {
        if (on == onlyMakeable) return false;
        onlyMakeable = on;
        applyFilters(true);
        return true;
    }
    bool setOnlySkillUps(bool on) {
        if (on == onlySkillUps) return false;
        onlySkillUps = on;
        applyFilters(true);
        return true;
    }

    // ---- repeat -------------------------------------------------------------
    /// GetTradeskillRepeatCount (0x5DA270): what is left of the selected
    /// recipe's run, one when it is not running.
    int repeatCount() const {
        int n = 1;
        if (repeatSpell != 0 && repeatSpell == selectedSpell) n = repeatLeft;
        else if (pendingSpell != 0 && pendingSpell == selectedSpell) n = pendingCount;
        return n < 2 ? 1 : n;
    }
    /// DoTradeSkill's count (0x5DAC00): never more than the bags make.
    static int clampCount(int count, int numAvailable) {
        if (numAvailable != -1 && numAvailable <= count) return numAvailable;
        return count;
    }
    /// 0x5D9FA0: a running craft takes the new count; anything else waits for
    /// its cast to start.
    void requestRepeat(int32_t spell, int count) {
        if (spell == repeatSpell) { repeatLeft = count; return; }
        pendingSpell = spell;
        pendingCount = count;
    }
    /// 0x5DA0F0, the player's cast starting.
    void onCastStart(int32_t spell) {
        if (spell == pendingSpell && pendingSpell != 0) {
            repeatSpell = pendingSpell;
            repeatLeft = pendingCount;
        }
        pendingSpell = 0;
        pendingCount = 0;
    }
    enum class GoResult { None, Done, CastAgain, Cleared };
    /// 0x5DA070, the player's cast going off.
    GoResult onCastGo(int32_t spell) {
        if (repeatSpell == 0) {
            if (pendingSpell == 0) return GoResult::None;
        } else if (repeatLeft != 0 && repeatSpell == spell) {
            if (--repeatLeft == 0) { repeatSpell = 0; return GoResult::Done; }
            return GoResult::CastAgain;
        }
        repeatSpell = 0;
        pendingSpell = 0;
        return GoResult::Cleared;
    }
    /// 0x5DA020, a cast failing: true when the event is due.
    bool onCastFailed(int32_t spell) {
        bool changed = false;
        if (spell == repeatSpell && repeatSpell != 0) { repeatSpell = 0; changed = true; }
        if (spell == pendingSpell && pendingSpell != 0) { pendingSpell = 0; changed = true; }
        return changed;
    }
    /// StopTradeSkillRepeat and every other stop (0x5D9FF0): true when the
    /// event is due.
    bool stopRepeat() {
        if (repeatSpell == 0 && pendingSpell == 0) return false;
        repeatSpell = 0;
        pendingSpell = 0;
        return true;
    }

private:
    bool matches(const Row& row) const {
        if (row.recipe < 0 || static_cast<size_t>(row.recipe) >= recipes_.size()) return false;
        const Recipe& r = recipes_[static_cast<size_t>(row.recipe)];
        if (r.requiredLevel >= 0 &&
            ((minLevel != 0 && r.requiredLevel < minLevel) || (maxLevel != 0 && maxLevel < r.requiredLevel)))
            return false;
        if (nameFilter.empty()) return true;
        for (const auto& t : r.searchText)
            if (t.find(nameFilter) != std::string::npos) return true;
        return false;
    }

    void sortHeaders() {
        if (headers_.empty()) return;
        std::stable_sort(headers_.begin(), headers_.end(), [this](const Header& a, const Header& b) {
            if (a.itemClass != b.itemClass) return a.itemClass < b.itemClass;
            return headerName(a.itemClass, a.itemSubClass) < headerName(b.itemClass, b.itemSubClass);
        });
    }

    void sortRows() {
        // 0x5DAE60: shown rows first, by their header's place; the header
        // before its recipes; then orange to grey, the higher TrivialHigh, the
        // higher item level, the name.
        auto shown = [this](const Row& r, int& h) {
            h = headerIndex(r.itemClass, r.itemSubClass);
            return r.visible && h >= 0 && headers_[static_cast<size_t>(h)].filterOn;
        };
        std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
            if (!headers_.empty()) {
                int ha = 0, hb = 0;
                const bool sa = shown(a, ha), sb = shown(b, hb);
                if (sa != sb) return sa;
                if (!sa) return false;
                if (ha != hb) return ha < hb;
            }
            if (a.isHeader() != b.isHeader()) return a.isHeader();
            if (a.isHeader()) return false;
            if (a.difficulty != b.difficulty) return a.difficulty < b.difficulty;
            if (a.trivialHigh != b.trivialHigh) return a.trivialHigh > b.trivialHigh;
            if (a.itemLevel != b.itemLevel) return a.itemLevel > b.itemLevel;
            const std::string& na = recipes_[static_cast<size_t>(a.recipe)].name;
            const std::string& nb = recipes_[static_cast<size_t>(b.recipe)].name;
            return na < nb;
        });
    }

    std::vector<Recipe> recipes_;
    std::vector<Header> headers_;
    std::vector<Row> rows_;
    size_t visible_ = 0;
    bool pending_ = false;
};

}  // namespace stock_trade_skill
}  // namespace game
}  // namespace wowee
