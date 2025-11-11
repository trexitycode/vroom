#ifndef BUDGET_REPAIR_H
#define BUDGET_REPAIR_H
/*
This file is part of VROOM.
*/

#include "structures/vroom/input/input.h"
#include "structures/vroom/solution/solution.h"

namespace vroom::utils {

// Enforce route-level budgets post-solve:
// - Try densifying deficit routes by inserting unassigned (non-contiguous PD and singles).
// - If still in deficit, remove lowest-yield jobs/shipments greedily.
// - If still over budget, drop the entire route (prefer no route over over-budget).
void repair_budget(const Input& input, Solution& sol);

} // namespace vroom::utils

#endif

