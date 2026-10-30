// bank.cpp - P_die, the per-role bank rules, altar choice and the tribute itself. [S7]
//
// Worked example (others' rule): Heart of Ruin in a 3-champion game carries 22 gems at 45% health.
//   His threshold BAG_HOR = 40 is halved (health < 50%) to 20 -> 22 >= 20 -> GoTribute.
//   With an altar 6 tiles away (<= HOR_ANY_BAG_ALTAR = 8) he would bank even 5 gems.
// Tribute facts: stand within 1 of an altar; it lasts max(1, ceil(bag / 10)) turn ends; damage does not
//   cancel it but a stun, death or a newly placed enemy altar lock does.
#include "bank.h"
#include <algorithm>
#include <cmath>
#include "combat.h"
#include "comms.h"
#include "config.h"
#include "mapinfo.h"
#include "move.h"
#include "world.h"

namespace bank {

// Steps to an altar we can tribute at now. Normally the static field; on a gate map (the only altar ring
// tiles are monster spawn tiles) the open field counts once every gate group is dead.
// v10 fix (boss-battle freeze, Oct 25): with no reachable altar this is FAR_AWAY and every banking rule
// below says "keep", instead of the endgame rule firing forever on a 9,999-step trip.
bool gates_open() {
    if (!mapinfo::any_gate()) return false;
    for (const GroupInfo& g : mapinfo::groups())
        if (g.gate && !(g.state == GroupState::Respawning || g.state == GroupState::RewardAvailable)) return false;
    return true;
}

int reach_steps(const Snapshot& s) {
    const int closed = mapinfo::steps_to_altar(s.me);
    if (closed < FAR_AWAY) return closed;
    if (gates_open()) return mapinfo::steps_to_altar_open(s.me);
    return FAR_AWAY;
}

namespace {

bool is_farming_pair(Role r) { return r == Role::TankFarmer || r == Role::Farmer; }

// Chance that one enemy, met during the trip, kills us (kill race with its live or base stats).
double p_lose_to(const Snapshot& s, bc::champion_type type, float health, float strength, float focus) {
    const double their_rounds = combat::rounds_to_kill(s.health, combat::burst(type, strength, focus),
                                                       combat::damage_per_round(type, strength, focus), 0);
    const double our_rounds = combat::rounds_to_kill(health, combat::my_burst_now(s),
                                                     combat::damage_per_round(s.my_type, s.strength, s.focus), 0);
    return 1.0 - combat::p_win(their_rounds, our_rounds, false);
}

// Chance we meet an enemy now `distance` tiles away during a trip of `t_bank` rounds.
double p_contact(double distance, double t_bank) {
    return std::clamp((t_bank + CONTACT_SLACK - distance) / (t_bank + CONTACT_SLACK), 0.0, 1.0);
}

double enemy_factor(const Snapshot& s, int t_bank) {
    double survive = 1.0;
    for (const EnemyNow& e : s.enemies) {
        if (e.on_their_spawn) continue;
        const double d = static_cast<double>(e.pos.chebyshev_distance(s.me));
        survive *= 1.0 - p_contact(d, t_bank) * p_lose_to(s, e.type, e.health, e.strength, e.focus);
    }
    for (const EnemyMemory& m : world::enemy_memory()) {
        if (!m.exists || !m.alive || m.last_seen_round == s.round || s.round - m.last_seen_round > MEMORY_ROUNDS) continue;
        const bc::champion_stats base = combat::base_stats_of(m.type);
        const double d = static_cast<double>(m.last_pos.chebyshev_distance(s.me)) - (s.round - m.last_seen_round);
        survive *= 1.0 - p_contact(std::max(0.0, d), t_bank) * p_lose_to(s, m.type, m.health, base.base_strength, base.base_focus);
    }
    return survive;
}

// Monsters destroy the bag when they kill us: being hit while hurt is a real risk.
double monster_factor(const Snapshot& s) {
    for (const CampNow& c : s.camps)
        if (c.awake && c.strength > 0 && c.pos.chebyshev_distance(s.me) <= c.attack_range)
            return s.health < 0.5f * s.max_health ? 0.8 : 0.95;
    return 1.0;
}

bool enemy_locked(vec2 altar) {
    return bc::vision::has_altar_lock(altar) && bc::vision::altar_lock_at(altar).is_enemy();
}

// Altar ring tiles we may walk to: the safe ring (outside the enemy zone) minus visibly enemy-locked
// altars (Marcella breaks locks, so she keeps them).
bc::bitboard altar_goals(const Snapshot& s) {
    bc::bitboard goals = mapinfo::safe_altar_ring();
    if (s.my_type == bc::champion_type::marcella) return goals;
    for (vec2 a : mapinfo::altars())
        if (world::tile_visible(a) && enemy_locked(a)) goals &= ~move::square(a, 1);
    return goals.is_empty() ? mapinfo::safe_altar_ring() : goals;
}

int bag_threshold(const Snapshot& s) {
    int threshold = s.role == Role::Interceptor ? BAG_HOR : (s.role == Role::FarPair ? BAG_FAR : BAG_CI);
    const comms::Emergency seen = comms::read_emergency();
    const bool danger_near = seen.fresh && seen.pos.chebyshev_distance(s.me) <= HELP_RANGE;
    if (s.health < 0.5 * s.max_health || danger_near) threshold /= 2;
    return std::max(1, threshold);
}

BankChoice escalate(const Snapshot& s, double pd) {
    if (pd >= VERY_HIGH_PDIE && recall_is_safe(s)) return BankChoice::RecallThenTribute;
    return BankChoice::GoTribute;
}

// Recall then walk from spawn, when that is clearly faster than walking from here.
bool recall_is_faster(const Snapshot& s) {
    const int from_here = reach_steps(s);
    const int via_home = mapinfo::home_to_altar_steps() + RECALL_TURNS + RECALL_BANK_MARGIN;
    return from_here < FAR_AWAY && via_home < FAR_AWAY && from_here > via_home;
}

}  // namespace

int tribute_turns(int bag) { return std::max(1, (bag + 9) / 10); }

int bank_rounds(const Snapshot& s) { return reach_steps(s) + tribute_turns(s.bag); }

double p_die(const Snapshot& s, int t_bank_rounds) {
    const double background = std::exp(-H0 * (s.max_health / std::max(1.0f, s.health)) * t_bank_rounds);
    return 1.0 - background * enemy_factor(s, t_bank_rounds) * monster_factor(s);
}

bool recall_is_safe(const Snapshot& s) {
    for (const EnemyNow& e : s.enemies)
        if (!e.on_their_spawn && e.pos.chebyshev_distance(s.me) <= RECALL_SAFE_ENEMY + std::max(e.attack_range, 4)) return false;
    for (const CampNow& c : s.camps)
        if (c.awake && c.strength > 0 && c.pos.chebyshev_distance(s.me) <= c.attack_range + 1) return false;
    return !s.on_our_spawn;
}

BankChoice bank_choice(const Snapshot& s) {
    if (s.bag <= 0 || mapinfo::altars().empty()) return BankChoice::Keep;
    const int steps = reach_steps(s);
    if (steps >= FAR_AWAY) return BankChoice::Keep;   // no altar we can reach: keep playing
    if (FREE_BANK_ENABLED && steps <= FREE_BANK_STEPS) return BankChoice::GoTribute;
    const double pd = p_die(s, bank_rounds(s));
    // v11: Prism's carried gems are +1.5 Focus each (a 40-gem bag is +60 Focus on every spell), so a
    // healthy Prism holds a bigger bag before banking. Never in contact (bag_threshold halves that).
    const double bank_factor = s.my_type == bc::champion_type::prism && s.health > 0.6 * s.max_health ? PRISM_BANK_FACTOR : 1.0;
    if (is_farming_pair(s.role)) {
        if (s.bag >= CHEAP_BANK_BAG && steps <= CHEAP_BANK_STEPS) return BankChoice::GoTribute;
        // Far altars: bank once the bag pays for the walk (rushbait, Oct 24: Marcella carried 598 gems
        // 36 steps from the nearest altar and never banked). Example: 36 steps -> 20 + 1.0 x 36 = 56 gems.
        // Capped (v10): shortcut, Oct 25 online: a 110-step altar made the threshold 140 and HoR carried
        // 157 gems until he died. Now at most FARM_BAG_CAP.
        if (s.bag >= std::min<double>(FARM_BAG_CAP, FARM_BAG_BASE * bank_factor + FARM_BAG_PER_STEP * steps))
            return escalate(s, pd);
        if (s.bag < FARM_BANK_BAG * bank_factor) return BankChoice::Keep;
        if (pd <= FARM_KEEP_PDIE) return BankChoice::Keep;
        return escalate(s, pd);
    }
    if (s.my_type == bc::champion_type::heart_of_ruin && steps <= HOR_ANY_BAG_ALTAR) return BankChoice::GoTribute;
    if (pd >= VERY_HIGH_PDIE) return escalate(s, pd);
    return s.bag >= bag_threshold(s) ? BankChoice::GoTribute : BankChoice::Keep;
}

// Gems carried by me and the allies I can see (gem_count of an ally out of vision throws
// api_not_in_vision, bench Oct 24), only asked when the team could be close to WIN_GEMS.
int team_bag(const Snapshot& s) {
    int bag = s.bag;
    for (const AllyNow& a : s.allies) bag += static_cast<int>(a.handle.gem_count());
    return bag;
}

bool endgame_or_win(const Snapshot& s) {
    if (s.bag <= 0 || mapinfo::altars().empty()) return false;
    if (reach_steps(s) >= FAR_AWAY) return false;   // v10: no path, no endgame walk (it froze v8 / v9)
    if (s.banked_us + s.bag >= WIN_GEMS) return true;
    // Team endgame: when banked + everything we carry reaches the win line, everyone banks now.
    // Example: banked 880, bags 40 + 35 + 30 + 20 = 125 -> 1,005 >= 1,000 -> all four walk to altars.
    if (TEAM_ENDGAME && s.banked_us + s.bag + TEAM_ENDGAME_SLACK >= WIN_GEMS - 400 && s.banked_us + team_bag(s) >= WIN_GEMS)
        return true;
    return s.round + bank_rounds(s) + ENDGAME_MARGIN >= LAST_ROUND;
}

namespace {

// Next to an altar: tribute (Marcella breaks an enemy lock with basic attacks first).
bool tribute_here(Snapshot& s, vec2 altar) {
    auto& me = bc::self();
    if (enemy_locked(altar)) {
        if (s.my_type != bc::champion_type::marcella || !me.can_attack()) return false;
        me.attack(bc::vision::altar_lock_at(altar));
        return true;
    }
    me.submit_gems(altar);
    return true;
}

// Not Marcella: an enemy with a stun that can reach range 4 before the tribute ends would cancel it.
bool stun_threat(const Snapshot& s) {
    if (s.my_type == bc::champion_type::marcella) return false;
    const int turns = tribute_turns(s.bag);
    for (const EnemyNow& e : s.enemies)
        if (!e.on_their_spawn && e.pos.chebyshev_distance(s.me) <= 4 + turns) return true;
    return false;
}

}  // namespace

bool go_tribute(Snapshot& s) {
    if (s.bag <= 0 || reach_steps(s) >= FAR_AWAY) return false;   // nothing to do: let the ladder go on
    // A stun threat makes us wait next to the altar, but at most STUN_WAIT_TURNS turns in a row: a cancelled
    // tribute only costs time, and waiting forever let the farm step pull us away and back every other turn
    // (gold-rush, Oct 24: Vendetta held 22 gems next to an altar for 700 rounds).
    static int waited = 0;
    static int last_wait_round = -10;
    for (vec2 altar : mapinfo::altars()) {
        if (altar.chebyshev_distance(s.me) > 1) continue;
        if (stun_threat(s) && !endgame_or_win(s) && waited < STUN_WAIT_TURNS) {
            waited = s.round - last_wait_round <= 3 ? waited + 1 : 1;
            last_wait_round = s.round;
            return true;   // hold position (still counts as our turn's job, so farming does not walk us off)
        }
        waited = 0;
        if (tribute_here(s, altar)) return true;
    }
    if (recall_is_faster(s) && recall_is_safe(s)) {
        bc::self().recall();
        return true;
    }
    return move::travel_to_altar(s, altar_goals(s));
}

bool handle(Snapshot& s) {
    switch (bank_choice(s)) {
        case BankChoice::Keep: return false;
        case BankChoice::GoTribute: return go_tribute(s);
        case BankChoice::RecallThenTribute:
            if (reach_steps(s) <= 3) return go_tribute(s);   // already there
            bc::self().recall();
            return true;
    }
    return false;
}

}  // namespace bank
