#pragma once

// The rest of the client's chat-side rules from Wow.exe 3.3.5a (build 12340):
// the /who filter, text emote wording, drunk speech, the mature language
// filter, /random's range, language comprehension and the zone channel names.
// Pure functions so the tests can pin them; docs/re/chat-combatlog.md has the
// addresses and the reasoning. Every function names the one it copies.

#include "game/chat_rules.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee {
namespace game {
namespace chat_rules {

// ---------------------------------------------------------------------------
// Case-insensitive text, as the client's 0x76EA40 (compare n code points) and
// 0x76F7E0 (find) do it. ASCII letters fold; other bytes compare as they are.
inline bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (toLowerByte(a[i]) != toLowerByte(b[i])) return false;
    return true;
}

inline bool startsWithNoCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && equalsNoCase(text.substr(0, prefix.size()), prefix);
}

/// 0x76F7E0: the needle anywhere in the haystack. An empty needle is found in
/// any haystack that is not itself empty.
inline bool containsNoCase(std::string_view hay, std::string_view needle) {
    if (hay.empty()) return false;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (equalsNoCase(hay.substr(i, needle.size()), needle)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// /who - SendWho(filter), 0x6B5630.
//
// The filter is split on white space outside double quotes (the quotes go).
// A word starting with WHO_TAG_NAME "n-" is the name, "g-" the guild, "z-"
// zones (every top-level AreaTable row whose name contains the rest, at most
// ten; none found sends the one zone 0), "r-" races and "c-" classes (a mask
// of every ChrRaces / ChrClasses row whose name contains the rest). A word
// starting with a digit is a level: "10" is 10-10, "10-" 10-100, "10-20"
// 10-20; "-20" sets only the top. Anything else is one of at most four free
// strings, and the fourth ends the parse.
struct WhoNamedRow {
    uint32_t id = 0;
    std::string name;
};

struct WhoTags {
    std::string name = "n-", guild = "g-", zone = "z-", race = "r-", cls = "c-";
};

struct WhoFilter {
    uint32_t minLevel = 0;
    uint32_t maxLevel = 100;
    std::string name;
    std::string guild;
    uint32_t raceMask = 0xFFFFFFFFu;
    uint32_t classMask = 0xFFFFFFFFu;
    std::vector<uint32_t> zones;
    std::vector<std::string> strings;
};

namespace detail {
inline bool isWhoSpace(uint32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
inline bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }
}  // namespace detail

/// `zones` are AreaTable's top-level rows (ParentAreaID 0) in file order;
/// `races` and `classes` ChrRaces / ChrClasses ids and names.
inline WhoFilter parseWhoFilter(std::string_view text, const std::vector<WhoNamedRow>& zones,
                                const std::vector<WhoNamedRow>& races,
                                const std::vector<WhoNamedRow>& classes,
                                const WhoTags& tags = WhoTags{}) {
    WhoFilter f;
    // 128 bytes copied, then a space so the last word ends like the others.
    std::string buf(text.substr(0, std::min<size_t>(text.size(), 127)));
    buf.push_back(' ');

    const char* p = buf.c_str();
    int len = 0;
    while (detail::isWhoSpace(decodeUtf8(p, &len))) p += len;

    std::string token;
    bool quoted = false;
    size_t freeStrings = 0;
    for (uint32_t c = decodeUtf8(p, &len); c != kEnd; c = decodeUtf8(p, &len)) {
        if (c == '"') {
            quoted = !quoted;
            p += len;
            continue;
        }
        if (quoted || !detail::isWhoSpace(c)) {
            if (token.size() + static_cast<size_t>(len) < 128) token.append(p, static_cast<size_t>(len));
            p += len;
            continue;
        }
        // A word ends.
        p += len;
        const std::string_view w = token;
        bool consumed = true;
        if (startsWithNoCase(w, tags.name)) {
            f.name = std::string(w.substr(tags.name.size()).substr(0, 47));
        } else if (startsWithNoCase(w, tags.guild)) {
            f.guild = std::string(w.substr(tags.guild.size()).substr(0, 47));
        } else if (startsWithNoCase(w, tags.zone)) {
            const std::string_view needle = w.substr(tags.zone.size());
            for (const auto& z : zones) {
                if (f.zones.size() >= 10) break;
                if (containsNoCase(z.name, needle)) f.zones.push_back(z.id);
            }
            if (f.zones.empty()) f.zones.push_back(0);
        } else if (startsWithNoCase(w, tags.race)) {
            const std::string_view needle = w.substr(tags.race.size());
            if (f.raceMask == 0xFFFFFFFFu) f.raceMask = 0;
            for (const auto& r : races)
                if (containsNoCase(r.name, needle)) f.raceMask |= 1u << (r.id & 31);
        } else if (startsWithNoCase(w, tags.cls)) {
            const std::string_view needle = w.substr(tags.cls.size());
            if (f.classMask == 0xFFFFFFFFu) f.classMask = 0;
            for (const auto& r : classes)
                if (containsNoCase(r.name, needle)) f.classMask |= 1u << (r.id & 31);
        } else if (!w.empty() && detail::isAsciiDigit(w[0])) {
            f.minLevel = static_cast<uint32_t>(std::atoi(token.c_str()));
            f.maxLevel = f.minLevel;
            size_t i = 1;
            while (i < w.size() && detail::isAsciiDigit(w[i])) ++i;
            if (i < w.size() && w[i] == '-') { f.maxLevel = 100; ++i; }
            if (i < w.size() && detail::isAsciiDigit(w[i]))
                f.maxLevel = static_cast<uint32_t>(std::atoi(token.c_str() + i));
        } else if (!w.empty() && w[0] == '-' && w.size() > 1 && detail::isAsciiDigit(w[1])) {
            f.maxLevel = static_cast<uint32_t>(std::atoi(token.c_str() + 1));
        } else {
            consumed = false;
        }
        if (!consumed) {
            f.strings.push_back(token);
            if (++freeStrings > 3) return f;
        }
        token.clear();
        while (detail::isWhoSpace(decodeUtf8(p, &len))) p += len;
    }
    return f;
}

/// SMSG_WHO (0x6B8720): the rows go to chat as WHO_LIST_FORMAT lines and
/// WHO_NUM_RESULTS when the panel has not asked (SetWhoToUI off) and there
/// are no more than three of them; otherwise WHO_LIST_UPDATE, and nothing in
/// chat. The threshold's cvar slot (0xC7BFE8) is never filled, so it is 3.
inline bool whoResultsToChat(bool whoToUI, uint32_t count) { return !whoToUI && count <= 3; }

// ---------------------------------------------------------------------------
// Text emotes. EmotesText.dbc's sixteen EmotesTextData slots (0x503660) are
// indexed by four bits: 1 the target is the reader, 2 the sender is the
// reader, 4 there is no target, 8 the sender is female. The client asks for
// the full index, then without the female bit, then - the target dropped -
// with bit 1 cleared and bit 4 set, female and not. A row with none of
// those says nothing at all.
inline int textEmoteSlot(const std::array<std::string, 16>& texts, bool female, bool senderIsMe,
                         bool targetIsMe, bool noTarget) {
    uint32_t bits = 0;
    if (senderIsMe) bits = 2;
    else if (targetIsMe) bits = 1;
    if (noTarget) bits |= 4;
    const uint32_t fem = female ? 8u : 0u;
    const uint32_t tries[4] = {fem | bits, bits, fem | ((bits & ~1u) | 4u), (bits & ~1u) | 4u};
    for (uint32_t t : tries)
        if (!texts[t].empty()) return static_cast<int>(t);
    return -1;
}

/// The pattern's %s filled the way 0x503660 fills it: the sender's own line
/// takes the target's name, a line aimed at the reader the sender's, and
/// anyone else's the sender's and then the target's.
inline std::string formatTextEmote(std::string_view pattern, bool senderIsMe, bool targetIsMe,
                                   const std::string& sender, const std::string& target) {
    std::vector<const std::string*> args;
    if (senderIsMe) args = {&target};
    else if (targetIsMe) args = {&sender};
    else args = {&sender, &target};
    std::string out;
    size_t next = 0;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 1 < pattern.size()) {
            if (pattern[i + 1] == 's') {
                if (next < args.size()) out += *args[next];
                ++next;
                ++i;
                continue;
            }
            if (pattern[i + 1] == '%') { out.push_back('%'); ++i; continue; }
        }
        out.push_back(pattern[i]);
    }
    return out;
}

/// DoEmote's refusals (0x6DD9E0 -> 0x4F5410), from Emotes.dbc's EmoteFlags of
/// the emote the text emote plays, the player's stand state and movement
/// flags. Refused is silent; Moving is ERR_NOEMOTEWHILERUNNING. Either way
/// nothing is sent. (Flag 0x8000's mount half is not mirrored.)
enum class EmoteGate { Allowed, Refused, Moving };
inline EmoteGate emoteGate(uint32_t emoteFlags, uint8_t standState, uint32_t moveFlags) {
    if (emoteFlags & 0x400) return EmoteGate::Refused;
    if ((emoteFlags & 0x1) && standState != 0) return EmoteGate::Refused;
    if ((emoteFlags & 0x80) && (moveFlags & 0x200000)) return EmoteGate::Refused;     // swimming
    if ((emoteFlags & 0x8000) && (moveFlags & 0x2000000)) return EmoteGate::Refused;  // flying
    if (!(emoteFlags & 0x200) && (standState == 3 || standState == 7)) return EmoteGate::Refused;
    if ((emoteFlags & 0x4000) && (moveFlags & 0xC010FF)) return EmoteGate::Moving;
    return EmoteGate::Allowed;
}

// ---------------------------------------------------------------------------
// Drunk speech (0x577F60), applied by SendChatMessage after the %t tokens
// when PLAYER_BYTES_3's drunk byte or PLAYER_FAKE_INEBRIATION is above zero.
// `factor` is the larger of the two, at most 100, times 0.01 (0x4F7290).
//
// An 's' starting a word, followed by anything but a space or another s,
// becomes "sh" with chance `factor` - unless "sh" and the rest of the word is
// a word the mature language filter catches (checked even with the filter
// off). An 's' ending a word becomes "sh" with chance `factor`. A |c colour
// run is copied through to its |r untouched. Afterwards, with chance
// factor / 4, the whole line is put into SLURRED_SPEECH ("%s ...hic!").
// `rand01` returns [0, 1); the client draws it once per candidate s.
template <class Rand, class Profane>
inline std::string slurSpeech(std::string_view src, float factor, Rand&& rand01, Profane&& isProfane,
                              std::string_view hicFormat = "%s ...hic!", size_t size = 256) {
    std::string out;
    size_t room = size;
    bool wordStart = true;
    size_t i = 0;
    while (i < src.size() && src[i] != '\0' && room >= 2) {
        const char c = src[i++];
        out.push_back(c);
        --room;
        const char next = i < src.size() ? src[i] : '\0';
        if (c == ' ') {
            wordStart = true;
            continue;
        }
        if (c == 's' || c == 'S') {
            if (wordStart) {
                if (room > 1 && next != ' ' && next != 'S' && next != 's') {
                    if (rand01() < factor) {
                        std::string word = "sh";
                        for (size_t k = i; k < src.size() && src[k] != ' ' && src[k] != '\0'; ++k)
                            word.push_back(src[k]);
                        if (!isProfane(word)) {
                            out.push_back('h');
                            --room;
                        }
                    }
                }
            } else if (room > 1 && next == ' ') {
                if (rand01() < factor) {
                    out.push_back('h');
                    --room;
                }
            }
        } else if (c == '|' && next == 'c') {
            const size_t end = src.find("|r", i);
            if (end != std::string_view::npos) {
                // From the c through the |r, or what room is left.
                size_t n = (end - i) + 2;
                if (room <= n) n = room - 1;
                out.append(src.substr(i, n));
                room -= n;
                i = end + 2;
            }
        }
        wordStart = false;
    }
    if (rand01() < factor * 0.25f) {
        std::string hic;
        const size_t at = hicFormat.find("%s");
        if (at == std::string_view::npos) {
            hic = std::string(hicFormat);
        } else {
            hic = std::string(hicFormat.substr(0, at)) + out + std::string(hicFormat.substr(at + 2));
        }
        if (hic.size() > size - 1) hic.resize(size - 1);
        out = std::move(hic);
    }
    return out;
}

/// The drunk factor (0x4F7290): the larger of the two values, capped at 100,
/// as a fraction. Zero is sober and slurs nothing.
inline float drunkFactor(uint32_t drunkByte, uint32_t fakeInebriation) {
    uint32_t v = drunkByte > fakeInebriation ? drunkByte : fakeInebriation;
    if (v >= 100) v = 100;
    return static_cast<float>(v & 0xFF) * 0.01f;
}

// ---------------------------------------------------------------------------
// The mature language filter (cvar profanityFilter, on as it ships):
// ChatProfanity.dbc's rows for the client's language group (enUS is group 0;
// rows of group -1 count for every group), compiled with PCRE through its
// POSIX wrapper, caseless (0x7E2250 -> 0x83DF20). PCRE has no \< or \>, so
// those match the characters themselves - which leaves most of the English
// rows matching only text in angle brackets, and the plain ones ("shit",
// "fuck", "bia?ts?ch" ...) doing the work.
//
// Applying it (0x509A50, and inline in 0x509DD0): each pattern in turn, from
// the start; while it matches at an offset that is not inside a |c colour
// run, the matched bytes become "!@#$%^&*" in turn - the position in that
// string carries over from one replacement to the next, for the life of the
// client - and the search goes on after them.
class ProfanityFilter {
public:
    static std::string toEcmaScript(std::string_view pcre) {
        std::string out;
        for (size_t i = 0; i < pcre.size(); ++i) {
            if (pcre[i] == '\\' && i + 1 < pcre.size() && (pcre[i + 1] == '<' || pcre[i + 1] == '>')) {
                out.push_back(pcre[i + 1]);
                ++i;
                continue;
            }
            out.push_back(pcre[i]);
            if (pcre[i] == '\\' && i + 1 < pcre.size()) out.push_back(pcre[++i]);
        }
        return out;
    }

    /// False when the pattern does not compile (the client warns and skips it).
    bool add(std::string_view pattern) {
        try {
            patterns_.emplace_back(toEcmaScript(pattern),
                                   std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
            return true;
        } catch (const std::regex_error&) {
            return false;
        }
    }

    [[nodiscard]] bool empty() const { return patterns_.empty(); }
    [[nodiscard]] size_t size() const { return patterns_.size(); }

    /// 0x4FCF40: whether the first `length` bytes of `text` end inside a |c
    /// run (a |c not yet closed by |r).
    static bool insideColor(std::string_view text, size_t length) {
        bool inColor = false;
        size_t i = 0;
        while (i < length && i < text.size()) {
            if (text[i] != '|') { ++i; continue; }
            if (i + 1 >= length) break;
            const char n = text[i + 1];
            if (n == 'c' || n == 'C') inColor = true;
            else if ((n == 'r' || n == 'R') && inColor) inColor = false;
            i += 2;
        }
        return inColor;
    }

    /// The text filtered; `changed` says whether anything was replaced.
    std::string apply(std::string text, bool* changed = nullptr) const {
        if (changed) *changed = false;
        if (text.empty()) return text;
        for (const auto& re : patterns_) {
            size_t from = 0;
            while (from <= text.size()) {
                std::cmatch m;
                const char* begin = text.c_str() + from;
                if (!std::regex_search(begin, text.c_str() + text.size(), m, re)) break;
                const size_t so = static_cast<size_t>(m.position(0));
                // The client measures this from the start of the line, with
                // the offset of the match in what is left of it.
                if (insideColor(text, so)) break;
                const size_t len = static_cast<size_t>(m.length(0));
                for (size_t k = 0; k < len; ++k) {
                    text[from + so + k] = kMask[counter_];
                    counter_ = (counter_ + 1) % 8;
                }
                if (len > 0 && changed) *changed = true;
                from += so + len;
                if (len == 0) break;
            }
        }
        return text;
    }

private:
    static constexpr const char* kMask = "!@#$%^&*";
    std::vector<std::regex> patterns_;
    mutable size_t counter_ = 0;
};

/// The chat types the filter runs over (0x9FA510): say, party, raid, guild,
/// officer, yell, whisper, whisper inform, emote, channel, AFK, DND, raid
/// leader, raid warning, battleground, battleground leader, party leader.
inline bool profanityFilteredType(uint8_t type) {
    switch (type) {
        case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x09: case 0x0A: case 0x11: case 0x17: case 0x18: case 0x27: case 0x28:
        case 0x2C: case 0x2D: case 0x33:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// RandomRoll(min, max), 0x516610. Both are strings to SStrToInt. Nothing is
// sent for 0-0, a negative low end, a low end above the high one, or a high
// end above a million; otherwise MSG_RANDOM_ROLL with the two as given.
inline std::optional<std::pair<int32_t, int32_t>> randomRollRange(int32_t lo, int32_t hi) {
    if (lo == 0 && hi == 0) return std::nullopt;
    if (lo < 0) return std::nullopt;
    if (lo > hi || hi > 1000000) return std::nullopt;
    return std::make_pair(lo, hi);
}

// ---------------------------------------------------------------------------
// Language comprehension (0x6E0640), for the reader. A language whose spell
// the reader knows is understood as far as its skill goes. One the reader has
// no spell for is understood fully (300) under an aura 244
// (COMPREHEND_LANGUAGE) naming it, which the client looks for only while
// UNIT_FIELD_FLAGS_2 has 0x8 (UNIT_FLAG2_COMPREHEND_LANG); otherwise not at
// all.
inline uint32_t languageComprehensionFor(bool knowsLanguage, uint32_t skillValue,
                                         bool comprehendFlag, bool auraNamesLanguage) {
    if (knowsLanguage) return skillValue;
    if (comprehendFlag && auraNamesLanguage) return kFullComprehension;
    return 0;
}

// ---------------------------------------------------------------------------
// Zone channels. ChatChannels.dbc's flags: 0x1 joined by default (0x501F00
// puts these rows in the joined mask a new character starts with: General,
// Trade, LocalDefense, LookingForGroup), 0x2 zone dependent, 0x4 global, 0x8
// trade, 0x10 / 0x20 city only, 0x10000 defense, 0x20000 guild recruitment
// (0x507D30: joined only while the player has no guild), 0x40000 LFG.
inline constexpr uint32_t kChannelJoinedByDefault = 0x1;
inline constexpr uint32_t kChannelCityName = 0x20;
inline constexpr uint32_t kChannelGuildRecruitment = 0x20000;
inline constexpr uint32_t kAreaCapital = 0x100;

/// The name a channel is joined under (0x507A80): its pattern with the zone's
/// name - or, for a row with flag 0x20 while the zone is a capital (AreaTable
/// flag 0x100), the name of the AreaTable row with flag 0x200, "City". A
/// pattern with no %s is the name as it stands. Empty when the zone name is
/// not known - even for a pattern with no %s - which the client treats as a
/// failed join.
inline std::string zoneChannelName(std::string_view pattern, uint32_t channelFlags,
                                   const std::string& zoneName, uint32_t zoneAreaFlags,
                                   const std::string& cityName) {
    const std::string& ext = ((channelFlags & kChannelCityName) && (zoneAreaFlags & kAreaCapital))
        ? cityName : zoneName;
    if (ext.empty()) return {};
    std::string name(pattern);
    if (const size_t at = name.find("%s"); at != std::string::npos) name.replace(at, 2, ext);
    return name;
}

}  // namespace chat_rules
}  // namespace game
}  // namespace wowee
