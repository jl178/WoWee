#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace rendering {

// ============================================================================
// EmoteRegistry - extracted from AnimationController
//
// Owns all static emote data, DBC loading, emote text lookup, and
// animation ID resolution.  Singleton - loaded once on first use.
// ============================================================================

struct EmoteInfo {
    uint32_t animId = 0;
    uint32_t dbcId = 0;
    bool loop = false;
    /// Emotes.dbc EmoteSpecProc (0 one-shot, 1 stand state, 2 state loop)
    /// and its parameter (the stand state for 1).
    uint8_t specProc = 0;
    uint8_t specParam = 0;
    std::string textNoTarget;
    std::string textTarget;
    std::string othersNoTarget;
    std::string othersTarget;
    /// "%s laughs at you." - what the target reads. EmotesText's text array
    /// carries it in the slot after the bystanders' targeted form.
    std::string othersAtYou;
    std::string command;
    /// EmotesText's sixteen EmotesTextData texts in file order, the slots
    /// 0x503660 picks between (chat_rules::textEmoteSlot): bit 1 the target
    /// is the reader, 2 the sender is, 4 no target, 8 a female sender.
    std::array<std::string, 16> texts{};
    /// Emotes.dbc EmoteFlags of the emote it plays: what DoEmote refuses
    /// (0x4F5410) - 0x4000 while moving, 0x1 unless standing, and so on.
    uint32_t emoteFlags = 0;
};

class EmoteRegistry {
public:
    static EmoteRegistry& instance();

    /// Load emotes from DBC files (called once on first use).
    void loadFromDbc();

    struct EmoteResult { uint32_t animId; bool loop; };

    /// Look up an emote by chat command (e.g. "dance", "wave").
    [[nodiscard]] std::optional<EmoteResult> findEmote(const std::string& command) const;


    /// The stand state an emote command sets (Emotes.dbc EmoteSpecProc 1:
    /// /sit 1, /sleep and /laydown 3, /kneel 8, /stand 0), as the stock
    /// client sends CMSG_STANDSTATECHANGE for it (0x6DD9E0 -> 0x6DCB40).
    [[nodiscard]] std::optional<uint8_t> standStateFor(const std::string& command) const;

    /// Get the animation ID for an Emotes.dbc emote ID, as used by SMSG_EMOTE
    /// and UNIT_NPC_EMOTESTATE.
    [[nodiscard]] uint32_t animByEmotesId(uint32_t emoteId) const;

    /// True if the Emotes.dbc entry is a persistent state emote (EmoteSpecProc
    /// != 0, e.g. STATE_WORK_NOSHEATHE) rather than a one-shot (ONESHOT_WAVE).
    [[nodiscard]] bool isStateEmote(uint32_t emoteId) const;


    /// Get first-person emote text for a command.
    std::string textFor(const std::string& emoteName,
                        const std::string* targetName = nullptr) const;

    /// Get DBC ID for an emote command.
    [[nodiscard]] uint32_t dbcIdFor(const std::string& emoteName) const;

    /// Get third-person emote text by DBC ID.
    std::string textByDbcId(uint32_t dbcId,
                            const std::string& senderName,
                            const std::string* targetName = nullptr) const;

    /// The sender's own wording by DBC ID - "You laugh." / "You laugh at %s." -
    /// for the echo of the player's own SMSG_TEXT_EMOTE. Empty if unknown.
    std::string selfTextByDbcId(uint32_t dbcId,
                                const std::string* targetName = nullptr) const;

    /// Everything known about an EmotesText row, or nullptr.
    [[nodiscard]] const EmoteInfo* infoByDbcId(uint32_t dbcId) const {
        auto it = emoteByDbcId_.find(dbcId);
        return it == emoteByDbcId_.end() ? nullptr : it->second;
    }
    [[nodiscard]] const EmoteInfo* infoFor(const std::string& command) const {
        auto it = emoteTable_.find(command);
        return it == emoteTable_.end() ? nullptr : &it->second;
    }

    /// The target's wording by DBC ID - "Name laughs at you." Falls back to
    /// the bystanders' targeted form when the file has none.
    std::string atYouTextByDbcId(uint32_t dbcId, const std::string& senderName,
                                 const std::string& playerName) const;

private:
    EmoteRegistry() = default;
    EmoteRegistry(const EmoteRegistry&) = delete;
    EmoteRegistry& operator=(const EmoteRegistry&) = delete;

    void loadFallbackEmotes();
    void buildDbcIdIndex();

    bool loaded_ = false;
    std::unordered_map<std::string, EmoteInfo> emoteTable_;
    std::unordered_map<uint32_t, const EmoteInfo*> emoteByDbcId_;
    std::unordered_map<uint32_t, uint32_t> animByEmotesId_;
    std::unordered_set<uint32_t> stateEmoteIds_;
};

} // namespace rendering
} // namespace wowee
