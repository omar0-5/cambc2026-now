// periodic.h - fuel rule for heavy jobs that repeat every few rounds (step 0c).
//
// A job runs only when the fuel left covers its estimated cost plus PERIODIC_FUEL_MARGIN; otherwise it
// waits for the next turn. The estimate is the job's own measured cost (it rises at once to a costlier
// run and decays slowly), and each body's first run is offset by its body slot so teammates do not all
// spike in the same round.
// Worked example: the target re-check measured 52k on 01islands; with the 15k margin it now runs only
//   on a turn that still has 67k+ left, and Vesper (slot 7) runs it 7 rounds after Marcella (slot 0)...
//   modulo the 10-round period: in rounds 7, 17, 27 instead of 0, 10, 20.
#pragma once
#include <algorithm>
#include <cstdint>
#include "config.h"
#include "types.h"

struct PeriodicJob {
    int period;
    std::uint64_t est_cost;   // fuel the job is expected to need
    int next_due = -1;

    bool due(int round, int body_slot) {
        if (next_due < 0) next_due = round + (body_slot >= 0 ? body_slot % period : 0);
        return round >= next_due && bc::fuel_remaining() > est_cost + PERIODIC_FUEL_MARGIN;
    }
    void ran(int round, std::uint64_t cost) {
        next_due = round + period;
        est_cost = std::max(cost, est_cost - est_cost / 8);
    }
};
