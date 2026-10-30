// belief.cpp - see belief.h. All storage lives in one static struct built on the first update.
#include "belief.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include "comms.h"
#include "config.h"
#include "mapinfo.h"

namespace belief {
namespace {

struct BeliefData {
    bool ready = false;
    std::array<bc::bitboard, SAFE_RING_ROUNDS> seen;   // vision of the last 8 rounds, slot = round mod 8
    std::array<int, SAFE_RING_ROUNDS> seen_round{};
    bc::bitboard safe_board, danger_board, scratch;
    std::array<AltarNote, MAX_ALTARS> altars{};
    std::array<Threat, MAX_THREATS> threat{};
};

BeliefData& data() {
    static BeliefData d;
    return d;
}

void setup(BeliefData& d) {
    for (bc::bitboard& b : d.seen) b.clear();
    d.seen_round.fill(-1000);
    d.safe_board.clear();
    d.danger_board.clear();
    d.ready = true;
}

// Own sightings keyed by champion type, the team record by -1; a full table replaces the oldest record.
void upsert(BeliefData& d, int key, vec2 pos, int round, int count, ThreatSource source, int reach) {
    Threat* slot = nullptr;
    for (Threat& t : d.threat)
        if (t.used && t.key == key) slot = &t;
    if (!slot)
        for (Threat& t : d.threat)
            if (!t.used) { slot = &t; break; }
    if (!slot) slot = &*std::min_element(d.threat.begin(), d.threat.end(), [](const Threat& a, const Threat& b) { return a.round < b.round; });
    *slot = Threat{true, pos, round, count, source, reach, key};
}

void update_threats(BeliefData& d, const Snapshot& s) {
    for (const EnemyNow& e : s.enemies)
        if (!e.on_their_spawn)
            upsert(d, static_cast<int>(e.type), e.pos, s.round, 1, ThreatSource::OwnSight, std::max(e.attack_range, 4));
    // Team source: the shared emergency (step b). Its count is only "1 vs 2+".
    const comms::Emergency seen = comms::read_emergency();
    if (seen.fresh) upsert(d, -1, seen.pos, s.round - seen.age, seen.type == comms::EmergencyType::DangerOne ? 1 : 2, ThreatSource::Team, 4);
    for (Threat& t : d.threat)
        if (t.used && s.round - t.round > THREAT_DROP_ROUNDS) t.used = false;
}

// Each record as a square of radius reach + age (at most 8), plus each awake monster's attack range.
void draw_danger(BeliefData& d, const Snapshot& s) {
    d.danger_board.clear();
    for (const Threat& t : d.threat)
        if (t.used) d.danger_board.draw_chebyshev(t.pos, static_cast<float>(std::min(t.reach + (s.round - t.round), THREAT_MAX_RADIUS)));
    for (const CampNow& c : s.camps)
        if (c.awake && c.strength > 0) d.danger_board.draw_chebyshev(c.pos, static_cast<float>(c.attack_range));
}

}  // namespace

void update(const Snapshot& s, const bc::bitboard& visible) {
    BeliefData& d = data();
    if (!d.ready) setup(d);
    // Known safe: this round's vision into its ring slot (copied in place: fill, then &=).
    const std::size_t slot = static_cast<std::size_t>(s.round % SAFE_RING_ROUNDS);
    d.seen[slot].fill();
    d.seen[slot] &= visible;
    d.seen_round[slot] = s.round;
    // Known valuable: altar notes for altars in sight (groups are v13's GroupInfo, updated in world).
    const auto& altars = mapinfo::altars();
    for (std::size_t i = 0; i < altars.size() && i < d.altars.size(); ++i) {
        if (!visible.is_set(altars[i])) continue;
        d.altars[i].last_seen = s.round;
        d.altars[i].enemy_lock = bc::vision::has_altar_lock(altars[i]) && bc::vision::altar_lock_at(altars[i]).is_enemy();
    }
    // Potentially dangerous. FUEL: the drawing is skipped below the guard (the old board stays).
    update_threats(d, s);
    if (bc::fuel_remaining() >= FUEL_FOR_DANGER) draw_danger(d, s);
    // safe = (union of the slots seen in the last 8 rounds) minus danger, rebuilt in place.
    d.safe_board.clear();
    for (std::size_t i = 0; i < d.seen.size(); ++i)
        if (s.round - d.seen_round[i] < SAFE_RING_ROUNDS) d.safe_board |= d.seen[i];
    d.scratch.fill();
    d.scratch ^= d.danger_board;   // ~danger without a temporary
    d.safe_board &= d.scratch;
    // Self-check (debug builds): every enemy in sight is on the danger board and no danger tile is safe.
    debug_block({
        for (const EnemyNow& e : s.enemies)
            if (!e.on_their_spawn && (!d.danger_board.is_set(e.pos) || d.safe_board.is_set(e.pos)))
                std::cout << "BELIEF CHECK FAILED r" << s.round << " enemy at " << e.pos.x << "," << e.pos.y << std::endl;
    });
}

const bc::bitboard& safe() { return data().safe_board; }
const bc::bitboard& danger() { return data().danger_board; }
const std::array<Threat, 8>& threats() { return data().threat; }

const AltarNote& altar_note(int altar_index) {
    static const AltarNote none{};
    if (altar_index < 0 || altar_index >= MAX_ALTARS) return none;
    return data().altars[static_cast<std::size_t>(altar_index)];
}

int rounds_unseen(const GroupInfo& g, int round) { return g.last_seen_round < 0 ? round + 1 : round - g.last_seen_round; }

bool group_stale(const GroupInfo& g, int round) {
    // Exact respawn rounds never decay; an Alive (or never seen) group goes stale after STALE_ROUNDS.
    if (g.state == GroupState::Respawning || g.state == GroupState::RewardAvailable) return false;
    return rounds_unseen(g, round) >= STALE_ROUNDS;
}

double threat_weight(const Threat& t, int round) {
    const int age = round - t.round;
    if (!t.used || age > THREAT_DROP_ROUNDS) return 0.0;
    return std::pow(THREAT_DECAY, std::max(0, age));
}

}  // namespace belief
