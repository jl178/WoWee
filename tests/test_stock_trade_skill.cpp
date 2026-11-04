// The stock 3.3.5a trade skill window's model (game/stock_trade_skill.hpp):
// the difficulty bands, the slot bits, the headers and their order, the
// filters, the selection that follows its recipe, the repeat and the trade
// link's bits. docs/re/professions.md has the addresses.
#include <catch_amalgamated.hpp>
#include "game/stock_trade_skill.hpp"

#include <string>
#include <vector>

namespace ts = wowee::game::stock_trade_skill;

namespace {

ts::Recipe item(int32_t spell, const std::string& name, int cls, int sub, int high, int low,
                int avail = -1, uint32_t invType = 0, int ilvl = 1) {
    ts::Recipe r;
    r.spellId = spell;
    r.name = name;
    r.trivialHigh = high;
    r.trivialLow = low;
    r.createsItem = true;
    r.itemKnown = true;
    r.itemClass = cls;
    r.itemSubClass = sub;
    r.itemLevel = ilvl;
    r.requiredLevel = ilvl;
    r.slotBits = ts::slotBitsForCreatedItem(invType);
    r.numAvailable = avail;
    r.searchText = {ts::lowered(name)};
    return r;
}

ts::Recipe enchant(int32_t spell, const std::string& name, int high, int low, uint32_t slotBits) {
    ts::Recipe r;
    r.spellId = spell;
    r.name = name;
    r.trivialHigh = high;
    r.trivialLow = low;
    r.slotBits = slotBits;
    r.searchText = {ts::lowered(name)};
    return r;
}

ts::Model tailoring() {
    ts::Model m;
    m.headerNames = {{{4, 1}, "Cloth"}, {{1, 0}, "Bag"}, {{7, 5}, "Cloth (Trade Goods)"}};
    m.beginOpen(3908, 197);
    std::vector<ts::Recipe> rs = {
        item(2963, "Bolt of Linen Cloth", 7, 5, 25, 0, 4),
        item(2386, "Linen Boots", 4, 1, 65, 50, 0, 8, 13),
        item(3755, "Linen Bag", 1, 0, 70, 55, 1, 18, 15),
        item(2393, "White Linen Shirt", 4, 1, 35, 0, 2, 4, 3),
    };
    m.rebuild(rs, 60);
    m.finishOpen();
    return m;
}

std::vector<std::string> shown(const ts::Model& m) {
    std::vector<std::string> out;
    for (size_t i = 0; i < m.numVisible(); ++i) {
        const auto& r = m.rows()[i];
        if (r.isHeader()) out.push_back("# " + m.headerName(r.itemClass, r.itemSubClass));
        else out.push_back(m.recipes()[static_cast<size_t>(r.recipe)].name);
    }
    return out;
}

}  // namespace

TEST_CASE("difficulty bands as 0x5DD5A0 colours them", "[trade_skill]") {
    // Linen Boots: High 65, Low 50 -> orange < 50 <= yellow < 57 <= green < 65 <= grey.
    CHECK(ts::difficulty(49, 65, 50) == 0);
    CHECK(ts::difficulty(50, 65, 50) == 1);
    CHECK(ts::difficulty(56, 65, 50) == 1);
    CHECK(ts::difficulty(57, 65, 50) == 2);
    CHECK(ts::difficulty(64, 65, 50) == 2);
    CHECK(ts::difficulty(65, 65, 50) == 3);
    // A Low of zero is High - 25.
    CHECK(ts::difficulty(9, 35, 0) == 0);
    CHECK(ts::difficulty(10, 35, 0) == 1);
    CHECK(ts::difficulty(22, 35, 0) == 2);
    CHECK(ts::difficulty(35, 35, 0) == 3);
    // Neither threshold: grey.
    CHECK(ts::difficulty(1, 0, 0) == 3);
    CHECK(std::string(ts::difficultyName(0)) == "optimal");
    CHECK(std::string(ts::difficultyName(3)) == "trivial");
}

TEST_CASE("slot bits from 0xA2D288 and the enchant rule", "[trade_skill]") {
    CHECK(ts::slotBitsForCreatedItem(1) == 0x1u);        // head
    CHECK(ts::slotBitsForCreatedItem(11) == 0x400u);     // a ring: the first finger
    CHECK(ts::slotBitsForCreatedItem(12) == 0x1000u);    // a trinket: the first
    CHECK(ts::slotBitsForCreatedItem(18) == 0x80000u);   // a bag: the first bag
    CHECK(ts::slotBitsForCreatedItem(20) == 0x10u);      // a robe is chest
    CHECK(ts::slotBitsForCreatedItem(0) == ts::kNonEquipSlotBit);
    CHECK(ts::slotBitsForEnchant(2, 0x1, 0) == 0x800000u);     // one-hand
    CHECK(ts::slotBitsForEnchant(2, 0x2, 0) == 0x1000000u);    // two-hand
    CHECK(ts::slotBitsForEnchant(4, 0, 1u << 9) == 0x100u);    // bracer (wrists)
    CHECK(ts::slotBitsForEnchant(4, 0x40, 0) == 0x2000000u);   // shield
    CHECK(ts::slotBitsForEnchant(-1, 0, 0) == ts::kNonEquipSlotBit);
    CHECK(std::string(ts::slotBitKey(9)) == "HANDSSLOT");
}

TEST_CASE("headers by item class, then name; recipes orange first", "[trade_skill]") {
    auto m = tailoring();
    REQUIRE(m.headers().size() == 3);
    // Container (1) before Armor (4) before Trade Goods (7).
    CHECK(shown(m) == std::vector<std::string>{
                          "# Bag", "Linen Bag",
                          "# Cloth", "Linen Boots", "White Linen Shirt",
                          "# Cloth (Trade Goods)", "Bolt of Linen Cloth"});
    // Rank 60: the bag (70/55) is yellow, the boots (65/50) green, the
    // shirt (35) and the bolt (25) grey.
    CHECK(m.rows()[1].difficulty == 1);
    CHECK(m.rows()[3].difficulty == 2);
    CHECK(m.firstRecipe() == 2);
}

TEST_CASE("enchants file under the line's own header", "[trade_skill]") {
    ts::Model m;
    m.headerNames = {{{-1, -1}, "Enchant"}};
    m.beginOpen(7411, 333);
    m.rebuild({enchant(7418, "Enchant Bracer - Minor Health", 70, 50, 0x100),
               enchant(7420, "Enchant Chest - Minor Health", 70, 50, 0x10)},
              1);
    REQUIRE(m.headers().size() == 1);
    CHECK(m.rows()[0].isHeader());
    CHECK(m.rows()[0].itemClass == -1);
    CHECK(m.numVisible() == 3);
    CHECK(m.allSlotBits == 0x110u);
}

TEST_CASE("the list waits for the items it files by", "[trade_skill]") {
    ts::Model m;
    m.beginOpen(3908, 197);
    auto r = item(2963, "Bolt of Linen Cloth", 7, 5, 25, 0);
    r.itemKnown = false;
    CHECK_FALSE(m.rebuild({r}, 1));
    CHECK(m.pending());
    CHECK(m.numVisible() == 0);
    r.itemKnown = true;
    CHECK(m.rebuild({r}, 1));
    CHECK(m.numVisible() == 2);
}

TEST_CASE("collapse, expand and the subclass filter", "[trade_skill]") {
    auto m = tailoring();
    // Fold "Cloth" (row 3).
    REQUIRE(m.setExpanded(3, false));
    CHECK(shown(m) == std::vector<std::string>{"# Bag", "Linen Bag", "# Cloth",
                                               "# Cloth (Trade Goods)", "Bolt of Linen Cloth"});
    CHECK_FALSE(m.rowExpanded(2));
    CHECK_FALSE(m.setExpanded(2, true));  // not a header row
    REQUIRE(m.setExpanded(0, true));      // all
    CHECK(m.numVisible() == 7);
    // Only the second header ("Cloth").
    m.setSubClassFilter(2, true, true);
    CHECK(shown(m) == std::vector<std::string>{"# Cloth", "Linen Boots", "White Linen Shirt"});
    CHECK(m.subClassFilterOn(2));
    CHECK_FALSE(m.subClassFilterOn(1));
    CHECK_FALSE(m.subClassFilterOn(0));
    m.setSubClassFilter(0, true, true);
    CHECK(m.subClassFilterOn(0));
    CHECK(m.numVisible() == 7);
}

TEST_CASE("the folding survives a rebuild by header", "[trade_skill]") {
    auto m = tailoring();
    REQUIRE(m.setExpanded(3, false));
    std::vector<ts::Recipe> rs = {
        item(2963, "Bolt of Linen Cloth", 7, 5, 25, 0, 4),
        item(2386, "Linen Boots", 4, 1, 65, 50, 0, 8, 13),
        item(3755, "Linen Bag", 1, 0, 70, 55, 1, 18, 15),
        item(2393, "White Linen Shirt", 4, 1, 35, 0, 2, 4, 3),
        item(2394, "Linen Cloak", 4, 1, 60, 45, 0, 16, 10),
    };
    REQUIRE(m.rebuild(rs, 60));
    CHECK(shown(m) == std::vector<std::string>{"# Bag", "Linen Bag", "# Cloth",
                                               "# Cloth (Trade Goods)", "Bolt of Linen Cloth"});
}

TEST_CASE("slot, makeable, skill-up, name and level filters", "[trade_skill]") {
    auto m = tailoring();
    // Listed slots: chest-ish shirt (8), feet (0x80), bag (0x80000), nothing.
    CHECK(m.listedSlotBits() == (0x8u | 0x80u | 0x80000u | ts::kNonEquipSlotBit));
    // The second listed slot is FEETSLOT (bit 7).
    CHECK(m.listedSlotBit(1) == 7);
    REQUIRE(m.setInvSlotFilter(2, true, true));
    CHECK(shown(m) == std::vector<std::string>{"# Cloth", "Linen Boots"});
    CHECK(m.invSlotFilterOn(2));
    CHECK_FALSE(m.invSlotFilterOn(0));
    REQUIRE(m.setInvSlotFilter(0, true, true));
    CHECK(m.invSlotFilterOn(0));
    CHECK_FALSE(m.invSlotFilterOn(2));  // nil while nothing is narrowed

    // Have Materials hides 0 but not -1.
    CHECK(m.setOnlyMakeable(true));
    CHECK(shown(m) == std::vector<std::string>{"# Bag", "Linen Bag", "# Cloth", "White Linen Shirt",
                                               "# Cloth (Trade Goods)", "Bolt of Linen Cloth"});
    CHECK_FALSE(m.setOnlyMakeable(true));
    m.setOnlyMakeable(false);

    CHECK(m.setOnlySkillUps(true));
    CHECK(shown(m) == std::vector<std::string>{"# Bag", "Linen Bag", "# Cloth", "Linen Boots"});
    m.setOnlySkillUps(false);

    CHECK(m.setNameFilter("SHIRT"));
    CHECK(shown(m) == std::vector<std::string>{"# Cloth", "White Linen Shirt"});
    CHECK_FALSE(m.setNameFilter("shirt"));
    m.setNameFilter("");

    CHECK(m.setLevelFilter(10, 14));
    CHECK(shown(m) == std::vector<std::string>{"# Cloth", "Linen Boots"});
}

TEST_CASE("the selection follows its recipe", "[trade_skill]") {
    auto m = tailoring();
    m.select(4);  // Linen Boots
    CHECK(m.selectedSpell == 2386);
    CHECK(m.selectionIndex() == 4);
    m.setExpanded(1, false);  // fold "Bag": the boots move up one
    CHECK(m.selectionIndex() == 3);
    m.select(1);  // a header selects nothing
    CHECK(m.selectedSpell == 0);
    CHECK(m.selectionIndex() == 0);
}

TEST_CASE("a new line opens unfiltered; the same line keeps its filters", "[trade_skill]") {
    auto m = tailoring();
    m.setNameFilter("linen");
    m.select(2);
    m.close();
    m.beginOpen(3908, 197);
    CHECK(m.nameFilter == "linen");
    CHECK(m.selectedSpell == 3755);
    m.close();
    m.beginOpen(2550, 185);
    CHECK(m.nameFilter.empty());
    CHECK(m.selectedSpell == 0);
}

TEST_CASE("DoTradeSkill's run", "[trade_skill]") {
    ts::Model m;
    CHECK(ts::Model::clampCount(10, 4) == 4);
    CHECK(ts::Model::clampCount(3, 4) == 3);
    CHECK(ts::Model::clampCount(5, -1) == 5);
    m.selectedSpell = 2963;
    m.requestRepeat(2963, 3);
    CHECK(m.repeatCount() == 3);         // asked for, not started
    m.onCastStart(2963);
    CHECK(m.repeatSpell == 2963);
    CHECK(m.repeatLeft == 3);
    CHECK(m.onCastGo(2963) == ts::Model::GoResult::CastAgain);
    CHECK(m.repeatCount() == 2);
    m.onCastStart(2963);                 // the next cast: nothing pending
    CHECK(m.repeatLeft == 2);
    m.requestRepeat(2963, 5);            // running: the count moves, no new run
    CHECK(m.repeatLeft == 5);
    CHECK(m.pendingSpell == 0);
    CHECK(m.onCastGo(2963) == ts::Model::GoResult::CastAgain);
    CHECK(m.onCastFailed(2963));
    CHECK(m.repeatSpell == 0);
    CHECK(m.repeatCount() == 1);
    // The last one ends it with no event.
    m.requestRepeat(2963, 1);
    m.onCastStart(2963);
    CHECK(m.onCastGo(2963) == ts::Model::GoResult::Done);
    CHECK(m.repeatSpell == 0);
    // Another spell going off clears a run.
    m.requestRepeat(2963, 4);
    m.onCastStart(2963);
    CHECK(m.onCastGo(133) == ts::Model::GoResult::Cleared);
    CHECK_FALSE(m.stopRepeat());
}

TEST_CASE("trade link bits and text", "[trade_skill]") {
    std::vector<bool> bits(14, false);
    bits[0] = true;
    bits[6] = true;
    bits[13] = true;
    const std::string data = ts::encodeTradeLinkBits(bits);
    CHECK(data == "BBC");
    CHECK(ts::tradeLinkDataLength(14) == 3);
    CHECK(ts::decodeTradeLinkBits(data, 14) == bits);
    ts::TradeLink tl;
    REQUIRE(ts::parseTradeLink("|cffffd000|Htrade:3908:75:150:9CE:BBC|h[Tailoring]|h|r", tl));
    CHECK(tl.spellId == 3908);
    CHECK(tl.rank == 75);
    CHECK(tl.maxRank == 150);
    CHECK(tl.guid == 0x9CE);
    CHECK(tl.data == "BBC");
    CHECK_FALSE(ts::parseTradeLink("item:25:0", tl));
}
