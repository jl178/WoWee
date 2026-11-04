#pragma once

// The client-side chat rules of Wow.exe 3.3.5a (build 12340), as pure
// functions so they can be pinned by tests. docs/re/chat-combatlog.md has the
// addresses and the reasoning; every function names the one it copies.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wowee {
namespace game {
namespace chat_rules {

// ---------------------------------------------------------------------------
// UTF-8 (0x47BD40): one code point and its byte length. End of text is
// kEnd; a malformed sequence is kInvalid (its length counts the bytes read).
inline constexpr uint32_t kEnd = 0xFFFFFFFFu;
inline constexpr uint32_t kInvalid = 0x80000000u;

inline uint32_t decodeUtf8(const char* text, int* length) {
    if (length) *length = 0;
    if (!text) return kEnd;
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    uint32_t c = *p;
    if (c == 0) return kEnd;
    if (length) ++*length;
    int extra;
    if ((c & 0xFE) == 0xFC)      { c &= 0x01; extra = 5; }
    else if ((c & 0xFC) == 0xF8) { c &= 0x03; extra = 4; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else return (c & 0x80) ? kInvalid : c;
    for (int i = 0; i < extra; ++i) {
        const uint32_t b = *++p;
        if (b == 0) return kEnd;
        if (length) ++*length;
        if ((b & 0xC0) != 0x80) return kInvalid;
        c = (c << 6) | (b & 0x3F);
    }
    return c;
}

/// Whether a line is UTF-8 text (0x7E0EC0). A received line that is not is
/// dropped before anything sees it (0x509DD0); SendChatMessage refuses one
/// with "Chat message must be UTF-8 text" (0x50D170).
inline bool isUtf8Text(std::string_view text) {
    const std::string s(text);
    const char* p = s.c_str();
    for (;;) {
        int len = 0;
        const uint32_t c = decodeUtf8(p, &len);
        if (c == kEnd) return true;
        if (c == kInvalid) return false;
        p += len;
    }
}

// ---------------------------------------------------------------------------
// Storm's SStrHash (0x76F340) with flags 0: case-folded to upper, '/' read as
// '\', seed 0x7FED7FED. The language scrambler hashes each word with it.
inline uint32_t sstrHash(std::string_view text) {
    static constexpr uint32_t kTable[16] = {
        0x486E26EE, 0xDCAA16B3, 0xE1918EEF, 0x202DAFDB, 0x341C7DC7, 0x1C365303,
        0x40EF2D37, 0x65FD5E49, 0xD6057177, 0x904ECE93, 0x1C38024F, 0x98FD323B,
        0xE3061AE7, 0xA39B0FA1, 0x9797F25F, 0xE4444563,
    };
    uint32_t seed = 0x7FED7FEDu;
    uint32_t shift = 0xEEEEEEEEu;
    for (const char ch : text) {
        uint32_t c = static_cast<unsigned char>(ch);
        if (c == 0) break;
        if (c - 0x61u < 0x1Au) c -= 0x20;
        if (c == 0x2F) c = 0x5C;
        seed = (seed + shift) ^ (kTable[c >> 4] - kTable[c & 0xF]);
        shift = shift * 33u + c + 3u + seed;
    }
    return seed == 0 ? 1u : seed;
}

// ---------------------------------------------------------------------------
// Character classes of the scrambler.

/// 0x7E0F30: A-Z, a-z and the Latin-1 letters.
inline bool isLetter(uint32_t c) {
    if (c - 0x41u <= 0x19u || c - 0x61u <= 0x19u) return true;
    if (c - 0xC0u <= 0x1Du && c != 0xD7) return true;
    if (c == 0xDF) return true;
    if (c - 0xE0u <= 0x1Fu && c != 0xF7 && c != 0xFE) return true;
    return false;
}
/// 0x7E0F10.
inline bool isDigit(uint32_t c) { return c - 0x30u < 10u; }
/// A character a word is made of: letter, digit, apostrophe, or anything
/// past Latin-1 (0x505930).
inline bool isWordChar(uint32_t c) {
    return c == kEnd ? false : (isLetter(c) || isDigit(c) || c == 0x27 || c >= 0x100);
}
/// 0x7E10F0 - true for everything that is not a lower-case letter, which is
/// how the replacement word copies the original's capitals.
inline bool isNotLower(uint32_t c) {
    const uint16_t s = static_cast<uint16_t>(c);
    if (static_cast<uint16_t>(s - 0x61u) <= 0x19u) return false;
    if (static_cast<uint16_t>(s - 0xE0u) <= 0x1Eu) return false;
    if (static_cast<uint16_t>(s - 0x430u) <= 0x1Fu) return false;
    return s != 0x451;
}
/// 0x7E1130 / 0x7E1180 on a replacement byte (signed, as the client reads it).
inline char toUpperByte(char ch) {
    const int16_t s = static_cast<int16_t>(static_cast<signed char>(ch));
    const uint16_t u = static_cast<uint16_t>(s);
    if (static_cast<uint16_t>(u - 0x61u) <= 0x19u || static_cast<uint16_t>(u - 0xE0u) <= 0x1Eu)
        return static_cast<char>(s - 0x20);
    return ch;
}
inline char toLowerByte(char ch) {
    const int16_t s = static_cast<int16_t>(static_cast<signed char>(ch));
    const uint16_t u = static_cast<uint16_t>(s);
    if (static_cast<uint16_t>(u - 0x41u) <= 0x19u || static_cast<uint16_t>(u - 0xC0u) <= 0x1Eu)
        return static_cast<char>(s + 0x20);
    return ch;
}

/// The text of a line with its markup taken out (0x7E1680): colour codes and
/// |r go, a hyperlink's |H...|h header and the brackets of its text go, a
/// texture |T...|t goes whole; "||" is kept as it is.
inline std::string stripMarkup(std::string_view in) {
    std::string out;
    bool inLink = false;
    size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];
        if (c != '|') {
            if (inLink && (c == '[' || c == ']')) { ++i; continue; }
            out.push_back(c);
            ++i;
            continue;
        }
        const char next = i + 1 < in.size() ? in[i + 1] : '\0';
        switch (next) {
            case 'c': case 'C': {
                if (i + 2 + 8 > in.size()) return out;
                i += 2 + 8;
                break;
            }
            case 'H': {
                const size_t end = in.find("|h", i + 1);
                inLink = true;
                if (end == std::string_view::npos) return out;
                i = end + 2;
                break;
            }
            case 'T': {
                const size_t end = in.find("|t", i + 1);
                if (end == std::string_view::npos) return out;
                i = end + 2;
                break;
            }
            case 'h': inLink = false; i += 2; break;
            case 'r': i += 2; break;
            default:
                out.push_back('|');
                if (next == '\0') return out;
                out.push_back(next);
                i += 2;
                break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Language scrambling (0x505930, with the word table built at 0x50EDD0).

/// LanguageWords.dbc keyed the way the client keys it: by language and the
/// word's byte length, in file order.
class LanguageWords {
public:
    void add(uint32_t language, std::string word) {
        if (word.empty()) return;
        const auto len = static_cast<uint32_t>(std::strlen(word.c_str()));
        byKey_[{language, len}].push_back(std::move(word));
    }
    [[nodiscard]] const std::vector<std::string>* find(uint32_t language, uint32_t length) const {
        auto it = byKey_.find({language, length});
        return it == byKey_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] bool empty() const { return byKey_.empty(); }
private:
    std::map<std::pair<uint32_t, uint32_t>, std::vector<std::string>> byKey_;
};

/// How much of a language a reader follows, 0..300 (0x6E0640: the skill of
/// the language's skill line). At 300 or more the line is untouched.
inline constexpr uint32_t kFullComprehension = 300;

/// A line as a reader with `comprehension` of `language` sees it (0x505930,
/// enUS path: called with keepTags = 0 and keepPunctuation = 0).
///
/// Language 0 or full comprehension: the text as sent. Otherwise markup is
/// stripped, every run of separators becomes one space, and each word whose
/// SStrHash % 300 is not below the comprehension is replaced by a word of the
/// language - the one at hash % count among those of min(length, 18) bytes,
/// or the next shorter length that has any; none at all drops the word. The
/// replacement takes the original's capitals, one per code point.
inline std::string garbleText(std::string_view text, uint32_t language, uint32_t comprehension,
                              const LanguageWords& words) {
    if (language == 0 || comprehension >= kFullComprehension) return std::string(text);
    const std::string stripped = stripMarkup(text);
    const char* p = stripped.c_str();
    std::string out;
    for (;;) {
        // The separators before a word: one space for the whole run.
        bool spaced = false;
        int len = 0;
        uint32_t c = decodeUtf8(p, &len);
        while (c != kEnd && !isWordChar(c)) {
            if (!spaced) { out.push_back(' '); spaced = true; }
            p += len;
            c = decodeUtf8(p, &len);
        }
        if (c == kEnd) return out;
        const char* start = p;
        while (c != kEnd && isWordChar(c)) {
            p += len;
            c = decodeUtf8(p, &len);
        }
        size_t wordBytes = static_cast<size_t>(p - start);
        if (wordBytes > 0x400) wordBytes = 0x400;
        const std::string word(start, wordBytes);
        const uint32_t hash = sstrHash(word);
        if (hash % 300u < comprehension) { out += word; continue; }
        uint32_t want = static_cast<uint32_t>(wordBytes > 18 ? 18 : wordBytes);
        const std::vector<std::string>* list = words.find(language, want);
        while (!list && want > 1) list = words.find(language, --want);
        if (!list || list->empty()) continue;
        const std::string& repl = (*list)[hash % list->size()];
        // 0x4FB410: one replacement byte per original code point, upper case
        // where the original is not lower case.
        const char* orig = word.c_str();
        size_t r = 0;
        while (r < repl.size()) {
            int olen = 0;
            const uint32_t oc = decodeUtf8(orig, &olen);
            if (oc == kEnd) break;
            out.push_back(isNotLower(oc) ? toUpperByte(repl[r]) : toLowerByte(repl[r]));
            orig += olen;
            ++r;
        }
    }
}

/// Languages.dbc id -> its SkillLine.dbc row (category 10, "Language: ...").
/// The client reaches the skill through the language spell's SkillLineAbility
/// row; these are the rows that chain gives.
inline uint32_t languageSkillLine(uint32_t language) {
    switch (language) {
        case 1:  return 109;  // Orcish
        case 2:  return 113;  // Darnassian
        case 3:  return 115;  // Taurahe
        case 6:  return 111;  // Dwarvish
        case 7:  return 98;   // Common
        case 8:  return 139;  // Demonic
        case 9:  return 140;  // Titan
        case 10: return 137;  // Thalassian
        case 11: return 138;  // Draconic
        case 12: return 141;  // Kalimag (Old Tongue)
        case 13: return 313;  // Gnomish
        case 14: return 315;  // Troll
        case 33: return 673;  // Gutterspeak
        case 35: return 759;  // Draenei
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// SMSG_MESSAGECHAT (0x50BE70, 0x509DD0, 0x4FDBC0).

/// The client's guid tests: a player has the high nibble clear (and is not
/// zero); a pet's high word is 0xF04xxxxx.
inline bool isPlayerGuid(uint64_t guid) {
    const auto hi = static_cast<uint32_t>(guid >> 32);
    return (hi & 0xF0000000u) == 0 && (static_cast<uint32_t>(guid) != 0 || (hi & 0xF07FFFFFu) != 0);
}
inline bool isPetGuid(uint64_t guid) {
    return (static_cast<uint32_t>(guid >> 32) & 0xF0F00000u) == 0xF0400000u;
}

/// arg6 of CHAT_MSG_*: the one flag word for the chat tag. DEV wins over GM,
/// GM over DND, DND over AFK (0x50BE70).
inline const char* chatFlagName(uint8_t tag) {
    if (tag & 0x10) return "DEV";
    if (tag & 0x04) return "GM";
    if (tag & 0x02) return "DND";
    if (tag & 0x01) return "AFK";
    return "";
}

/// Chat types whose language the client forces to 0 before anything else:
/// emote, monster emote, system and raid boss emote (0x509DD0).
inline bool languageIgnored(uint8_t type) {
    return type == 0x0A || type == 0x10 || type == 0x00 || type == 0x29;
}

/// Types whose line goes through the $-token parser (0x579590): the monster
/// and boss lines, the battleground system lines and the achievement ones.
inline bool resolvesTextTokens(uint8_t type) {
    switch (type) {
        case 0x0C: case 0x0E: case 0x29: case 0x2A: case 0x10: case 0x0F:
        case 0x0D: case 0x24: case 0x25: case 0x26: case 0x30: case 0x31:
            return true;
        default:
            return false;
    }
}

/// An empty say, yell or emote is printed as the stock "unknown" emote
/// (0x509DD0): CHAT_SAY_UNKNOWN / CHAT_YELL_UNKNOWN become an EMOTE line,
/// CHAT_EMOTE_UNKNOWN stays one. Returns nullptr for any other type.
inline const char* unknownSpeechKey(uint8_t type) {
    switch (type) {
        case 0x01: return "CHAT_SAY_UNKNOWN";
        case 0x06: return "CHAT_YELL_UNKNOWN";
        case 0x0A: return "CHAT_EMOTE_UNKNOWN";
        default:   return nullptr;
    }
}

/// CHAT_MSG_ADDON's distribution word (0x509DD0). OFFICER, RAID_LEADER and
/// PARTY_LEADER are not in the client's switch and come out UNKNOWN.
inline const char* addonDistribution(uint8_t type) {
    switch (type) {
        case 0x02: return "PARTY";
        case 0x03: return "RAID";
        case 0x04: return "GUILD";
        case 0x07: case 0x08: case 0x2F: return "WHISPER";
        case 0x2C: return "BATTLEGROUND";
        default:   return "UNKNOWN";
    }
}

/// An addon line's prefix and body: split at the first tab; with no tab the
/// whole text is the prefix and the body is empty (0x509DD0).
inline std::pair<std::string, std::string> splitAddonMessage(std::string_view text) {
    const size_t tab = text.find('\t');
    if (tab == std::string_view::npos) return {std::string(text), std::string()};
    return {std::string(text.substr(0, tab)), std::string(text.substr(tab + 1))};
}

/// The talking animation a say/yell/party line starts on its speaker
/// (0x509DD0 -> 0x73AD70): 4 when the whole line is one of LAUGH_WORD1..n,
/// otherwise for say 1 when it ends in '?', 2 in '!', else 0; 3 for a yell.
/// Party lines only laugh. -1 means no animation.
inline int talkAnimationKind(uint8_t type, std::string_view text,
                             const std::vector<std::string>& laughWords) {
    if (type != 0x01 && type != 0x06 && type != 0x02 && type != 0x33) return -1;
    auto iequal = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char x = a[i], y = b[i];
            if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
            if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
            if (x != y) return false;
        }
        return true;
    };
    for (const auto& w : laughWords)
        if (!w.empty() && iequal(w, text)) return 4;
    if (type != 0x01 && type != 0x06) return -1;
    if (type == 0x06) return 3;
    if (!text.empty() && text.back() == '?') return 1;
    if (!text.empty() && text.back() == '!') return 2;
    return 0;
}

// ---------------------------------------------------------------------------
// Quest links (0x509DD0 with 0x61E540, 0x5E2860).

/// GetQuestGreenRange (0x5E2860): a table by level / 5, capped at row 19.
inline int questGreenRange(int playerLevel) {
    static constexpr int kRange[20] = {4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
    int row = playerLevel / 5;
    if (row < 0) row = 0;
    if (row > 19) row = 19;
    return kRange[row];
}

/// The difficulty colour code for a quest of this level (0x61E540).
inline const char* questDifficultyColor(int questLevel, int playerLevel) {
    const int diff = questLevel - playerLevel;
    if (diff > 4) return "|cffff2020";
    if (diff > 2) return "|cffff8040";
    if (diff > -3) return "|cffffff00";
    const int range = questGreenRange(playerLevel);
    return (-diff > range) ? "|cff808080" : "|cff40c040";
}

/// Every received quest link's colour rewritten for the reader (0x509DD0):
/// the ten bytes before "|Hquest:" are replaced with the colour for the
/// link's level, and a level of -1 means the reader's own.
inline std::string recolorQuestLinks(std::string text, int playerLevel) {
    size_t at = text.find("|Hquest:");
    while (at != std::string::npos) {
        if (at >= 10 && text[at - 1] != '|') {
            const size_t c1 = text.find(':', at);
            const size_t c2 = c1 == std::string::npos ? c1 : text.find(':', c1 + 1);
            if (c2 != std::string::npos) {
                int level = std::atoi(text.c_str() + c2 + 1);
                if (level == -1) level = playerLevel;
                const char* color = questDifficultyColor(level, playerLevel);
                text.replace(at - 10, 10, color);
            }
        }
        at = text.find("|Hquest:", at + 8);
    }
    return text;
}

// ---------------------------------------------------------------------------
// SendChatMessage (0x50D170).

/// The chat type a SendChatMessage type word names (0x4FBB20); 0 when the
/// word is not one of the client's.
inline uint8_t chatTypeFromWord(std::string_view word) {
    static constexpr std::pair<uint8_t, const char*> kTypes[] = {
        {0x01, "SAY"}, {0x02, "PARTY"}, {0x03, "RAID"}, {0x04, "GUILD"}, {0x05, "OFFICER"},
        {0x06, "YELL"}, {0x07, "WHISPER"}, {0x0A, "EMOTE"}, {0x11, "CHANNEL"}, {0x17, "AFK"},
        {0x18, "DND"}, {0x28, "RAID_WARNING"}, {0x2C, "BATTLEGROUND"}, {0x2F, "BN"},
        {0x35, "BN_WHISPER"}, {0x36, "BN_WHISPER_INFORM"}, {0x37, "BN_CONVERSATION"},
    };
    for (const auto& [id, name] : kTypes) {
        if (word.size() != std::strlen(name)) continue;
        bool same = true;
        for (size_t i = 0; i < word.size() && same; ++i) {
            char a = word[i];
            if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 32);
            same = a == name[i];
        }
        if (same) return id;
    }
    return 0;
}

/// The escape-code check (0x50D170): every '|' must start "||" or a colour
/// "|c...", and a colour runs to its "|r" (anything between, links included,
/// is not looked at). A colour with no "|r" after it cuts the line there.
/// False is the Lua error "Invalid escape code in chat message".
inline bool checkOutgoingEscapes(std::string& text) {
    size_t at = text.find('|');
    while (at != std::string::npos) {
        const char next = at + 1 < text.size() ? text[at + 1] : '\0';
        size_t resume;
        if (next == '|') {
            resume = at + 2;
        } else if (next == 'c') {
            const size_t end = text.find("|r", at);
            if (end == std::string::npos) { text.resize(at); return true; }
            resume = end + 2;
        } else {
            return false;
        }
        at = resume < text.size() ? text.find('|', resume) : std::string::npos;
    }
    return true;
}

/// A line stops at its first line break (0x7E1640): '\r', '\n', or the two
/// characters "\n" or "|n".
inline void cutAtLineBreak(std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char n = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '\r' || c == '\n' || ((c == '\\' || c == '|') && n == 'n')) {
            text.resize(i);
            return;
        }
    }
}

/// %t / %n (target) and %f (focus) expanded, everything else kept, the whole
/// held to 255 bytes (0x577D00). The names are what the caller knows them
/// as; empty means there is none, which writes the token-not-found string.
inline std::string expandChatTokens(std::string_view text, const std::string& targetName,
                                    const std::string& focusName,
                                    const std::string& noTarget = "<no target>",
                                    const std::string& noFocus = "<no focus>") {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t pct = text.find('%', i);
        if (pct == std::string_view::npos || pct + 1 >= text.size()) {
            out.append(text.substr(i));
            break;
        }
        out.append(text.substr(i, pct - i));
        const char k = text[pct + 1];
        if (k == 't' || k == 'T' || k == 'n' || k == 'N') {
            out += targetName.empty() ? noTarget : targetName;
            i = pct + 2;
        } else if (k == 'f' || k == 'F') {
            out += focusName.empty() ? noFocus : focusName;
            i = pct + 2;
        } else {
            out.push_back('%');
            i = pct + 1;
        }
    }
    if (out.size() > 255) out.resize(255);
    return out;
}

/// The client's own send throttles (0x50D170, 0x5098B0, 0x5097D0, 0x4FB8F0):
/// ten whispers to people who have not whispered recently, and ten lines per
/// channel, in any ten seconds. A send over either is held, not refused, and
/// goes out once the oldest of the ten is ten seconds old.
class ChatSendThrottle {
public:
    static constexpr uint32_t kWindowMs = 10000;
    static constexpr size_t kSlots = 10;

    /// Whether a whisper sent now has to wait (0x4FB8F0).
    [[nodiscard]] bool whisperHeld(uint32_t nowMs) const {
        const uint32_t oldest = whispers_[whisperNext_];
        return oldest != 0 && static_cast<int32_t>(nowMs - oldest - kWindowMs) < 0;
    }
    void recordWhisper(uint32_t nowMs) {
        whispers_[whisperNext_] = nowMs;
        whisperNext_ = (whisperNext_ + 1) % kSlots;
    }
    /// Whether a line to this channel has to wait (0x502F60).
    [[nodiscard]] bool channelHeld(const std::string& channel, uint32_t nowMs) const {
        auto it = channels_.find(upper(channel));
        if (it == channels_.end()) return false;
        const uint32_t oldest = it->second.times[it->second.next];
        return oldest != 0 && static_cast<int32_t>(nowMs - oldest - kWindowMs) < 0;
    }
    void recordChannel(const std::string& channel, uint32_t nowMs) {
        Ring& r = channels_[upper(channel)];
        r.times[r.next] = nowMs;
        r.next = (r.next + 1) % kSlots;
    }
private:
    struct Ring { std::array<uint32_t, kSlots> times{}; size_t next = 0; };
    static std::string upper(std::string s) {
        for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        return s;
    }
    std::array<uint32_t, kSlots> whispers_{};
    size_t whisperNext_ = 0;
    std::map<std::string, Ring> channels_;
};

/// The sixteen players who whispered last, newest first (0x4FB710). A whisper
/// to one of them is not throttled (0x4FB6A0).
class RecentWhisperers {
public:
    void note(uint64_t guid) {
        if (guid == 0) return;
        size_t at = 0;
        while (at < list_.size() && list_[at] != guid) ++at;
        if (at > 14) at = 15;
        for (size_t i = at; i > 0; --i) list_[i] = list_[i - 1];
        list_[0] = guid;
    }
    [[nodiscard]] const std::array<uint64_t, 16>& guids() const { return list_; }
    [[nodiscard]] bool contains(uint64_t guid) const {
        if (guid == 0) return false;
        for (const uint64_t g : list_) if (g == guid) return true;
        return false;
    }
private:
    std::array<uint64_t, 16> list_{};
};

}  // namespace chat_rules
}  // namespace game
}  // namespace wowee
