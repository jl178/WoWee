#include "game/calendar_data.hpp"

#include <algorithm>
#include <ctime>

#include "network/packet.hpp"
#include "core/local_time.hpp"

namespace wowee {
namespace game {

namespace {

/// The smallest a row of each list can be, used to reject a count before any
/// of it is reserved. A packed guid is one byte at its shortest and a string
/// is one byte when empty, so these are lower bounds rather than sizes.
constexpr size_t kMinInviteBytes  = 8 + 8 + 1 + 1 + 1 + 1;
constexpr size_t kMinEventBytes   = 8 + 1 + 4 + 4 + 4 + 4 + 1;
constexpr size_t kLockoutBytes    = 4 + 4 + 4 + 8;
constexpr size_t kResetBytes      = 4 + 4 + 4;
constexpr size_t kMinHolidayBytes =
    5 * 4 + (CalendarHoliday::kNumDates + CalendarHoliday::kNumDurations +
             CalendarHoliday::kNumFlags) * 4 + 1;

/// A count is only believed if the packet is long enough to hold that many
/// rows at their smallest. Without this a corrupt uint32 reserves billions of
/// rows before the first read fails.
bool countFits(const network::Packet& packet, uint32_t count, size_t minRow) {
    return count == 0 || (minRow != 0 && count <= packet.getRemainingSize() / minRow);
}

/// A string, but only if its terminator is actually in the buffer.
///
/// Packet::readString stops at the end of the data as well as at a NUL, so a
/// name whose last bytes were cut off comes back looking like a complete one
/// and the parse reports success on a packet that is missing its tail. Every
/// truncation inside the final holiday's texture name read that way - 19 of
/// them, all reported clean.
bool readTerminatedString(network::Packet& packet, std::string& out) {
    const std::vector<uint8_t>& bytes = packet.getData();
    size_t at = packet.getReadPos();
    while (at < bytes.size() && bytes[at] != 0) ++at;
    if (at >= bytes.size()) return false;
    out = packet.readString();
    return true;
}

}  // namespace

bool parseCalendarSendCalendar(network::Packet& packet, CalendarData& out) {
    out = CalendarData{};

    if (!packet.hasRemaining(4)) return false;
    const uint32_t numInvites = packet.readUInt32();
    if (!countFits(packet, numInvites, kMinInviteBytes)) return false;
    out.invites.reserve(numInvites);
    for (uint32_t i = 0; i < numInvites; ++i) {
        if (!packet.hasRemaining(8 + 8 + 1 + 1 + 1)) return false;
        CalendarInvite inv;
        inv.eventId = packet.readUInt64();
        inv.inviteId = packet.readUInt64();
        inv.status = packet.readUInt8();
        inv.rank = packet.readUInt8();
        inv.isGuildEvent = packet.readUInt8() != 0;
        // The creator when the event still exists, the sender when it does
        // not - the server writes one field for both cases and there is
        // nothing in the packet that says which it chose.
        if (!packet.hasFullPackedGuid()) return false;
        inv.creatorGuid = packet.readPackedGuid();
        out.invites.push_back(inv);
    }

    if (!packet.hasRemaining(4)) return false;
    const uint32_t numEvents = packet.readUInt32();
    if (!countFits(packet, numEvents, kMinEventBytes)) return false;
    out.events.reserve(numEvents);
    for (uint32_t i = 0; i < numEvents; ++i) {
        if (!packet.hasRemaining(8 + 1)) return false;
        CalendarEvent ev;
        ev.eventId = packet.readUInt64();
        if (!readTerminatedString(packet, ev.title)) return false;
        if (!packet.hasRemaining(4 + 4 + 4 + 4)) return false;
        ev.type = packet.readUInt32();
        ev.eventTimePacked = packet.readUInt32();
        ev.eventTime = unpackWowPackedTime(ev.eventTimePacked);
        ev.flags = packet.readUInt32();
        ev.dungeonId = static_cast<int32_t>(packet.readUInt32());
        if (!packet.hasFullPackedGuid()) return false;
        ev.creatorGuid = packet.readPackedGuid();
        out.events.push_back(ev);
    }

    if (!packet.hasRemaining(4 + 4)) return false;
    out.serverTimeUnix = packet.readUInt32();
    out.zoneTime = unpackWowPackedTime(packet.readUInt32());

    if (!packet.hasRemaining(4)) return false;
    const uint32_t numLockouts = packet.readUInt32();
    if (!countFits(packet, numLockouts, kLockoutBytes)) return false;
    out.lockouts.reserve(numLockouts);
    for (uint32_t i = 0; i < numLockouts; ++i) {
        if (!packet.hasRemaining(kLockoutBytes)) return false;
        CalendarRaidLockout lock;
        lock.mapId = packet.readUInt32();
        lock.difficulty = packet.readUInt32();
        lock.secondsRemaining = packet.readUInt32();
        // A full guid rather than a packed one. The server writes this as an
        // ObjectGuid straight into the buffer, not WriteAsPacked like the two
        // creator fields above it.
        lock.instanceGuid = packet.readUInt64();
        out.lockouts.push_back(lock);
    }

    if (!packet.hasRemaining(4 + 4)) return false;
    out.resetRelationTime = packet.readUInt32();
    const uint32_t numResets = packet.readUInt32();
    if (!countFits(packet, numResets, kResetBytes)) return false;
    out.resets.reserve(numResets);
    for (uint32_t i = 0; i < numResets; ++i) {
        if (!packet.hasRemaining(kResetBytes)) return false;
        CalendarRaidReset reset;
        reset.mapId = static_cast<int32_t>(packet.readUInt32());
        reset.periodSeconds = static_cast<int32_t>(packet.readUInt32());
        reset.offsetSeconds = static_cast<int32_t>(packet.readUInt32());
        out.resets.push_back(reset);
    }

    if (!packet.hasRemaining(4)) return false;
    const uint32_t numHolidays = packet.readUInt32();
    if (!countFits(packet, numHolidays, kMinHolidayBytes)) return false;
    out.holidays.reserve(numHolidays);
    for (uint32_t i = 0; i < numHolidays; ++i) {
        if (!packet.hasRemaining(kMinHolidayBytes - 1)) return false;
        CalendarHoliday holiday;
        holiday.id = packet.readUInt32();
        holiday.region = packet.readUInt32();
        holiday.looping = packet.readUInt32();
        holiday.priority = packet.readUInt32();
        holiday.calendarFilterType = packet.readUInt32();
        for (uint32_t& date : holiday.dates) date = packet.readUInt32();
        for (uint32_t& dur : holiday.durations) dur = packet.readUInt32();
        for (uint32_t& flag : holiday.calendarFlags) flag = packet.readUInt32();
        if (!readTerminatedString(packet, holiday.textureFilename)) return false;
        out.holidays.push_back(std::move(holiday));
    }

    return true;
}


namespace {

/// A day number that can be compared and subtracted across month ends, so
/// "does this holiday still run on the 3rd" does not need a calendar walk.
/// Any consistent epoch will do - only differences are used.
int64_t dayNumber(int year, int month, int day) {
    // Howard Hinnant's days_from_civil, which is exact for the proleptic
    // Gregorian calendar the server's clock uses.
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yoe = year - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

}  // namespace

std::vector<CalendarDayEntry> calendarEntriesForDay(const CalendarData& data,
                                                    int month, int day,
                                                    int year) {
    std::vector<CalendarDayEntry> out;
    const int64_t wanted = dayNumber(year, month, day);

    for (size_t h = 0; h < data.holidays.size(); ++h) {
        const CalendarHoliday& holiday = data.holidays[h];
        for (int i = 0; i < CalendarHoliday::kNumDates; ++i) {
            if (holiday.dates[i] == 0) continue;
            const WowDate start = unpackWowPackedTime(holiday.dates[i]);
            const int64_t first = dayNumber(start.fullYear(), start.month, start.day);
            if (wanted < first) continue;
            // Hours, rounded up to whole days and never less than one: a
            // holiday sent with a duration of zero still happens on its day.
            const uint32_t hours = (i < CalendarHoliday::kNumDurations)
                                       ? holiday.durations[i] : 0;
            const int64_t days = hours > 0 ? static_cast<int64_t>((hours + 23) / 24) : 1;
            if (wanted >= first + days) continue;
            CalendarDayEntry entry;
            entry.kind = CalendarEntryKind::Holiday;
            entry.index = h;
            entry.ongoing = wanted > first;
            entry.sequenceIndex = static_cast<int>(wanted - first) + 1;
            entry.numSequenceDays = static_cast<int>(days);
            out.push_back(entry);
            break;  // one row per holiday per day, whichever run covers it
        }
    }

    // Raid lockouts, which the packet dates only by how long they have left.
    //
    // serverTimeUnix is the server's own clock at the moment it sent the
    // calendar, and every lockout counts down from there - so the expiry is
    // that instant plus the remainder, and it is converted here rather than at
    // each reader so the day it lands on and the time it shows cannot
    // disagree.
    if (data.serverTimeUnix != 0) {
        for (size_t l = 0; l < data.lockouts.size(); ++l) {
            const std::time_t expiry =
                static_cast<std::time_t>(data.serverTimeUnix) +
                static_cast<std::time_t>(data.lockouts[l].secondsRemaining);
            std::tm tmv{};
            tmv = core::localTime(expiry);
            if (dayNumber(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday) != wanted) {
                continue;
            }
            CalendarDayEntry entry;
            entry.kind = CalendarEntryKind::RaidLockout;
            entry.index = l;
            entry.hour = tmv.tm_hour;
            entry.minute = tmv.tm_min;
            out.push_back(entry);
        }
    }

    for (size_t e = 0; e < data.events.size(); ++e) {
        const WowDate when = data.events[e].eventTime;
        if (when.month != month || when.day != day || when.fullYear() != year) {
            continue;
        }
        CalendarDayEntry entry;
        entry.kind = CalendarEntryKind::Event;
        entry.index = e;
        out.push_back(entry);
    }

    // Events by time of day, holidays left where they are at the front.
    std::stable_sort(out.begin(), out.end(),
                     [&data](const CalendarDayEntry& a, const CalendarDayEntry& b) {
                         if (a.kind != b.kind) {
                             // The enum is in drawing order: holidays head the
                             // day, then raid lockouts, then the player's own
                             // events.
                             return a.kind < b.kind;
                         }
                         if (a.kind != CalendarEntryKind::Event) return false;
                         const WowDate& ta = data.events[a.index].eventTime;
                         const WowDate& tb = data.events[b.index].eventTime;
                         if (ta.hour != tb.hour) return ta.hour < tb.hour;
                         return ta.minute < tb.minute;
                     });
    return out;
}

bool parseCalendarSendEvent(network::Packet& packet, CalendarEventDetail& out) {
    out = CalendarEventDetail{};

    if (!packet.hasRemaining(1)) return false;
    out.sendType = packet.readUInt8();
    if (!packet.hasFullPackedGuid()) return false;
    out.creatorGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(8 + 1)) return false;
    out.eventId = packet.readUInt64();
    if (!readTerminatedString(packet, out.title)) return false;
    if (!readTerminatedString(packet, out.description)) return false;
    if (!packet.hasRemaining(1 + 1 + 4 + 4 + 4 + 4 + 4 + 4)) return false;
    out.type = packet.readUInt8();
    out.repeatOption = packet.readUInt8();
    out.maxInvites = packet.readUInt32();
    out.dungeonId = static_cast<int32_t>(packet.readUInt32());
    out.flags = packet.readUInt32();
    out.eventTimePacked = packet.readUInt32();
    out.eventTime = unpackWowPackedTime(out.eventTimePacked);
    out.timeZoneTime = unpackWowPackedTime(packet.readUInt32());
    out.guildId = packet.readUInt32();

    if (!packet.hasRemaining(4)) return false;
    const uint32_t numInvitees = packet.readUInt32();
    // A guid is one byte at its shortest and the note one when empty, so this
    // is a lower bound rather than a row size.
    constexpr size_t kMinInviteeBytes = 1 + 1 + 1 + 1 + 1 + 8 + 4 + 1;
    if (!countFits(packet, numInvitees, kMinInviteeBytes)) return false;
    out.invitees.reserve(numInvitees);
    for (uint32_t i = 0; i < numInvitees; ++i) {
        CalendarEventInvitee inv;
        if (!packet.hasFullPackedGuid()) return false;
        inv.guid = packet.readPackedGuid();
        if (!packet.hasRemaining(1 + 1 + 1 + 1 + 8 + 4)) return false;
        inv.level = packet.readUInt8();
        inv.status = packet.readUInt8();
        inv.rank = packet.readUInt8();
        inv.isGuildMember = packet.readUInt8() != 0;
        inv.inviteId = packet.readUInt64();
        inv.statusTime = unpackWowPackedTime(packet.readUInt32());
        if (!readTerminatedString(packet, inv.note)) return false;
        out.invitees.push_back(std::move(inv));
    }
    return true;
}

const char* calendarHolidaySequenceType(const CalendarDayEntry& entry) {
    if (entry.numSequenceDays <= 1) return "";
    if (entry.sequenceIndex <= 1) return "START";
    if (entry.sequenceIndex >= entry.numSequenceDays) return "END";
    return "ONGOING";
}

void writeCalendarAddEvent(network::Packet& pkt, const CalendarEventDraft& draft,
                           const std::vector<CalendarAddEventInvite>& invites) {
    pkt.writeString(draft.title);
    pkt.writeString(draft.description);
    pkt.writeUInt8(draft.type);
    pkt.writeUInt8(draft.repeatOption);
    pkt.writeUInt32(draft.maxInvites);
    pkt.writeUInt32(static_cast<uint32_t>(draft.dungeonId));
    // The second time is stored and never read by the server, but it is read
    // off the wire, so leaving it out shifts the flags into it.
    pkt.writeUInt32(packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(draft.flags);
    // An announcement has no list: the server branches on the flag before it
    // reads one, and anything written would be left unread.
    if (draft.flags & kCalendarFlagAnnouncement) return;
    constexpr size_t kMaxInvites = 100;  // CALENDAR_MAX_INVITES
    const size_t count = std::min(invites.size(), kMaxInvites);
    pkt.writeUInt32(static_cast<uint32_t>(count));
    for (size_t i = 0; i < count; ++i) {
        pkt.writePackedGuid(invites[i].guid);
        pkt.writeUInt8(invites[i].status);
        pkt.writeUInt8(invites[i].rank);
    }
}

bool parseCalendarEventInvite(network::Packet& packet, CalendarEventInviteNotice& out) {
    out = CalendarEventInviteNotice{};
    if (!packet.hasFullPackedGuid()) return false;
    out.inviteeGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(8 + 8 + 1 + 1 + 1)) return false;
    out.eventId = packet.readUInt64();
    out.inviteId = packet.readUInt64();
    out.level = packet.readUInt8();
    out.status = packet.readUInt8();
    // The time is only there when the server has one: it leaves it out for
    // the 2000-01-01 it uses as "never answered".
    out.hasStatusTime = packet.readUInt8() != 0;
    if (out.hasStatusTime) {
        if (!packet.hasRemaining(4)) return false;
        out.statusTimePacked = packet.readUInt32();
    }
    if (!packet.hasRemaining(1)) return false;
    out.isInvite = packet.readUInt8() != 0;
    return true;
}

bool parseCalendarInviteAlert(network::Packet& packet, CalendarInviteAlert& out) {
    out = CalendarInviteAlert{};
    if (!packet.hasRemaining(8 + 1)) return false;
    out.event.eventId = packet.readUInt64();
    if (!readTerminatedString(packet, out.event.title)) return false;
    if (!packet.hasRemaining(4 + 4 + 4 + 4 + 8 + 1 + 1)) return false;
    out.event.eventTimePacked = packet.readUInt32();
    out.event.eventTime = unpackWowPackedTime(out.event.eventTimePacked);
    out.event.flags = packet.readUInt32();
    out.event.type = packet.readUInt32();
    out.event.dungeonId = static_cast<int32_t>(packet.readUInt32());
    out.inviteId = packet.readUInt64();
    out.status = packet.readUInt8();
    out.rank = packet.readUInt8();
    if (!packet.hasFullPackedGuid()) return false;
    out.event.creatorGuid = packet.readPackedGuid();
    if (!packet.hasFullPackedGuid()) return false;
    out.senderGuid = packet.readPackedGuid();
    return true;
}

bool parseCalendarEventStatus(network::Packet& packet, CalendarEventStatusNotice& out) {
    out = CalendarEventStatusNotice{};
    if (!packet.hasFullPackedGuid()) return false;
    out.inviteeGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(8 + 4 + 4 + 1 + 1 + 4)) return false;
    out.eventId = packet.readUInt64();
    out.eventTimePacked = packet.readUInt32();
    out.flags = packet.readUInt32();
    out.status = packet.readUInt8();
    out.rank = packet.readUInt8();
    out.statusTimePacked = packet.readUInt32();
    return true;
}

bool parseCalendarCommandResult(network::Packet& packet, CalendarCommandResult& out) {
    out = CalendarCommandResult{};
    if (!packet.hasRemaining(4 + 1)) return false;
    packet.readUInt32();  // always 0
    packet.readUInt8();   // always 0
    if (!readTerminatedString(packet, out.param)) return false;
    if (!packet.hasRemaining(4)) return false;
    out.error = packet.readUInt32();
    return true;
}

const char* calendarErrorStringName(uint32_t error) {
    // CalendarError (CalendarMgr.h) to the GlobalStrings.lua key the stock
    // client shows for it. Four have no CALENDAR_ERROR_ string of their own
    // and borrow the general ERR_ one that says the same thing.
    switch (error) {
        case 1:  return "CALENDAR_ERROR_GUILD_EVENTS_EXCEEDED";
        case 2:  return "CALENDAR_ERROR_EVENTS_EXCEEDED";
        case 3:  return "CALENDAR_ERROR_SELF_INVITES_EXCEEDED";
        case 4:  return "CALENDAR_ERROR_OTHER_INVITES_EXCEEDED";
        case 5:  return "CALENDAR_ERROR_PERMISSIONS";
        case 6:  return "CALENDAR_ERROR_EVENT_INVALID";
        case 7:  return "CALENDAR_ERROR_NOT_INVITED";
        case 8:  return "CALENDAR_ERROR_INTERNAL";
        case 9:  return "ERR_GUILD_PLAYER_NOT_IN_GUILD";
        case 10: return "CALENDAR_ERROR_ALREADY_INVITED_TO_EVENT_S";
        case 11: return "ERR_BAD_PLAYER_NAME_S";
        case 12: return "CALENDAR_ERROR_NOT_ALLIED";
        case 13: return "CALENDAR_ERROR_IGNORED";
        case 14: return "CALENDAR_ERROR_INVITES_EXCEEDED";
        case 16: return "CALENDAR_ERROR_INVALID_DATE";
        case 17: return "CALENDAR_ERROR_INVALID_TIME";
        case 19: return "CALENDAR_ERROR_NEEDS_TITLE";
        case 20: return "CALENDAR_ERROR_EVENT_PASSED";
        case 21: return "CALENDAR_ERROR_EVENT_LOCKED";
        case 22: return "CALENDAR_ERROR_DELETE_CREATOR_FAILED";
        case 24: return "ERR_SYSTEM_DISABLED";
        case 25: return "ERR_RESTRICTED_ACCOUNT";
        case 26: return "CALENDAR_ERROR_ARENA_EVENTS_EXCEEDED";
        case 27: return "CALENDAR_ERROR_RESTRICTED_LEVEL";
        case 28: return "ERR_USER_SQUELCHED";
        case 29: return "CALENDAR_ERROR_NO_INVITE";
        case 36: return "CALENDAR_ERROR_EVENT_WRONG_SERVER";
        case 37: return "CALENDAR_ERROR_INVITE_WRONG_SERVER";
        case 38: return "CALENDAR_ERROR_NO_GUILD_INVITES";
        case 39: return "CALENDAR_ERROR_INVALID_SIGNUP";
        case 40: return "CALENDAR_ERROR_NO_MODERATOR";
        default: return "";
    }
}

bool parseCalendarEventUpdatedAlert(network::Packet& packet, CalendarEventUpdatedAlert& out) {
    out = CalendarEventUpdatedAlert{};
    if (!packet.hasRemaining(1 + 8 + 4 + 4 + 4 + 1 + 4)) return false;
    packet.readUInt8();
    out.eventId = packet.readUInt64();
    out.oldEventTimePacked = packet.readUInt32();
    out.flags = packet.readUInt32();
    out.eventTimePacked = packet.readUInt32();
    out.type = packet.readUInt8();
    out.dungeonId = static_cast<int32_t>(packet.readUInt32());
    if (!readTerminatedString(packet, out.title)) return false;
    if (!readTerminatedString(packet, out.description)) return false;
    if (!packet.hasRemaining(1 + 4 + 4)) return false;
    out.repeatOption = packet.readUInt8();
    out.maxInvites = packet.readUInt32();
    packet.readUInt32();
    return true;
}

bool parseCalendarEventRemovedAlert(network::Packet& packet, uint64_t& eventId) {
    if (!packet.hasRemaining(1 + 8 + 4)) return false;
    packet.readUInt8();
    eventId = packet.readUInt64();
    packet.readUInt32();
    return true;
}

bool parseCalendarInviteRemoved(network::Packet& packet, CalendarInviteRemovedNotice& out) {
    out = CalendarInviteRemovedNotice{};
    if (!packet.hasFullPackedGuid()) return false;
    out.inviteeGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(8 + 4 + 1)) return false;
    out.eventId = packet.readUInt64();
    out.flags = packet.readUInt32();
    packet.readUInt8();
    return true;
}

bool parseCalendarInviteRemovedAlert(network::Packet& packet, CalendarInviteRemovedAlert& out) {
    out = CalendarInviteRemovedAlert{};
    if (!packet.hasRemaining(8 + 4 + 4 + 1)) return false;
    out.eventId = packet.readUInt64();
    packet.readUInt32();
    out.flags = packet.readUInt32();
    out.status = packet.readUInt8();
    return true;
}

bool parseCalendarModeratorStatus(network::Packet& packet, CalendarModeratorNotice& out) {
    out = CalendarModeratorNotice{};
    if (!packet.hasFullPackedGuid()) return false;
    out.inviteeGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(8 + 1 + 1)) return false;
    out.eventId = packet.readUInt64();
    out.rank = packet.readUInt8();
    packet.readUInt8();
    return true;
}

bool parseCalendarMassInviteList(network::Packet& packet, std::vector<uint64_t>& out) {
    out.clear();
    if (!packet.hasRemaining(4)) return false;
    const uint32_t count = packet.readUInt32();
    if (!countFits(packet, count, 1 + 1)) return false;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!packet.hasFullPackedGuid()) return false;
        out.push_back(packet.readPackedGuid());
        if (!packet.hasRemaining(1)) return false;
        packet.readUInt8();
    }
    return true;
}

}  // namespace game
}  // namespace wowee
