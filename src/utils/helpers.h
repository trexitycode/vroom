#ifndef HELPERS_H
#define HELPERS_H

/*

This file is part of VROOM.

Copyright (c) 2015-2025, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "structures/typedefs.h"
#include "structures/vroom/raw_route.h"
#include "structures/vroom/solution_state.h"
#include "structures/vroom/tw_route.h"
#include "utils/exception.h"

namespace vroom::utils {

template <typename T> T round(double value) {
  constexpr double round_increment = 0.5;
  return static_cast<T>(value + round_increment);
}

TimePoint now();

Amount max_amount(std::size_t size);

inline UserCost add_without_overflow(UserCost a, UserCost b) {
  if (a > std::numeric_limits<UserCost>::max() - b) {
    throw InputException(
      "Too high cost values, stopping to avoid overflowing.");
  }
  return a + b;
}

// Taken from https://stackoverflow.com/a/72073933.
inline uint32_t get_vector_hash(const std::vector<uint32_t>& vec) {
  uint32_t seed = vec.size();
  for (auto x : vec) {
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    seed ^= x + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  }
  return seed;
}

inline unsigned get_depth(unsigned exploration_level) {
  return exploration_level;
}

inline unsigned get_nb_searches(unsigned exploration_level) {
  assert(exploration_level <= MAX_EXPLORATION_LEVEL);

  unsigned nb_searches = 4 * (exploration_level + 1);
  if (exploration_level >= 4) {
    nb_searches += 4;
  }
  if (exploration_level == MAX_EXPLORATION_LEVEL) {
    nb_searches += 4;
  }

  return nb_searches;
}

INIT get_init(std::string_view s);

SORT get_sort(std::string_view s);

HeuristicParameters str_to_heuristic_param(const std::string& s);

// Evaluate adding job with rank job_rank in given route at given rank
// for vehicle at rank v_rank. Travel-only (no objective penalties).
inline Eval addition_cost_travel(const Input& input,
                                 Index job_rank,
                                 Index v_rank,
                                 const std::vector<Index>& route,
                                 Index rank) {
  assert(rank <= route.size());

  const auto& v = input.vehicles[v_rank];
  const Index job_index = input.jobs[job_rank].index();
  Eval previous_eval;
  Eval next_eval;
  Eval old_edge_eval;

  if (rank == route.size()) {
    if (route.empty()) {
      if (v.has_start()) {
        previous_eval = v.eval(v.start.value().index(), job_index);
      }
      if (v.has_end()) {
        next_eval = v.eval(job_index, v.end.value().index());
      }
    } else {
      // Adding job past the end after a real job.
      auto p_index = input.jobs[route[rank - 1]].index();
      previous_eval = v.eval(p_index, job_index);
      if (v.has_end()) {
        auto n_index = v.end.value().index();
        old_edge_eval = v.eval(p_index, n_index);
        next_eval = v.eval(job_index, n_index);
      }
    }
  } else {
    // Adding before one of the jobs.
    auto n_index = input.jobs[route[rank]].index();
    next_eval = v.eval(job_index, n_index);

    if (rank == 0) {
      if (v.has_start()) {
        auto p_index = v.start.value().index();
        previous_eval = v.eval(p_index, job_index);
        old_edge_eval = v.eval(p_index, n_index);
      }
    } else {
      auto p_index = input.jobs[route[rank - 1]].index();
      previous_eval = v.eval(p_index, job_index);
      old_edge_eval = v.eval(p_index, n_index);
    }
  }

  return previous_eval + next_eval - old_edge_eval;
}

// Evaluate adding job with rank job_rank in given route at given rank for the
// vehicle at rank v_rank. Objective cost includes per-(job,vehicle) penalties.
inline Eval addition_cost(const Input& input,
                          Index job_rank,
                          Index v_rank,
                          const std::vector<Index>& route,
                          Index rank) {
  auto e = addition_cost_travel(input, job_rank, v_rank, route, rank);
  e.cost += input.job_vehicle_penalty(job_rank, v_rank);
  return e;
}

// Evaluate adding pickup with rank job_rank and associated delivery
// (with rank job_rank + 1) in given route for vehicle v. Pickup is
// inserted at pickup_rank in route and delivery is inserted at
// delivery_rank in route **with pickup**.
inline Eval addition_cost_travel(const Input& input,
                                 Index job_rank,
                                 Index v_rank,
                                 const std::vector<Index>& route,
                                 Index pickup_rank,
                                 Index delivery_rank) {
  assert(pickup_rank < delivery_rank && delivery_rank <= route.size() + 1);
  const auto& v = input.vehicles[v_rank];

  // Start with pickup eval.
  auto eval = addition_cost_travel(input, job_rank, v_rank, route, pickup_rank);

  if (delivery_rank == pickup_rank + 1) {
    // Delivery is inserted just after pickup.
    const Index p_index = input.jobs[job_rank].index();
    const Index d_index = input.jobs[job_rank + 1].index();
    eval += v.eval(p_index, d_index);

    Eval after_delivery;
    Eval remove_after_pickup;

    if (pickup_rank == route.size()) {
      // Addition at the end of a route.
      if (v.has_end()) {
        after_delivery = v.eval(d_index, v.end.value().index());
        remove_after_pickup = v.eval(p_index, v.end.value().index());
      }
    } else {
      // There is a job after insertion.
      const Index next_index = input.jobs[route[pickup_rank]].index();
      after_delivery = v.eval(d_index, next_index);
      remove_after_pickup = v.eval(p_index, next_index);
    }

    eval += after_delivery;
    eval -= remove_after_pickup;
  } else {
    // Delivery is further away so edges sets for pickup and delivery
    // addition are disjoint.
    eval += addition_cost_travel(input,
                                 job_rank + 1,
                                 v_rank,
                                 route,
                                 delivery_rank - 1);
  }

  return eval;
}

inline Eval addition_cost(const Input& input,
                          Index job_rank,
                          Index v_rank,
                          const std::vector<Index>& route,
                          Index pickup_rank,
                          Index delivery_rank) {
  // Travel deltas + pickup penalty only (shipment penalties apply once).
  auto e = addition_cost_travel(input, job_rank, v_rank, route, pickup_rank, delivery_rank);
  e.cost += input.job_vehicle_penalty(job_rank, v_rank);
  return e;
}

inline Cost penalty_sum_for_range(const SolutionState& sol_state,
                                 Index route_vehicle,
                                 Index target_vehicle,
                                 Index first_rank,
                                 Index last_rank) {
  assert(first_rank <= last_rank);
  if (last_rank == first_rank) {
    return 0;
  }
  assert(last_rank <= sol_state.fwd_penalties[route_vehicle][target_vehicle].size());
  const auto& pref = sol_state.fwd_penalties[route_vehicle][target_vehicle];
  if (first_rank == 0) {
    return pref[last_rank - 1];
  }
  return pref[last_rank - 1] - pref[first_rank - 1];
}

inline auto get_indices(const Input& input,
                        const RawRoute& route,
                        Index first_rank,
                        Index last_rank) {
  const auto& r = route.route;
  const auto& v = input.vehicles[route.v_rank];

  std::array<std::optional<Index>, 3> indices;

  auto& before_first = indices[0];
  if (first_rank > 0) {
    before_first = input.jobs[r[first_rank - 1]].index();
  } else {
    if (v.has_start()) {
      before_first = v.start.value().index();
    }
  }

  auto& first_index = indices[1];
  if (first_rank < r.size()) {
    first_index = input.jobs[r[first_rank]].index();
  } else {
    if (v.has_end()) {
      first_index = v.end.value().index();
    }
  }

  auto& last_index = indices[2];
  if (last_rank < r.size()) {
    last_index = input.jobs[r[last_rank]].index();
  } else {
    if (v.has_end()) {
      last_index = v.end.value().index();
    }
  }

  return indices;
}

inline Eval get_range_removal_gain(const SolutionState& sol_state,
                                   Index v,
                                   Index first_rank,
                                   Index last_rank) {
  assert(first_rank <= last_rank);

  Eval removal_gain;

  if (last_rank > first_rank) {
    // Gain related to removed portion.
    removal_gain += sol_state.fwd_costs[v][v][last_rank - 1];
    removal_gain -= sol_state.fwd_costs[v][v][first_rank];
    // Removing jobs also removes their per-vehicle penalties (objective-only).
    removal_gain.cost +=
      penalty_sum_for_range(sol_state, v, v, first_rank, last_rank);
  }

  return removal_gain;
}

// Compute objective *gain* (i.e., decrease in objective cost) when replacing the
// [first_rank, last_rank) portion for route_1 with the range
// [insertion_start; insertion_end) from route_2.
// Returns a tuple to evaluate at once both options where the new range is
// inserted as-is, or reversed.
inline std::tuple<Eval, Eval>
addition_cost_delta(const Input& input,
                    const SolutionState& sol_state,
                    const RawRoute& route_1,
                    const Index first_rank,
                    const Index last_rank,
                    const RawRoute& route_2,
                    const Index insertion_start,
                    const Index insertion_end) {
  assert(first_rank <= last_rank);
  assert(last_rank <= route_1.route.size());
  assert(insertion_start <= insertion_end);

  const bool empty_insertion = (insertion_start == insertion_end);

  const auto& r1 = route_1.route;
  const auto v1_rank = route_1.v_rank;
  const auto& r2 = route_2.route;
  const auto v2_rank = route_2.v_rank;
  const auto& v1 = input.vehicles[v1_rank];

  // Common part of the cost.
  Eval cost_delta =
    get_range_removal_gain(sol_state, v1_rank, first_rank, last_rank);

  // Part of the cost that depends on insertion orientation.
  Eval straight_delta;
  Eval reversed_delta;
  if (insertion_start != insertion_end) {
    straight_delta += sol_state.fwd_costs[v2_rank][v1_rank][insertion_start];
    straight_delta -= sol_state.fwd_costs[v2_rank][v1_rank][insertion_end - 1];

    reversed_delta += sol_state.bwd_costs[v2_rank][v1_rank][insertion_start];
    reversed_delta -= sol_state.bwd_costs[v2_rank][v1_rank][insertion_end - 1];
  }

  // Penalties for inserted range depend on target vehicle v1_rank, but not on
  // insertion orientation. Note: this function returns *gain*, so we subtract
  // the penalty *cost* of inserted jobs. This correctly makes negative penalties
  // (preferences) increase gain (more attractive) and positive penalties
  // decrease gain (less attractive).
  const Cost inserted_penalty_cost =
    penalty_sum_for_range(sol_state,
                          v2_rank,
                          v1_rank,
                          insertion_start,
                          insertion_end);
  straight_delta.cost -= inserted_penalty_cost;
  reversed_delta.cost -= inserted_penalty_cost;

  // Determine useful values if present.
  const auto [before_first, first_index, last_index] =
    get_indices(input, route_1, first_rank, last_rank);

  // Gain of removed edge before replaced range. If route is empty,
  // before_first and first_index are respectively the start and end
  // of vehicle if defined.
  if (before_first && first_index && !r1.empty()) {
    cost_delta += v1.eval(before_first.value(), first_index.value());
  }

  if (empty_insertion) {
    if (before_first && last_index &&
        !(first_rank == 0 && last_rank == r1.size())) {
      // Add cost of new edge replacing removed range, except if
      // resulting route is empty.
      cost_delta -= v1.eval(before_first.value(), last_index.value());
    }
  } else {
    if (before_first) {
      // Cost of new edge to inserted range.
      straight_delta -=
        v1.eval(before_first.value(), input.jobs[r2[insertion_start]].index());
      reversed_delta -= v1.eval(before_first.value(),
                                input.jobs[r2[insertion_end - 1]].index());
    }

    if (last_index) {
      // Cost of new edge after inserted range.
      straight_delta -=
        v1.eval(input.jobs[r2[insertion_end - 1]].index(), last_index.value());
      reversed_delta -=
        v1.eval(input.jobs[r2[insertion_start]].index(), last_index.value());
    }
  }

  // Gain of removed edge after replaced range, if any.
  if (last_index && last_rank > first_rank) {
    const Index before_last = input.jobs[r1[last_rank - 1]].index();
    cost_delta += v1.eval(before_last, last_index.value());
  }

  // Handle fixed cost addition.
  if (r1.empty() && !empty_insertion) {
    cost_delta.cost -= v1.fixed_cost();
  }

  if (empty_insertion && first_rank == 0 && last_rank == r1.size()) {
    cost_delta.cost += v1.fixed_cost();
  }

  return std::make_tuple(cost_delta + straight_delta,
                         cost_delta + reversed_delta);
}

// Compute cost variation when replacing the *non-empty* [first_rank,
// last_rank) portion for route raw_route with the job at
// job_rank. The case where the replaced range is empty is already
// covered by addition_cost.
inline Eval addition_cost_delta(const Input& input,
                                const SolutionState& sol_state,
                                const RawRoute& raw_route,
                                Index first_rank,
                                Index last_rank,
                                Index job_rank) {
  assert(first_rank < last_rank && !raw_route.empty());
  assert(last_rank <= raw_route.route.size());

  const auto& r = raw_route.route;
  const auto v_rank = raw_route.v_rank;
  const auto& v = input.vehicles[v_rank];
  const auto job_index = input.jobs[job_rank].index();

  Eval cost_delta =
    get_range_removal_gain(sol_state, v_rank, first_rank, last_rank);

  // Determine useful values if present.
  const auto [before_first, first_index, last_index] =
    get_indices(input, raw_route, first_rank, last_rank);

  // Gain of removed edge before replaced range.
  if (before_first && first_index) {
    cost_delta += v.eval(before_first.value(), first_index.value());
  }

  if (before_first) {
    // Cost of new edge to inserted job.
    cost_delta -= v.eval(before_first.value(), job_index);
  }

  if (last_index) {
    // Cost of new edge after inserted job.
    cost_delta -= v.eval(job_index, last_index.value());
  }

  // Gain of removed edge after replaced range, if any.
  if (last_index) {
    const Index before_last = input.jobs[r[last_rank - 1]].index();
    cost_delta += v.eval(before_last, last_index.value());
  }

  // Adding the job also adds its objective penalty for this vehicle.
  cost_delta.cost -= input.job_vehicle_penalty(job_rank, v_rank);

  return cost_delta;
}

// Compute cost variation when removing the "count" elements starting
// from rank in route.
inline Eval removal_cost_delta(const Input& input,
                               const SolutionState& sol_state,
                               const RawRoute& route,
                               Index rank,
                               unsigned count) {
  assert(!route.empty());
  assert(rank + count <= route.size());

  return std::get<0>(addition_cost_delta(input,
                                         sol_state,
                                         route,
                                         rank,
                                         rank + count,
                                         // dummy values for empty insertion
                                         route,
                                         0,
                                         0));
}

inline Eval max_edge_eval(const Input& input,
                          const Vehicle& v,
                          const std::vector<Index>& route) {
  Eval max_eval;

  if (!route.empty()) {
    if (v.has_start()) {
      const auto start_to_first =
        v.eval(v.start.value().index(), input.jobs[route.front()].index());
      max_eval = std::max(max_eval, start_to_first);
    }

    for (std::size_t i = 0; i < route.size() - 1; ++i) {
      const auto job_to_next =
        v.eval(input.jobs[route[i]].index(), input.jobs[route[i + 1]].index());
      max_eval = std::max(max_eval, job_to_next);
    }

    if (v.has_end()) {
      const auto last_to_end =
        v.eval(input.jobs[route.back()].index(), v.end.value().index());
      max_eval = std::max(max_eval, last_to_end);
    }
  }

  return max_eval;
}

// -------- Budget helpers (route-level) --------
inline Duration setup_for_prev(const Job& job,
                               const Vehicle& v,
                               std::optional<Index> prev_index) {
  if (prev_index.has_value() && prev_index.value() == job.index()) {
    return 0;
  }
  return job.setups[v.type];
}

inline Duration service_for(const Job& job, const Vehicle& v) {
  return job.services[v.type];
}

inline Cost action_cost_from_duration(const Vehicle& v, Duration d) {
  if (d == 0) {
    return 0;
  }
  const UserDuration ud = utils::scale_to_user_duration(d);
  const UserCost uc = v.cost_wrapper.user_cost_from_user_metrics(ud, 0);
  return utils::scale_from_user_cost(uc);
}

inline Cost job_budget(const Job& j) {
  // For shipments, budget is counted once on the pickup.
  if (j.type == JOB_TYPE::DELIVERY) {
    return 0;
  }
  return j.budget;
}

inline Cost route_budget_sum(const Input& input,
                             const std::vector<Index>& route) {
  Cost sum = 0;
  for (const auto r : route) {
    sum += job_budget(input.jobs[r]);
  }
  return sum;
}

inline Duration route_action_time_duration(const Input& input,
                                           const Vehicle& v,
                                           const std::vector<Index>& route) {
  if (route.empty()) {
    return 0;
  }
  Duration total = 0;
  // First job action: setup depends on start or same-location, plus service
  std::optional<Index> prev =
    v.has_start() ? std::optional<Index>(v.start.value().index())
                  : std::optional<Index>();
  for (std::size_t i = 0; i < route.size(); ++i) {
    const auto& job = input.jobs[route[i]];
    total += setup_for_prev(job, v, prev);
    total += service_for(job, v);
    prev = job.index();
  }
  return total;
}

inline Duration action_time_delta_single(const Input& input,
                                         const Vehicle& v,
                                         const std::vector<Index>& route,
                                         Index job_rank,
                                         Index insert_rank) {
  // Delta action time = setup/service for inserted job
  //                   + change of setup for the next job (if any)
  Duration delta = 0;
  const Job& j = input.jobs[job_rank];
  std::optional<Index> prev =
    (insert_rank == 0)
      ? (v.has_start() ? std::optional<Index>(v.start.value().index())
                       : std::optional<Index>())
      : std::optional<Index>(input.jobs[route[insert_rank - 1]].index());
  delta += setup_for_prev(j, v, prev);
  delta += service_for(j, v);

  if (insert_rank < route.size()) {
    const Job& n = input.jobs[route[insert_rank]];
    // Old setup for n with previous "prev"
    const Duration old_setup = setup_for_prev(n, v, prev);
    // New setup with previous now being j
    const Duration new_setup =
      setup_for_prev(n, v, std::optional<Index>(j.index()));
    delta += (new_setup - old_setup);
  }
  return delta;
}

// Forward declaration to allow use in action_time_delta_pd before its definition.
inline Duration action_time_delta_pd_contiguous(const Input& input,
                                                const Vehicle& v,
                                                Index pickup_rank_in_input);

inline Duration action_time_delta_pd(const Input& input,
                                     const Vehicle& v,
                                     const std::vector<Index>& route,
                                     Index pickup_rank_in_input,
                                     Index pickup_insert_rank,
                                     Index delivery_insert_rank) {
  // Handle contiguous insertion explicitly to avoid invalid indexing when
  // the route is empty or when both pickup and delivery share the same
  // insertion position in the original route.
  if (delivery_insert_rank == pickup_insert_rank) {
    return action_time_delta_pd_contiguous(input, v, pickup_rank_in_input);
  }
  Duration delta = 0;
  const Job& p = input.jobs[pickup_rank_in_input];
  const Job& d = input.jobs[pickup_rank_in_input + 1];

  // Pickup insertion
  std::optional<Index> prev_p =
    (pickup_insert_rank == 0)
      ? (v.has_start() ? std::optional<Index>(v.start.value().index())
                       : std::optional<Index>())
      : std::optional<Index>(input.jobs[route[pickup_insert_rank - 1]].index());
  delta += setup_for_prev(p, v, prev_p);
  delta += service_for(p, v);
  if (pickup_insert_rank < route.size()) {
    const Job& next_after_p = input.jobs[route[pickup_insert_rank]];
    const Duration old_setup_next = setup_for_prev(next_after_p, v, prev_p);
    const Duration new_setup_next =
      setup_for_prev(next_after_p, v, std::optional<Index>(p.index()));
    delta += (new_setup_next - old_setup_next);
  }

  // Compute delivery insertion index in the route after pickup is inserted.
  Index delivery_rank_after =
    (delivery_insert_rank <= pickup_insert_rank) ? delivery_insert_rank
                                                 : delivery_insert_rank;
  // If delivery is after pickup position in the original route, the route
  // has one more element due to pickup; insertion point shifts by +1.
  if (delivery_insert_rank > pickup_insert_rank) {
    delivery_rank_after += 1;
  }

  std::optional<Index> prev_d =
    (delivery_rank_after == 0)
      ? (v.has_start() ? std::optional<Index>(v.start.value().index())
                       : std::optional<Index>())
      : std::optional<Index>(
          (delivery_rank_after - 1 < pickup_insert_rank
             ? input.jobs[route[delivery_rank_after - 1]].index()
             : (delivery_rank_after - 1 == pickup_insert_rank
                  ? p.index()
                  : input.jobs[route[delivery_rank_after - 2]].index())));

  delta += setup_for_prev(d, v, prev_d);
  delta += service_for(d, v);
  if (delivery_rank_after < route.size() + 1) {
    // The route size increased by 1 after pickup
    Index next_index_in_route_block =
      (delivery_rank_after <= pickup_insert_rank
         ? route[delivery_rank_after]
         : (delivery_rank_after == pickup_insert_rank + 1
              ? (delivery_rank_after < route.size() + 1 ? route[delivery_rank_after - 1] : route.back())
              : route[delivery_rank_after - 1]));
    const Job& next_after_d = input.jobs[next_index_in_route_block];
    const Duration old_setup_next =
      setup_for_prev(next_after_d, v, prev_d);
    const Duration new_setup_next =
      setup_for_prev(next_after_d, v, std::optional<Index>(d.index()));
    delta += (new_setup_next - old_setup_next);
  }

  return delta;
}

inline Duration action_time_delta_pd_contiguous(const Input& input,
                                                const Vehicle& v,
                                                Index pickup_rank_in_input) {
  const Job& p = input.jobs[pickup_rank_in_input];
  const Job& d = input.jobs[pickup_rank_in_input + 1];
  Duration delta = 0;
  std::optional<Index> prev =
    v.has_start() ? std::optional<Index>(v.start.value().index())
                  : std::optional<Index>();
  delta += setup_for_prev(p, v, prev);
  delta += service_for(p, v);
  // delivery setup with previous being pickup (contiguous insertion)
  delta += setup_for_prev(d, v, std::optional<Index>(p.index()));
  delta += service_for(d, v);
  return delta;
}

// General PD action delta for non-contiguous placements (delivery after pickup
// with potential gap). Uses original route indices for pickup_insert_rank and
// delivery_insert_rank (before any insertion).
inline Duration action_time_delta_pd_general(const Input& input,
                                             const Vehicle& v,
                                             const std::vector<Index>& route,
                                             Index pickup_insert_rank,
                                             Index delivery_insert_rank,
                                             Index pickup_rank_in_input) {
  assert(delivery_insert_rank >= pickup_insert_rank);
  // If delivery is directly after pickup, delegate to contiguous case.
  if (delivery_insert_rank == pickup_insert_rank) {
    return action_time_delta_pd_contiguous(input, v, pickup_rank_in_input);
  }

  const Job& p = input.jobs[pickup_rank_in_input];
  const Job& d = input.jobs[pickup_rank_in_input + 1];
  Duration delta = 0;

  // Pickup insertion effects.
  std::optional<Index> prev_p =
    (pickup_insert_rank == 0)
      ? (v.has_start() ? std::optional<Index>(v.start.value().index())
                       : std::optional<Index>())
      : std::optional<Index>(input.jobs[route[pickup_insert_rank - 1]].index());
  delta += setup_for_prev(p, v, prev_p);
  delta += service_for(p, v);

  // Next after pickup setup change.
  if (pickup_insert_rank < route.size()) {
    const auto next_after_p = input.jobs[route[pickup_insert_rank]];
    const Duration old_setup_next_p = setup_for_prev(next_after_p, v, prev_p);
    const Duration new_setup_next_p =
      setup_for_prev(next_after_p, v, std::optional<Index>(p.index()));
    delta += (new_setup_next_p - old_setup_next_p);
  }

  // Delivery insertion effects (non-contiguous).
  // Previous before delivery in original route is the element at rd-1 (or start).
  std::optional<Index> prev_d =
    (delivery_insert_rank == 0)
      ? (v.has_start() ? std::optional<Index>(v.start.value().index())
                       : std::optional<Index>())
      : std::optional<Index>(input.jobs[route[delivery_insert_rank - 1]].index());
  delta += setup_for_prev(d, v, prev_d);
  delta += service_for(d, v);

  // Next after delivery setup change.
  if (delivery_insert_rank < route.size()) {
    const auto next_after_d = input.jobs[route[delivery_insert_rank]];
    const std::optional<Index> old_prev_next_d =
      (delivery_insert_rank == 0)
        ? (v.has_start() ? std::optional<Index>(v.start.value().index())
                         : std::optional<Index>())
        : std::optional<Index>(input.jobs[route[delivery_insert_rank - 1]].index());
    const Duration old_setup_next_d =
      setup_for_prev(next_after_d, v, old_prev_next_d);
    const Duration new_setup_next_d =
      setup_for_prev(next_after_d, v, std::optional<Index>(d.index()));
    delta += (new_setup_next_d - old_setup_next_d);
  }

  return delta;
}

// Helper function for SwapStar operator, computing part of the eval
// for in-place replacing of job at rank in route with job at
// job_rank.
inline Eval in_place_delta_cost(const Input& input,
                                Index job_rank,
                                const Vehicle& v,
                                const std::vector<Index>& route,
                                Index rank) {
  assert(!route.empty());
  const Index new_index = input.jobs[job_rank].index();

  Eval new_previous_eval;
  Eval new_next_eval;
  std::optional<Index> p_index;
  std::optional<Index> n_index;

  if (rank == 0) {
    if (v.has_start()) {
      p_index = v.start.value().index();
      new_previous_eval = v.eval(p_index.value(), new_index);
    }
  } else {
    p_index = input.jobs[route[rank - 1]].index();
    new_previous_eval = v.eval(p_index.value(), new_index);
  }

  if (rank == route.size() - 1) {
    if (v.has_end()) {
      n_index = v.end.value().index();
      new_next_eval = v.eval(new_index, n_index.value());
    }
  } else {
    n_index = input.jobs[route[rank + 1]].index();
    new_next_eval = v.eval(new_index, n_index.value());
  }

  Eval old_virtual_eval;
  if (p_index && n_index) {
    old_virtual_eval = v.eval(p_index.value(), n_index.value());
  }

  return new_previous_eval + new_next_eval - old_virtual_eval;
}

Priority priority_sum_for_route(const Input& input,
                                const std::vector<Index>& route);

Eval route_eval_for_vehicle(const Input& input,
                            Index vehicle_rank,
                            std::vector<Index>::const_iterator first_job,
                            std::vector<Index>::const_iterator last_job);

Eval route_eval_for_vehicle(const Input& input,
                            Index vehicle_rank,
                            const std::vector<Index>& route);

void check_tws(const std::vector<TimeWindow>& tws,
               Id id,
               const std::string& type);

void check_priority(Priority priority, Id id, const std::string& type);

void check_no_empty_keys(const TypeToDurationMap& type_to_duration,
                         const Id id,
                         const std::string& type,
                         const std::string& key_name);

using RawSolution = std::vector<RawRoute>;
using TWSolution = std::vector<TWRoute>;

Solution format_solution(const Input& input, const RawSolution& raw_routes);

Route format_route(const Input& input,
                   const TWRoute& tw_r,
                   std::unordered_set<Index>& unassigned_ranks);

Solution format_solution(const Input& input, const TWSolution& tw_routes);

} // namespace vroom::utils

#endif
