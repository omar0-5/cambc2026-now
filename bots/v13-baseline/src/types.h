// types.h - the plain structs and enums that several modules share.
//
// What it decides: nothing; it names things. Body slots and roles come from [S10].
// v11: a body slot per champion_type value (13 slots, champion_draft never uses
// champion_type::cassi_and_issac), so Willow, Cassi, Issac, Prism and Tavi can be drafted
// as first-class picks (candidate lineups of OPTIMAL_STRATEGY.md §5.3), not just spares.
// Worked example: in a 5-champion elite-mirror game the draft picks Marcella, Vesper, Prism,
//   Willow, Heart of Ruin; Marcella -> squad 0 TankFarmer, Prism -> squad 0 Farmer,
//   Willow -> squad 0 Support, Heart of Ruin + Vesper -> squad 1 FarPair.
#pragma once
#include <cambc.h>
#include <cstdint>
#include <vector>

namespace bc = cambc;
using bc::vec2;

// Name of the ladder step running now; main.cpp prints it if an action throws (a cheap pointer store).
inline const char* g_stage = "";
inline int g_round = -1;   // current round, set once per turn in world::sense (saves engine calls)

// ---- Roles [S5, S10] ------------------------------------------------------------------------
enum class Role : std::uint8_t {
    TankFarmer = 0,       // Marcella: stands next to monsters so they hit her
    Farmer = 1,           // farms with the tank (ranged or melee)
    Interceptor = 2,      // Heart of Ruin, 3 champions: farms the middle, hunts carriers
    FarPair = 3,          // Heart of Ruin + Vesper, 4-5 champions: the second farming pair
    DisruptorCaster = 4,  // Cassi: locks altars, stuns channelers
    DisruptorMelee = 5,   // Issac: Cassi's partner
    Support = 6,          // v11 Willow: heals the squad on cooldown, farms between heals
};

constexpr int BODY_SLOTS = 13;   // one per champion_type value (v11)
constexpr int NO_SLOT = -1;
constexpr int NO_GROUP = -1;
constexpr int FAR_AWAY = 9999;        // "no path" distance
constexpr std::uint16_t NO_DISTANCE = 0xFFFF;

// Body slot of a champion type: the champion_type value itself (v11). Cassi and Issac are separate
// bodies (they act separately and die together), so a "team size" of N picks can have N+1 bodies.
inline int body_slot_of(bc::champion_type type) {
    return static_cast<int>(type);
}

// ---- Camp groups [S4] -----------------------------------------------------------------------
enum class GroupState : std::uint8_t {
    Unknown,          // never seen: assume alive
    Alive,            // at least one monster alive
    RewardAvailable,  // every monster dead, reward waiting to be captured
    Respawning,       // reward taken; back at respawn_ready_round
};

struct GroupInfo {                  // one entry per bc::map::camp_groups() index
    int index = 0;                  // position in bc::map::camp_groups()
    std::vector<vec2> tiles;        // monster spawn tiles (monsters never move)
    bc::bitboard spawn;             // the same tiles as a mask
    vec2 centre{};                  // first spawn tile, used for distances
    int gems = 0, gold = 0, respawn_full = 0;
    float total_health = 0;         // sum of monster health (design stats)
    float total_strength = 0;       // sum of monster strength (damage per round when all hit)
    float max_strength = 0;         // strongest single monster
    int max_attack_range = 0;       // longest monster attack range in the group
    float edge_strength = 0;        // damage per round on the best tile next to the group (fewest monsters reach it)
    std::vector<float> strengths;   // per monster tile (same order as tiles)
    std::vector<int> ranges;        // per monster tile
    int steps_from_us = FAR_AWAY, steps_from_enemy = FAR_AWAY, steps_to_altar = FAR_AWAY;
    int nearest_altar = -1;         // index into mapinfo::altars()
    bool in_enemy_zone = false, in_home = false, in_middle_band = false;
    bool clusters_only = false, has_monsters = false, has_monolith = false;
    bool gate = false;              // a spawn tile touches an altar: tributing there needs this group dead (boss-battle)
    // Live memory (world.cpp keeps this up to date from what this champion sees)
    GroupState state = GroupState::Unknown;
    int respawn_ready_round = 0;    // when state == Respawning
    int last_seen_round = -1;
    // v11 (engine 2.0.0 camp_group::capture_state): an enemy is capturing this cleared region, with
    // this many capture rounds left (walk in and stand on the region to reset their progress).
    int enemy_capture_timer = -1;   // -1 = no enemy capture seen
};

// ---- Enemy memory [S5] ----------------------------------------------------------------------
struct EnemyMemory {                // one per enemy champion handle
    bool exists = false;            // this champion type is on the enemy team
    bc::champion_type type = bc::champion_type::ryth;
    vec2 last_pos{};
    int last_seen_round = -1000;
    float health = 0, max_health = 1;
    int bag = 0;                    // gem_count() when visible
    bool channeling = false;        // tributing or recalling when last seen
    bool alive = true;              // is_alive() (works without vision: probe, Oct 21)
};

// ---- What this champion sees this turn [S5] -------------------------------------------------
struct EnemyNow {                   // a visible enemy champion
    bc::champion handle;
    vec2 pos;
    bc::champion_type type;
    float health, max_health, shield;
    float strength, focus;
    int bag, attack_range;
    bool channeling, on_their_spawn, stunned;
};

struct AllyNow {                    // a visible allied champion (not me); ask handle for health when needed
    bc::champion handle;
    vec2 pos;
    bc::champion_type type;
    // v10: fight stats read once per turn on first use (each engine query ~300 fuel; the race asked them
    // again for every enemy: 5 v 5 cost ~25k per turn).
    mutable bool stats_read = false;
    mutable float strength = 0, focus = 0, health = 0, max_health = 1;
    void read_stats() const {
        if (stats_read) return;
        stats_read = true;
        strength = handle.strength(); focus = handle.focus(); health = handle.health(); max_health = handle.max_health();
    }
};

struct CampNow {                    // a visible living camp (monster or cluster)
    bc::camp handle;
    vec2 pos;
    int group;                      // index into mapinfo::groups()
    bc::camp_type kind;
    float health, strength;
    int attack_range;
    bool awake;
};

struct Snapshot {                   // rebuilt every turn by world::sense()
    int round = 0;
    vec2 me{};
    bc::champion_type my_type = bc::champion_type::ryth;
    int my_slot = NO_SLOT;
    Role role = Role::Farmer;
    int squad = 0;
    float health = 0, max_health = 1, strength = 0, focus = 0;
    int bag = 0, gold = 0, attack_range = 1;
    bool melee = true, on_our_spawn = false;
    int banked_us = 0, banked_them = 0;   // global_submitted_gems(ally / enemy)
    bool cast_exclusive = false;          // we cast an "excludes walking" spell: no walking this turn
    bool moved = false;                   // we walked or dashed: no "excludes walking" spell this turn
    const char* step = "";                // ladder step taken this turn (debug log)
    const char* detail = "";              // finer step inside farming (walk / hold / fight / capture)
    bool hit = false;                     // we damaged something this turn (debug log)
    std::vector<EnemyNow> enemies;
    std::vector<AllyNow> allies;
    std::vector<CampNow> camps;
};
