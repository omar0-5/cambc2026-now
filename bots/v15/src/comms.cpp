// comms.cpp - encode/decode draft memory and read/write shared memory (layouts in comms.h). [S10]
//
// Worked example (draft memory, a 3-champion game on eight: 8 groups, 1 altar):
//   bytes 0-3 = D5 01 03 03; bytes 4-9 = types of slots 0..5 = 0C 07 05 FF FF FF
//   (heart_of_ruin = 12, marcella = 7, stellar = 5); total size = 28 + 2*1 + 8*8 = 94 bytes.
#include "comms.h"
#include <algorithm>
#include <span>
#include "config.h"
#include "mapinfo.h"

namespace comms {
namespace {

void put16(std::vector<std::uint8_t>& out, int value) {
    const std::uint16_t v = static_cast<std::uint16_t>(std::clamp(value, 0, 0xFFFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

std::uint8_t byte_at(const bc::draft_memory_buffer& m, std::size_t i) {
    return static_cast<std::uint8_t>(m[i]);
}

std::uint16_t u16_at(const bc::draft_memory_buffer& m, std::size_t i) {
    return static_cast<std::uint16_t>(byte_at(m, i) | (byte_at(m, i + 1) << 8));
}

std::uint8_t shared_byte(int i) { return static_cast<std::uint8_t>(bc::shared_memory[static_cast<std::size_t>(i)]); }
void write_shared_byte(int i, std::uint8_t v) { bc::shared_memory[static_cast<std::size_t>(i)] = v; }

int target_byte_of_squad(int squad) { return squad == 0 ? 0 : (squad == 1 ? 2 : 3); }

std::uint8_t group_flags(const GroupInfo& g) {
    return static_cast<std::uint8_t>((g.in_enemy_zone ? 1 : 0) | (g.in_home ? 2 : 0) | (g.in_middle_band ? 4 : 0) |
                                     (g.clusters_only ? 8 : 0) | (g.has_monsters ? 16 : 0) | (g.has_monolith ? 32 : 0));
}

std::vector<std::uint8_t> encode(const Plan& p) {
    std::vector<std::uint8_t> out{PLAN_MAGIC, PLAN_VERSION, static_cast<std::uint8_t>(p.team_size),
                                  static_cast<std::uint8_t>(p.body_count)};
    for (auto v : p.type_of_slot) out.push_back(v);
    for (auto v : p.squad_of_slot) out.push_back(v);
    for (auto v : p.role_of_slot) out.push_back(v);
    put16(out, p.d15);
    put16(out, p.d60);
    const std::size_t group_count = std::min<std::size_t>(p.groups.size(), MAX_PLAN_GROUPS);
    const std::size_t altar_count = std::min<std::size_t>(p.altars.size(), MAX_PLAN_ALTARS);
    out.push_back(static_cast<std::uint8_t>(group_count));
    out.push_back(static_cast<std::uint8_t>(altar_count));
    for (std::size_t i = 0; i < altar_count; ++i) {
        out.push_back(static_cast<std::uint8_t>(p.altars[i].x));
        out.push_back(static_cast<std::uint8_t>(p.altars[i].y));
    }
    for (std::size_t i = 0; i < group_count; ++i) {
        const GroupPlan& g = p.groups[i];
        put16(out, g.steps_from_us);
        put16(out, g.steps_from_enemy);
        out.push_back(g.nearest_altar);
        put16(out, g.steps_to_altar);
        out.push_back(g.flags);
    }
    return out;
}

// v11 layout offsets (13 body slots; see comms.h).
constexpr std::size_t OFF_TYPES = 4;
constexpr std::size_t OFF_SQUADS = OFF_TYPES + BODY_SLOTS;
constexpr std::size_t OFF_ROLES = OFF_SQUADS + BODY_SLOTS;
constexpr std::size_t OFF_D15 = OFF_ROLES + BODY_SLOTS;          // 43
constexpr std::size_t OFF_GROUPS = OFF_D15 + 4;                   // 47
constexpr std::size_t OFF_ALTARS = OFF_GROUPS + 1;                // 48
constexpr std::size_t OFF_ALTAR_XY = OFF_ALTARS + 1;              // 49

}  // namespace

void write_plan(const Plan& plan) {
    const std::vector<std::uint8_t> bytes = encode(plan);
    bc::draft_memory = bytes;   // 49 + 2A + 8G <= 49 + 32 + 936 = 1,017 bytes
}

Plan plan_from_mapinfo(int team_size) {
    Plan p;
    p.team_size = team_size;
    p.d15 = mapinfo::d15();
    p.d60 = mapinfo::d60();
    p.altars = mapinfo::altars();
    for (const GroupInfo& g : mapinfo::groups()) {
        GroupPlan gp;
        gp.steps_from_us = static_cast<std::uint16_t>(std::min(g.steps_from_us, 0xFFFF));
        gp.steps_from_enemy = static_cast<std::uint16_t>(std::min(g.steps_from_enemy, 0xFFFF));
        gp.nearest_altar = static_cast<std::uint8_t>(g.nearest_altar < 0 ? 0xFF : g.nearest_altar);
        gp.steps_to_altar = static_cast<std::uint16_t>(std::min(g.steps_to_altar, 0xFFFF));
        gp.flags = group_flags(g);
        p.groups.push_back(gp);
    }
    return p;
}

Plan read_plan() {
    Plan p;
    const bc::draft_memory_buffer& m = bc::draft_memory;
    if (m.size() < OFF_ALTAR_XY || byte_at(m, 0) != PLAN_MAGIC || byte_at(m, 1) != PLAN_VERSION) return p;
    p.team_size = byte_at(m, 2);
    p.body_count = byte_at(m, 3);
    for (int i = 0; i < BODY_SLOTS; ++i) {
        p.type_of_slot[static_cast<std::size_t>(i)] = byte_at(m, OFF_TYPES + static_cast<std::size_t>(i));
        p.squad_of_slot[static_cast<std::size_t>(i)] = byte_at(m, OFF_SQUADS + static_cast<std::size_t>(i));
        p.role_of_slot[static_cast<std::size_t>(i)] = byte_at(m, OFF_ROLES + static_cast<std::size_t>(i));
    }
    p.d15 = u16_at(m, OFF_D15);
    p.d60 = u16_at(m, OFF_D15 + 2);
    const std::size_t group_count = byte_at(m, OFF_GROUPS), altar_count = byte_at(m, OFF_ALTARS);
    if (group_count > MAX_PLAN_GROUPS || altar_count > MAX_PLAN_ALTARS) return p;
    if (m.size() < OFF_ALTAR_XY + 2 * altar_count + 8 * group_count) return p;
    for (std::size_t i = 0; i < altar_count; ++i)
        p.altars.push_back(vec2{byte_at(m, OFF_ALTAR_XY + 2 * i), byte_at(m, OFF_ALTAR_XY + 2 * i + 1)});
    const std::size_t base = OFF_ALTAR_XY + 2 * altar_count;
    for (std::size_t i = 0; i < group_count; ++i) {
        const std::size_t o = base + 8 * i;
        p.groups.push_back(GroupPlan{u16_at(m, o), u16_at(m, o + 2), byte_at(m, o + 4), u16_at(m, o + 5), byte_at(m, o + 7)});
    }
    p.valid = p.body_count > 0;
    return p;
}

int squad_target(int squad) { return int(shared_byte(target_byte_of_squad(squad))) - 1; }

void set_squad_target(int squad, int group) {
    write_shared_byte(target_byte_of_squad(squad), static_cast<std::uint8_t>(group < 0 ? 0 : group + 1));
}

int split_target() { return int(shared_byte(1)) - 1; }
void set_split_target(int group) { write_shared_byte(1, static_cast<std::uint8_t>(group < 0 ? 0 : group + 1)); }

Sighting read_sighting() {
    Sighting s;
    const std::uint8_t x = shared_byte(4), y = shared_byte(5), info = shared_byte(6);
    if (x == 255) return s;
    s.pos = vec2{x, y};
    s.count = info & 7;
    s.channeling = (info >> 3) & 1;
    s.age = (static_cast<int>(bc::round_number()) - (info >> 4)) & 15;
    s.fresh = s.count > 0 && s.age <= SIGHTING_FRESH;
    return s;
}

void write_sighting(vec2 pos, int count, bool channeling) {
    write_shared_byte(4, static_cast<std::uint8_t>(pos.x));
    write_shared_byte(5, static_cast<std::uint8_t>(pos.y));
    const int stamp = static_cast<int>(bc::round_number()) & 15;
    write_shared_byte(6, static_cast<std::uint8_t>(std::min(count, 7) | (channeling ? 8 : 0) | (stamp << 4)));
}

int focus_type() {
    const int t = (shared_byte(7) & 15) - 1;
    if (t < 0 || !read_sighting().fresh) return -1;
    return t;
}

void set_focus(bc::champion_type enemy) {
    const std::uint8_t old = shared_byte(7);
    const int value = (static_cast<int>(enemy) + 1) & 15;
    write_shared_byte(7, static_cast<std::uint8_t>((old & 0xF0) | value));
}

bool flag(int bit) { return (shared_byte(7) >> bit) & 1; }

void set_flag(int bit, bool value) {
    const std::uint8_t old = shared_byte(7);
    const std::uint8_t mask = static_cast<std::uint8_t>(1 << bit);
    const std::uint8_t now = value ? static_cast<std::uint8_t>(old | mask) : static_cast<std::uint8_t>(old & ~mask);
    if (now != old) write_shared_byte(7, now);
}

}  // namespace comms
