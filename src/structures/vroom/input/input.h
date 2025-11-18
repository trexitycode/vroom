#ifndef INPUT_H
#define INPUT_H

/*

This file is part of VROOM.

Copyright (c) 2015-2025, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include <chrono>
#include <memory>
#include <optional>
#include <unordered_map>

#include "routing/wrapper.h"
#include "structures/generic/matrix.h"
#include "structures/typedefs.h"
#include "structures/vroom/matrices.h"
#include "structures/vroom/solution/solution.h"
#include "structures/vroom/vehicle.h"

namespace vroom {

namespace io {
// Profile name used as key.
using Servers =
  std::unordered_map<std::string, Server, StringHash, std::equal_to<>>;
} // namespace io

class VRP;

struct PinnedBoundaryRequirement {
  // For pinned_position on a single job
  std::optional<Index> job_rank;
  // For pinned_position on a shipment (pair)
  std::optional<Index> pickup_rank;
  std::optional<Index> delivery_rank;
};

class Input {
private:
  TimePoint _start_loading{std::chrono::high_resolution_clock::now()};
  TimePoint _end_loading;
  TimePoint _end_solving;
  TimePoint _end_routing;
  std::unordered_set<std::string, StringHash, std::equal_to<>> _profiles;
  std::unordered_set<std::string, StringHash, std::equal_to<>>
    _profiles_requiring_distances;
  std::vector<std::unique_ptr<routing::Wrapper>> _routing_wrappers;
  bool _apply_TSPFix;
  bool _no_addition_yet{true};
  bool _has_skills{false};
  bool _has_TW{false};
  bool _has_all_coordinates{true};
  bool _has_custom_location_index;
  bool _has_initial_routes{false};
  bool _homogeneous_locations{true};
  bool _homogeneous_profiles{true};
  bool _homogeneous_costs{true};
  bool _geometry{false};
  bool _report_distances;
  bool _has_jobs{false};
  bool _has_shipments{false};
  // Preserve pinned semantics flags
  bool _pinned_soft_timing{false};
  Duration _pinned_violation_budget{0};
  std::unordered_map<std::string,
                     Matrix<UserDuration>,
                     StringHash,
                     std::equal_to<>>
    _durations_matrices;
  std::unordered_map<std::string,
                     Matrix<UserDistance>,
                     StringHash,
                     std::equal_to<>>
    _distances_matrices;
  std::unordered_map<std::string, Matrix<UserCost>, StringHash, std::equal_to<>>
    _costs_matrices;
  std::unordered_map<std::string, Cost, StringHash, std::equal_to<>>
    _max_cost_per_hour;
  Cost _cost_upper_bound{0};
  // Budget semantics
  bool _include_action_time_in_budget{false};
  std::vector<Location> _locations;
  std::unordered_map<Location, Index> _locations_to_index;
  std::unordered_set<Location> _locations_used_several_times;
  std::vector<std::vector<unsigned char>> _vehicle_to_job_compatibility;
  std::vector<std::vector<bool>> _vehicle_to_vehicle_compatibility;
  // For pinned semantics: if set, job j must stay on pinned vehicle
  std::vector<std::optional<Index>> _pinned_vehicle_by_job;
  // For pinned_position semantics: requirements per vehicle
  std::vector<std::optional<PinnedBoundaryRequirement>> _pinned_first_by_vehicle;
  std::vector<std::optional<PinnedBoundaryRequirement>> _pinned_last_by_vehicle;
  std::unordered_set<Index> _matrices_used_index;
  Index _max_matrices_used_index{0};
  bool _all_locations_have_coords{true};
  std::vector<std::vector<Eval>> _jobs_vehicles_evals;
  // Repair tuning: max candidate unassigned jobs/shipments to consider for densify
  unsigned _budget_densify_candidates_k{20};

  // Default vehicle type is NO_TYPE, related to the fact that we do
  // not allow empty types as keys for jobs.
  std::vector<std::string> _vehicle_types{NO_TYPE};
  std::unordered_map<std::string, Index, StringHash, std::equal_to<>>
    _type_to_rank_in_vehicle_types{{NO_TYPE, 0}};

  // Used in plan mode since we store route geometries while
  // generating sparse matrices.
  std::vector<std::string> _vehicles_geometry;

  std::optional<unsigned> _amount_size;
  Amount _zero;

  const io::Servers _servers;
  const ROUTER _router;

  std::unique_ptr<VRP> get_problem() const;

  void check_amount_size(const Amount& amount);
  void check_job(Job& job);

  void run_basic_checks() const;

  UserCost check_cost_bound(const Matrix<UserCost>& matrix) const;

  void set_skills_compatibility();
  void set_extra_compatibility();
  void set_vehicles_compatibility();
  void set_vehicles_costs();
  void set_vehicles_max_tasks();
  void set_jobs_vehicles_evals();
  void set_jobs_durations_per_vehicle_type();
  void set_vehicle_steps_ranks();
  void init_missing_matrices(const std::string& profile);

  routing::Matrices get_matrices_by_profile(const std::string& profile,
                                            bool sparse_filling);

  void set_matrices(unsigned nb_thread, bool sparse_filling = false);

  void add_routing_wrapper(const std::string& profile);

  // Ensure pinned tasks remain eligible on their pinned vehicle during seeding
  // (relax pre-compat restrictions; feasibility is handled during solve).
  void enforce_pinned_eligibility();

  // Final validation pass: ensure first-leg distance limit holds in all routes.
  // If a violation is found, drop the violating route and mark its jobs unassigned.
  void validate_first_leg_limits(Solution& sol) const;

public:
  std::vector<Job> jobs;
  std::vector<Vehicle> vehicles;

  // Store rank in jobs accessible from job/pickup/delivery id.
  std::unordered_map<Id, Index> job_id_to_rank;
  std::unordered_map<Id, Index> pickup_id_to_rank;
  std::unordered_map<Id, Index> delivery_id_to_rank;

  // Store list of compatible vehicles for each job.
  std::vector<std::vector<Index>> compatible_vehicles_for_job;

  Input(io::Servers servers = {},
        ROUTER router = ROUTER::OSRM,
        bool apply_TSPFix = false);

  unsigned get_amount_size() const {
    assert(_amount_size.has_value());
    return _amount_size.value();
  }

  void set_geometry(bool geometry);

  void add_job(const Job& job);

  void add_shipment(const Job& pickup, const Job& delivery);

  void add_vehicle(const Vehicle& vehicle);

  void set_durations_matrix(const std::string& profile,
                            Matrix<UserDuration>&& m);

  void set_distances_matrix(const std::string& profile,
                            Matrix<UserDistance>&& m);

  void set_costs_matrix(const std::string& profile, Matrix<UserCost>&& m);

  const Amount& zero_amount() const {
    return _zero;
  }

  bool apply_TSPFix() const {
    return _apply_TSPFix;
  }

  // Preserve pinned
  void set_pinned_soft_timing(bool v) {
    _pinned_soft_timing = v;
  }

  void set_pinned_violation_budget(UserDuration s) {
    _pinned_violation_budget = utils::scale_from_user_duration(s);
  }

  bool pinned_soft_timing() const {
    return _pinned_soft_timing;
  }

  Duration pinned_violation_budget() const {
    return _pinned_violation_budget;
  }
  // Budget semantics flag (action times priced into budget check)
  void set_include_action_time_in_budget(bool v) {
    _include_action_time_in_budget = v;
  }
  bool include_action_time_in_budget() const {
    return _include_action_time_in_budget;
  }

  bool is_used_several_times(const Location& location) const;

  bool has_skills() const;

  bool has_jobs() const;

  bool has_shipments() const;

  bool report_distances() const;

  Cost get_cost_upper_bound() const {
    return _cost_upper_bound;
  }

  bool all_locations_have_coords() const {
    return _all_locations_have_coords;
  }

  const std::vector<std::vector<Eval>>& jobs_vehicles_evals() const {
    return _jobs_vehicles_evals;
  }

  bool has_homogeneous_locations() const;

  bool has_homogeneous_profiles() const;

  bool has_homogeneous_costs() const;

  bool has_initial_routes() const;

  bool vehicle_ok_with_job(size_t v_index, size_t j_index) const {
    return static_cast<bool>(_vehicle_to_job_compatibility[v_index][j_index]);
  }

  // Pinned helpers
  bool job_is_pinned(Index job_rank) const {
    return _pinned_vehicle_by_job.size() > job_rank &&
           _pinned_vehicle_by_job[job_rank].has_value();
  }

  std::optional<Index> pinned_vehicle(Index job_rank) const {
    if (_pinned_vehicle_by_job.size() <= job_rank) {
      return std::nullopt;
    }
    return _pinned_vehicle_by_job[job_rank];
  }

  // Returns true iff both vehicles have common job candidates.
  bool vehicle_ok_with_vehicle(Index v1_index, Index v2_index) const;

  // Anchors API
  std::optional<PinnedBoundaryRequirement>
  pinned_first_for_vehicle(Index v_index) const {
    if (_pinned_first_by_vehicle.size() <= v_index) {
      return std::nullopt;
    }
    return _pinned_first_by_vehicle[v_index];
  }

  std::optional<PinnedBoundaryRequirement>
  pinned_last_for_vehicle(Index v_index) const {
    if (_pinned_last_by_vehicle.size() <= v_index) {
      return std::nullopt;
    }
    return _pinned_last_by_vehicle[v_index];
  }

  // Repair tuning
  void set_budget_densify_candidates_k(unsigned v) {
    _budget_densify_candidates_k = (v == 0 ? 1u : v);
  }
  unsigned budget_densify_candidates_k() const {
    return _budget_densify_candidates_k;
  }

  Solution solve(unsigned nb_searches,
                 unsigned depth,
                 unsigned nb_thread,
                 const Timeout& timeout = Timeout(),
                 const std::vector<HeuristicParameters>& h_param =
                   std::vector<HeuristicParameters>());

  // Overload designed to expose the same interface as the `-x`
  // command-line flag for out-of-the-box setup of exploration level.
  Solution solve(unsigned exploration_level,
                 unsigned nb_thread,
                 const Timeout& timeout = Timeout(),
                 const std::vector<HeuristicParameters>& h_param =
                   std::vector<HeuristicParameters>());

  Solution check(unsigned nb_thread);
};

} // namespace vroom

#endif
