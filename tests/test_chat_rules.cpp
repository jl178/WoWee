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

// ---------------------------------------------------------------------------
// include/game/chat_social_rules.hpp: /who, text emotes, drunk speech, the
// mature language filter, /random, comprehension and zone channel names.
#include "game/chat_social_rules.hpp"

namespace {
std::vector<WhoNamedRow> whoZones() {
    return {{12, "Elwynn Forest"}, {1519, "Stormwind City"}, {40, "Westfall"}, {14, "Durotar"}};
}
std::vector<WhoNamedRow> whoRaces() {
    return {{1, "Human"}, {2, "Orc"}, {3, "Dwarf"}, {4, "Night Elf"}, {10, "Blood Elf"}};
}
std::vector<WhoNamedRow> whoClasses() {
    return {{1, "Warrior"}, {2, "Paladin"}, {5, "Priest"}, {8, "Mage"}};
}
WhoFilter who(const char* text) { return parseWhoFilter(text, whoZones(), whoRaces(), whoClasses()); }
}  // namespace

TEST_CASE("SendWho: an empty filter asks for everyone", "[chat][who]") {
    const WhoFilter f = who("");
    CHECK(f.minLevel == 0);
    CHECK(f.maxLevel == 100);
    CHECK(f.raceMask == 0xFFFFFFFFu);
    CHECK(f.classMask == 0xFFFFFFFFu);
    CHECK(f.zones.empty());
    CHECK(f.strings.empty());
}

TEST_CASE("SendWho: levels", "[chat][who]") {
    CHECK(who("70").minLevel == 70);
    CHECK(who("70").maxLevel == 70);
    CHECK(who("10-20").minLevel == 10);
    CHECK(who("10-20").maxLevel == 20);
    CHECK(who("10-").maxLevel == 100);
    CHECK(who("-20").minLevel == 0);
    CHECK(who("-20").maxLevel == 20);
    // "-" alone is a word like any other.
    CHECK(who("-").strings == std::vector<std::string>{"-"});
}

TEST_CASE("SendWho: tags, any case, and the free words", "[chat][who]") {
    const WhoFilter f = who("N-Bob g-\"The Guild\" 60 orc");
    CHECK(f.name == "Bob");
    CHECK(f.guild == "The Guild");
    CHECK(f.minLevel == 60);
    CHECK(f.strings == std::vector<std::string>{"orc"});
    // r- and c- are masks of every row whose name holds the text.
    CHECK(who("r-elf").raceMask == ((1u << 4) | (1u << 10)));
    CHECK(who("r-orc r-human").raceMask == ((1u << 2) | (1u << 1)));
    CHECK(who("c-pri").classMask == (1u << 5));
    CHECK(who("r-gnome").raceMask == 0);
    // z- is up to ten top-level zones whose name holds it; none is zone 0.
    CHECK(who("z-storm").zones == std::vector<uint32_t>{1519});
    CHECK(who("z-o").zones == std::vector<uint32_t>{12, 1519, 14});
    CHECK(who("z-nowhere").zones == std::vector<uint32_t>{0});
}

TEST_CASE("SendWho: four free words, then the parse stops", "[chat][who]") {
    const WhoFilter f = who("a b c d e 60");
    CHECK(f.strings == std::vector<std::string>{"a", "b", "c", "d"});
    CHECK(f.minLevel == 0);
    // Quoted white space stays in the word; an unclosed quote loses it.
    CHECK(who("\"Stormwind City\"").strings == std::vector<std::string>{"Stormwind City"});
    CHECK(who("\"open").strings.empty());
}

TEST_CASE("SMSG_WHO goes to chat for three rows or fewer", "[chat][who]") {
    CHECK(whoResultsToChat(false, 0));
    CHECK(whoResultsToChat(false, 3));
    CHECK_FALSE(whoResultsToChat(false, 4));
    CHECK_FALSE(whoResultsToChat(true, 1));
}

namespace {
// EmotesText 3.3.5a: WAVE, and ANGRY with its female forms.
std::array<std::string, 16> waveTexts() {
    return {"%s waves at %s.", "%s waves at you.", "You wave at %s.", "", "%s waves.", "",
            "You wave.", "", "", "", "", "", "", "", "", ""};
}
std::array<std::string, 16> angryTexts() {
    return {"%s raises his fist in anger at %s.", "%s raises his fist in anger at you.",
            "You raise your fist in anger at %s.", "", "%s raises his fist in anger.", "",
            "You raise your fist in anger.", "", "%s raises her fist in anger at %s.",
            "%s raises her fist in anger at you.", "", "", "%s raises her fist in anger.", "", "",
            ""};
}
std::string emoteLine(const std::array<std::string, 16>& t, bool female, bool me, bool atMe,
                      const std::string& sender, const std::string& target) {
    const int slot = textEmoteSlot(t, female, me, atMe, target.empty());
    if (slot < 0) return "<none>";
    return formatTextEmote(t[static_cast<size_t>(slot)], me, atMe, sender, target);
}
}  // namespace

TEST_CASE("Text emotes: the slot for each reader", "[chat][emote]") {
    const auto w = waveTexts();
    CHECK(emoteLine(w, false, true, false, "Me", "Bob") == "You wave at Bob.");
    CHECK(emoteLine(w, false, true, false, "Me", "") == "You wave.");
    CHECK(emoteLine(w, false, false, true, "Bob", "Me") == "Bob waves at you.");
    CHECK(emoteLine(w, false, false, false, "Bob", "Ann") == "Bob waves at Ann.");
    CHECK(emoteLine(w, false, false, false, "Bob", "") == "Bob waves.");
    // No female forms: the male ones.
    CHECK(emoteLine(w, true, false, false, "Ann", "") == "Ann waves.");
}

TEST_CASE("Text emotes: the female forms, and the fall back to no target", "[chat][emote]") {
    const auto a = angryTexts();
    CHECK(emoteLine(a, true, false, false, "Ann", "Bob") == "Ann raises her fist in anger at Bob.");
    CHECK(emoteLine(a, true, false, true, "Ann", "Me") == "Ann raises her fist in anger at you.");
    CHECK(emoteLine(a, true, false, false, "Ann", "") == "Ann raises her fist in anger.");
    CHECK(emoteLine(a, false, false, false, "Bob", "Ann") == "Bob raises his fist in anger at Ann.");
    // The sender's own female slot is empty: the male one.
    CHECK(emoteLine(a, true, true, false, "Me", "Bob") == "You raise your fist in anger at Bob.");
    // Only an "at you" form missing: the line without a target.
    std::array<std::string, 16> t{};
    t[4] = "%s nods.";
    CHECK(emoteLine(t, false, false, true, "Bob", "Me") == "Bob nods.");
    // Nothing at all: no line.
    CHECK(emoteLine(std::array<std::string, 16>{}, false, false, false, "Bob", "") == "<none>");
}

TEST_CASE("DoEmote's refusals from EmoteFlags", "[chat][emote]") {
    CHECK(emoteGate(0x0, 0, 0) == EmoteGate::Allowed);
    CHECK(emoteGate(0x4000, 0, 0x1) == EmoteGate::Moving);        // forward
    CHECK(emoteGate(0x4000, 0, 0x0) == EmoteGate::Allowed);
    CHECK(emoteGate(0x1, 1, 0) == EmoteGate::Refused);            // stand only, sitting
    CHECK(emoteGate(0x0, 3, 0) == EmoteGate::Refused);            // asleep
    CHECK(emoteGate(0x200, 3, 0) == EmoteGate::Allowed);
    CHECK(emoteGate(0x400, 0, 0) == EmoteGate::Refused);
    CHECK(emoteGate(0x80, 0, 0x200000) == EmoteGate::Refused);    // swimming
}

TEST_CASE("Drunk speech: s at a word's ends, colours left alone", "[chat][drunk]") {
    auto always = [] { return 0.0f; };   // every draw passes
    auto never = [] { return 0.99f; };
    auto clean = [](const std::string&) { return false; };
    // Factor 1: every candidate s slurs, and the hic always comes.
    CHECK(slurSpeech("sit his seat", 0.5f, never, clean) == "sit his seat");
    CHECK(slurSpeech("sit his seat", 0.5f, always, clean) == "shit hish sheat ...hic!");
    // A word starting "ss" is not touched at its start, though its last s
    // (before a space) is; a lone s at the very end is a word start.
    CHECK(slurSpeech("ss s", 0.5f, always, clean) == "ssh sh ...hic!");
    // The filter vetoes the start: "shit" from "sit".
    auto profane = [](const std::string& w) { return w == "shit"; };
    CHECK(slurSpeech("sit sat", 0.5f, always, profane) == "sit shat ...hic!");
    // A colour run is copied through to its |r.
    CHECK(slurSpeech("|cff00ff00sss is|r so", 0.5f, always, clean) ==
          "|cff00ff00sss is|r sho ...hic!");
    CHECK(drunkFactor(0, 0) == 0.0f);
    CHECK(drunkFactor(50, 10) == Catch::Approx(0.5f));
    CHECK(drunkFactor(10, 200) == Catch::Approx(1.0f));
}

TEST_CASE("The mature language filter", "[chat][profanity]") {
    ProfanityFilter f;
    REQUIRE(f.add("shit"));
    REQUIRE(f.add("bia?ts?ch"));
    REQUIRE(f.add("\\<anal\\>"));   // PCRE: \< is a literal <
    CHECK(f.apply("oh SHIT no") == "oh !@#$ no");
    // The mask's position carries on from one match to the next.
    CHECK(f.apply("shit") == "%^&*");
    CHECK(f.apply("analysis") == "analysis");
    CHECK(f.apply("<anal>") == "!@#$%^");
    // Not inside a colour run.
    CHECK(f.apply("|cffffffffshit|r") == "|cffffffffshit|r");
    CHECK(ProfanityFilter::toEcmaScript("\\<a\\.b\\>") == "<a\\.b>");
    CHECK(profanityFilteredType(0x01));
    CHECK(profanityFilteredType(0x11));
    CHECK_FALSE(profanityFilteredType(0x00));
    CHECK_FALSE(profanityFilteredType(0x0C));
}

TEST_CASE("RandomRoll's range", "[chat][roll]") {
    CHECK(randomRollRange(1, 100) == std::make_pair(1, 100));
    CHECK(randomRollRange(0, 5) == std::make_pair(0, 5));
    CHECK_FALSE(randomRollRange(0, 0));
    CHECK_FALSE(randomRollRange(-1, 5));
    CHECK_FALSE(randomRollRange(10, 5));
    CHECK(randomRollRange(1, 1000000));
    CHECK_FALSE(randomRollRange(1, 1000001));
}

TEST_CASE("Language comprehension: the skill, or the aura for a language not had", "[chat][language]") {
    CHECK(languageComprehensionFor(true, 300, false, false) == 300);
    CHECK(languageComprehensionFor(true, 75, true, true) == 75);
    CHECK(languageComprehensionFor(false, 0, true, true) == 300);
    CHECK(languageComprehensionFor(false, 0, false, true) == 0);
    CHECK(languageComprehensionFor(false, 0, true, false) == 0);
}

TEST_CASE("Zone channel names", "[chat][channels]") {
    // General: the zone. Trade (0x3B) and GuildRecruitment (0x20032): "City"
    // in a capital (0x100), the zone elsewhere.
    CHECK(zoneChannelName("General - %s", 0x80003, "Elwynn Forest", 0x40, "City") ==
          "General - Elwynn Forest");
    CHECK(zoneChannelName("Trade - %s", 0x3B, "Stormwind City", 0x200138, "City") == "Trade - City");
    CHECK(zoneChannelName("Trade - %s", 0x3B, "Shattrath City", 0x4D28, "City") == "Trade - City");
    CHECK(zoneChannelName("Trade - %s", 0x3B, "Elwynn Forest", 0x40, "City") ==
          "Trade - Elwynn Forest");
    CHECK(zoneChannelName("General - %s", 0x80003, "Stormwind City", 0x200138, "City") ==
          "General - Stormwind City");
    CHECK(zoneChannelName("LookingForGroup", 0x40039, "Durotar", 0x40, "City") == "LookingForGroup");
    CHECK(zoneChannelName("General - %s", 0x80003, "", 0, "City").empty());
    CHECK((0x80003u & kChannelJoinedByDefault) != 0);
    CHECK((0x10004u & kChannelJoinedByDefault) == 0);   // WorldDefense
}
