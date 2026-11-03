#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/packed_time.hpp"

namespace wowee {
namespace network { class Packet; }
namespace game {

/**
 * The calendar the server sends in one packet, unpacked.
 *
 * SMSG_CALENDAR_SEND_CALENDAR is the answer to CMSG_CALENDAR_GET_CALENDAR and
 * carries six lists back to back, each a count followed by its rows. Nothing
 * in it is length-prefixed as a whole and four of the six rows contain a
 * packed guid or a string, so a row read wrong does not fail - it slides
 * everything after it, and the next count is read out of the middle of a
 * string. That is why this is a parser with a test rather than a read done
 * inline: the failure mode is silent and total.
 *
 * Layout, from AzerothCore's WorldSession::HandleCalendarGetCalendar
 * (CalendarHandler.cpp:53) which is the writer this parses:
 *
 *     uint32 numInvites
 *       uint64 eventId, uint64 inviteId, uint8 status, uint8 rank,
 *       uint8 isGuildEvent, packedGuid creator
 *     uint32 numEvents
 *       uint64 eventId, cstring title, uint32 type, packedTime eventTime,
 *       uint32 flags, int32 dungeonId, packedGuid creator
 *     uint32 serverTime, packedTime zoneTime
 *     uint32 numLockouts
 *       uint32 mapId, uint32 difficulty, uint32 secondsLeft, uint64 instanceGuid
 *     uint32 resetRelationTime
 *     uint32 numResets
 *       int32 mapId, int32 period, int32 offset
 *     uint32 numHolidays
 *       uint32 id, region, looping, priority, calendarFilterType,
 *       uint32 date[26], uint32 duration[10], uint32 calendarFlags[10],
 *       cstring textureFilename
 *
 * The holiday row is 49 uint32s and a string; the three array lengths are the
 * server's MAX_HOLIDAY_DATES, MAX_HOLIDAY_DURATIONS and MAX_HOLIDAY_FLAGS
 * (DBCStructure.h:1122). Getting one of those counts wrong misreads every
 * holiday after the first and nothing says so.
 */

/// An invitation waiting on an answer.
struct CalendarInvite {
    uint64_t eventId = 0;
    uint64_t inviteId = 0;
    uint8_t status = 0;        ///< CalendarInviteStatus
    uint8_t rank = 0;          ///< CalendarModerationRank
    bool isGuildEvent = false;
    uint64_t creatorGuid = 0;  ///< the sender's, when the event itself is gone
};

/// An event the player can see, whether or not they have answered it.
struct CalendarEvent {
    uint64_t eventId = 0;
    std::string title;
    uint32_t type = 0;         ///< CalendarEventType
    WowDate eventTime;
    uint32_t eventTimePacked = 0;  ///< kept raw: the interface sorts on it
    uint32_t flags = 0;
    int32_t dungeonId = 0;
    uint64_t creatorGuid = 0;
};

/// A raid the player is saved to, and how long that lasts.
struct CalendarRaidLockout {
    uint32_t mapId = 0;
    uint32_t difficulty = 0;
    uint32_t secondsRemaining = 0;
    uint64_t instanceGuid = 0;
};

/// When a raid's lock resets, and how often.
struct CalendarRaidReset {
    int32_t mapId = 0;
    int32_t periodSeconds = 0;
    int32_t offsetSeconds = 0;
};

/// A world holiday, with the dates it runs on.
struct CalendarHoliday {
    static constexpr int kNumDates = 26;      ///< server MAX_HOLIDAY_DATES
    static constexpr int kNumDurations = 10;  ///< server MAX_HOLIDAY_DURATIONS
    static constexpr int kNumFlags = 10;      ///< server MAX_HOLIDAY_FLAGS

    uint32_t id = 0;
    uint32_t region = 0;
    uint32_t looping = 0;
    uint32_t priority = 0;
    uint32_t calendarFilterType = 0;
    uint32_t dates[kNumDates] = {};
    uint32_t durations[kNumDurations] = {};
    uint32_t calendarFlags[kNumFlags] = {};
    std::string textureFilename;
};

/// Everything the packet carried.
struct CalendarData {
    std::vector<CalendarInvite> invites;
    std::vector<CalendarEvent> events;
    /// The server's own clock, so a client whose clock is wrong still places
    /// events on the right day.
    uint32_t serverTimeUnix = 0;
    WowDate zoneTime;
    std::vector<CalendarRaidLockout> lockouts;
    uint32_t resetRelationTime = 0;
    std::vector<CalendarRaidReset> resets;
    std::vector<CalendarHoliday> holidays;
};

/**
 * Read one SMSG_CALENDAR_SEND_CALENDAR.
 *
 * False when the packet runs out mid-row, and `out` is then whatever had been
 * read whole - a truncated calendar rather than a mixture of real values and
 * values read past the end. Every count is checked against what is left before
 * it is trusted: the counts are server-controlled uint32s and a corrupt one
 * would otherwise reserve four billion rows.
 */
bool parseCalendarSendCalendar(network::Packet& packet, CalendarData& out);

/// One person on an event's invite list.
struct CalendarEventInvitee {
    uint64_t guid = 0;
    uint64_t inviteId = 0;
    uint8_t level = 0;
    uint8_t status = 0;        ///< CalendarInviteStatus
    uint8_t rank = 0;          ///< CalendarModerationRank
    bool isGuildMember = false;
    WowDate statusTime;
    std::string note;
};

/**
 * One event in full, as SMSG_CALENDAR_SEND_EVENT sends it.
 *
 * The answer to opening an event. Laid out against CalendarMgr::
 * SendCalendarEvent (CalendarMgr.cpp:627): a header, then a count and that
 * many invitee rows - and every row carries a packed guid, a packed time and
 * a string, so a row read wrong slides the rest exactly as the calendar packet
 * does.
 */
struct CalendarEventDetail {
    uint8_t sendType = 0;
    uint64_t creatorGuid = 0;
    uint64_t eventId = 0;
    std::string title;
    std::string description;
    uint8_t type = 0;
    uint8_t repeatOption = 0;
    uint32_t maxInvites = 0;
    int32_t dungeonId = -1;
    uint32_t flags = 0;
    WowDate eventTime;
    uint32_t eventTimePacked = 0;
    WowDate timeZoneTime;
    uint32_t guildId = 0;
    std::vector<CalendarEventInvitee> invitees;
};

/// Read one SMSG_CALENDAR_SEND_EVENT. False when it runs out mid-row.
bool parseCalendarSendEvent(network::Packet& packet, CalendarEventDetail& out);

/// An event being built up before it is sent.
///
/// The interface fills this in over several calls - a title here, a date
/// there - and commits it with one, so it has to be held somewhere between
/// them. That is how WoW works too: CalendarNewEvent starts one,
/// CalendarEventSetTitle and friends fill it, CalendarAddEvent sends it.
struct CalendarEventDraft {
    std::string title;
    std::string description;
    uint8_t type = 0;            ///< CalendarEventType
    uint8_t repeatOption = 0;    ///< CalendarRepeatType: 0 never, 1 weekly, 2 fortnightly
    uint32_t maxInvites = 100;
    int32_t dungeonId = -1;      ///< -1 for no dungeon, and signed for that reason
    WowDate eventTime;
    uint32_t flags = 0;
};

/// What a row on a given day came from. The interface reads a day's rows
/// through one index space - `CalendarGetNumDayEvents` counts them and
/// `CalendarGetDayEvent`, `CalendarGetHolidayInfo` and `CalendarGetRaidInfo`
/// all index into the same list - so the order has to be decided once, here,
/// rather than in each of those.
enum class CalendarEntryKind { Holiday, RaidLockout, Event };

struct CalendarDayEntry {
    CalendarEntryKind kind = CalendarEntryKind::Event;
    /// Into CalendarData::holidays, ::lockouts or ::events, by kind.
    size_t index = 0;
    /// For a raid lockout, the hour and minute it expires. The packet gives
    /// seconds remaining rather than a time, so it is worked out once here
    /// with the rest of the day's placement rather than at each reader.
    int hour = 0;
    int minute = 0;
    /// True on every day of a holiday after the first. FrameXML skips a row
    /// whose sequenceType is "ONGOING" when drawing the day's list, so a
    /// week-long holiday draws its name once rather than seven times.
    bool ongoing = false;
    /// For a holiday: which day of its run this is (1-based) and how many
    /// days the run lasts. CalendarGetDayEvent returns both; the calendar
    /// compares numSequenceDays with a number and broke on holiday days.
    int sequenceIndex = 0;
    int numSequenceDays = 0;
};

/**
 * The rows for one day, holidays first and then events by time.
 *
 * Holidays lead because that is the order WoW draws them in - a world event
 * heads the day and the player's own events follow underneath.
 *
 * A holiday covers `durations[i]` hours from `dates[i]`, and both arrays are
 * walked together: a holiday with several date/duration pairs is several
 * separate runs in the same row, which is how a recurring one is sent.
 */
std::vector<CalendarDayEntry> calendarEntriesForDay(const CalendarData& data,
                                                    int month, int day,
                                                    int year);

/// The holiday's place in its run, as the string CalendarGetDayEvent answers.
///
/// Blizzard_Calendar formats a holiday name through
/// CALENDAR_CALENDARTYPE_NAMEFORMAT.HOLIDAY[sequenceType], which has entries
/// for "START" ("%s Begins"), "END" ("%s Ends") and "" ("%s") and none for
/// "ONGOING" - the middle days are skipped before it is read. So a one-day
/// holiday is "", and only a run of two days or more has a start and an end.
const char* calendarHolidaySequenceType(const CalendarDayEntry& entry);

// ---- Flags, statuses and ranks, as CalendarMgr.h numbers them ----

/// CALENDAR_FLAG_INVITES_LOCKED. The view frame greys every RSVP on it.
inline constexpr uint32_t kCalendarFlagInvitesLocked = 0x010;
/// CALENDAR_FLAG_WITHOUT_INVITES - what makes an event a guild announcement
/// (CalendarEvent::IsGuildAnnouncement tests exactly this bit).
inline constexpr uint32_t kCalendarFlagAnnouncement = 0x040;
/// CALENDAR_FLAG_GUILD_EVENT.
inline constexpr uint32_t kCalendarFlagGuildEvent = 0x400;

/// CalendarInviteStatus (CalendarMgr.h). The interface's
/// CALENDAR_INVITESTATUS_* are these plus one.
enum CalendarInviteStatusValue : uint8_t {
    kCalendarStatusInvited = 0,
    kCalendarStatusAccepted = 1,
    kCalendarStatusDeclined = 2,
    kCalendarStatusConfirmed = 3,
    kCalendarStatusOut = 4,
    kCalendarStatusStandby = 5,
    kCalendarStatusSignedUp = 6,
    kCalendarStatusNotSignedUp = 7,
    kCalendarStatusTentative = 8,
    kCalendarStatusRemoved = 9,
};

/// CalendarModerationRank.
enum CalendarRankValue : uint8_t {
    kCalendarRankPlayer = 0,
    kCalendarRankModerator = 1,
    kCalendarRankOwner = 2,
};

/// Whether an invite in this status still wants an answer - the statuses
/// CalendarMgr::GetPlayerNumPending counts, so a count kept here agrees with
/// the one SMSG_CALENDAR_SEND_NUM_PENDING answers.
constexpr bool calendarStatusIsPending(uint8_t status) {
    return status == kCalendarStatusInvited || status == kCalendarStatusTentative ||
           status == kCalendarStatusNotSignedUp;
}

/// "GUILD_ANNOUNCEMENT", "GUILD_EVENT" or "PLAYER" - the calendarType the
/// interface compares against, which is decided by the flags alone.
constexpr const char* calendarTypeForFlags(uint32_t flags) {
    return (flags & kCalendarFlagAnnouncement) ? "GUILD_ANNOUNCEMENT"
         : (flags & kCalendarFlagGuildEvent)   ? "GUILD_EVENT"
                                               : "PLAYER";
}

// ---- Writing ----

/// One row of the invite list CMSG_CALENDAR_ADD_EVENT carries.
struct CalendarAddEventInvite {
    uint64_t guid = 0;
    uint8_t status = kCalendarStatusInvited;
    uint8_t rank = kCalendarRankPlayer;
};

/**
 * The body of CMSG_CALENDAR_ADD_EVENT.
 *
 * WorldSession::HandleCalendarAddEvent (CalendarHandler.cpp:228) reads the
 * header and then, unless the flags make it a guild announcement, a uint32
 * count and that many {packedGuid, uint8 status, uint8 rank}. Those rows are
 * the whole invite list - the server adds nobody on its own, not even the
 * creator - and it reads the count without a length check, so a packet that
 * stops after the flags throws on the server and no event is made at all.
 * At most CALENDAR_MAX_INVITES (100) rows are written: the server reads no
 * more than that and would leave the rest in the buffer.
 */
void writeCalendarAddEvent(network::Packet& packet, const CalendarEventDraft& draft,
                           const std::vector<CalendarAddEventInvite>& invites);

// ---- The server's notices, each read whole or not at all ----

/// SMSG_CALENDAR_EVENT_INVITE - one row joining an invite list
/// (CalendarMgr::SendCalendarEventInvite). With eventId 0 it is the answer to
/// a pre-invite: the server looked the name up and hands the row back for the
/// event still being drafted.
struct CalendarEventInviteNotice {
    uint64_t inviteeGuid = 0;
    uint64_t eventId = 0;
    uint64_t inviteId = 0;
    uint8_t level = 0;
    uint8_t status = 0;
    bool hasStatusTime = false;
    uint32_t statusTimePacked = 0;
    /// False only for a sign-up, where the invitee invited themselves.
    bool isInvite = true;
};
bool parseCalendarEventInvite(network::Packet& packet, CalendarEventInviteNotice& out);

/// SMSG_CALENDAR_EVENT_INVITE_ALERT - an event and this player's invite to it
/// (CalendarMgr::SendCalendarEventInviteAlert).
struct CalendarInviteAlert {
    CalendarEvent event;
    uint64_t inviteId = 0;
    uint8_t status = 0;
    uint8_t rank = 0;
    uint64_t senderGuid = 0;
};
bool parseCalendarInviteAlert(network::Packet& packet, CalendarInviteAlert& out);

/// SMSG_CALENDAR_EVENT_STATUS - someone's answer changed
/// (CalendarMgr::SendCalendarEventStatus).
struct CalendarEventStatusNotice {
    uint64_t inviteeGuid = 0;
    uint64_t eventId = 0;
    uint32_t eventTimePacked = 0;
    uint32_t flags = 0;
    uint8_t status = 0;
    uint8_t rank = 0;
    uint32_t statusTimePacked = 0;
};
bool parseCalendarEventStatus(network::Packet& packet, CalendarEventStatusNotice& out);

/// SMSG_CALENDAR_COMMAND_RESULT (CalendarMgr::SendCalendarCommandResult):
/// uint32 0, uint8 0, a string that is the player's name for the three
/// errors that name one and empty otherwise, then the CalendarError.
struct CalendarCommandResult {
    std::string param;
    uint32_t error = 0;
};
bool parseCalendarCommandResult(network::Packet& packet, CalendarCommandResult& out);

/// The GlobalStrings.lua name for a CalendarError, empty for CALENDAR_OK and
/// for codes the interface has no string for.
const char* calendarErrorStringName(uint32_t error);

/// SMSG_CALENDAR_EVENT_UPDATED_ALERT (CalendarMgr::SendCalendarEventUpdateAlert).
struct CalendarEventUpdatedAlert {
    uint64_t eventId = 0;
    uint32_t oldEventTimePacked = 0;
    uint32_t flags = 0;
    uint32_t eventTimePacked = 0;
    uint8_t type = 0;
    int32_t dungeonId = -1;
    std::string title;
    std::string description;
    uint8_t repeatOption = 0;
    uint32_t maxInvites = 0;
};
bool parseCalendarEventUpdatedAlert(network::Packet& packet, CalendarEventUpdatedAlert& out);

/// SMSG_CALENDAR_EVENT_REMOVED_ALERT: uint8, uint64 eventId, packed time.
bool parseCalendarEventRemovedAlert(network::Packet& packet, uint64_t& eventId);

/// SMSG_CALENDAR_EVENT_INVITE_REMOVED: packedGuid invitee, uint64 eventId,
/// uint32 flags, uint8.
struct CalendarInviteRemovedNotice {
    uint64_t inviteeGuid = 0;
    uint64_t eventId = 0;
    uint32_t flags = 0;
};
bool parseCalendarInviteRemoved(network::Packet& packet, CalendarInviteRemovedNotice& out);

/// SMSG_CALENDAR_EVENT_INVITE_REMOVED_ALERT, to the one removed: uint64
/// eventId, packed time, uint32 flags, uint8 status.
struct CalendarInviteRemovedAlert {
    uint64_t eventId = 0;
    uint32_t flags = 0;
    uint8_t status = 0;
};
bool parseCalendarInviteRemovedAlert(network::Packet& packet, CalendarInviteRemovedAlert& out);

/// SMSG_CALENDAR_EVENT_MODERATOR_STATUS_ALERT: packedGuid invitee, uint64
/// eventId, uint8 rank, uint8.
struct CalendarModeratorNotice {
    uint64_t inviteeGuid = 0;
    uint64_t eventId = 0;
    uint8_t rank = 0;
};
bool parseCalendarModeratorStatus(network::Packet& packet, CalendarModeratorNotice& out);

/// SMSG_CALENDAR_FILTER_GUILD and SMSG_CALENDAR_ARENA_TEAM, the answers to a
/// mass invite: a uint32 count and that many {packedGuid, uint8}
/// (Guild::MassInviteToEvent, ArenaTeam::MassInviteToEvent).
bool parseCalendarMassInviteList(network::Packet& packet, std::vector<uint64_t>& out);

}  // namespace game
}  // namespace wowee
