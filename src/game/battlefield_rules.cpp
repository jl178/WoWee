// Stock 3.3.5a battleground client rules; see include/game/battlefield_rules.hpp
// and docs/re/pvp-instances.md.
#include "game/battlefield_rules.hpp"

#include "network/packet.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wowee::game::bf {

int64_t clockMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() + 1;
}

// ---------------------------------------------------------------------------
// SMSG_BATTLEFIELD_STATUS (0x54AE40)
// ---------------------------------------------------------------------------

bool parseStatus(network::Packet& packet, StatusPacket& out) {
    out = StatusPacket{};
    if (!packet.hasRemaining(4)) return false;
    out.queueSlot = packet.readUInt32();
    if (!packet.hasRemaining(8)) return false;
    out.header = packet.readUInt64();
    // STATUS_NONE is the slot and eight zero bytes, nothing more: the client
    // tests the eight for zero before reading anything else.
    if (out.header == 0) return true;
    out.teamSize = static_cast<uint8_t>(out.header & 0x7F);
    out.bgTypeId = static_cast<uint32_t>((out.header >> 16) & 0xFFFFFFFFu);
    if (!packet.hasRemaining(7)) return true;
    out.minLevel = packet.readUInt8();
    out.maxLevel = packet.readUInt8();
    out.instanceId = packet.readUInt32();
    out.rated = packet.readUInt8() != 0;
    if (!packet.hasRemaining(4)) return true;
    out.status = packet.readUInt32();
    switch (out.status) {
        case 1:
            if (packet.hasRemaining(4)) out.avgWaitMs = packet.readUInt32();
            if (packet.hasRemaining(4)) out.timeInQueueMs = packet.readUInt32();
            break;
        case 2:
            if (packet.hasRemaining(4)) out.mapId = packet.readUInt32();
            if (packet.hasRemaining(8)) packet.readUInt64();
            if (packet.hasRemaining(4)) out.inviteTimeoutMs = packet.readUInt32();
            break;
        case 3:
            if (packet.hasRemaining(4)) out.mapId = packet.readUInt32();
            if (packet.hasRemaining(8)) packet.readUInt64();
            if (packet.hasRemaining(4)) out.autoLeaveMs = packet.readUInt32();
            if (packet.hasRemaining(4)) out.elapsedMs = packet.readUInt32();
            if (packet.hasRemaining(1)) out.arenaFaction = packet.readUInt8() != 0;
            break;
        default:
            break;
    }
    return true;
}

StatusEffect Queues::apply(const StatusPacket& s, int64_t nowMs,
                           const std::function<uint32_t(uint32_t)>& mapInstanceType) {
    StatusEffect fx;
    if (s.queueSlot >= kQueueSlots) return fx;   // "1 < slot": dropped
    fx.slot = static_cast<int>(s.queueSlot);
    QueueSlot& q = slots_[s.queueSlot];
    const bool wasActive = active_.slot == fx.slot;

    if (s.header == 0) {
        q = QueueSlot{};
        if (wasActive) {
            active_ = ActiveBattlefield{};
            fx.leftActive = true;
        }
        return fx;
    }

    q.header = s.header;
    q.bgTypeId = s.bgTypeId;
    q.teamSize = s.teamSize;
    q.minLevel = s.minLevel;
    q.maxLevel = s.maxLevel;
    q.instanceId = s.instanceId;
    q.rated = s.rated;
    q.status = s.status;

    if (s.status == 2) {
        q.mapId = s.mapId;
        q.portExpiresAtMs = s.inviteTimeoutMs ? nowMs + s.inviteTimeoutMs : 0;
        fx.invited = true;
    } else {
        q.portExpiresAtMs = 0;
    }

    if (s.status == 3) {
        active_.slot = fx.slot;
        active_.mapId = s.mapId;
        active_.instanceType = mapInstanceType ? mapInstanceType(s.mapId) : 0;
        active_.expiresAtMs = s.autoLeaveMs ? nowMs + s.autoLeaveMs : 0;
        active_.startedAtMs = s.elapsedMs ? nowMs - s.elapsedMs : 0;
        active_.arenaFaction = s.arenaFaction;
        fx.enteredActive = true;
    } else if (wasActive) {
        // A slot that was the battleground and is now something else: the
        // two timers stop and the slot is no longer the active one.
        active_.slot = -1;
        active_.expiresAtMs = 0;
        active_.startedAtMs = 0;
    }

    if (s.status == 1) {
        q.avgWaitMs = s.avgWaitMs;
        q.queuedSinceMs = nowMs - static_cast<int64_t>(s.timeInQueueMs);
    } else {
        q.avgWaitMs = 0;
        q.queuedSinceMs = 0;
    }
    return fx;
}

void Queues::reset() {
    slots_ = {};
    active_ = ActiveBattlefield{};
}

bool Queues::hasInvite() const {
    for (const auto& q : slots_) if (q.status == 2) return true;
    return false;
}

uint32_t Queues::portExpirationSec(uint32_t index, int64_t nowMs) const {
    const QueueSlot* q = slot(index);
    if (!q || q->portExpiresAtMs == 0 || q->portExpiresAtMs <= nowMs) return 0;
    return static_cast<uint32_t>((q->portExpiresAtMs - nowMs) / 1000);
}

int64_t Queues::timeWaitedMs(uint32_t index, int64_t nowMs) const {
    const QueueSlot* q = slot(index);
    if (!q || q->queuedSinceMs == 0) return 0;
    return nowMs - q->queuedSinceMs;
}

uint32_t Queues::estimatedWaitMs(uint32_t index) const {
    const QueueSlot* q = slot(index);
    return q ? q->avgWaitMs : 0;
}

int64_t Queues::instanceExpirationMs(int64_t nowMs) const {
    if (active_.expiresAtMs == 0 || active_.expiresAtMs <= nowMs) return 0;
    return active_.expiresAtMs - nowMs;
}

int64_t Queues::instanceRunTimeMs(int64_t nowMs) const {
    if (active_.startedAtMs == 0) return 0;
    return nowMs - active_.startedAtMs;
}

const char* statusToken(uint32_t status) {
    switch (status) {
        case 0: return "none";
        case 1: return "queued";
        case 2: return "confirm";
        case 3: return "active";
        default: return "error";
    }
}

// ---------------------------------------------------------------------------
// SMSG_BATTLEFIELD_LIST (0x54E390)
// ---------------------------------------------------------------------------

bool parseList(network::Packet& packet, ListPacket& out) {
    out = ListPacket{};
    if (!packet.hasRemaining(8 + 1 + 4 + 2 + 1 + 12 + 1)) return false;
    out.battlemaster = packet.readUInt64();
    out.fromAnywhere = packet.readUInt8() != 0;
    out.bgTypeId = packet.readUInt32();
    out.minLevel = packet.readUInt8();
    out.maxLevel = packet.readUInt8();
    out.hasWin = packet.readUInt8() != 0;
    out.winHonor = packet.readUInt32();
    out.winArena = packet.readUInt32();
    out.lossHonor = packet.readUInt32();
    out.isRandom = packet.readUInt8() != 0;
    if (out.isRandom) {
        if (!packet.hasRemaining(13)) return false;
        out.hasWinRandom = packet.readUInt8() != 0;
        out.winHonorRandom = packet.readUInt32();
        out.winArenaRandom = packet.readUInt32();
        out.lossHonorRandom = packet.readUInt32();
    }
    if (!packet.hasRemaining(4)) return true;
    const uint32_t count = packet.readUInt32();
    for (uint32_t i = 0; i < count && packet.hasRemaining(4); ++i)
        out.instanceIds.push_back(packet.readUInt32());
    return true;
}

const char* listEvent(const ListPacket& list) {
    if (list.bgTypeId == kRandomBattleground && list.battlemaster != 0) return "NPC_PVPQUEUE_ANYWHERE";
    if (list.fromAnywhere) return "PVPQUEUE_ANYWHERE_SHOW";
    return list.battlemaster != 0 ? "BATTLEFIELDS_SHOW" : "BATTLEFIELDS_CLOSED";
}

// ---------------------------------------------------------------------------
// MSG_PVP_LOG_DATA (0x54D280)
// ---------------------------------------------------------------------------

bool parsePvpLog(network::Packet& packet, PvpLogPacket& out) {
    out = PvpLogPacket{};
    if (!packet.hasRemaining(1)) return false;
    out.isArena = packet.readUInt8() != 0;
    if (out.isArena) {
        // Both teams' three numbers first, then both names (AzerothCore's
        // BuildRatingInfoBlock x2, BuildTeamInfoBlock x2).
        for (auto& t : out.teams) {
            if (!packet.hasRemaining(12)) return false;
            t.rating1 = packet.readUInt32();
            t.rating2 = packet.readUInt32();
            t.mmr = packet.readUInt32();
        }
        for (auto& t : out.teams) {
            if (!packet.hasRemaining(1)) return false;
            t.name = packet.readString();
        }
    }
    if (!packet.hasRemaining(1)) return false;
    out.ended = packet.readUInt8() != 0;
    if (out.ended) {
        if (!packet.hasRemaining(1)) return false;
        out.winner = packet.readUInt8();
    }
    if (!packet.hasRemaining(4)) return false;
    const uint32_t count = packet.readUInt32();
    for (uint32_t i = 0; i < count && out.rows.size() < kMaxScores; ++i) {
        ScoreRow r;
        if (!packet.hasRemaining(12)) break;
        r.guid = packet.readUInt64();
        r.killingBlows = packet.readUInt32();
        if (out.isArena) {
            if (!packet.hasRemaining(1)) break;
            r.faction = packet.readUInt8();
        } else {
            if (!packet.hasRemaining(12)) break;
            r.honorableKills = packet.readUInt32();
            r.deaths = packet.readUInt32();
            r.honorGained = packet.readUInt32();
        }
        if (!packet.hasRemaining(12)) break;
        r.damageDone = packet.readUInt32();
        r.healingDone = packet.readUInt32();
        const uint32_t stats = packet.readUInt32();
        for (uint32_t k = 0; k < stats; ++k) {
            if (!packet.hasRemaining(4)) break;
            const uint32_t v = packet.readUInt32();
            if (k < r.stats.size()) r.stats[k] = v;
        }
        out.rows.push_back(std::move(r));
    }
    return true;
}

ScoreSort::ScoreSort() {
    for (int i = 0; i < 17; ++i) keys_[static_cast<size_t>(i)] = {i, false};
}

int ScoreSort::keyOf(const std::string& c) {
    if (c == "kills") return 0;
    if (c == "deaths") return 1;
    if (c == "cp") return 2;
    if (c == "name") return 3;
    if (c == "class") return 4;
    if (c == "hk") return 5;
    if (c == "damage") return 6;
    if (c == "healing") return 7;
    if (c == "team") return 8;
    if (c.compare(0, 4, "stat") == 0) {
        const int n = std::atoi(c.c_str() + 4);
        return n + 8 <= 16 ? n + 8 : -1;
    }
    return -1;
}

void ScoreSort::select(const std::string& column) { selectKey(keyOf(column)); }

void ScoreSort::selectKey(int key) {
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i].id != key) continue;
        Key front{key, keys_[i].reversed};
        if (i == 0) front.reversed = !front.reversed;
        else for (size_t j = i; j > 0; --j) keys_[j] = keys_[j - 1];
        keys_[0] = front;
        return;
    }
}

bool ScoreSort::before(const ScoreRow& a, const ScoreRow& b, int factionFilter,
                       size_t statColumns) const {
    if (factionFilter != -1 && a.faction != b.faction)
        return a.faction == factionFilter;
    for (const Key& k : keys_) {
        int c = 0;  // -1 a first, +1 b first
        auto desc = [&c](uint32_t x, uint32_t y) { if (x != y) c = x > y ? -1 : 1; };
        switch (k.id) {
            case 0: desc(a.killingBlows, b.killingBlows); break;
            case 1: desc(a.deaths, b.deaths); break;
            case 2: desc(a.honorGained, b.honorGained); break;
            case 3: { const int r = std::strcmp(a.name.c_str(), b.name.c_str());
                      c = r < 0 ? -1 : (r > 0 ? 1 : 0); break; }
            case 4: desc(a.classId, b.classId); break;
            case 5: desc(a.honorableKills, b.honorableKills); break;
            case 6: desc(a.damageDone, b.damageDone); break;
            case 7: desc(a.healingDone, b.healingDone); break;
            case 8: if (a.faction != b.faction) c = a.faction < b.faction ? -1 : 1; break;
            default: {
                const size_t s = static_cast<size_t>(k.id - 9);
                if (k.id >= 9 && s < statColumns && s < a.stats.size())
                    desc(a.stats[s], b.stats[s]);
                break;
            }
        }
        if (c != 0) return (k.reversed ? -c : c) < 0;
    }
    return false;
}

void Scoreboard::resort(size_t statColumns) {
    shown = 0;
    for (const auto& r : log.rows)
        if (factionFilter == -1 || r.faction == factionFilter) ++shown;
    std::stable_sort(log.rows.begin(), log.rows.end(),
                     [&](const ScoreRow& a, const ScoreRow& b) {
                         return sort.before(a, b, factionFilter, statColumns);
                     });
}

int factionOfRace(uint32_t raceId) {
    switch (raceId) {
        case 1: case 3: case 4: case 7: case 11: return 1;    // Alliance
        case 2: case 5: case 6: case 8: case 10: return 0;    // Horde
        default: return -1;
    }
}

// ---------------------------------------------------------------------------
// WorldStateUI.dbc (0x5487D0, 0x54AE40, 0x5770D0)
// ---------------------------------------------------------------------------

std::vector<size_t> visibleRows(const std::vector<WorldStateUiRow>& rows, uint32_t mapId,
                                uint32_t zoneId, uint32_t areaId, uint32_t phaseMask) {
    std::vector<size_t> out;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (r.mapId != -1 && static_cast<uint32_t>(r.mapId) != mapId) continue;
        if (r.areaId != 0 && r.areaId != zoneId && r.areaId != areaId) continue;
        if (r.phaseMask != 0 && (r.phaseMask & phaseMask) == 0) continue;
        if (r.type != 0 && r.type != 1 && r.type != 3) continue;
        out.push_back(i);
    }
    return out;
}

std::vector<size_t> scoreColumns(const std::vector<WorldStateUiRow>& rows, uint32_t mapId) {
    std::vector<size_t> out;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const bool match = (r.mapId == -1 || static_cast<uint32_t>(r.mapId) == mapId) && r.type == 2;
        if (match) out.push_back(i);
        else if (!out.empty()) break;
    }
    return out;
}

std::string formatText(const std::string& text,
                       const std::function<uint32_t(uint32_t)>& worldState,
                       int64_t serverNow) {
    std::string out;
    size_t p = 0;
    const size_t n = text.size();
    for (;;) {
        const size_t pct = text.find('%', p);
        if (pct == std::string::npos) break;
        out.append(text, p, pct - p);
        size_t q = pct + 1;
        const size_t digits = q;
        while (q < n && text[q] >= '0' && text[q] <= '9') ++q;
        const uint32_t id = static_cast<uint32_t>(std::strtoul(text.c_str() + digits, nullptr, 10));
        const char c = q < n ? text[q] : '\0';
        if (c == 'w' || c == 'W') {
            out += std::to_string(static_cast<int32_t>(worldState ? worldState(id) : 0));
            p = q + 1;
        } else if (c == 'k' || c == 'K') {
            const int64_t until = static_cast<int64_t>(worldState ? worldState(id) : 0);
            int64_t left = until - serverNow;
            if (left < 0) left = 0;
            char buf[24];
            if (left / 3600 < 1)
                std::snprintf(buf, sizeof(buf), "%02d:%02d",
                              static_cast<int>((left % 3600) / 60), static_cast<int>(left % 60));
            else
                std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", static_cast<int>(left / 3600),
                              static_cast<int>((left % 3600) / 60), static_cast<int>(left % 60));
            out += buf;
            p = q + 1;
        } else {
            // Anything else keeps a bare "%" and carries on from the letter.
            out += '%';
            p = q;
            if (q >= n) break;
        }
    }
    if (p < n) out.append(text, p, std::string::npos);
    return out;
}

// ---------------------------------------------------------------------------
// Encounter units (0x5EDDD0)
// ---------------------------------------------------------------------------

void EncounterUnits::sort() {
    std::stable_sort(units_.begin(), units_.end(), [](const Unit& a, const Unit& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.order < b.order;
    });
}

bool EncounterUnits::engage(uint64_t guid, uint8_t priority) {
    if (guid == 0) return false;
    for (const auto& u : units_) if (u.guid == guid) return false;
    if (units_.size() >= kMax) return false;
    units_.push_back({guid, priority, nextOrder_++});
    sort();
    return true;
}

bool EncounterUnits::disengage(uint64_t guid) {
    const auto before = units_.size();
    units_.erase(std::remove_if(units_.begin(), units_.end(),
                                [guid](const Unit& u) { return u.guid == guid; }),
                 units_.end());
    return units_.size() != before;
}

bool EncounterUnits::updatePriority(uint64_t guid, uint8_t priority) {
    for (auto& u : units_) {
        if (u.guid != guid) continue;
        u.priority = priority;
        sort();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// GetZonePVPInfo (0x51BA50)
// ---------------------------------------------------------------------------

ZonePvp zonePvpInfo(const AreaPvp* area, const AreaPvp* zone, bool pvpRealm, bool hasPlayer,
                    uint32_t friendGroups, uint32_t enemyGroups,
                    const std::function<std::string(uint32_t zoneMask)>& factionName) {
    ZonePvp out;
    uint32_t flags = 0;
    if (area && area->flags != 0xFFFFFFFFu) {
        flags = area->flags;
        out.fromArea = true;
    } else {
        flags = zone ? zone->flags : 0;
    }
    const char* early = nullptr;
    if (flags & 0x800) early = "sanctuary";
    else if (flags & 0x80) early = "arena";
    else if (flags & 0x1000000) early = "combat";
    if (early) {
        out.type = early;
        out.count = 2;
        return out;
    }
    out.fromArea = false;
    if (!zone || !hasPlayer || (!pvpRealm && (zone->flags & 0x10) == 0)) return out;
    if ((flags & 0x40000) == 0 && (friendGroups & zone->factionMask) == 0 &&
        (enemyGroups & zone->factionMask) != 0) {
        out.type = "hostile";
    } else if ((flags & 0x40000) == 0 && (friendGroups & zone->factionMask) != 0) {
        out.type = "friendly";
    } else {
        out.type = "contested";
    }
    out.faction = factionName ? factionName(zone->factionMask) : std::string();
    out.count = 3;
    return out;
}

}  // namespace wowee::game::bf
