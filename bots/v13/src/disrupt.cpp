// disrupt.cpp - Cassi & Issac only. Order each turn: flee 2+ enemies, regroup, lock, wake a monster. [S9]
//
// Worked example (waking a monster): a sleeping elk (range 4) sits 3 tiles from an enemy Stellar who
//   is recalling, and 4 tiles from Cassi. The Stellar is nearer to the elk than Cassi and Issac, so Cassi
//   hits the elk with her range-4 spell. The elk wakes and hits its NEAREST champion - the Stellar -
//   and the damage cancels her recall.
// Stuns on channelers and fights with weak enemies are handled earlier by combat::handle_enemies.
#include "disrupt.h"
#include <vector>
#include "actions.h"
#include "comms.h"
#include "config.h"
#include "mapinfo.h"
#include "move.h"
#include "world.h"

namespace disrupt {
namespace {

bc::champion_type partner_of(bc::champion_type me) {
    return me == bc::champion_type::cassi ? bc::champion_type::issac : bc::champion_type::cassi;
}

const AllyNow* visible_partner(const Snapshot& s) {
    for (const AllyNow& a : s.allies)
        if (a.type == partner_of(s.my_type)) return &a;
    return nullptr;
}

bool flee_groups(Snapshot& s) {
    std::vector<vec2> threats;
    for (const EnemyNow& e : s.enemies)
        if (!e.on_their_spawn && e.pos.chebyshev_distance(s.me) <= ENGAGE_RANGE) threats.push_back(e.pos);
    if (threats.size() < 2) return false;
    actions::Aim aim;
    aim.hit_camps = false;
    actions::cast_best(s, true, aim);
    return move::step_away(s, threats) || true;
}

// Cassi-Issac rule: never end a turn more than CI_MAX_GAP from the partner when it is visible.
bool regroup(Snapshot& s) {
    const AllyNow* partner = visible_partner(s);
    if (!partner || partner->pos.chebyshev_distance(s.me) < CI_MAX_GAP) return false;
    return move::step_toward(s, move::square(partner->pos, 2));
}

bool enemy_heading_to(const EnemyNow& e, vec2 altar) {
    return e.handle.is_submitting_gems() || (e.bag >= LOCK_ENEMY_BAG && e.pos.chebyshev_distance(altar) <= 3);
}

bool place_lock(Snapshot& s) {
    auto& me = bc::self();
    const bc::consumable& lock = me.consumables()[bc::consumable_type::altar_lock];
    if (lock.count() == 0) return false;
    for (vec2 altar : mapinfo::altars()) {
        if (altar.chebyshev_distance(s.me) > 2 || bc::vision::has_altar_lock(altar)) continue;
        for (const EnemyNow& e : s.enemies)
            if (enemy_heading_to(e, altar) && lock.use(altar)) return true;
    }
    return false;
}

bool wakeable(const CampNow& c) {
    return !c.awake && (c.kind == bc::camp_type::elk || c.kind == bc::camp_type::golem || c.kind == bc::camp_type::monolith);
}

bool good_victim(const Snapshot& s, const EnemyNow& e, const CampNow& c) {
    const long their = e.pos.chebyshev_distance(c.pos);
    if (their > c.attack_range || s.me.chebyshev_distance(c.pos) <= their) return false;
    const AllyNow* partner = visible_partner(s);
    if (partner && partner->pos.chebyshev_distance(c.pos) <= their) return false;
    const bool capturing = mapinfo::group_at(e.pos) != NO_GROUP;
    return e.channeling || capturing || e.health < 0.5f * e.max_health;
}

bool wake_monster(Snapshot& s) {
    for (const CampNow& c : s.camps) {
        if (!wakeable(c) || c.pos.chebyshev_distance(s.me) > 4) continue;
        for (const EnemyNow& e : s.enemies) {
            if (!good_victim(s, e, c)) continue;
            actions::Aim aim;
            aim.focus_group = c.group;   // allows hitting this sleeping camp
            if (actions::cast_best(s, false, aim) || actions::attack_best(s, aim)) return true;
        }
    }
    return false;
}

}  // namespace

bool step(Snapshot& s) {
    if (s.role != Role::DisruptorCaster && s.role != Role::DisruptorMelee) return false;
    if (flee_groups(s)) return true;
    if (place_lock(s)) return true;
    if (wake_monster(s)) return true;
    return regroup(s);
}

}  // namespace disrupt
