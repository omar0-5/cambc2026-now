// comms.cpp - encode/decode draft memory and read/write shared memory (layouts in comms.h). [S10]
//
// Worked example (draft memory, a 3-champion game on eight: 8 groups, 1 altar):
//   bytes 0-3 = D5 01 03 03; bytes 4-9 = types of slots 0..5 = 0C 07 05 FF FF FF
//   (heart_of_ruin = 12, marcella = 7, stellar = 5); total size = 28 + 2*1 + 8*8 = 94 bytes.
#include "comms.h"
#include <algorithm>
#include <iostream>
#include <span>
#include "config.h"
#include "mapinfo.h"
#include "world.h"

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

// ---- Shared memory: one 64-bit word (design step b) ----------------------------------------------

namespace {

struct Field { int shift, bits; };
constexpr Field CLAIM0{0, 7}, CLAIM1{7, 7}, SPLIT{14, 7}, SCOUT{21, 3}, SCORER{24, 3};
constexpr Field OBJ_TYPE{27, 2}, OBJ_X{29, 7}, OBJ_Y{36, 7}, OBJ_STAMP{43, 2};
constexpr Field EM_TYPE{45, 2}, EM_X{47, 7}, EM_Y{54, 7}, EM_STAMP{61, 3};
// Stored as value + 1 with 0 = none, so the all-zero memory a game starts with means "nothing claimed"
// (with 127 = none, a zeroed word read as "every claim is group 0" and squad 0 chased group 0).
constexpr int MAX_CLAIM = 126, MAX_BODY = 6;

struct Word {
    std::uint64_t value = 0;   // as read at turn start, plus my own writes
    std::uint64_t mine = 0;    // mask of the bits I wrote this turn
};

Word& word() {
    static Word w;
    return w;
}

std::uint64_t mask_of(Field f) { return ((std::uint64_t{1} << f.bits) - 1) << f.shift; }
int get(Field f) { return static_cast<int>((word().value >> f.shift) & ((std::uint64_t{1} << f.bits) - 1)); }

void put(Field f, int v) {
    Word& w = word();
    const std::uint64_t m = mask_of(f);
    w.value = (w.value & ~m) | ((static_cast<std::uint64_t>(v) << f.shift) & m);
    w.mine |= m;
}

std::uint64_t read_word() {
    std::array<std::byte, 8> bytes{};
    bc::shared_memory.read(0, std::span<std::byte>(bytes));
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[i])) << (8 * i);
    return v;
}

int round_now() { return static_cast<int>(bc::round_number()); }
bool on_map(int x, int y) { return x >= 0 && y >= 0 && x < bc::map::width() && y < bc::map::height(); }

int group_or_none(int stored) {
    const int g = stored - 1;
    return (g < 0 || g >= static_cast<int>(mapinfo::groups().size())) ? NO_GROUP : g;
}

int body_or_none(int stored) {
    const int b = stored - 1;
    return (b < 0 || b >= body_count()) ? -1 : b;
}

int claim_value(int group) { return group < 0 || group > MAX_CLAIM ? 0 : group + 1; }
int body_value(int body) { return body < 0 || body > MAX_BODY ? 0 : body + 1; }

}  // namespace

void begin_turn() {
    Word& w = word();
    w.value = read_word();
    w.mine = 0;
    // Self-check (debug builds, once): every field round-trips through the 64-bit word.
    debug_block({
        static bool checked = false;
        if (!checked && !mapinfo::groups().empty()) {
            checked = true;
            const Word saved = w;
            const int g = static_cast<int>(mapinfo::groups().size()) - 1;
            set_squad_target(0, g); set_squad_target(1, 0); set_split_target(NO_GROUP);
            set_roles(body_count() - 1, -1);
            write_objective(ObjectiveType::Regroup, vec2{bc::map::width() - 1, bc::map::height() - 1});
            put(EM_TYPE, 0);
            raise_emergency(EmergencyType::DangerMany, vec2{3, 5});
            const Objective o = read_objective();
            const Emergency e = read_emergency();
            const bool ok = squad_target(0) == g && squad_target(1) == 0 && split_target() == NO_GROUP &&
                            scout_body() == body_count() - 1 && scorer_body() == -1 && o.valid && o.type == ObjectiveType::Regroup &&
                            o.pos == vec2{bc::map::width() - 1, bc::map::height() - 1} && e.fresh && e.type == EmergencyType::DangerMany &&
                            e.pos == vec2{3, 5} && e.age == 0;
            std::cout << (ok ? "COMMS SELF-CHECK OK" : "COMMS SELF-CHECK FAILED") << std::endl;
            w = saved;
        }
    });
}

// Merge into a fresh read, so a teammate who wrote while this turn was suspended keeps its fields.
void end_turn() {
    Word& w = word();
    if (w.mine == 0) return;
    const std::uint64_t now = (read_word() & ~w.mine) | (w.value & w.mine);
    std::array<std::byte, 8> bytes{};
    for (std::size_t i = 0; i < 8; ++i) bytes[i] = static_cast<std::byte>((now >> (8 * i)) & 0xFF);
    bc::shared_memory.write(0, std::span<const std::byte>(bytes));
    w.value = now;
    w.mine = 0;
}

int squad_target(int squad) {
    if (squad == 0) return group_or_none(get(CLAIM0));
    if (squad == 1) return group_or_none(get(CLAIM1));
    return NO_GROUP;   // no draft set has a third squad (the old byte 3 was never used)
}

void set_squad_target(int squad, int group) {
    if (squad != 0 && squad != 1) return;
    put(squad == 0 ? CLAIM0 : CLAIM1, claim_value(group));
}

int split_target() { return group_or_none(get(SPLIT)); }
void set_split_target(int group) { put(SPLIT, claim_value(group)); }

int scout_body() { return body_or_none(get(SCOUT)); }
int scorer_body() { return body_or_none(get(SCORER)); }

void set_roles(int scout, int scorer) {
    put(SCOUT, body_value(scout));
    put(SCORER, body_value(scorer));
}

Objective read_objective() {
    Objective o;
    const int age = (round_now() - get(OBJ_STAMP)) & 3;
    const int x = get(OBJ_X), y = get(OBJ_Y);
    if (age > OBJECTIVE_VALID_AGE || !on_map(x, y)) return o;
    o.valid = true;
    o.type = static_cast<ObjectiveType>(get(OBJ_TYPE));
    o.pos = vec2{x, y};
    return o;
}

void write_objective(ObjectiveType type, vec2 pos) {
    put(OBJ_TYPE, static_cast<int>(type));
    put(OBJ_X, std::clamp(pos.x, 0, 127));
    put(OBJ_Y, std::clamp(pos.y, 0, 127));
    put(OBJ_STAMP, round_now() & 3);
}

Emergency read_emergency() {
    Emergency e;
    const EmergencyType type = static_cast<EmergencyType>(get(EM_TYPE));
    const int x = get(EM_X), y = get(EM_Y);
    if (type == EmergencyType::None || !on_map(x, y)) return e;
    e.type = type;
    e.pos = vec2{x, y};
    e.age = (round_now() - get(EM_STAMP)) & 7;
    e.fresh = e.age <= EMERGENCY_FRESH_AGE;
    return e;
}

void raise_emergency(EmergencyType type, vec2 pos) {
    const Emergency current = read_emergency();
    if (current.fresh && static_cast<int>(type) <= static_cast<int>(current.type)) return;   // only a higher type, or over a stale one
    put(EM_TYPE, static_cast<int>(type));
    put(EM_X, std::clamp(pos.x, 0, 127));
    put(EM_Y, std::clamp(pos.y, 0, 127));
    put(EM_STAMP, round_now() & 7);
}

// Body order: champion type value from the draft plan. Body slots ARE type values (types.h), so walking
// the slots in order gives the order, and Cassi (lower type value) comes before Issac. Without a plan
// the allies list gives the same order on every teammate. Unit ids would break a tie of equal type
// values, which one team cannot have.
namespace {
struct BodyOrder { bool built = false; int count = 0; std::array<int, BODY_SLOTS> index_of{}; std::array<int, BODY_SLOTS> slot_at{}; };

const BodyOrder& body_order() {
    static BodyOrder o;
    if (o.built) return o;
    o.index_of.fill(-1);
    o.slot_at.fill(-1);
    std::array<bool, BODY_SLOTS> present{};
    const Plan& plan = world::plan();
    if (plan.valid) {
        for (int slot = 0; slot < BODY_SLOTS; ++slot) present[static_cast<std::size_t>(slot)] = plan.type_of_slot[static_cast<std::size_t>(slot)] != UNUSED_SLOT;
    } else {
        for (const bc::champion& c : bc::champions(bc::team::ally)) {
            const int slot = body_slot_of(c.champion_name());
            if (slot != NO_SLOT) present[static_cast<std::size_t>(slot)] = true;
        }
    }
    for (int slot = 0; slot < BODY_SLOTS; ++slot)
        if (present[static_cast<std::size_t>(slot)]) {
            o.index_of[static_cast<std::size_t>(slot)] = o.count;
            o.slot_at[static_cast<std::size_t>(o.count)] = slot;
            ++o.count;
        }
    o.built = true;
    return o;
}
}  // namespace

int body_index(bc::champion_type type) {
    const int slot = body_slot_of(type);
    return slot == NO_SLOT ? -1 : body_order().index_of[static_cast<std::size_t>(slot)];
}

int body_count() { return body_order().count; }

bc::champion_type body_type(int index) {
    const BodyOrder& o = body_order();
    if (index < 0 || index >= o.count) return bc::champion_type::marcella;
    return static_cast<bc::champion_type>(o.slot_at[static_cast<std::size_t>(index)]);
}

}  // namespace comms
