// draft.h - the draft instance: picks champions and writes the opening plan. [S3, S10]
// Example: team size 3 -> picks Heart of Ruin, Marcella, Stellar (one per draft turn), and on the first
// draft turn (10x fuel) writes the plan bytes described in comms.h.
#pragma once

void draft_turn();   // called every draft turn by main.cpp (bc::is_draft() == true)
