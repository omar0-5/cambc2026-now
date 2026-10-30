// shop.h - what to buy, and when to go home to buy it. [S8]
//
// What it decides: the shopping list per champion and the "recall home to shop" trigger.
// Worked example: Marcella with 190 gold on her spawn buys 5 x jungle_shard_of_health (150 gold) ->
//   they combine into a Jungle emblem (-30% damage from camps); 40 gold left -> 1 altar lock (10)
//   and 1 monster_shard_of_focus (30).
#pragma once
#include "types.h"

namespace shop {

void buy_all(const Snapshot& s);     // on our spawn or while dead: v13 shop v4 (spec §12)
void note_gold(int gold_now);        // v13: lifetime gold earned (Greed decision)
bool maybe_trip(Snapshot& s);        // ladder step 8: recall home to shop when rich and safe
bool capture_emblem_ready(const Snapshot& s);  // v11: I am my squad's Capture-emblem buyer and own it

}  // namespace shop
