// actions.h - casting spells and basic attacks on the best target in reach. [S6]
//
// What it decides: WHICH ability slot to use this turn and on WHAT, once a module decided to act.
// Worked example: Stellar is 3 tiles from a 120-health gem cluster and 4 from a wolf she is farming.
//   cast_best(): slot 0 (2 x Focus = 140 damage, range 4) on the cluster -> it dies in one cast.
//   attack_best(): basic attack (30 damage, range 3) on the wolf. She did not walk, so both are legal.
// Rules kept here: one successful cast per turn across both slots; "excludes walking" spells are
//   skipped when we will walk (and walking is blocked after one); a ranged champion that walked has
//   no basic attack left (me.can_attack() says so); attack resets give a second basic attack.
#pragma once
#include "types.h"

namespace actions {

// Target priorities for one call. focus_group: camp group we are farming (NO_GROUP = none; camps are
// then hit only when they are awake). focus_enemy: index into s.enemies to prefer (-1 = none).
struct Aim {
    int focus_group = NO_GROUP;
    int focus_enemy = -1;
    bool hit_camps = true;        // false in pure champion fights
};

bool cast_best(Snapshot& s, bool will_move, const Aim& aim);   // true if a spell was cast
bool attack_best(Snapshot& s, const Aim& aim);                  // true if we basic-attacked
bool use_shield_if_hit(Snapshot& s);                            // Heart of Ruin: shield when threatened
// Heart of Ruin: dash to a tile next to `target` so the landing (1.5 x Focus + stun) hits it.
bool dash_onto(Snapshot& s, vec2 target);
bool has_ready_stun(const Snapshot& s);                         // a ready slot that stuns a champion
// v11 Willow: heal the visible ally within range missing the most health (>= HEAL_MIN_MISSING).
// Willow's heal is "compatible with walking", so the caller may still move and attack this turn.
bool heal_best(Snapshot& s);

}  // namespace actions
