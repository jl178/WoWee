#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace wowee {
namespace game {

/// What a bar on the meter measures.
enum class MeterMode : uint8_t {
    DamageDone,
    Dps,
    HealingDone,
    Hps,
    DamageTaken,
    Overhealing,
    Deaths,
    Count
};

/// The spell id a hit from the world itself - a fall, lava, drowning - is
/// recorded under, so it is not mistaken for a melee swing (spell 0).
inline constexpr uint32_t kMeterEnvironment = 0xFFFFFFFFu;

/// One thing the combat log said happened, already sorted out by who it
/// belongs to. The game side does the sorting (it knows the group, the pets
/// and who summoned what); the meter only adds up.
struct MeterEvent {
    enum Kind : uint8_t { Damage, Heal, Death };
    Kind kind = Damage;
    /// The unit that did it, and the one it was done to.
    uint64_t src = 0;
    uint64_t dst = 0;
    /// The tracked combatant credited with `src`: the unit itself when it is
    /// the player or a group member, its owner when it is a pet, totem or
    /// guardian of one. 0 when the source is nobody the meter follows.
    uint64_t srcCombatant = 0;
    /// The tracked combatant that `dst` is - the unit itself, never through a
    /// pet: damage to a pet is not damage taken by its owner.
    uint64_t dstCombatant = 0;
    uint32_t spellId = 0;  ///< 0: a melee swing
    /// Damage including overkill, or healing including overhealing - the
    /// combat log's own amount.
    int32_t amount = 0;
    /// The overkill of a hit or the overhealing of a heal.
    int32_t over = 0;
    bool crit = false;
};

/// One ability's share of a combatant's numbers.
struct MeterAbility {
    uint32_t spellId = 0;
    /// The pet that used it, or 0 for the combatant's own.
    uint64_t pet = 0;
    double amount = 0;  ///< damage, effective healing, or damage taken
    double over = 0;    ///< overhealing
    uint32_t hits = 0;
    uint32_t crits = 0;
};

/// The hit that killed a combatant.
struct MeterDeath {
    double at = 0;          ///< seconds into the fight
    uint64_t killer = 0;
    uint32_t spellId = 0;
    int32_t amount = 0;
};

struct MeterCombatant {
    uint64_t guid = 0;
    double damage = 0;
    double healing = 0;   ///< effective: what was healed, not what was cast
    double overheal = 0;
    double taken = 0;
    uint32_t deaths = 0;
    std::vector<MeterAbility> dealt;
    std::vector<MeterAbility> heals;
    std::vector<MeterAbility> takenBy;
    std::vector<uint64_t> pets;
    /// The last hit taken, which becomes the death's cause if it was the end.
    MeterDeath lastHit;
    MeterDeath lastDeath;
};

/// A fight: from the group entering combat to it leaving, or a burst of
/// fighting the group was in while nobody's combat flag said so.
struct MeterFight {
    double start = 0;
    double end = 0;
    bool active = false;
    /// Whether anything was recorded; an empty fight is not kept.
    bool hasData = false;
    /// The unit the group did most damage to: what the fight is named after.
    uint64_t mainTarget = 0;
    std::vector<MeterCombatant> combatants;
    /// Damage done to each enemy, to pick the main target. Short: a fight
    /// rarely has more than a handful.
    std::vector<std::pair<uint64_t, double>> targets;

    [[nodiscard]] double duration(double now) const;
    [[nodiscard]] const MeterCombatant* find(uint64_t guid) const;
};

/// A bar: one combatant's value for the mode being shown.
struct MeterRow {
    const MeterCombatant* combatant = nullptr;
    double value = 0;      ///< what the bar is sorted by and filled to
    double total = 0;      ///< the amount (damage, healing...)
    double perSecond = 0;
    double percent = 0;    ///< of the group's total; for overhealing, of own healing
};

/// One line of a bar's tooltip.
struct MeterAbilityRow {
    uint32_t spellId = 0;
    uint64_t pet = 0;
    double amount = 0;
    double percent = 0;
    uint32_t hits = 0;
    uint32_t crits = 0;
};

/// The group's damage meter, apart from whatever draws it.
///
/// Fed one MeterEvent at a time from the combat log, and told every frame
/// whether the group is in combat, which is what decides where a fight begins
/// and ends. Nothing here allocates on a frame: storage grows when a new
/// combatant or ability first appears, and the views reuse what they have.
class DamageMeter {
public:
    /// Fights kept besides the current one.
    static constexpr std::size_t kHistory = 5;
    /// After combat drops, what still arrives this long after - a DoT's last
    /// tick, a pet's last swing - belongs to the fight that just ended.
    static constexpr double kLinger = 3.0;
    /// A fight nobody's combat flag started ends after this long without an
    /// event.
    static constexpr double kIdleEnd = 5.0;

    /// Which fight a view is of: the current (or last) one, all of them since
    /// the last reset, or one of the previous ones, newest first.
    static constexpr int kCurrent = 0;
    static constexpr int kOverall = 1;
    static constexpr int kPrevious = 2;  ///< kPrevious + i: the i-th previous

    DamageMeter();

    void record(double now, const MeterEvent& e);
    void update(double now, bool groupInCombat);
    void reset();

    [[nodiscard]] bool inFight() const { return !fights_.empty() && fights_.front().active; }
    /// The fights there are to choose among besides current and overall.
    [[nodiscard]] std::size_t previousCount() const {
        return fights_.empty() ? 0 : fights_.size() - 1;
    }
    /// The fight a selection names, or nullptr when there is none.
    [[nodiscard]] const MeterFight* fight(int sel) const;
    /// How long a selection lasted, for its per-second figures.
    [[nodiscard]] double duration(int sel, double now) const;

    /// The bars for a fight and mode, sorted, highest first. The span is
    /// valid until the next call to view(), record() or reset().
    struct View {
        const MeterRow* rows = nullptr;
        std::size_t count = 0;
        double total = 0;
        double maxValue = 0;
        double duration = 0;
    };
    const View& view(int sel, MeterMode mode, double now);

    /// The top abilities behind one combatant's bar, highest first. Valid
    /// until the next call.
    struct Abilities {
        const MeterAbilityRow* rows = nullptr;
        std::size_t count = 0;
    };
    const Abilities& abilities(int sel, MeterMode mode, uint64_t combatant, std::size_t max);

private:
    MeterFight& beginFight(double now);
    void endFight(double now);
    void addTo(MeterFight& f, const MeterEvent& e, double at);

    /// Newest first; front() is the current fight, active or not.
    std::deque<MeterFight> fights_;
    MeterFight overall_;
    /// The time of every finished fight added together, for overall's rate.
    double overallTime_ = 0;
    double lastEvent_ = 0;
    /// Whether the current fight was begun by a combat flag, rather than by
    /// an event that came while none was set.
    bool flagged_ = false;

    std::vector<MeterRow> rows_;
    std::vector<MeterAbilityRow> abilityRows_;
    View view_;
    Abilities abilities_;
};

}  // namespace game
}  // namespace wowee
