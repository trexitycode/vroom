#include "utils/budget_repair.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <new>

#include "structures/vroom/tw_route.h"
#include "structures/vroom/solution/route.h"
#include "utils/helpers.h"

namespace vroom::utils {

static Cost compute_total_internal_cost(const Input& input,
                                        const Vehicle& v,
                                        Index v_index,
                                        const std::vector<Index>& ranks) {
  const Eval e = utils::route_eval_for_vehicle(input, v_index, ranks);
  Cost c = e.cost + v.fixed_cost();
  if (input.include_action_time_in_budget()) {
    const Duration d = utils::route_action_time_duration(input, v, ranks);
    c += utils::action_cost_from_duration(v, d);
  }
  return c;
}

static Cost compute_route_budget_sum_from_ranks(const Input& input,
                                                const std::vector<Index>& ranks) {
  Cost s = 0;
  for (const auto r : ranks) {
    const auto& j = input.jobs[r];
    if (j.type == JOB_TYPE::DELIVERY) {
      continue;
    }
    s += utils::job_budget(j);
  }
  return s;
}

void repair_budget(const Input& input, Solution& sol) {
  std::vector<Route> kept_routes;
  kept_routes.reserve(sol.routes.size());
  std::vector<Job> extra_unassigned;
  extra_unassigned.reserve(input.jobs.size());
  std::unordered_set<Id> remove_from_unassigned_ids;
  remove_from_unassigned_ids.reserve(sol.unassigned.size());

  // Prepare unassigned id set for quick membership checks across routes
  std::unordered_set<Id> unassigned_ids;
  unassigned_ids.reserve(sol.unassigned.size());
  for (const auto& uj : sol.unassigned) {
    unassigned_ids.insert(uj.id);
  }

  for (const auto& route : sol.routes) {
    // Resolve vehicle by id and index
    const Vehicle* v_ptr = nullptr;
    Index v_index = std::numeric_limits<Index>::max();
    for (Index vi = 0; vi < input.vehicles.size(); ++vi) {
      if (input.vehicles[vi].id == route.vehicle) {
        v_ptr = &input.vehicles[vi];
        v_index = vi;
        break;
      }
    }
    if (v_ptr == nullptr) {
      // Unknown vehicle id: conservatively unassign all tasks on the route
      for (const auto& st : route.steps) {
        if (st.step_type == STEP_TYPE::JOB) {
          Index r = 0;
          if (st.job_type.has_value() && st.job_type.value() == JOB_TYPE::PICKUP) {
            r = input.pickup_id_to_rank.at(st.id);
          } else if (st.job_type.has_value() && st.job_type.value() == JOB_TYPE::DELIVERY) {
            r = input.delivery_id_to_rank.at(st.id);
          } else {
            r = input.job_id_to_rank.at(st.id);
          }
          extra_unassigned.push_back(input.jobs[r]);
        }
      }
      continue;
    }

    // Build ranks for current route
    std::vector<Index> ranks;
    ranks.reserve(route.steps.size());
    for (const auto& st : route.steps) {
      if (st.step_type != STEP_TYPE::JOB) {
        continue;
      }
      Index r = 0;
      if (st.job_type.has_value() && st.job_type.value() == JOB_TYPE::PICKUP) {
        r = input.pickup_id_to_rank.at(st.id);
      } else if (st.job_type.has_value() && st.job_type.value() == JOB_TYPE::DELIVERY) {
        r = input.delivery_id_to_rank.at(st.id);
      } else {
        r = input.job_id_to_rank.at(st.id);
      }
      ranks.push_back(r);
    }

    // Internal cost and budget
    Cost cur_cost = compute_total_internal_cost(input, *v_ptr, v_index, ranks);
    Cost cur_budget = compute_route_budget_sum_from_ranks(input, ranks);
    bool has_any_budget = false;
    for (const auto r : ranks) {
      const auto& j = input.jobs[r];
      if (j.type != JOB_TYPE::DELIVERY && utils::job_budget(j) > 0) {
        has_any_budget = true;
        break;
      }
    }

    // If no task provides a budget on this route, skip enforcement entirely.
    if (!has_any_budget) {
      kept_routes.push_back(route);
      continue;
    }

    if (cur_budget >= cur_cost) {
      kept_routes.push_back(route);
      continue;
    }

    // Densify from unassigned candidates (non-contiguous PD supported)
    {
      // Candidate list (top-K by budget)
      struct Cand {
        Index job_rank;
        bool is_pd;
        Cost budget;
      };
      std::vector<Cand> cands;
      cands.reserve(sol.unassigned.size());
      for (const auto& uj : sol.unassigned) {
        if (uj.type == JOB_TYPE::PICKUP) {
          const Index pr = input.pickup_id_to_rank.at(uj.id);
          const Index dr = pr + 1;
          const auto& del = input.jobs[dr];
          if (unassigned_ids.contains(del.id)) {
            cands.push_back(Cand{pr, true, utils::job_budget(input.jobs[pr])});
          }
        } else if (uj.type == JOB_TYPE::SINGLE) {
          const Index jr = input.job_id_to_rank.at(uj.id);
          cands.push_back(Cand{jr, false, utils::job_budget(input.jobs[jr])});
        }
      }
      std::ranges::sort(cands, std::greater{}, &Cand::budget);
      const std::size_t K_add =
        static_cast<std::size_t>(input.budget_densify_candidates_k());
      if (cands.size() > K_add) {
        cands.resize(K_add);
      }

      // TWRoute for feasibility checks
      TWRoute tw_cur(input, v_index, input.get_amount_size());
      // Seed full TWRoute state (earliest/latest, breaks arrays, loads, etc.)
      tw_cur.seed_relaxed_from_job_ranks(input, input.zero_amount(), ranks);

      Cost best_gain = 0;
      std::vector<Index> best_new_ranks;
      std::optional<std::pair<Index, Index>> best_added; // (pickup, delivery) or (single, invalid)

      for (const auto& cand : cands) {
        if (cand.is_pd) {
          const Index pr = cand.job_rank;
          for (Index pickup_r = 0; pickup_r <= tw_cur.route.size(); ++pickup_r) {
            for (Index delivery_r = pickup_r; delivery_r <= tw_cur.route.size(); ++delivery_r) {
              // Build inclusion range
              std::vector<Index> modified_with_pd;
              modified_with_pd.reserve((delivery_r - pickup_r) + 2);
              modified_with_pd.push_back(pr);
              Amount md = input.zero_amount();
              for (Index t = pickup_r; t < delivery_r; ++t) {
                modified_with_pd.push_back(tw_cur.route[t]);
                const auto& between_job = input.jobs[tw_cur.route[t]];
                if (between_job.type == JOB_TYPE::SINGLE) {
                  md += between_job.delivery;
                }
              }
              modified_with_pd.push_back(pr + 1);

              if (!tw_cur.is_valid_addition_for_capacity_inclusion(
                    input, md, modified_with_pd.begin(), modified_with_pd.end(), pickup_r, delivery_r)) {
                continue;
              }
              if (!tw_cur.is_valid_addition_for_tw(
                    input, md, modified_with_pd.begin(), modified_with_pd.end(), pickup_r, delivery_r)) {
                continue;
              }

              const Index delivery_after =
                (delivery_r == pickup_r) ? (pickup_r + 1) : (delivery_r + 1);
              const Eval delta_eval =
                utils::addition_cost_travel(input,
                                            pr,
                                            v_index,
                                            tw_cur.route,
                                            pickup_r,
                                            delivery_after);
              Cost delta_cost = delta_eval.cost;
              if (input.include_action_time_in_budget()) {
                const Duration ad = utils::action_time_delta_pd_general(
                  input, *v_ptr, tw_cur.route, pickup_r, delivery_r, pr);
                delta_cost += utils::action_cost_from_duration(*v_ptr, ad);
              }
              const Cost budget_added = utils::job_budget(input.jobs[pr]);
              const Cost new_cost = cur_cost + delta_cost;
              const Cost new_budget = cur_budget + budget_added;
              const Cost gain = new_budget - new_cost - (cur_budget - cur_cost);
              if (new_budget >= new_cost && gain > best_gain) {
                std::vector<Index> cand_ranks = tw_cur.route;
                cand_ranks.insert(cand_ranks.begin() + pickup_r, pr);
                const Index ins_d = (delivery_r == pickup_r) ? (pickup_r + 1) : (delivery_r + 1);
                cand_ranks.insert(cand_ranks.begin() + ins_d, pr + 1);
                best_gain = gain;
                best_new_ranks = std::move(cand_ranks);
                best_added = std::make_pair(pr, pr + 1);
              }
            }
          }
        } else {
          const Index jr = cand.job_rank;
          for (Index rpos = 0; rpos <= tw_cur.route.size(); ++rpos) {
            if (!tw_cur.is_valid_addition_for_capacity(input, input.jobs[jr].pickup, input.jobs[jr].delivery, rpos) ||
                !tw_cur.is_valid_addition_for_tw(input, jr, rpos)) {
              continue;
            }
            const Eval delta_eval =
              utils::addition_cost_travel(input, jr, v_index, tw_cur.route, rpos);
            Cost delta_cost = delta_eval.cost;
            if (input.include_action_time_in_budget()) {
              const Duration ad = utils::action_time_delta_single(input, *v_ptr, tw_cur.route, jr, rpos);
              delta_cost += utils::action_cost_from_duration(*v_ptr, ad);
            }
            const Cost budget_added = utils::job_budget(input.jobs[jr]);
            const Cost new_cost = cur_cost + delta_cost;
            const Cost new_budget = cur_budget + budget_added;
            const Cost gain = new_budget - new_cost - (cur_budget - cur_cost);
            if (new_budget >= new_cost && gain > best_gain) {
              std::vector<Index> cand_ranks = tw_cur.route;
              cand_ranks.insert(cand_ranks.begin() + rpos, jr);
              best_gain = gain;
              best_new_ranks = std::move(cand_ranks);
              best_added = std::make_pair(jr, std::numeric_limits<Index>::max());
            }
          }
        }
      }

      if (best_gain > 0 && !best_new_ranks.empty()) {
        // Commit densify insertion
        TWRoute tw_new(input, v_index, input.get_amount_size());
        tw_new.seed_relaxed_from_job_ranks(input, input.zero_amount(), best_new_ranks);
        std::unordered_set<Index> dummy;
        Route rebuilt = utils::format_route(input, tw_new, dummy);
        // Mark inserted items for removal from unassigned
        if (best_added.has_value()) {
          remove_from_unassigned_ids.insert(input.jobs[best_added->first].id);
          if (best_added->second != std::numeric_limits<Index>::max()) {
            remove_from_unassigned_ids.insert(input.jobs[best_added->second].id);
          }
        }
        kept_routes.push_back(std::move(rebuilt));
        continue;
      }
    }

    // Selective removals (greedy) then rebuild if feasible
    {
      std::vector<Index> ranks_local = ranks;
      std::vector<Index> removed_ranks;
      removed_ranks.reserve(ranks_local.size());
      // Map job rank to position for quick lookup will be built each loop
      while (!ranks_local.empty()) {
        Cost cur_cost_local = compute_total_internal_cost(input, *v_ptr, v_index, ranks_local);
        Cost cur_budget_local = compute_route_budget_sum_from_ranks(input, ranks_local);
        if (cur_budget_local >= cur_cost_local) {
          break;
        }
        // Find best removal
        Cost best_delta = 0;
        std::vector<Index> best_new_ranks;
        std::optional<std::pair<Index, Index>> best_removed_pair;

        std::unordered_map<Index, std::size_t> pos_by_rank;
        for (std::size_t p = 0; p < ranks_local.size(); ++p) {
          pos_by_rank[ranks_local[p]] = p;
        }

        for (std::size_t p = 0; p < ranks_local.size(); ++p) {
          const Index jr = ranks_local[p];
          const auto& j = input.jobs[jr];
          if (j.pinned) {
            continue;
          }
          if (j.type == JOB_TYPE::SINGLE) {
            std::vector<Index> cand = ranks_local;
            cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(p));
            const Cost new_cost = compute_total_internal_cost(input, *v_ptr, v_index, cand);
            const Cost new_budget = compute_route_budget_sum_from_ranks(input, cand);
            const Cost delta = (new_budget - new_cost) - (cur_budget_local - cur_cost_local);
            if (delta > best_delta) {
              best_delta = delta;
              best_new_ranks = std::move(cand);
              best_removed_pair = std::make_pair(jr, std::numeric_limits<Index>::max());
            }
          } else if (j.type == JOB_TYPE::PICKUP) {
            const Index dr = jr + 1;
            auto it = pos_by_rank.find(dr);
            if (it == pos_by_rank.end() || input.jobs[dr].pinned) {
              continue;
            }
            const std::size_t dp = it->second;
            const std::size_t first = std::min(p, dp);
            const std::size_t second = std::max(p, dp);
            std::vector<Index> cand;
            cand.reserve(ranks_local.size() - 2);
            for (std::size_t q = 0; q < ranks_local.size(); ++q) {
              if (q == first || q == second) {
                continue;
              }
              cand.push_back(ranks_local[q]);
            }
            const Cost new_cost = compute_total_internal_cost(input, *v_ptr, v_index, cand);
            const Cost new_budget = compute_route_budget_sum_from_ranks(input, cand);
            const Cost delta = (new_budget - new_cost) - (cur_budget_local - cur_cost_local);
            if (delta > best_delta) {
              best_delta = delta;
              best_new_ranks = std::move(cand);
              best_removed_pair = std::make_pair(jr, dr);
            }
          }
        }

        if (best_delta <= 0 || !best_removed_pair.has_value()) {
          break;
        }

        if (best_removed_pair->first != std::numeric_limits<Index>::max()) {
          removed_ranks.push_back(best_removed_pair->first);
        }
        if (best_removed_pair->second != std::numeric_limits<Index>::max()) {
          removed_ranks.push_back(best_removed_pair->second);
        }
        ranks_local = std::move(best_new_ranks);
      }

      // Final check and commit
      const Cost final_cost = compute_total_internal_cost(input, *v_ptr, v_index, ranks_local);
      const Cost final_budget = compute_route_budget_sum_from_ranks(input, ranks_local);
      if (!ranks_local.empty() && final_budget >= final_cost) {
        TWRoute tw(input, v_index, input.get_amount_size());
        tw.seed_relaxed_from_job_ranks(input, input.zero_amount(), ranks_local);
        std::unordered_set<Index> dummy;
        Route rebuilt = utils::format_route(input, tw, dummy);
        kept_routes.push_back(std::move(rebuilt));
        for (const auto r : removed_ranks) {
          extra_unassigned.push_back(input.jobs[r]);
        }
      } else {
        // Drop route entirely
        for (const auto& st : route.steps) {
          if (st.step_type == STEP_TYPE::JOB) {
            Index r = 0;
            if (st.job_type.has_value() && st.job_type.value() == JOB_TYPE::PICKUP) {
              r = input.pickup_id_to_rank.at(st.id);
            } else if (st.job_type.has_value() && st.job_type.value() == JOB_TYPE::DELIVERY) {
              r = input.delivery_id_to_rank.at(st.id);
            } else {
              r = input.job_id_to_rank.at(st.id);
            }
            extra_unassigned.push_back(input.jobs[r]);
          }
        }
      }
    }
  }

  if (kept_routes.size() != sol.routes.size()) {
    // Merge unassigned without using erase/insert on vector<Job>
    std::vector<Job> merged_unassigned;
    merged_unassigned.reserve(sol.unassigned.size() + extra_unassigned.size());
    for (const auto& j : sol.unassigned) {
      if (!remove_from_unassigned_ids.contains(j.id)) {
        merged_unassigned.push_back(j); // copy-construct
      }
    }
    for (const auto& j : extra_unassigned) {
      merged_unassigned.push_back(j); // copy-construct
    }

    // Preserve computing times then rebuild summary from kept routes
    const auto old_times = sol.summary.computing_times;

    sol.routes = std::move(kept_routes);
    sol.unassigned = std::move(merged_unassigned);

    // Rebuild summary in place
    new (&sol.summary)
      Summary(static_cast<unsigned>(sol.routes.size()),
              static_cast<unsigned>(sol.unassigned.size()),
              input.zero_amount());
    for (const auto& route : sol.routes) {
      sol.summary.cost += route.cost;
      sol.summary.delivery += route.delivery;
      sol.summary.pickup += route.pickup;
      sol.summary.setup += route.setup;
      sol.summary.service += route.service;
      sol.summary.priority += route.priority;
      sol.summary.duration += route.duration;
      sol.summary.distance += route.distance;
      sol.summary.waiting_time += route.waiting_time;
      sol.summary.violations += route.violations;
    }
    sol.summary.computing_times = old_times;
  }
}

} // namespace vroom::utils


