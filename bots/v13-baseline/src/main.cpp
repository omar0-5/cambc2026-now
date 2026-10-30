// main.cpp - entry point: the draft instance runs draft_turn(), every champion runs champion_turn().
//
// What it decides: nothing; it keeps the bot alive. One bad action must never crash the bot, so each
// turn is wrapped in try/catch and ALWAYS ends with bc::end_turn(). Exceptions are a safety net only
// (each thrown bc::error costs 10k+ fuel), never control flow.
// Fuel pauses are safe: if a turn runs out of fuel the bot is suspended mid-code and resumes there next
// round; every decision is rebuilt from a fresh Snapshot, so only small caches persist between turns.
// Worked example: Stellar tries to cast on a camp that died a moment ago -> bc::error
//   (invalid_action_spell_cast_bad_target) is caught, printed, and the turn still ends normally.
#include <cambc.h>
#include <iostream>
#include "draft.h"
#include "roles.h"
#include "types.h"

int main() {
    while (true) {
        const bool draft = cambc::is_draft();
        const auto start_round = draft ? 0 : cambc::round_number();
        try {
            if (draft)
                draft_turn();
            else
                champion_turn();
        } catch (const cambc::error& e) {
            std::cout << "bot error: " << cambc::to_string(e.code()) << " " << e.message() << " [" << g_stage << "]" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "bot error: " << e.what() << " [" << g_stage << "]" << std::endl;
        } catch (...) {
            std::cout << "bot error: unknown exception" << std::endl;
        }
        // A turn that ran out of fuel was suspended and resumed in a later round: log it for the test report.
        if (!draft && cambc::round_number() != start_round)
            std::cout << "fuel out: turn of round " << start_round << " ended in round " << cambc::round_number() << std::endl;
        cambc::end_turn();
    }
}
