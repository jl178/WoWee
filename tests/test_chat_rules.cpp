// The client's own chat rules (Wow.exe 3.3.5a, docs/re/chat-combatlog.md):
// the language scrambler, markup, the send checks and throttles, the event
// words, quest-link colours.
#include <catch_amalgamated.hpp>

#include "game/chat_rules.hpp"

using namespace wowee::game::chat_rules;

namespace {
// LanguageWords.dbc 3.3.5a rows, in file order, for the lengths the cases use.
LanguageWords stockWords() {
    LanguageWords w;
    for (const char* s : {"Lok", "Tar", "Kaz", "Ruk", "Kek", "Mog", "Zug", "Gul", "Nuk", "Aaz",
                          "Kil", "Ogg"})
        w.add(1, s);
    for (const char* s : {"Regas", "Nogah", "Kazum", "Magan", "No'bu", "Golar", "Throm", "Zugas",
                          "Re'ka", "No'ku", "Ro'th"})
        w.add(1, s);
    for (const char* s : {"Vil", "Bor", "Ras", "Gol", "Nud", "Far", "Wos", "Mod", "Ver", "Ash",
                          "Lon", "Bur", "Hir"})
        w.add(7, s);
    for (const char* s : {"Wirsh", "Novas", "Regen", "Gloin", "Tiras", "Barad", "Garde", "Majis",
                          "Ergin", "Nagan", "Algos", "Eynes", "Borne", "Melka"})
        w.add(7, s);
    return w;
}
}  // namespace

TEST_CASE("SStrHash of a word, case-folded", "[chat][language]") {
    CHECK(sstrHash("lol") % 300 == 256);
    CHECK(sstrHash("LOL") == sstrHash("lol"));
    CHECK(sstrHash("hello") % 300 == 91);
    CHECK(sstrHash("a/b") == sstrHash("A\\B"));
}

TEST_CASE("The famous lol: kek in Orcish, bur in Common", "[chat][language]") {
    const LanguageWords w = stockWords();
    CHECK(garbleText("lol", 1, 0, w) == "kek");
    CHECK(garbleText("lol", 7, 0, w) == "bur");
    CHECK(garbleText("LOL", 1, 0, w) == "KEK");
    CHECK(garbleText("Hello", 1, 0, w) == "Kazum");
}

TEST_CASE("Separators collapse to one space; known text is untouched", "[chat][language]") {
    const LanguageWords w = stockWords();
    CHECK(garbleText("Hello, world!", 7, 0, w) == "Majis borne ");
    CHECK(garbleText("Hello, world!", 7, 300, w) == "Hello, world!");
    CHECK(garbleText("Hello, world!", 0, 0, w) == "Hello, world!");
    // Partial skill keeps the words whose hash % 300 is below it.
    CHECK(garbleText("hello", 7, 92, w) == "hello");
    CHECK(garbleText("hello", 7, 91, w) == "majis");
}

TEST_CASE("A word with no list of its length takes a shorter one, or goes", "[chat][language]") {
    LanguageWords w;
    w.add(7, "Ab");
    CHECK(garbleText("abcdef", 7, 0, w) == "ab");
    LanguageWords none;
    CHECK(garbleText("abc def", 7, 0, none) == " ");
}

TEST_CASE("Links lose their markup before scrambling", "[chat][language]") {
    CHECK(stripMarkup("|cff1eff00|Hitem:2589:0:0:0:0:0:0:0:80|h[Linen Cloth]|h|r x") ==
          "Linen Cloth x");
    CHECK(stripMarkup("a||b |TInterface\\Icons\\X:0|t c") == "a||b  c");
}

TEST_CASE("UTF-8 check", "[chat]") {
    CHECK(isUtf8Text("plain"));
    CHECK(isUtf8Text("caf\xC3\xA9"));
    CHECK_FALSE(isUtf8Text("bad\xC3("));
    CHECK_FALSE(isUtf8Text("\x80"));
}

TEST_CASE("Chat flag word: DEV over GM over DND over AFK", "[chat]") {
    CHECK(std::string(chatFlagName(0)) == "");
    CHECK(std::string(chatFlagName(0x01)) == "AFK");
    CHECK(std::string(chatFlagName(0x03)) == "DND");
    CHECK(std::string(chatFlagName(0x07)) == "GM");
    CHECK(std::string(chatFlagName(0x14)) == "DEV");
    CHECK(std::string(chatFlagName(0x08)) == "");
}

TEST_CASE("Addon lines: split at the first tab, the client's distribution words", "[chat]") {
    CHECK(splitAddonMessage("DBM\tv1\tx") == std::make_pair(std::string("DBM"), std::string("v1\tx")));
    CHECK(splitAddonMessage("NoTab") == std::make_pair(std::string("NoTab"), std::string()));
    CHECK(std::string(addonDistribution(0x02)) == "PARTY");
    CHECK(std::string(addonDistribution(0x33)) == "UNKNOWN");
    CHECK(std::string(addonDistribution(0x05)) == "UNKNOWN");
    CHECK(std::string(addonDistribution(0x07)) == "WHISPER");
    CHECK(std::string(addonDistribution(0x2C)) == "BATTLEGROUND");
}

TEST_CASE("Guid tests the chat handler uses", "[chat]") {
    CHECK(isPlayerGuid(0x0000000000000042ULL));
    CHECK_FALSE(isPlayerGuid(0));
    CHECK_FALSE(isPlayerGuid(0xF130000000000001ULL));
    CHECK(isPetGuid(0xF140000000000001ULL));
    CHECK_FALSE(isPetGuid(0xF130000000000001ULL));
}

TEST_CASE("Talking animation on say, yell and party", "[chat]") {
    const std::vector<std::string> laugh = {"lol", "rofl", "hehe", "haha"};
    CHECK(talkAnimationKind(0x01, "hello", laugh) == 0);
    CHECK(talkAnimationKind(0x01, "really?", laugh) == 1);
    CHECK(talkAnimationKind(0x01, "now!", laugh) == 2);
    CHECK(talkAnimationKind(0x06, "now", laugh) == 3);
    CHECK(talkAnimationKind(0x01, "LOL", laugh) == 4);
    CHECK(talkAnimationKind(0x02, "haha", laugh) == 4);
    CHECK(talkAnimationKind(0x02, "hello", laugh) == -1);
    CHECK(talkAnimationKind(0x07, "lol", laugh) == -1);
}

TEST_CASE("Quest link colour for the reader", "[chat][quest]") {
    CHECK(questGreenRange(1) == 4);
    CHECK(questGreenRange(20) == 6);
    CHECK(questGreenRange(80) == 8);
    CHECK(std::string(questDifficultyColor(15, 10)) == "|cffff2020");
    CHECK(std::string(questDifficultyColor(13, 10)) == "|cffff8040");
    CHECK(std::string(questDifficultyColor(8, 10)) == "|cffffff00");
    CHECK(std::string(questDifficultyColor(7, 10)) == "|cff40c040");
    CHECK(std::string(questDifficultyColor(4, 10)) == "|cff808080");
    const std::string link = "|cffffff00|Hquest:123:5|h[A Quest]|h|r";
    CHECK(recolorQuestLinks(link, 20) == "|cff808080|Hquest:123:5|h[A Quest]|h|r");
    CHECK(recolorQuestLinks("|cff000000|Hquest:9:-1|h[Mine]|h|r", 20) ==
          "|cffffff00|Hquest:9:-1|h[Mine]|h|r");
}

TEST_CASE("SendChatMessage type words", "[chat][send]") {
    CHECK(chatTypeFromWord("SAY") == 0x01);
    CHECK(chatTypeFromWord("say") == 0x01);
    CHECK(chatTypeFromWord("RAID_WARNING") == 0x28);
    CHECK(chatTypeFromWord("CHANNEL") == 0x11);
    CHECK(chatTypeFromWord("PARTY_LEADER") == 0);
    CHECK(chatTypeFromWord("BOGUS") == 0);
}

TEST_CASE("Outgoing escape codes", "[chat][send]") {
    std::string ok = "|cff1eff00|Hitem:2589:0:0:0:0:0:0:0:80|h[Linen Cloth]|h|r and ||pipes";
    CHECK(checkOutgoingEscapes(ok));
    CHECK(ok == "|cff1eff00|Hitem:2589:0:0:0:0:0:0:0:80|h[Linen Cloth]|h|r and ||pipes");
    std::string open = "hi |cffff0000red";
    CHECK(checkOutgoingEscapes(open));
    CHECK(open == "hi ");
    std::string bad = "bare |Hitem:1|h[x]|h";
    CHECK_FALSE(checkOutgoingEscapes(bad));
    std::string tex = "|TInterface\\Icons\\x:0|t";
    CHECK_FALSE(checkOutgoingEscapes(tex));
}

TEST_CASE("A line ends at its first break", "[chat][send]") {
    std::string a = "one\ntwo";
    cutAtLineBreak(a);
    CHECK(a == "one");
    std::string b = "one|ntwo";
    cutAtLineBreak(b);
    CHECK(b == "one");
    std::string c = "one\\ntwo";
    cutAtLineBreak(c);
    CHECK(c == "one");
}

TEST_CASE("%t, %n and %f in an outgoing line", "[chat][send]") {
    CHECK(expandChatTokens("heal %t now", "Bob", "") == "heal Bob now");
    CHECK(expandChatTokens("%n and %f", "", "Al") == "<no target> and Al");
    CHECK(expandChatTokens("100% sure", "", "") == "100% sure");
    CHECK(expandChatTokens("50%%", "", "") == "50%%");
    CHECK(expandChatTokens(std::string(300, 'x'), "", "").size() == 255);
}

TEST_CASE("Ten whispers to strangers or ten lines a channel per ten seconds", "[chat][send]") {
    ChatSendThrottle t;
    for (uint32_t i = 0; i < 10; ++i) {
        CHECK_FALSE(t.whisperHeld(1000 + i));
        t.recordWhisper(1000 + i);
    }
    CHECK(t.whisperHeld(5000));
    CHECK_FALSE(t.whisperHeld(11000));
    for (uint32_t i = 0; i < 10; ++i) t.recordChannel("Trade - City", 1000 + i);
    CHECK(t.channelHeld("trade - city", 2000));
    CHECK_FALSE(t.channelHeld("General", 2000));
    CHECK_FALSE(t.channelHeld("Trade - City", 11000));
}

TEST_CASE("The sixteen who whispered last", "[chat]") {
    RecentWhisperers r;
    for (uint64_t g = 1; g <= 20; ++g) r.note(g);
    CHECK_FALSE(r.contains(1));
    CHECK(r.contains(5));
    CHECK(r.contains(20));
    r.note(5);
    CHECK(r.guids()[0] == 5);
}

TEST_CASE("Which types the client treats specially", "[chat]") {
    CHECK(languageIgnored(0x0A));
    CHECK(languageIgnored(0x29));
    CHECK_FALSE(languageIgnored(0x01));
    CHECK(resolvesTextTokens(0x0C));
    CHECK(resolvesTextTokens(0x24));
    CHECK_FALSE(resolvesTextTokens(0x01));
    CHECK(std::string(unknownSpeechKey(0x01)) == "CHAT_SAY_UNKNOWN");
    CHECK(unknownSpeechKey(0x07) == nullptr);
    CHECK(languageSkillLine(7) == 98);
    CHECK(languageSkillLine(1) == 109);
}
