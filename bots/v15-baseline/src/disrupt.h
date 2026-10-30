// disrupt.h - Cassi & Issac: altar locks, waking monsters on enemies, staying together. [S9]
//
// What it decides: the extra moves only the disruptor pair makes, before they farm.
// Worked example: an enemy Varg (30 gems) walks toward an altar 3 tiles from Cassi; she carries a lock
//   and is within 2 of that altar -> she places it: Varg's tribute is now blocked until he lands 5 basic
//   attacks on the lock, and any tribute already running there is cancelled.
#pragma once
#include "types.h"

namespace disrupt {

bool step(Snapshot& s);   // true = turn used (lock placed, monster woken, regrouped or fled)

}  // namespace disrupt
