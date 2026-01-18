<!--
AI editing guide:
- Only add entries under the "Unreleased" section. Do not restructure headings.
- Use Keep a Changelog categories: Added, Changed, Fixed, Removed, Deprecated, Security.
- When releasing, move "Unreleased" entries under a new version heading and leave an empty "Unreleased" section.
- Version headings should follow: ### [<upstream-version>] - Trexity (<YYYY-MM-DD>), optionally with tag notes.
- Prefer short, high-signal bullets that reference keys/flags as they appear in docs/API.md.
- If unsure, diff against upstream to discover Trexity-only changes:
  - Files changed vs upstream: `git diff --name-status upstream/master..HEAD`
  - API changes: `git diff upstream/master...HEAD -- docs/API.md`
-->

## Trexity Edition Changelog

This document tracks Trexity-specific additions and behavior changes on top of upstream VROOM. For full API details, see `docs/API.md`. Version labels mirror upstream versions with Trexity edition dates/tags.

### Unreleased

- Added:
  - `vehicle_penalties` on jobs and shipments: optional per-(vehicle, task) objective penalties (non-negative integers) to discourage assignment to specific vehicles. For shipments, applied once (counted on pickup only). Does not affect feasibility or route-level budget checks.
- Changed:
  - Output `cost` in `summary.cost` and `routes[].cost` includes `vehicle_penalties` (objective cost reporting).
- Fixed:
  - 

### [1.15.0] - Trexity (2025-11-18)

- Added:
  - Pinned tasks in solving mode:
    - `pinned` on jobs and shipments to keep seeded work on the same vehicle indicated via `vehicles.steps` seeding.
    - `pinned_position` for jobs and shipments (`"first"`/`"last"`); for shipments, pickup and delivery must be contiguous in the required order.
    - `allowed_vehicles` on jobs and shipments to restrict task eligibility.
  - Pinned soft timing and lateness budget:
    - `pinned_soft_timing` global flag to allow returning a solution even if the seeded pinned route is time-window infeasible (violations are surfaced instead of failing).
    - `pinned_lateness_limit_sec` to cap additional lateness allowed before any pinned step when interleaving extra work. `0` means strict no-worsen before the first pinned step.
  - Route-level budget model:
    - Per-task `budget` (jobs) and per-shipment `budget` (counted once on pickup).
    - `include_action_time_in_budget` to price setup+service time into route budget using the vehicle `per_hour` rate (when costs derive from durations/distances).
    - `budget_densify_candidates_k` to bound candidates when densifying over-budget routes.
    - Post-construction budget repair pass: densify with high-yield candidates, then greedily remove lowest-yield tasks until feasible; pinned tasks are never removed. If still infeasible, drop the route.
  - Vehicle constraint: `max_first_leg_distance` to cap the very first driving leg from `start` to the first task (or to pickup for shipments).
  - Versioning/build ergonomics:
    - Trexity edition tag and datestamp included in `--version`/help output.
    - Added Trexity scripts: `scripts/trexity-tests.js`, `scripts/build-macos.sh`, `scripts/trexity-publish.sh`, and `scripts/MANUAL_TESTING.md`.

- Changed:
  - Clarified `vehicles.steps` as a seeding mechanism for solving mode. With `pinned: true`, membership on the seeded vehicle is enforced; reordering within that vehicle remains possible.
  - Budget enforcement occurs after initial construction as a dedicated pass. With custom `matrices.costs`, only travel costs are enforced; to include action time pricing, do not provide `matrices.costs` and set `include_action_time_in_budget: true`.

- Fixed:
  - Stability and safety improvements (segfault and OOB conditions), defensive checks, and include fixes.

Notes:
- Pinned with `pinned_soft_timing=true` and `pinned_lateness_limit_sec=0` enforces a hard no-prepend rule before the first pinned step during insertion.
- For shipments, pinning applies to both pickup and delivery on the same vehicle; contiguous constraints apply when `pinned_position` is set.

### [1.15.0] - Trexity (2025-10-07)

- Added:
  - Task-to-vehicle locking via `allowed_vehicles` on jobs and shipments to limit eligibility to specific vehicles.

- Changed:
  - Included Trexity edition string in binaries and CI scaffolding to identify custom builds.

References:
- Upstream base: `upstream/master` at release time.
- Compare against upstream for Trexity deltas:
  - Files changed: `git diff --name-status upstream/master..HEAD`
  - API changes: `git diff upstream/master...HEAD -- docs/API.md`


