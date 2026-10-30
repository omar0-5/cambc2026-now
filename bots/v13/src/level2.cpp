// level2.cpp - see level2.h.
#include "level2.h"
#include "config.h"
#if LEVEL2   // off: no Level 2 code in the build (identical to step c)
#include <algorithm>
#include <cmath>
#include <iostream>
#include "belief.h"
#include "combat.h"
#include "comms.h"
#include "config.h"

namespace level2 {
namespace {

int bodies_on_team() {
    static int n = 0;
    if (n == 0) n = std::max<int>(1, static_cast<int>(bc::champions(bc::team::ally).size()));
    return n;
}

// Chance a type with these stats beats me (v13 bank.cpp's p_lose_to, kept local).
double p_lose_to(const Snapshot& s, bc::champion_type type, float health, float strength, float focus) {
    const double their_rounds = combat::rounds_to_kill(s.health, combat::burst(type, strength, focus),
                                                       combat::damage_per_round(type, strength, focus), 0);
    const double our_rounds = combat::rounds_to_kill(health, combat::my_burst_now(s),
                                                     combat::damage_per_round(s.my_type, s.strength, s.focus), 0);
    return 1.0 - combat::p_win(their_rounds, our_rounds, false);
}

double p_contact(double distance, double T) {
    return std::clamp((T + CONTACT_SLACK - distance) / (T + CONTACT_SLACK), 0.0, 1.0);
}

double nearest(vec2 p, vec2 me, vec2 dest) {
    return static_cast<double>(std::min(p.chebyshev_distance(me), p.chebyshev_distance(dest)));
}

}  // namespace

bool active() { return LEVEL2_ON && bc::fuel_remaining() >= LEVEL2_FUEL_FLOOR; }

double income_rate(const Snapshot& s) {
    if (s.round < LEVEL2_EARLY_ROUNDS) return INCOME_EARLY;
    return std::max(INCOME_FLOOR, static_cast<double>(s.banked_us) / (std::max(100, s.round) * bodies_on_team()));
}

double loss_on_death(const Snapshot& s, double enemy_share) {
    return (s.bag + KILL_GOLD_GEMS) * enemy_share + s.bag + RESPAWN_ROUNDS_DEAD * income_rate(s);
}

double p_death(const Snapshot& s, double T, vec2 dest, double* enemy_share) {
    const double background = std::exp(-H0 * (s.max_health / std::max(1.0f, s.health)) * T);
    double enemies = 1.0;
    for (const EnemyNow& e : s.enemies) {
        if (e.on_their_spawn) continue;
        enemies *= 1.0 - p_contact(nearest(e.pos, s.me, dest), T) * p_lose_to(s, e.type, e.health, e.strength, e.focus);
    }
    // Stale danger records: threats not in sight now, weighted 0.85^age (own records know the type).
    for (const belief::Threat& t : belief::threats()) {
        if (!t.used || t.round == s.round) continue;
        double lose = DANGER_UNKNOWN_PLOSE;
        if (t.key >= 0) {
            const bc::champion_stats base = combat::base_stats_of(static_cast<bc::champion_type>(t.key));
            lose = p_lose_to(s, static_cast<bc::champion_type>(t.key), base.base_health, base.base_strength, base.base_focus);
        }
        const double d = std::max(0.0, nearest(t.pos, s.me, dest) - (s.round - t.round));
        enemies *= 1.0 - belief::threat_weight(t, s.round) * p_contact(d, T) * lose * std::min(t.count, 3);
    }
    enemies = std::clamp(enemies, 0.0, 1.0);
    const double p = (1.0 - background * enemies) * P_DEATH_CALIBRATION;
    if (enemy_share) *enemy_share = p > 1e-9 ? std::clamp((1.0 - enemies) * P_DEATH_CALIBRATION / p, 0.0, 1.0) : 0.0;
    if (LOG_PDEATH)
        std::cout << "PD " << bc::to_string(s.my_type) << " r" << s.round << " T" << static_cast<int>(T + 0.5) << " p" << static_cast<int>(std::clamp(p, 0.0, 1.0) * 1000)
                  << " m" << g_label << "\n";
    return std::clamp(p, 0.0, 1.0);
}

double q(const Snapshot& s, const Plan& p) {
    const double r = income_rate(s);
    const double l_d = loss_on_death(s, p.enemy_share);
    const double result = p.dS + LAMBDA_E * p.dE - LAMBDA_D * p.p_death * l_d - LAMBDA_T * p.T * r * std::max(1, p.bodies);
    // Self-check (debug builds): every term in range and the score finite.
    debug_block({
        if (!std::isfinite(result) || p.p_death < 0 || p.p_death > 1 || p.enemy_share < 0 || p.enemy_share > 1 || r <= 0)
            std::cout << "LEVEL2 CHECK FAILED r" << s.round << " q " << result << " p " << p.p_death << " e " << p.enemy_share << std::endl;
    });
    return result;
}

}  // namespace level2
#endif
