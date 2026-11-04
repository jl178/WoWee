// Stock 3.3.5a arena client rules; see include/game/arena_rules.hpp and
// docs/re/pvp-instances.md §3.
#include "game/arena_rules.hpp"

#include "network/packet.hpp"

namespace wowee::game::arena {

namespace {

std::string token(const char* prefix, size_t slot) {
    return std::string(prefix) + std::to_string(slot + 1);
}

}  // namespace

// 0x54B840: the player slot takes the guid; "seen", and "unseen" too when
// there is no object for it.
std::vector<OpponentEvent> Opponents::assignPlayer(uint64_t guid, size_t slot,
                                                   const Present& present) {
    players_[slot] = guid;
    std::vector<OpponentEvent> out{{token("arena", slot), "seen"}};
    if (!present || !present(guid)) out.push_back({token("arena", slot), "unseen"});
    return out;
}

// 0x54B920: the pet slot takes the pet and its owner.
std::vector<OpponentEvent> Opponents::assignPet(uint64_t pet, uint64_t owner, size_t slot,
                                                const Present& present) {
    pets_[slot] = {pet, owner};
    std::vector<OpponentEvent> out{{token("arenapet", slot), "seen"}};
    if (!present || !present(pet)) out.push_back({token("arenapet", slot), "unseen"});
    return out;
}

std::vector<OpponentEvent> Opponents::playerSeen(uint64_t guid, const Present& present) {
    if (guid == 0) return {};
    // A pet already seen with this owner keeps them together.
    for (size_t i = 0; i < kMaxOpponents; ++i)
        if (pets_[i].owner == guid) return assignPlayer(guid, i, present);
    for (size_t i = 0; i < kMaxOpponents; ++i) {
        if (pets_[i].pet != 0) continue;
        if (players_[i] == guid || players_[i] == 0) return assignPlayer(guid, i, present);
    }
    return {};
}

std::vector<OpponentEvent> Opponents::petSeen(uint64_t pet, uint64_t owner, const Present& present) {
    if (pet == 0) return {};
    for (size_t i = 0; i < kMaxOpponents; ++i) {
        if (players_[i] == owner || pets_[i].owner == owner) return assignPet(pet, owner, i, present);
    }
    for (size_t i = 0; i < kMaxOpponents; ++i) {
        if (players_[i] == 0 && pets_[i].pet == 0) return assignPet(pet, owner, i, present);
    }
    return {};
}

std::vector<OpponentEvent> Opponents::unseen(uint64_t guid, const Present& present,
                                             const PetOf& petOf) const {
    if (guid == 0) return {};
    for (size_t i = 0; i < kMaxOpponents; ++i) {
        if (players_[i] == guid) return {{token("arena", i), "unseen"}};
        if (players_[i] != 0 && present && present(players_[i]) && petOf &&
            petOf(players_[i]) == guid)
            return {{token("arenapet", i), "unseen"}};
    }
    for (size_t i = 0; i < kMaxOpponents; ++i)
        if (pets_[i].pet == guid) return {{token("arenapet", i), "unseen"}};
    return {};
}

std::vector<OpponentEvent> Opponents::destroyed(uint64_t guid, const Present& present,
                                                const PetOf& petOf) {
    if (guid == 0) return {};
    for (size_t i = 0; i < kMaxOpponents; ++i) {
        if (players_[i] == guid) return {{token("arena", i), "destroyed"}};
        if (players_[i] != 0 && present && present(players_[i]) && petOf &&
            petOf(players_[i]) == guid)
            return {{token("arenapet", i), "destroyed"}};
    }
    for (size_t i = 0; i < kMaxOpponents; ++i) {
        if (pets_[i].pet == guid) {
            pets_[i] = PetSlot{};
            return {{token("arenapet", i), "destroyed"}};
        }
    }
    return {};
}

std::vector<OpponentEvent> Opponents::clear() {
    players_.fill(0);
    pets_.fill(PetSlot{});
    std::vector<OpponentEvent> out;
    for (size_t i = 0; i < kMaxOpponents; ++i) {
        out.push_back({token("arena", i), "cleared"});
        out.push_back({token("arenapet", i), "cleared"});
    }
    return out;
}

uint32_t Opponents::count() const {
    uint32_t n = 0;
    while (n < kMaxOpponents && (players_[n] != 0 || pets_[n].pet != 0)) ++n;
    return n;
}

uint64_t Opponents::pet(size_t index, const Present& present, const PetOf& petOf) const {
    if (index >= kMaxOpponents) return 0;
    if (players_[index] != 0 && present && present(players_[index]))
        return petOf ? petOf(players_[index]) : 0;
    return pets_[index].pet;
}

int Opponents::slotOf(uint64_t guid) const {
    if (guid == 0) return -1;
    for (size_t i = 0; i < kMaxOpponents; ++i)
        if (players_[i] == guid || pets_[i].pet == guid) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------------------
// Arena teams
// ---------------------------------------------------------------------------

TeamInfo teamInfoFromFields(const std::array<uint32_t, kTeamInfoFields>& f) {
    TeamInfo t;
    t.teamId = f[0];
    t.type = f[1];
    t.member = f[2];
    t.gamesWeek = f[3];
    t.gamesSeason = f[4];
    t.winsSeason = f[5];
    t.personalRating = f[6];
    return t;
}

bool parseTeamQuery(network::Packet& packet, TeamQuery& out) {
    out = TeamQuery{};
    if (!packet.hasRemaining(4)) return false;
    out.teamId = packet.readUInt32();
    out.name = packet.readString();
    if (!packet.hasRemaining(4)) return true;
    out.type = packet.readUInt32();
    if (!packet.hasRemaining(20)) return true;
    out.backgroundColor = packet.readUInt32();
    out.emblemStyle = packet.readUInt32();
    out.emblemColor = packet.readUInt32();
    out.borderStyle = packet.readUInt32();
    out.borderColor = packet.readUInt32();
    return true;
}

std::array<float, 3> colorComponents(uint32_t color) {
    constexpr float k = 1.0f / 255.0f;
    return {static_cast<float>((color >> 16) & 0xFF) * k,
            static_cast<float>((color >> 8) & 0xFF) * k,
            static_cast<float>(color & 0xFF) * k};
}

bool parseInspectTeam(network::Packet& packet, uint64_t& guid, InspectTeam& out) {
    out = InspectTeam{};
    if (!packet.hasRemaining(9)) return false;
    guid = packet.readUInt64();
    out.slot = packet.readUInt8();
    if (!packet.hasRemaining(24)) return false;
    out.teamId = packet.readUInt32();
    out.rating = packet.readUInt32();
    out.seasonGames = packet.readUInt32();
    out.seasonWins = packet.readUInt32();
    out.playerSeasonGames = packet.readUInt32();
    out.personalRating = packet.readUInt32();
    return true;
}

}  // namespace wowee::game::arena
