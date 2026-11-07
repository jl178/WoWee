#include "game/damage_meter.hpp"

#include <algorithm>

namespace wowee {
namespace game {

namespace {

MeterCombatant& combatantIn(MeterFight& f, uint64_t guid) {
    for (auto& c : f.combatants)
        if (c.guid == guid) return c;
    f.combatants.emplace_back();
    f.combatants.back().guid = guid;
    return f.combatants.back();
}

MeterAbility& abilityIn(std::vector<MeterAbility>& list, uint32_t spellId, uint64_t pet) {
    for (auto& a : list)
        if (a.spellId == spellId && a.pet == pet) return a;
    list.emplace_back();
    list.back().spellId = spellId;
    list.back().pet = pet;
    return list.back();
}

void notePet(MeterCombatant& c, uint64_t pet) {
    if (pet == 0) return;
    if (std::find(c.pets.begin(), c.pets.end(), pet) == c.pets.end()) c.pets.push_back(pet);
}

double totalFor(const MeterCombatant& c, MeterMode mode) {
    switch (mode) {
        case MeterMode::DamageDone:
        case MeterMode::Dps: return c.damage;
        case MeterMode::HealingDone:
        case MeterMode::Hps: return c.healing;
        case MeterMode::DamageTaken: return c.taken;
        case MeterMode::Overhealing: return c.overheal;
        case MeterMode::Deaths: return static_cast<double>(c.deaths);
        case MeterMode::Count: break;
    }
    return 0;
}

}  // namespace

double MeterFight::duration(double now) const {
    const double stop = active ? now : end;
    return stop > start ? stop - start : 0.0;
}

const MeterCombatant* MeterFight::find(uint64_t guid) const {
    for (const auto& c : combatants)
        if (c.guid == guid) return &c;
    return nullptr;
}

DamageMeter::DamageMeter() {
    // A raid and its pets, so a 25-man never grows these mid-fight.
    rows_.reserve(64);
    abilityRows_.reserve(64);
}

void DamageMeter::reset() {
    fights_.clear();
    overall_ = MeterFight{};
    overallTime_ = 0;
    lastEvent_ = 0;
    flagged_ = false;
    rows_.clear();
    abilityRows_.clear();
    view_ = View{};
    abilities_ = Abilities{};
}

MeterFight& DamageMeter::beginFight(double now) {
    // The oldest goes before the new one comes, so the history never holds
    // more than kHistory fights besides the current.
    while (fights_.size() > kHistory) fights_.pop_back();
    fights_.emplace_front();
    MeterFight& f = fights_.front();
    f.start = now;
    f.end = now;
    f.active = true;
    lastEvent_ = now;
    return f;
}

void DamageMeter::endFight(double now) {
    if (!inFight()) return;
    MeterFight& f = fights_.front();
    f.active = false;
    f.end = now;
    flagged_ = false;
    // A fight with nothing in it - a mob that evaded, a combat flag from an
    // aggro that never landed a blow - would only push a real one out of the
    // history.
    if (!f.hasData) {
        fights_.pop_front();
        return;
    }
    overallTime_ += f.duration(now);
}

void DamageMeter::update(double now, bool groupInCombat) {
    if (groupInCombat) {
        if (!inFight()) beginFight(now);
        // A fight an event began is the same fight once the flag arrives.
        flagged_ = true;
        return;
    }
    if (!inFight()) return;
    // A flagged fight ends with its flag. One without ends where its last
    // event was, so the wait for more does not count against its rate.
    if (flagged_) endFight(now);
    else if (now - lastEvent_ >= kIdleEnd) endFight(lastEvent_);
}

void DamageMeter::record(double now, const MeterEvent& e) {
    if (e.srcCombatant == 0 && e.dstCombatant == 0) return;
    // Healing is only ever done by a combatant; a heal on one from someone
    // the meter does not follow says nothing about the group.
    if (e.kind == MeterEvent::Heal && e.srcCombatant == 0) return;
    if (e.kind == MeterEvent::Death && e.dstCombatant == 0) return;

    if (!inFight()) {
        const bool lingering = !fights_.empty() && now - fights_.front().end <= kLinger;
        if (!lingering) {
            // Fighting nobody's combat flag announced - a group member out of
            // sight, a flag that arrives late. Healing out of combat is a
            // top-up between pulls and starts nothing.
            if (e.kind != MeterEvent::Damage) return;
            beginFight(now);
            flagged_ = false;
        }
    }
    MeterFight& f = fights_.front();
    const double at = now - f.start;
    addTo(f, e, at);
    addTo(overall_, e, at);
    overall_.active = false;
    lastEvent_ = now;
}

void DamageMeter::addTo(MeterFight& f, const MeterEvent& e, double at) {
    f.hasData = true;
    const uint64_t pet = (e.srcCombatant != 0 && e.src != e.srcCombatant) ? e.src : 0;
    switch (e.kind) {
        case MeterEvent::Damage: {
            const double amount = e.amount > 0 ? e.amount : 0;
            // Damage done to the group by the group - a warlock's Life Tap, a
            // mind-controlled friend - is not damage done.
            if (e.srcCombatant != 0 && e.dstCombatant == 0) {
                MeterCombatant& c = combatantIn(f, e.srcCombatant);
                c.damage += amount;
                MeterAbility& a = abilityIn(c.dealt, e.spellId, pet);
                a.amount += amount;
                ++a.hits;
                if (e.crit) ++a.crits;
                notePet(c, pet);
                if (e.dst != 0) {
                    double* tally = nullptr;
                    for (auto& t : f.targets)
                        if (t.first == e.dst) tally = &t.second;
                    if (!tally) {
                        f.targets.emplace_back(e.dst, 0.0);
                        tally = &f.targets.back().second;
                    }
                    *tally += amount;
                    double best = 0;
                    for (const auto& t : f.targets)
                        if (t.first == f.mainTarget) best = t.second;
                    if (*tally > best) f.mainTarget = e.dst;
                }
            }
            if (e.dstCombatant != 0) {
                MeterCombatant& c = combatantIn(f, e.dstCombatant);
                c.taken += amount;
                MeterAbility& a = abilityIn(c.takenBy, e.spellId, 0);
                a.amount += amount;
                ++a.hits;
                if (e.crit) ++a.crits;
                c.lastHit = MeterDeath{at, e.src, e.spellId, e.amount};
            }
            break;
        }
        case MeterEvent::Heal: {
            const double over = std::clamp(static_cast<double>(e.over), 0.0,
                                           static_cast<double>(std::max(e.amount, 0)));
            const double effective = std::max(static_cast<double>(e.amount) - over, 0.0);
            MeterCombatant& c = combatantIn(f, e.srcCombatant);
            c.healing += effective;
            c.overheal += over;
            MeterAbility& a = abilityIn(c.heals, e.spellId, pet);
            a.amount += effective;
            a.over += over;
            ++a.hits;
            if (e.crit) ++a.crits;
            notePet(c, pet);
            break;
        }
        case MeterEvent::Death: {
            MeterCombatant& c = combatantIn(f, e.dstCombatant);
            ++c.deaths;
            c.lastDeath = c.lastHit;
            c.lastDeath.at = at;
            break;
        }
    }
}

const MeterFight* DamageMeter::fight(int sel) const {
    if (sel == kOverall) return &overall_;
    if (sel == kCurrent) return fights_.empty() ? nullptr : &fights_.front();
    if (sel < kPrevious) return nullptr;
    const auto index = static_cast<std::size_t>(sel - kPrevious) + 1;
    return index < fights_.size() ? &fights_[index] : nullptr;
}

double DamageMeter::duration(int sel, double now) const {
    if (sel == kOverall) {
        const double running = inFight() ? fights_.front().duration(now) : 0.0;
        return overallTime_ + running;
    }
    const MeterFight* f = fight(sel);
    return f ? f->duration(now) : 0.0;
}

const DamageMeter::View& DamageMeter::view(int sel, MeterMode mode, double now) {
    rows_.clear();
    view_ = View{};
    const MeterFight* f = fight(sel);
    if (!f) return view_;
    view_.duration = duration(sel, now);
    // A second at least, or the first hit of a fight reads as a vast rate.
    const double perSecondOver = std::max(view_.duration, 1.0);
    const bool rate = mode == MeterMode::Dps || mode == MeterMode::Hps;
    for (const auto& c : f->combatants) {
        const double total = totalFor(c, mode);
        if (total <= 0) continue;
        MeterRow r;
        r.combatant = &c;
        r.total = total;
        r.perSecond = total / perSecondOver;
        r.value = rate ? r.perSecond : total;
        if (mode == MeterMode::Overhealing) {
            const double cast = c.healing + c.overheal;
            r.percent = cast > 0 ? 100.0 * c.overheal / cast : 0.0;
        }
        view_.total += total;
        rows_.push_back(r);
    }
    if (mode != MeterMode::Overhealing && view_.total > 0) {
        for (auto& r : rows_) r.percent = 100.0 * r.total / view_.total;
    }
    std::sort(rows_.begin(), rows_.end(), [](const MeterRow& a, const MeterRow& b) {
        if (a.value != b.value) return a.value > b.value;
        return a.combatant->guid < b.combatant->guid;
    });
    view_.rows = rows_.data();
    view_.count = rows_.size();
    view_.maxValue = rows_.empty() ? 0.0 : rows_.front().value;
    return view_;
}

const DamageMeter::Abilities& DamageMeter::abilities(int sel, MeterMode mode, uint64_t combatant,
                                                     std::size_t max) {
    abilityRows_.clear();
    abilities_ = Abilities{};
    const MeterFight* f = fight(sel);
    const MeterCombatant* c = f ? f->find(combatant) : nullptr;
    if (!c) return abilities_;
    const std::vector<MeterAbility>* list = nullptr;
    bool overheal = false;
    switch (mode) {
        case MeterMode::DamageDone:
        case MeterMode::Dps: list = &c->dealt; break;
        case MeterMode::HealingDone:
        case MeterMode::Hps: list = &c->heals; break;
        case MeterMode::Overhealing: list = &c->heals; overheal = true; break;
        case MeterMode::DamageTaken: list = &c->takenBy; break;
        case MeterMode::Deaths:
        case MeterMode::Count: break;
    }
    if (!list) return abilities_;
    double total = 0;
    for (const auto& a : *list) {
        const double v = overheal ? a.over : a.amount;
        if (v <= 0) continue;
        MeterAbilityRow r;
        r.spellId = a.spellId;
        r.pet = a.pet;
        r.amount = v;
        r.hits = a.hits;
        r.crits = a.crits;
        total += v;
        abilityRows_.push_back(r);
    }
    const auto keep = std::min(max, abilityRows_.size());
    std::partial_sort(abilityRows_.begin(), abilityRows_.begin() + static_cast<std::ptrdiff_t>(keep),
                      abilityRows_.end(), [](const MeterAbilityRow& a, const MeterAbilityRow& b) {
                          if (a.amount != b.amount) return a.amount > b.amount;
                          return a.spellId < b.spellId;
                      });
    abilityRows_.resize(keep);
    for (auto& r : abilityRows_) r.percent = total > 0 ? 100.0 * r.amount / total : 0.0;
    abilities_.rows = abilityRows_.data();
    abilities_.count = abilityRows_.size();
    return abilities_;
}

}  // namespace game
}  // namespace wowee
