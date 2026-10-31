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
#include <array>
#include <cmath>
#include <optional>
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

// ---------------------------------------------------------------- v13: shop v4 (spec §12)
// Passive families first in the role's order (each passive emblem = the stats of 5 plain shards plus a free
// passive), finish the family in progress, the stat chosen by the §12.2 marginal value from live stats and
// design growths, then Greed (>= 750 g ahead in that stat) / Power, Bruiser only for three-stat champions.
// v12's Capture emblem for its squad's capturer and its altar locks are kept.
namespace {

using F = bc::item_family;
using St = bc::item_stat;

struct Kit {
    float g_hp = 0, g_str = 0, g_focus = 0, camp_mult = 1;
    int resets = 0;
    float spell = 0, aoe = 0, heal = 0;
    bool three_stat = false;
    int role = 1;   // 0 tank, 1 farmer, 2 economy, 3 caster, 4 support
};

const Kit& kit(bc::champion_type t) {
    static std::array<std::optional<Kit>, 16> cache;
    auto& slot = cache[static_cast<std::size_t>(t) & 15];
    if (slot) return *slot;
    Kit k;
    const bc::champion c(t);
    const auto bs = c.base_stats();
    k.g_hp = bs.health_growth; k.g_str = bs.strength_growth; k.g_focus = bs.focus_growth;
    k.camp_mult = bs.camp_damage_multiplier > 0 ? bs.camp_damage_multiplier : 1.0f;
    for (const bc::ability& a : c.abilities()) {
        const auto& info = a.info();
        if (!info) continue;
        if (std::get_if<bc::ability_attack_reset>(&*info)) ++k.resets;
        else if (const auto* p = std::get_if<bc::ability_damage>(&*info)) k.spell += p->modifier;
        else if (const auto* p = std::get_if<bc::ability_cc>(&*info)) k.spell += p->modifier;
        else if (const auto* p = std::get_if<bc::ability_aoe_damage>(&*info)) k.aoe += p->modifier;
        else if (const auto* p = std::get_if<bc::ability_dash>(&*info)) k.aoe += p->modifier;
        else if (const auto* p = std::get_if<bc::ability_heal>(&*info)) k.heal += p->modifier;
    }
    using T = bc::champion_type;
    k.three_stat = t == T::marcella || t == T::stellar || t == T::prism || t == T::tavi || t == T::cassi;
    k.role = (t == T::marcella || t == T::ryth || t == T::heart_of_ruin) ? 0
           : t == T::vesper_vale ? 2
           : (t == T::stellar || t == T::prism || t == T::cassi || t == T::tavi) ? 3
           : t == T::willow ? 4 : 1;
    slot = k;
    return *slot;
}

// v13 task 2: Monster first for everyone (+15 % camp damage multiplies every capture, spec §3.3), then the
// role's passives. Ladder builds never had Monster.
std::vector<F> family_order(const Kit& k) {
    switch (k.role) {
        case 0: return {F::monster, F::jungle, F::durability, F::collectors};
        case 2: return {F::monster, F::treasure, F::durability, F::collectors};
        case 3: return {F::monster, F::skirmish, F::durability, F::collectors};
        case 4: return {F::monster, F::durability, F::home, F::collectors};
        default: return {F::monster, F::durability, F::collectors};
    }
}

struct Stats { float S, Fo, H; };

float dps_camp(const Kit& k, const Stats& s) { return k.camp_mult * (s.S * (1.0f + k.resets / 8.0f) + (k.spell * s.Fo + 1.5f * k.aoe * s.Fo) / 8.0f); }
float dps_champ(const Kit& k, const Stats& s) { return s.S * (1.0f + k.resets / 8.0f) + (k.spell + k.aoe + k.heal) * s.Fo / 8.0f; }
float burst(const Kit& k, const Stats& s) { return s.S * (k.resets ? 2.0f : 1.0f) + std::max(k.spell, k.aoe) * s.Fo; }

float value(bc::champion_type t, const Kit& k, const Stats& a, const Stats& b) {
    // Role weights §12.2 {farm, fight, tank, burst}.
    static const float W[5][4] = {{0.5f, 0.5f, 1.0f, 0.0f}, {1.0f, 0.4f, 0.2f, 0.0f}, {1.0f, 0.4f, 0.2f, 0.0f},
                                  {0.6f, 1.0f, 0.2f, 0.5f}, {0.3f, 1.0f, 0.3f, 0.0f}};
    const float* w = W[k.role];
    auto dl = [](float x, float y) { return (x > 0 && y > 0) ? std::log(y / x) : 0.0f; };
    (void)t;
    return w[0] * dl(dps_camp(k, a), dps_camp(k, b)) + w[1] * (dl(dps_champ(k, a), dps_champ(k, b)) + dl(a.H, b.H)) +
           w[2] * dl(a.H, b.H) + w[3] * dl(burst(k, a), burst(k, b));
}

St best_stat(bc::champion_type t, const Kit& k, const Stats& s) {
    St best = St::strength;
    float bv = -1e9f;
    for (St st : {St::strength, St::health, St::focus}) {
        Stats b = s;
        if (st == St::strength) b.S += k.g_str;
        if (st == St::focus) b.Fo += k.g_focus;
        if (st == St::health) { b.H += k.g_hp; if (t == bc::champion_type::ryth) b.S += 0.01f * k.g_hp; }
        const float v = value(t, k, s, b);
        if (v > bv) { bv = v; best = st; }
    }
    return best;
}

bool has_passive(const bc::enum_map<bc::item_type>& c, F f) {
    for (St st : {St::strength, St::health, St::focus})
        if (c[bc::get_item_enum(f, st, false)] > 0) return true;
    return false;
}

int g_last_gold = -1, g_earned = 0;

// Next shard to buy and how many in a row (to complete its emblem).
bool next_item(bc::champion_type t, const bc::enum_map<bc::item_type>& c, const Stats& s, bc::item_type& item, int& count) {
    const Kit& k = kit(t);
    for (int f = 0; f <= static_cast<int>(F::collectors); ++f)
        for (St st : {St::strength, St::health, St::focus}) {
            const bc::item_type shard = bc::get_item_enum(static_cast<F>(f), st, true);
            const std::uint32_t have = c[shard];
            if (have == 0) continue;
            const int need = static_cast<int>(shard.combine_count()) - static_cast<int>(have);
            if (need <= 0) continue;
            item = shard;
            count = need;
            return true;
        }
    int done = 0;
    for (int f = 0; f <= static_cast<int>(F::collectors); ++f) {
        const F fam = static_cast<F>(f);
        if (fam != F::power && fam != F::bruiser && fam != F::greed && has_passive(c, fam)) ++done;
    }
    if constexpr (SHOP_TOP_BUILD) {   // the ladder top teams' build (gap analysis, Oct 31): fixed families and stats
        static const std::pair<F, St> top[] = {{F::bruiser, St::health}, {F::durability, St::health}, {F::bruiser, St::strength},
                                               {F::jungle, St::strength}, {F::prospector, St::strength}};
        for (const auto& [fam, st] : top) {
            if (c[bc::get_item_enum(fam, st, false)] > 0) continue;   // this emblem is done
            item = bc::get_item_enum(fam, st, true);
            count = static_cast<int>(item.combine_count());
            return true;
        }
    }
    for (F fam : family_order(k)) {
        if (has_passive(c, fam)) continue;
        if (fam == F::collectors && done < 3) continue;
        item = bc::get_item_enum(fam, best_stat(t, k, s), true);
        count = static_cast<int>(item.combine_count());
        return true;
    }
    const St st = best_stat(t, k, s);
    const int round = static_cast<int>(bc::round_number());
    const float rate = round > 50 ? static_cast<float>(g_earned) / static_cast<float>(round) : 3.0f;
    const float future = rate * static_cast<float>(2000 - round);
    const F fam = future >= 750.0f ? F::greed : (k.three_stat ? F::bruiser : F::power);
    item = bc::get_item_enum(fam, st, true);
    count = static_cast<int>(item.combine_count());
    return true;
}

}  // namespace

void note_gold(int gold_now) {
    if (g_last_gold >= 0 && gold_now > g_last_gold) g_earned += gold_now - g_last_gold;
    g_last_gold = gold_now;
}

void buy_all(const Snapshot& s) {
    auto& me = bc::self();
    if (s.gold < LOCK_COST) return;
    auto counts = me.item_count();
    // v12: the squad's capturer completes a Capture emblem (health) when the whole emblem is affordable.
    if (is_capture_buyer(s) && !owns_any_capture_emblem(counts) && me.gold_count() >= 5u * 30u &&
        counts[IT::capture_shard_of_health] == 0) {
        const bool cluster_map = mapinfo::monster_gem_share() < CAPTURE_FIRST_MONSTER_SHARE;
        bool first_done = false;
        for (int f = 0; f <= static_cast<int>(F::collectors); ++f)
            if (static_cast<F>(f) != F::capture && has_passive(counts, static_cast<F>(f))) first_done = true;
        if ((cluster_map || first_done) && me.can_buy(IT::capture_shard_of_health, 5)) {
            me.buy(IT::capture_shard_of_health, 5);
            counts = me.item_count();
        }
    }
    bool any_passive = false;
    for (int f = 0; f <= static_cast<int>(F::collectors); ++f) {
        const F fam = static_cast<F>(f);
        if (fam != F::power && fam != F::bruiser && fam != F::greed && has_passive(counts, fam)) any_passive = true;
    }
    if (any_passive) buy_lock_if_wanted(true);
    for (int guard = 0; guard < 12 && me.gold_count() >= 30u; ++guard) {
        if (bc::fuel_remaining() < 15000) break;
        const Stats st{me.strength(), me.focus(), me.max_health()};
        bc::item_type item{};
        int count = 0;
        if (!next_item(s.my_type, counts, st, item, count)) break;
        std::uint32_t n = std::min<std::uint32_t>(static_cast<std::uint32_t>(count), me.gold_count() / item.cost());
        while (n > 0 && !me.can_buy(item, n)) --n;
        if (n == 0) break;
        me.buy(item, n);
        counts = me.item_count();
    }
    buy_lock_if_wanted(any_passive);
    g_last_gold = static_cast<int>(me.gold_count());
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
