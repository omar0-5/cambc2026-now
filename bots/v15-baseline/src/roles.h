// roles.h - the 12-step ladder every champion runs each turn, and the role steps. [S5]
//
// What it decides: which ONE thing this champion does this turn, in priority order:
//   1 dead -> shop | 2 channeling / stunned -> wait | 3 on spawn -> shop, heal | 4 endgame / winning bag
//   -> tribute | 5 low health -> retreat or recall | 6 enemies -> fight / interrupt / walk away
//   7 bank | 8 shop trip | 9 finish a capture | 10 role step (farm / disrupt / idle) | 11 ward | 12 comms
// Worked example: round 640, Stellar at 25% health with a wolf awake 3 tiles away. Step 5 fires
//   (25% < RETREAT_HEALTH 30%); the wolf can still hit her, so no recall: she steps away first and
//   recalls next turn once nothing is in range.
#pragma once

void champion_turn();   // called every champion turn by main.cpp
