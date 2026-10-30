// draft.cpp - picks champions, assigns body slots / squads / roles, writes draft memory. [S3, S10]
//
// What it decides: the lineup (one fixed list per team size) and who farms with whom.
// Worked example, 4-champion map: LINEUP_4 = Heart of Ruin, Marcella, Stellar, Ren.
//   Marcella -> squad 0 TankFarmer, Ren -> squad 0 Farmer, Heart of Ruin + Stellar -> squad 1 FarPair.
// Order matters: we PICK FIRST every draft turn (a crash before picking makes the rest random),
// then, on the first draft turn only (10x fuel), compute the map plan.
#include "draft.h"
#include <algorithm>
#include <iostream>
#include <vector>
#include "comms.h"
#include "config.h"
#include "mapinfo.h"
#include "types.h"

namespace {

using CD = bc::champion_draft;
using CT = bc::champion_type;

// Pick order per team size [S3]. Cassi & Issac = one pick, two bodies.
// v10 (game_plan_v2): Marcella in every lineup, offensive splits (Omar): 3 = 2 + hunter, 4 = 2 + 2 with a
// tank in each pair, 5 = farm trio + raid duo. Ranked regression (181k matches, rating-adjusted) puts
// Vesper Vale and Marcella on top at 3-4 picks and Ryth / Marcella at 5; Heart of Ruin is neutral or
// negative at 4 picks, so the 4-pick raid pair is Ryth + Vendetta.
//
// v11 draft sets (OPTIMAL_STRATEGY.md §5): 0 = v10 default (submit this one); 1 = elite-mirror
// (the Prism/Vesper/Marcella/Willow/HoR comp that beat v10 online 4-0; patch buffed Prism and Willow);
// 2 = burst-counter (Cassi & Issac 200-burst + stun to delete enemy carriers, Willow sustain);
// 3 = body-advantage 3-pick (4 bodies vs their 3 on spread maps). Sets 1-3 are bench candidates:
// switch DRAFT_SET only after a candidate beats set 0 head-to-head over the full map pool (§19).
constexpr int DRAFT_SET = 0;

struct SlotSetup { CT type; int squad; Role role; };

struct DraftSet {
    std::vector<CD> picks3, picks4, picks5;
    std::vector<SlotSetup> setup3, setup4, setup5;
};

const DraftSet& draft_set() {
    static const DraftSet V10 = [] {
        DraftSet d;
        d.picks3 = {CD::marcella, CD::vesper_vale, CD::vendetta};
        d.picks4 = {CD::marcella, CD::vesper_vale, CD::ryth, CD::vendetta};
        d.picks5 = {CD::marcella, CD::vesper_vale, CD::ryth, CD::heart_of_ruin, CD::vendetta};
        // Bench Oct 25 (v10 code): 3 and 4 picks start together (26/36, 34/48); 5 picks 3 + 2 (54/84
        // vs 41/84 all together). Split only for a fight worth taking or a side job (dynamic split).
        d.setup3 = {{CT::marcella, 0, Role::TankFarmer}, {CT::vesper_vale, 0, Role::Farmer}, {CT::vendetta, 0, Role::Farmer}};
        d.setup4 = {{CT::marcella, 0, Role::TankFarmer}, {CT::vesper_vale, 0, Role::Farmer},
                    {CT::ryth, 0, Role::Farmer},        {CT::vendetta, 0, Role::Farmer}};
        d.setup5 = {{CT::marcella, 0, Role::TankFarmer},   {CT::ryth, 0, Role::Farmer}, {CT::vendetta, 0, Role::Farmer},
                    {CT::heart_of_ruin, 1, Role::FarPair}, {CT::vesper_vale, 1, Role::FarPair}};
        return d;
    }();
    static const DraftSet ELITE_MIRROR = [] {
        DraftSet d;
        d.picks3 = {CD::marcella, CD::vesper_vale, CD::prism};
        d.picks4 = {CD::marcella, CD::vesper_vale, CD::prism, CD::willow};
        d.picks5 = {CD::marcella, CD::vesper_vale, CD::prism, CD::willow, CD::heart_of_ruin};
        d.setup3 = {{CT::marcella, 0, Role::TankFarmer}, {CT::vesper_vale, 0, Role::Farmer}, {CT::prism, 0, Role::Farmer}};
        d.setup4 = {{CT::marcella, 0, Role::TankFarmer}, {CT::vesper_vale, 0, Role::Farmer},
                    {CT::prism, 0, Role::Farmer},        {CT::willow, 0, Role::Support}};
        d.setup5 = {{CT::marcella, 0, Role::TankFarmer}, {CT::prism, 0, Role::Farmer}, {CT::willow, 0, Role::Support},
                    {CT::heart_of_ruin, 1, Role::FarPair}, {CT::vesper_vale, 1, Role::FarPair}};
        return d;
    }();
    static const DraftSet BURST_COUNTER = [] {
        DraftSet d;
        d.picks3 = {CD::marcella, CD::cassi_and_issac, CD::vesper_vale};
        d.picks4 = {CD::marcella, CD::cassi_and_issac, CD::vesper_vale, CD::vendetta};
        d.picks5 = {CD::marcella, CD::cassi_and_issac, CD::vesper_vale, CD::willow, CD::vendetta};
        d.setup3 = {{CT::marcella, 0, Role::TankFarmer}, {CT::vesper_vale, 0, Role::Farmer},
                    {CT::cassi, 0, Role::DisruptorCaster}, {CT::issac, 0, Role::DisruptorMelee}};
        d.setup4 = {{CT::marcella, 0, Role::TankFarmer}, {CT::vendetta, 0, Role::Farmer}, {CT::vesper_vale, 0, Role::Farmer},
                    {CT::cassi, 0, Role::DisruptorCaster}, {CT::issac, 0, Role::DisruptorMelee}};
        d.setup5 = {{CT::marcella, 0, Role::TankFarmer}, {CT::vendetta, 0, Role::Farmer}, {CT::willow, 0, Role::Support},
                    {CT::vesper_vale, 1, Role::FarPair},  {CT::cassi, 1, Role::DisruptorCaster}, {CT::issac, 1, Role::DisruptorMelee}};
        return d;
    }();
    switch (DRAFT_SET) {
        case 1: return ELITE_MIRROR;
        case 2: return BURST_COUNTER;
        default: return V10;
    }
}

const std::vector<CD>& lineup_for(int team_size) {
    const DraftSet& d = draft_set();
    if (team_size <= 3) return d.picks3;
    if (team_size == 4) return d.picks4;
    return d.picks5;
}

bool already_picked(const std::vector<CD>& mine, CD c) { return std::find(mine.begin(), mine.end(), c) != mine.end(); }

// Spare picks if a lineup champion is somehow unavailable (never expected: picks are per team).
const std::vector<CD> SPARES = {CD::stellar, CD::varg, CD::tavi, CD::prism, CD::ren, CD::willow};

void pick_next(int team_size) {
    const std::vector<CD> mine = bc::draft::current_draft(bc::team::ally);
    for (CD c : lineup_for(team_size))
        if (!already_picked(mine, c)) { bc::draft::pick(c); return; }
    for (CD c : SPARES)
        if (!already_picked(mine, c)) { bc::draft::pick(c); return; }
}

// Fill the slot tables from this team size's setup. Cassi & Issac contribute two bodies from one
// pick, so body_count is the number of setup entries actually used (v11).
void fill_slots(comms::Plan& plan, int team_size) {
    plan.type_of_slot.fill(comms::UNUSED_SLOT);
    plan.squad_of_slot.fill(comms::UNUSED_SLOT);
    plan.role_of_slot.fill(comms::UNUSED_SLOT);
    const DraftSet& d = draft_set();
    const std::vector<SlotSetup>& setup = team_size <= 3 ? d.setup3 : team_size == 4 ? d.setup4 : d.setup5;
    int bodies = 0;
    for (const SlotSetup& s : setup) {
        const int slot = body_slot_of(s.type);
        if (slot == NO_SLOT) continue;
        plan.type_of_slot[static_cast<std::size_t>(slot)] = static_cast<std::uint8_t>(s.type);
        plan.squad_of_slot[static_cast<std::size_t>(slot)] = static_cast<std::uint8_t>(s.squad);
        plan.role_of_slot[static_cast<std::size_t>(slot)] = static_cast<std::uint8_t>(s.role);
        ++bodies;
    }
    plan.body_count = bodies;
}

}  // namespace

void draft_turn() {
    const int team_size = static_cast<int>(bc::draft::team_size());
    pick_next(team_size);   // 1. pick first
    // v10: a draft turn must never run out of fuel. A suspended draft turn makes the game pick the rest of
    // the lineup at random (GR "Missing a pick"; big-world, Oct 25: the 100x100 map plan used ~880k of the
    // first turn's 1M and the lineup came out HoR, Willow, Cassi, Ren, Vesper).
    // 2. Squads and roles first: a tiny plan without the map part (champions then build the map themselves).
    static bool slots_written = false, map_written = false;
    static int turn = 0;
    ++turn;
    if (!slots_written) {
        comms::Plan plan;
        plan.team_size = team_size;
        fill_slots(plan, team_size);
        comms::write_plan(plan);
        slots_written = true;
    }
    // 3. The map part only on the first (10x fuel) turn, and only while far from the fuel limit.
    if (map_written || turn > 1 || bc::fuel_remaining() < DRAFT_MAP_START_FUEL) return;
    if (!mapinfo::build_step(false, 0, 0)) return;
    if (bc::fuel_remaining() < DRAFT_PLAN_RESERVE) return;
    comms::Plan plan = comms::plan_from_mapinfo(team_size);
    fill_slots(plan, team_size);
    comms::write_plan(plan);
    map_written = true;
}
