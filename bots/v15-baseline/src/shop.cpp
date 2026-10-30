// shop.cpp - shopping lists and the shop-trip trigger. Never throws: can_buy() is checked first. [S8]
//
// | Champion      | First 5 shards              | Then (repeat)                                         |
// | marcella      | jungle_shard_of_health      | monster_shard_of_focus x5, then power_shard_of_focus  |
// | stellar       | monster_shard_of_focus      | power_shard_of_focus                                  |
// | ren           | jungle_shard_of_health      | power_shard_of_strength                               |
// | heart_of_ruin | durability_shard_of_health  | power_shard_of_focus                                  |
// | cassi         | power_shard_of_focus        | power_shard_of_focus                                  |
// | issac         | durability_shard_of_health  | power_shard_of_strength                               |
// Worked example: Stellar has 3 monster shards and 100 gold -> buys 2 more (60), the emblem forms,
//   then 1 power_shard_of_focus (30); 10 gold left -> an altar lock if she has none.
#include "shop.h"
#include <vector>
#include "bank.h"
#include "comms.h"
#include "config.h"
#include "mapinfo.h"
#include "world.h"

namespace shop {
namespace {

using IT = bc::item_type;

struct ListStep { bc::item_type item; std::uint32_t count; };   // buy `item` until we own `count`

std::vector<ListStep> list_for(bc::champion_type t) {
    // v9: Treasure first (more gold), then Durability, then Jungle/Power (top-50 builds, ranked_meta_analysis.md).
    // v11: lists for the candidate-set champions (Willow heals scale with Focus; Prism and Tavi are
    // spell carries, so Power of Focus; Cassi's 200-burst nuke scales with Focus).
    switch (t) {
        case bc::champion_type::vesper_vale:
            return {{IT::treasure_shard_of_strength, 5}, {IT::durability_shard_of_health, 5}, {IT::power_shard_of_strength, 99}};
        case bc::champion_type::heart_of_ruin:   // v10 raider: Durability first (+500 health, -10% damage)
            return {{IT::durability_shard_of_health, 5}, {IT::treasure_shard_of_health, 5}, {IT::jungle_shard_of_focus, 5},
                    {IT::power_shard_of_focus, 99}};
        case bc::champion_type::marcella:
            return {{IT::treasure_shard_of_health, 5}, {IT::durability_shard_of_health, 5}, {IT::jungle_shard_of_health, 5},
                    {IT::power_shard_of_focus, 99}};
        case bc::champion_type::ryth:
            return {{IT::treasure_shard_of_health, 5}, {IT::durability_shard_of_health, 5}, {IT::jungle_shard_of_strength, 5},
                    {IT::power_shard_of_health, 99}};
        case bc::champion_type::vendetta:   // v10 hunter / raider: Durability first (vs Ryth: even -> clear win)
            return {{IT::durability_shard_of_health, 5}, {IT::treasure_shard_of_health, 5}, {IT::jungle_shard_of_strength, 5},
                    {IT::power_shard_of_strength, 99}};
        case bc::champion_type::stellar:
        case bc::champion_type::prism:      // v11: spell carries
        case bc::champion_type::tavi:
        case bc::champion_type::cassi:
        case bc::champion_type::willow:     // v11: 3 x Focus heal scales with Focus
            return {{IT::monster_shard_of_focus, 5}, {IT::power_shard_of_focus, 99}};
        default: return {{IT::power_shard_of_health, 5}, {IT::power_shard_of_strength, 99}};
    }
}

// Shards owned of this kind, counting those already combined into emblems (5 shards each;
// a Power emblem counts as 6 shards of stats but is still made of 5 bought shards).
std::uint32_t owned(const bc::enum_map<bc::item_type>& counts, bc::item_type shard) {
    const auto emblem = shard.result();
    const std::uint32_t per_emblem = shard.combine_count() > 0 ? shard.combine_count() : 5;
    return counts[shard] + (emblem ? counts[*emblem] * per_emblem : 0u);
}

bool owns_any_capture_emblem(const bc::enum_map<bc::item_type>& counts) {
    return counts[IT::capture_emblem_of_strength] + counts[IT::capture_emblem_of_focus] +
           counts[IT::capture_emblem_of_health] > 0;
}

// v11: one Capture emblem per farming squad, on the squad's lowest body slot — the champion that
// ends up standing on cleared regions (farm.cpp capture_step hands the region to the lowest body
// slot present). The engine cuts that squad's captures from 5 rounds to 3 while the holder stands
// there: about +5-10% farming throughput for 150 gold (OPTIMAL_STRATEGY.md §12).
bool is_capture_buyer(const Snapshot& s) {
    if (!CAPTURE_EMBLEMS) return false;
    for (bc::champion_type t : world::squad_members(s.squad, false))
        if (body_slot_of(t) != NO_SLOT && body_slot_of(t) < s.my_slot) return false;
    return true;
}

bool wants_lock_early(bc::champion_type t) {
    (void)t;
    return true;   // v9: every champion carries an altar lock once its first emblem is done (top-50: ~5 per game)
}

void buy_lock_if_wanted(bool first_emblem_done) {
    auto& me = bc::self();
    if (me.consumables()[bc::consumable_type::altar_lock].count() > 0) return;
    const bool early = wants_lock_early(me.champion_name()) && first_emblem_done;
    if (!early && me.gold_count() > static_cast<std::uint32_t>(LOCK_COST) + 20) return;   // others: leftover gold only
    if (me.gold_count() >= static_cast<std::uint32_t>(LOCK_COST) && me.can_buy(bc::consumable_type::altar_lock))
        me.buy(bc::consumable_type::altar_lock);
}

}  // namespace

bool capture_emblem_ready(const Snapshot& s) {
    if (!CAPTURE_EMBLEMS || !is_capture_buyer(s)) return false;
    return owns_any_capture_emblem(bc::self().item_count());
}

void buy_all(const Snapshot& s) {
    auto& me = bc::self();
    if (s.gold < LOCK_COST) return;
    const auto counts = me.item_count();
    std::vector<ListStep> list = list_for(s.my_type);
    // v11: insert the Capture emblem for this squad's buyer (health shards: the capturer is usually
    // the tank, and health is the stat it wants anyway). First on cluster maps, after the survival /
    // economy emblem on elk / golem maps where the tank's Jungle matters more.
    // v12: only when the whole emblem (5 x 30) is affordable in one visit - on gold-starved maps the
    // split purchases stalled at 2-3 shards forever (match 8d0f4189: 150 gold across two champions,
    // no emblem, no capture bonus, ever).
    if (is_capture_buyer(s) && !owns_any_capture_emblem(counts) && me.gold_count() >= 5u * 30u) {
        const ListStep capture{IT::capture_shard_of_health, 5};
        const std::size_t at = mapinfo::monster_gem_share() < CAPTURE_FIRST_MONSTER_SHARE ? 0 : 1;
        list.insert(list.begin() + std::min(at, list.size()), capture);
    }
    const bool first_done = owned(counts, list.front().item) >= list.front().count;
    if (first_done) buy_lock_if_wanted(true);
    for (const ListStep& step : list) {
        const std::uint32_t have = owned(counts, step.item);
        if (have >= step.count) continue;
        const std::uint32_t cost = step.item.cost();
        if (cost == 0) return;
        std::uint32_t n = std::min<std::uint32_t>(step.count - have, me.gold_count() / cost);
        while (n > 0 && !me.can_buy(step.item, n)) --n;
        if (n > 0) me.buy(step.item, n);
        break;   // finish one list step before starting the next
    }
    buy_lock_if_wanted(first_done);
}

bool maybe_trip(Snapshot& s) {
    if (s.on_our_spawn || s.gold < SHOP_GOLD || s.bag > 0) return false;
    if (!mapinfo::in_home(s.me)) return false;
    // A trip costs the recall plus the walk back out: on maps with a far spawn, only when rich.
    if (mapinfo::steps_from_us(s.me) > SHOP_TRIP_STEPS && s.gold < SHOP_RICH_GOLD) return false;
    const comms::Sighting seen = comms::read_sighting();
    if (seen.fresh && seen.pos.chebyshev_distance(s.me) <= 10) return false;
    // Farming pair: go together (the partner is also home) so neither farms alone.
    if (s.role == Role::TankFarmer && !comms::flag(comms::FLAG_PARTNER_HOME)) return false;
    if (s.role == Role::Farmer && !comms::flag(comms::FLAG_MARCELLA_HOME)) return false;
    if (!bank::recall_is_safe(s)) return false;
    bc::self().recall();
    return true;
}

}  // namespace shop
