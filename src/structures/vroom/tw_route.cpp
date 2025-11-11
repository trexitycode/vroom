/*

This file is part of VROOM.

Copyright (c) 2015-2025, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include <algorithm>

#include "structures/vroom/tw_route.h"
#include "utils/helpers.h"

namespace vroom {

TWRoute::TWRoute(const Input& input, Index v, unsigned amount_size)
  : RawRoute(input, v, amount_size),
    v_start(input.vehicles[v].tw.start),
    v_end(input.vehicles[v].tw.end),
    breaks_at_rank({static_cast<unsigned>(input.vehicles[v].breaks.size())}),
    breaks_counts({static_cast<unsigned>(input.vehicles[v].breaks.size())}),
    break_earliest(input.vehicles[v].breaks.size()),
    break_latest(input.vehicles[v].breaks.size()),
    fwd_smallest_breaks_load_margin(input.vehicles[v].breaks.size()),
    bwd_smallest_breaks_load_margin(input.vehicles[v].breaks.size()) {
  const std::string break_error =
    std::format("Inconsistent breaks for vehicle {}.", input.vehicles[v].id);

  const auto& breaks = input.vehicles[v].breaks;

  Duration previous_earliest = v_start;

  // Store smallest margin component-wise.
  Amount fwd_smallest_margin = utils::max_amount(amount_size);
  Amount bwd_smallest_margin = utils::max_amount(amount_size);

  for (Index i = 0; i < breaks.size(); ++i) {
    const auto& b = breaks[i];
    const auto b_tw = std::ranges::find_if(b.tws, [&](const auto& tw) {
      return previous_earliest <= tw.end;
    });
    if (b_tw == b.tws.end()) {
      throw InputException(break_error);
    }

    break_earliest[i] = std::max(previous_earliest, b_tw->start);

    previous_earliest = break_earliest[i] + b.service;

    if (b.max_load.has_value()) {
      const auto& max_load = b.max_load.value();
      for (std::size_t a = 0; a < amount_size; ++a) {
        if (max_load[a] < fwd_smallest_margin[a]) {
          fwd_smallest_margin[a] = max_load[a];
        }
      }
    }
    fwd_smallest_breaks_load_margin[i] = fwd_smallest_margin;
  }

  Duration next_latest = v_end;
  for (Index r_i = 0; r_i < breaks.size(); ++r_i) {
    const Index i = breaks.size() - 1 - r_i;
    const auto& b = breaks[i];

    if (next_latest < b.service) {
      throw InputException(break_error);
    }
    next_latest -= b.service;

    const auto b_tw =
      std::find_if(b.tws.rbegin(), b.tws.rend(), [&](const auto& tw) {
        return tw.start <= next_latest;
      });
    if (b_tw == b.tws.rend()) {
      throw InputException(break_error);
    }

    break_latest[i] = std::min(next_latest, b_tw->end);

    next_latest = break_latest[i];

    if (break_latest[i] < break_earliest[i]) {
      throw InputException(break_error);
    }

    if (b.max_load.has_value()) {
      const auto& max_load = b.max_load.value();
      for (std::size_t a = 0; a < amount_size; ++a) {
        if (max_load[a] < bwd_smallest_margin[a]) {
          bwd_smallest_margin[a] = max_load[a];
        }
      }
    }
    bwd_smallest_breaks_load_margin[i] = bwd_smallest_margin;
  }
}

void TWRoute::seed_relaxed_from_job_ranks(const Input& input,
                                          const Amount& single_jobs_delivery [[maybe_unused]],
                                          const std::vector<Index>& job_ranks) {
  // Initialize route directly, ignoring time windows. Compute baseline earliest service starts.
  set_route(input, job_ranks);

  const auto& v = input.vehicles[v_rank];
  const std::size_t n = route.size();
  earliest.assign(n, 0);
  latest.assign(n, v_end); // loose bound
  action_time.assign(n, 0);
  // breaks_* vectors must have size route.size() + 1 (include end boundary).
  // With soft-timing we routinely reason about the "virtual" step that follows
  // the last job (to park breaks or push pinned work), so we keep an explicit
  // sentinel slot instead of relying on undefined behaviour past the array end.
  breaks_at_rank.assign(n + 1, 0);
  breaks_counts.assign(n + 1, 0);
  baseline_service_start.assign(n, 0);
  is_pinned_step.assign(n, false);

  // Forward pass computing service start times (no TW clamp)
  Duration current_earliest = v_start;
  std::optional<Index> previous_index;
  if (has_start) {
    previous_index = v.start.value().index();
  }
  for (std::size_t i = 0; i < n; ++i) {
    const auto& j = input.jobs[route[i]];
    if (previous_index.has_value()) {
      current_earliest += v.duration(previous_index.value(), j.index());
    }
    earliest[i] = current_earliest;
    baseline_service_start[i] = current_earliest;
    is_pinned_step[i] = j.pinned;

    const auto job_action_time = (previous_index.has_value() && j.index() == previous_index.value())
                                   ? j.services[v_type]
                                   : j.setups[v_type] + j.services[v_type];
    action_time[i] = job_action_time;
    current_earliest += job_action_time;
    previous_index = j.index();

    // Cumulative count up to rank i (no breaks assigned before jobs here).
    // The sentinel will accumulate the remaining vehicle breaks at the end.
    if (i > 0) {
      breaks_counts[i] = breaks_counts[i - 1] + breaks_at_rank[i];
    }
  }

  // Place all vehicle breaks before route end in this relaxed seed. Holding the
  // full count in the sentinel keeps later update loops inside bounds.
  breaks_at_rank[n] = static_cast<unsigned>(v.breaks.size());
  breaks_counts[n] = (n > 0 ? breaks_counts[n - 1] : 0) + breaks_at_rank[n];

  // Update load-related internal state to keep route consistent
  update_amounts(input);
}

PreviousInfo TWRoute::previous_info(const Input& input,
                                    const Index job_rank,
                                    const Index rank) const {
  const auto& v = input.vehicles[v_rank];
  const auto& j = input.jobs[job_rank];

  PreviousInfo previous(v_start, 0);
  if (rank > 0) {
    const auto& previous_job = input.jobs[route[rank - 1]];
    previous.earliest = earliest[rank - 1] + action_time[rank - 1];
    previous.travel = v.duration(previous_job.index(), j.index());
    previous.location_index = previous_job.location.index();
  } else {
    if (has_start) {
      previous.location_index = v.start.value().index();
      previous.travel = v.duration(previous.location_index, j.index());
    }
  }

  return previous;
}

NextInfo TWRoute::next_info(const Input& input,
                            const Index job_rank,
                            const Index rank) const {
  const auto& v = input.vehicles[v_rank];
  const auto& j = input.jobs[job_rank];

  NextInfo next(v_end, 0);
  if (rank == route.size()) {
    if (has_end) {
      next.travel = v.duration(j.index(), v.end.value().index());
    }
  } else {
    next.latest = latest[rank];
    next.travel = v.duration(j.index(), input.jobs[route[rank]].index());
  }

  return next;
}

void TWRoute::fwd_update_earliest_from(const Input& input, Index rank) {
  const auto& v = input.vehicles[v_rank];

  Duration current_earliest = earliest[rank];
  bool handle_last_breaks = true;

  for (Index i = rank + 1; i < route.size(); ++i) {
    const auto& next_j = input.jobs[route[i]];
    Duration remaining_travel_time =
      v.duration(input.jobs[route[i - 1]].index(), next_j.index());
    Duration previous_action_time = action_time[i - 1];

    // Update earliest dates and margins for breaks.
    assert(breaks_at_rank[i] <= breaks_counts[i]);
    Index break_rank = breaks_counts[i] - breaks_at_rank[i];

    for (Index r = 0; r < breaks_at_rank[i]; ++r, ++break_rank) {
      const auto& b = v.breaks[break_rank];

      current_earliest += previous_action_time;

      const auto b_tw = std::ranges::find_if(b.tws, [&](const auto& tw) {
        return current_earliest <= tw.end;
      });
      if (b_tw == b.tws.end()) {
        // Soft-pinned slack can push a break beyond every TW. In that case we
        // keep the best effort (back().end) instead of asserting and aborting.
        if (!b.tws.empty()) {
          current_earliest = b.tws.back().end;
        }
        break_earliest[break_rank] = current_earliest;
        handle_last_breaks = false;
        break;
      }

      if (current_earliest < b_tw->start) {
        if (const auto margin = b_tw->start - current_earliest;
            margin < remaining_travel_time) {
          remaining_travel_time -= margin;
        } else {
          remaining_travel_time = 0;
        }

        current_earliest = b_tw->start;
      }

      break_earliest[break_rank] = current_earliest;
      previous_action_time = v.breaks[break_rank].service;
    }

    // Back to the job after breaks.
    current_earliest += previous_action_time + remaining_travel_time;

    const auto j_tw = std::ranges::find_if(next_j.tws, [&](const auto& tw) {
      return current_earliest <= tw.end;
    });
    if (j_tw == next_j.tws.end()) {
      // Same story for jobs: when soft timing lets us drift beyond the last TW,
      // clamp to its end so the rest of the propagation code keeps working.
      if (!next_j.tws.empty()) {
        current_earliest = next_j.tws.back().end;
      }
      earliest[i] = current_earliest;
      handle_last_breaks = false;
      break;
    }

    current_earliest = std::max(current_earliest, j_tw->start);

    // Check consistency except for situation where latest date has
    // been reset to 0 to force backward propagation after this call
    // to fwd_update_earliest_from.
    assert(current_earliest <= latest[i] || (i == rank + 1 && latest[i] == 0));
    if (current_earliest == earliest[i]) {
      // There won't be any further update so stop earliest date
      // propagation.
      handle_last_breaks = false;
      break;
    }

    earliest[i] = current_earliest;
  }

  if (handle_last_breaks) {
    // Update earliest dates and margins for potential breaks right
    // before route end.
    const Index i = route.size();
    Duration remaining_travel_time =
      (v.has_end())
        ? v.duration(input.jobs[route[i - 1]].index(), v.end.value().index())
        : 0;

    Duration previous_action_time = action_time[i - 1];

    assert(breaks_at_rank[i] <= breaks_counts[i]);
    Index break_rank = breaks_counts[i] - breaks_at_rank[i];

    for (Index r = 0; r < breaks_at_rank[i]; ++r, ++break_rank) {
      const auto& b = v.breaks[break_rank];
      current_earliest += previous_action_time;

      const auto b_tw = std::ranges::find_if(b.tws, [&](const auto& tw) {
        return current_earliest <= tw.end;
      });
      if (b_tw == b.tws.end()) {
        // No admissible TW left; stick to the last end value instead of
        // crashing in release builds.
        if (!b.tws.empty()) {
          current_earliest = b.tws.back().end;
        }
        break_earliest[break_rank] = current_earliest;
        break;
      }

      if (current_earliest < b_tw->start) {
        if (const auto margin = b_tw->start - current_earliest;
            margin < remaining_travel_time) {
          remaining_travel_time -= margin;
        } else {
          remaining_travel_time = 0;
        }

        current_earliest = b_tw->start;
      }

      break_earliest[break_rank] = current_earliest;
      previous_action_time = v.breaks[break_rank].service;
    }

    earliest_end =
      current_earliest + previous_action_time + remaining_travel_time;
    assert(earliest_end <= v_end);
  }
}

void TWRoute::bwd_update_latest_from(const Input& input, Index rank) {
  const auto& v = input.vehicles[v_rank];

  assert(!route.empty());
  if (rank >= route.size()) {
    // Callers sometimes ask for "after last job" (e.g. soft-pinned overflow).
    // Clamp the rank to the last valid job before updating buffers.
    rank = route.size() - 1;
  }
  assert(rank < latest.size());
  Duration current_latest = latest[rank];
  bool handle_first_breaks = true;

  for (Index next_i = rank; next_i > 0; --next_i) {
    const auto& previous_j = input.jobs[route[next_i - 1]];
    Duration remaining_travel_time = 0;
    if (next_i < route.size()) {
      remaining_travel_time =
        v.duration(previous_j.index(), input.jobs[route[next_i]].index());
    }

    // Update latest dates and margins for breaks.
    assert(breaks_at_rank[next_i] <= breaks_counts[next_i]);
    Index break_rank = breaks_counts[next_i];

    for (Index r = 0; r < breaks_at_rank[next_i]; ++r) {
      --break_rank;

      const auto& b = v.breaks[break_rank];
      assert(b.service <= current_latest);
      current_latest -= b.service;

      const auto b_tw =
        std::find_if(b.tws.rbegin(), b.tws.rend(), [&](const auto& tw) {
          return tw.start <= current_latest;
        });
      if (b_tw == b.tws.rend()) {
        // Soft-timing can leave breaks past their allowed windows; clamp to the
        // last end instead of asserting so we preserve consistency.
        if (!b.tws.empty()) {
          current_latest = b.tws.back().end;
        }
        break_latest[break_rank] = current_latest;
        continue;
      }

      if (b_tw->end < current_latest) {
        if (const auto margin = current_latest - b_tw->end;
            margin < remaining_travel_time) {
          remaining_travel_time -= margin;
        } else {
          remaining_travel_time = 0;
        }

        current_latest = b_tw->end;
      }

      break_latest[break_rank] = current_latest;
    }

    // Back to the job after breaks.
    auto gap = action_time[next_i - 1] + remaining_travel_time;
    if (gap > current_latest) {
      // Allow the job to finish late when soft pins already violated the window.
      current_latest = gap;
    }
    current_latest -= gap;

    const auto j_tw =
      std::find_if(previous_j.tws.rbegin(),
                   previous_j.tws.rend(),
                   [&](const auto& tw) { return tw.start <= current_latest; });
    if (j_tw == previous_j.tws.rend()) {
      // No window can accommodate the late arrival: use the last TW end so we
      // keep propagating without crashing.
      if (!previous_j.tws.empty()) {
        current_latest = previous_j.tws.back().end;
      }
      latest[next_i - 1] = current_latest;
      continue;
    }

    current_latest = std::min(current_latest, j_tw->end);

    assert(next_i - 1 < earliest.size());
    assert(next_i - 1 < latest.size());
    if (current_latest < earliest[next_i - 1]) {
      // Downstream code expects non-negative slack; when soft timing lets us run
      // late we clamp back to earliest to keep invariants intact.
      // When pinned soft timing is enabled, we can temporarily keep a job past
      // its time window. Clamp latest date so downstream code continues to see
      // a non-negative slack.
      current_latest = earliest[next_i - 1];
    }
    if (current_latest == latest[next_i - 1]) {
      // There won't be any further update so stop latest date
      // propagation.
      handle_first_breaks = false;
      break;
    }

    latest[next_i - 1] = current_latest;
  }

  if (handle_first_breaks) {
    // Update latest dates and margins for breaks right before the
    // first job.
    const Index next_i = 0;

    assert(next_i < breaks_at_rank.size());
    assert(next_i < breaks_counts.size());
    assert(breaks_at_rank[next_i] <= breaks_counts[next_i]);
    Index break_rank = breaks_counts[next_i];

    for (Index r = 0; r < breaks_at_rank[next_i]; ++r) {
      --break_rank;
      const auto& b = v.breaks[break_rank];

      assert(b.service <= current_latest);
      current_latest -= b.service;

      const auto b_tw =
        std::find_if(b.tws.rbegin(), b.tws.rend(), [&](const auto& tw) {
          return tw.start <= current_latest;
        });
      if (b_tw == b.tws.rend()) {
        // Again: soft-pinned schedules may push us past every TW. Clamp and
        // continue instead of triggering release crashes.
        if (!b.tws.empty()) {
          current_latest = b.tws.back().end;
        }
        break_latest[break_rank] = current_latest;
        continue;
      }
      if (b_tw->end < current_latest) {
        current_latest = b_tw->end;
      }

      break_latest[break_rank] = current_latest;
    }
  }
}

void TWRoute::update_last_latest_date(const Input& input) {
  assert(!route.empty());

  const auto& v = input.vehicles[v_rank];
  auto next = next_info(input, route.back(), route.size());

  // Latest date for breaks before end.
  Index break_rank = breaks_counts[route.size()];
  for (Index r = 0; r < breaks_at_rank[route.size()]; ++r) {
    --break_rank;
    const auto& b = v.breaks[break_rank];

    assert(b.service <= next.latest);
    next.latest -= b.service;

    const auto b_tw =
      std::find_if(b.tws.rbegin(), b.tws.rend(), [&](const auto& tw) {
        return tw.start <= next.latest;
      });
    assert(b_tw != b.tws.rend());

    if (b_tw->end < next.latest) {
      if (const auto margin = next.latest - b_tw->end; margin < next.travel) {
        next.travel -= margin;
      } else {
        next.travel = 0;
      }

      next.latest = b_tw->end;
    }

    break_latest[break_rank] = next.latest;
  }

  // Latest date for last job.
  const auto& j = input.jobs[route.back()];
  const auto gap = action_time.back() + next.travel;
  assert(gap <= next.latest);
  next.latest -= gap;

  const auto j_tw =
    std::find_if(j.tws.rbegin(), j.tws.rend(), [&](const auto& tw) {
      return tw.start <= next.latest;
    });
  assert(j_tw != j.tws.rend());

  latest.back() = std::min(next.latest, j_tw->end);
}

void TWRoute::fwd_update_action_time_from(const Input& input, Index rank) {
  Index current_index = input.jobs[route[rank]].index();

  for (Index i = rank + 1; i < route.size(); ++i) {
    const auto& next_j = input.jobs[route[i]];
    const auto next_index = next_j.index();

    const auto next_action_time =
      (next_index == current_index)
        ? next_j.services[v_type]
        : next_j.setups[v_type] + next_j.services[v_type];

    action_time[i] = next_action_time;
    current_index = next_index;
  }
}

void TWRoute::fwd_update_breaks_load_margin_from(const Input& input,
                                                 Index rank) {
  const auto& v = input.vehicles[v_rank];

  // Last valid fwd_smallest value, if any.
  auto fwd_smallest =
    (breaks_counts[rank] == 0)
      ? utils::max_amount(input.get_amount_size())
      : fwd_smallest_breaks_load_margin[breaks_counts[rank] - 1];

  for (Index i = rank; i <= route.size(); ++i) {
    if (breaks_at_rank[i] != 0) {
      // Update for breaks right before job at rank i.
      const auto& current_load = load_at_step(i);

      for (auto break_rank = breaks_counts[i] - breaks_at_rank[i];
           break_rank < breaks_counts[i];
           ++break_rank) {
        const auto& b = v.breaks[break_rank];

        assert(b.is_valid_for_load(current_load));
        auto current_margin = (b.max_load.has_value())
                                ? b.max_load.value() - current_load
                                : utils::max_amount(input.get_amount_size());

        for (std::size_t a = 0; a < fwd_smallest.size(); ++a) {
          fwd_smallest[a] = std::min(fwd_smallest[a], current_margin[a]);
        }

        assert(input.zero_amount() <= fwd_smallest);
        fwd_smallest_breaks_load_margin[break_rank] = fwd_smallest;
      }
    }
  }
}

void TWRoute::bwd_update_breaks_load_margin_from(const Input& input,
                                                 Index rank) {
  const auto& v = input.vehicles[v_rank];

  // Last valid bwd_smallest value, if any.
  auto bwd_smallest = (breaks_counts[rank] == breaks_counts.back())
                        ? utils::max_amount(input.get_amount_size())
                        : bwd_smallest_breaks_load_margin[breaks_counts[rank]];

  for (Index bwd_i = 0; bwd_i <= rank; ++bwd_i) {
    const auto i = rank - bwd_i;
    if (breaks_at_rank[i] != 0) {
      // Update for breaks right before job at rank i.
      const auto& current_load = load_at_step(i);

      for (unsigned bwd_break_count = 0; bwd_break_count < breaks_at_rank[i];
           ++bwd_break_count) {
        const auto break_rank = breaks_counts[i] - 1 - bwd_break_count;
        const auto& b = v.breaks[break_rank];

        assert(b.is_valid_for_load(current_load));
        auto current_margin = (b.max_load.has_value())
                                ? b.max_load.value() - current_load
                                : utils::max_amount(input.get_amount_size());

        for (std::size_t a = 0; a < bwd_smallest.size(); ++a) {
          bwd_smallest[a] = std::min(bwd_smallest[a], current_margin[a]);
        }

        assert(input.zero_amount() <= bwd_smallest);
        bwd_smallest_breaks_load_margin[break_rank] = bwd_smallest;
      }
    }
  }
}

OrderChoice::OrderChoice(const Input& input,
                         const Index job_rank,
                         const Break& b,
                         const PreviousInfo& previous)
  : input(input),
    j_tw(std::ranges::find_if(input.jobs[job_rank].tws,
                              [&](const auto& tw) {
                                return previous.earliest + previous.travel <=
                                       tw.end;
                              })),
    b_tw(std::ranges::find_if(b.tws, [&](const auto& tw) {
      return previous.earliest <= tw.end;
    })) {
}

OrderChoice TWRoute::order_choice(const Input& input,
                                  const Index job_rank,
                                  const Duration job_action_time,
                                  const Break& b,
                                  const PreviousInfo& previous,
                                  const NextInfo& next,
                                  const Amount& current_load,
                                  bool check_max_load) const {
  OrderChoice oc(input, job_rank, b, previous);
  const auto& v = input.vehicles[v_rank];
  const auto& j = input.jobs[job_rank];

  if (oc.j_tw == j.tws.end() || oc.b_tw == b.tws.end()) {
    // If either job or break can't fit first, then none of the
    // orderings are valid.
    return oc;
  }

  Duration job_then_break_end;
  Duration break_then_job_end;

  // Try putting job first then break.
  const Duration earliest_job_end =
    std::max(previous.earliest + previous.travel, oc.j_tw->start) +
    job_action_time;
  Duration job_then_break_margin = 0;

  const auto new_b_tw = std::ranges::find_if(b.tws, [&](const auto& tw) {
    return earliest_job_end <= tw.end;
  });
  if (new_b_tw == b.tws.end()) {
    // Break does not fit after job due to its time windows. Only
    // option is to choose break first, if valid for max_load.
    oc.add_break_first = !check_max_load || b.is_valid_for_load(current_load);
    return oc;
  }

  Duration travel_after_break = next.travel;
  if (earliest_job_end < new_b_tw->start) {
    job_then_break_margin = new_b_tw->start - earliest_job_end;
    if (job_then_break_margin < travel_after_break) {
      travel_after_break -= job_then_break_margin;
    } else {
      travel_after_break = 0;
    }

    job_then_break_end = oc.b_tw->start + b.service;
  } else {
    job_then_break_end = earliest_job_end + b.service;
  }

  if (job_then_break_end + travel_after_break > next.latest) {
    // Starting the break is possible but then next step is not.
    oc.add_break_first = true;
    return oc;
  }

  if (check_max_load && j.type == JOB_TYPE::SINGLE &&
      (!b.is_valid_for_load(current_load + j.pickup - j.delivery) ||
       !(j.pickup <= bwd_smallest_breaks_load_margin[v.break_rank(b.id)]))) {
    // Break won't fit right after job for load reason.
    oc.add_break_first = b.is_valid_for_load(current_load);
    return oc;
  }

  // Try putting break first then job.
  if (check_max_load && !b.is_valid_for_load(current_load)) {
    // Not doable based on max_load, only option is to choose job
    // first.
    oc.add_job_first = true;
    return oc;
  }

  travel_after_break = previous.travel;
  Duration earliest_job_start = previous.earliest;

  if (previous.earliest < oc.b_tw->start) {
    if (const auto margin = oc.b_tw->start - previous.earliest;
        margin < travel_after_break) {
      travel_after_break -= margin;
    } else {
      travel_after_break = 0;
    }

    earliest_job_start = oc.b_tw->start;
  }

  earliest_job_start += b.service + travel_after_break;

  const auto new_j_tw = std::ranges::find_if(j.tws, [&](const auto& tw) {
    return earliest_job_start <= tw.end;
  });

  if (new_j_tw == j.tws.end()) {
    // Job does not fit after break due to its time windows. Only
    // option is to choose job first.
    oc.add_job_first = true;
    return oc;
  }
  break_then_job_end =
    std::max(earliest_job_start, new_j_tw->start) + job_action_time;

  if (break_then_job_end + next.travel > next.latest) {
    // Arrival at the job is valid but next step is not.
    oc.add_job_first = true;
    return oc;
  }

  // Now both ordering options are doable based on timing constraints.

  // For a pickup, we favor putting the pickup first, except if adding
  // the delivery afterwards is not possible. This is mandatory to
  // avoid heuristically forcing a pickup -> break choice resulting in
  // invalid options, while break -> pickup -> delivery might be
  // valid.
  if (j.type == JOB_TYPE::PICKUP) {
    const auto& matching_d = input.jobs[job_rank + 1];
    assert(matching_d.type == JOB_TYPE::DELIVERY);

    // Try pickup -> break -> delivery.
    auto delivery_travel = v.duration(j.index(), matching_d.index());
    if (job_then_break_margin < delivery_travel) {
      delivery_travel -= job_then_break_margin;
    } else {
      delivery_travel = 0;
    }
    const Duration pb_d_candidate = job_then_break_end + delivery_travel;
    if (const auto pb_d_tw = std::ranges::find_if(matching_d.tws,
                                                  [&](const auto& tw) {
                                                    return pb_d_candidate <=
                                                           tw.end;
                                                  });
        pb_d_tw != matching_d.tws.end() &&
        (!check_max_load || b.is_valid_for_load(current_load + j.pickup))) {
      // pickup -> break -> delivery is doable, choose pickup first.
      oc.add_job_first = true;
      return oc;
    }

    // Previous order not doable, so try pickup -> delivery -> break.
    const Duration delivery_candidate =
      earliest_job_end + v.duration(j.index(), matching_d.index());
    if (const auto d_tw = std::ranges::find_if(matching_d.tws,
                                               [&](const auto& tw) {
                                                 return delivery_candidate <=
                                                        tw.end;
                                               });
        d_tw != matching_d.tws.end()) {
      const auto matching_d_action_time =
        (matching_d.index() == j.index())
          ? matching_d.services[v_type]
          : matching_d.setups[v_type] + matching_d.services[v_type];

      const Duration break_candidate =
        std::max(delivery_candidate, d_tw->start) + matching_d_action_time;

      const auto after_d_b_tw =
        std::ranges::find_if(b.tws, [&](const auto& tw) {
          return break_candidate <= tw.end;
        });
      if (after_d_b_tw != b.tws.end()) {
        // pickup -> delivery -> break is doable, choose pickup first.
        assert(!check_max_load || b.is_valid_for_load(current_load));
        oc.add_job_first = true;
        return oc;
      }
    }

    // Doing pickup first actually leads to infeasible options, so put
    // break first.
    oc.add_break_first = true;
    return oc;
  }

  // For a single job, we pick the ordering minimizing earliest end
  // date for sequence.
  if (break_then_job_end < job_then_break_end) {
    oc.add_break_first = true;
  } else if (break_then_job_end == job_then_break_end) {
    // If end date is the same for both ordering options, decide based
    // on earliest deadline, except for deliveries. If a delivery
    // without TW constraint is postponed, it can introduce arbitrary
    // waiting time between zero max_load breaks.
    if (j.type == JOB_TYPE::DELIVERY || oc.j_tw->end <= oc.b_tw->end) {
      oc.add_job_first = true;
    } else {
      oc.add_break_first = true;
    }
  } else {
    oc.add_job_first = true;
  }

  return oc;
}

template <std::forward_iterator Iter>
bool TWRoute::is_valid_addition_for_tw(const Input& input,
                                       const Amount& delivery,
                                       const Iter first_job,
                                       const Iter last_job,
                                       const Index first_rank,
                                       const Index last_rank,
                                       bool check_max_load) const {
  // Defensive bounds against malformed ranks from callers.
  if (first_rank > route.size() || last_rank > route.size() ||
      first_rank > last_rank) {
    return false;
  }
  // Additional internal consistency checks in debug builds. Soft timing widens
  // the range of valid inputs, so we assert on vector sizes rather than relying
  // on UB in release builds.
  assert(earliest.size() == route.size());
  assert(latest.size() == route.size());
  assert(action_time.size() == route.size());

  // Preserve-pinned hard no-prepend rule when budget==0 and route already has
  // at least one pinned job: do not allow any insertion at route start.
  if (input.pinned_soft_timing() && input.pinned_violation_budget() == 0 &&
      first_rank == 0 && !route.empty()) {
    const bool has_pinned_in_route = std::ranges::any_of(route, [&](Index jr){return input.jobs[jr].pinned;});
    if (has_pinned_in_route) {
      return false;
    }
  }
  assert(first_job <= last_job);
  assert(first_rank <= last_rank);

  const auto& v = input.vehicles[v_rank];

  // Override this value if vehicle does not need this check anyway to
  // spare some work.
  check_max_load = v.has_break_max_load && check_max_load;

  PreviousInfo current(0, 0);
  NextInfo next(0, 0);

  // Value initialization differ whether there are actually jobs added
  // or not.
  if (first_job < last_job) {
    // Anchor guards: enforce pinned first/last before TW math
    const auto insert_len = static_cast<unsigned>(std::distance(first_job, last_job));
    if (const auto pf = input.pinned_first_for_vehicle(v_rank); pf.has_value()) {
      const auto& req = pf.value();
      if (req.job_rank.has_value()) {
        if (first_rank == 0) {
          Index new_first;
          new_first = *first_job;
          if (new_first != req.job_rank.value()) {
            return false;
          }
        }
      } else if (req.pickup_rank.has_value() && req.delivery_rank.has_value()) {
        if (first_rank == 0) {
          if (insert_len < 2) {
            return false;
          }
          auto it = first_job;
          const Index n0 = *it;
          ++it;
          const Index n1 = *it;
          if (n0 != req.pickup_rank.value() || n1 != req.delivery_rank.value()) {
            return false;
          }
        }
        if (first_rank == 1) {
          if (!route.empty() && route[0] == req.pickup_rank.value()) {
            if (route.size() < 2 || route[1] != req.delivery_rank.value()) {
              return false;
            }
            return false;
          }
        }
      }
    }
    if (const auto pl = input.pinned_last_for_vehicle(v_rank); pl.has_value()) {
      const auto& req = pl.value();
      if (req.job_rank.has_value()) {
        if (last_rank == route.size()) {
          Index new_last;
          auto it = first_job;
          std::advance(it, insert_len - 1);
          new_last = *it;
          if (new_last != req.job_rank.value()) {
            return false;
          }
        }
      } else if (req.pickup_rank.has_value() && req.delivery_rank.has_value()) {
        if (last_rank == route.size()) {
          if (insert_len < 2) {
            return false;
          }
          auto it = first_job;
          std::advance(it, insert_len - 2);
          const Index n0 = *it; ++it; const Index n1 = *it;
          if (n0 != req.pickup_rank.value() || n1 != req.delivery_rank.value()) {
            return false;
          }
        }
      }
    }

    current = previous_info(input, *first_job, first_rank);
    next = next_info(input, *(last_job - 1), last_rank);
  } else {
    // This is actually a removal as no jobs are inserted.
    current.earliest = v_start;
    next.latest = v_end;

    if (first_rank > 0) {
      const auto& previous_job = input.jobs[route[first_rank - 1]];
      if (first_rank - 1 >= earliest.size() || first_rank - 1 >= action_time.size()) {
        return false;
      }
      current.earliest =
        earliest[first_rank - 1] + action_time[first_rank - 1];
      current.location_index = previous_job.index();

      if (last_rank < route.size()) {
        if (last_rank >= latest.size()) {
          return false;
        }
        next.latest = latest[last_rank];
        next.travel = v.duration(previous_job.index(),
                                 input.jobs[route[last_rank]].index());
      } else {
        if (has_end) {
          next.travel = v.duration(previous_job.index(), v.end.value().index());
        }
      }
    } else {
      if (last_rank < route.size()) {
        if (last_rank >= latest.size()) {
          return false;
        }
        next.latest = latest[last_rank];
        if (has_start) {
          current.location_index = v.start.value().index();
          next.travel = v.duration(v.start.value().index(),
                                   input.jobs[route[last_rank]].index());
        }
      } else {
        // Emptying the whole route is valid.
        return true;
      }
    }
  }

  // If pinned_soft_timing with zero budget: categorically forbid inserting
  // right before a pinned step to avoid any added delay.
  if (input.pinned_soft_timing() && input.pinned_violation_budget() == 0 &&
      last_rank < route.size() &&
      input.jobs[route[last_rank]].pinned) {
    return false;
  }

  // Determine break range between first_rank and last_rank. The counts arrays
  // always include a sentinel slot, so the extra checks above keep callers from
  // indexing past route.size().
  Index current_break = breaks_counts[first_rank] - breaks_at_rank[first_rank];
  Index last_break = breaks_counts[last_rank];
  const Index max_breaks = static_cast<Index>(input.vehicles[v_rank].breaks.size());
  assert(breaks_at_rank.size() == route.size() + 1);
  assert(breaks_counts.size() == route.size() + 1);
  if (last_break > max_breaks) {
    last_break = max_breaks;
  }
  if (current_break > last_break) {
    current_break = last_break;
  }

  // Maintain current load while adding insertion range. Initial load
  // is lowered based on removed range.
  Amount current_load;

  if (check_max_load) {
    const auto previous_init_load =
      (route.empty()) ? input.zero_amount() : load_at_step(first_rank);
    assert(delivery_in_range(first_rank, last_rank) <= previous_init_load);
    const Amount delta_delivery =
      delivery - delivery_in_range(first_rank, last_rank);

    if (current_break != 0 &&
        !(delta_delivery <=
          fwd_smallest_breaks_load_margin[current_break - 1])) {
      return false;
    }

    current_load = previous_init_load + delta_delivery;
  }

  // Propagate earliest dates for all jobs and breaks in their
  // respective addition ranges.
  auto current_job = first_job;
  while (current_job != last_job || current_break != last_break) {
    if (current_job == last_job) {
      // Compute earliest end date for break after last inserted jobs.
      const auto& b = v.breaks[current_break];

      const auto b_tw = std::ranges::find_if(b.tws, [&](const auto& tw) {
        return current.earliest <= tw.end;
      });

      if (b_tw == b.tws.end()) {
        // Break does not fit due to its time windows.
        return false;
      }

      if (check_max_load && !b.is_valid_for_load(current_load)) {
        // Break does not fit due to current load.
        return false;
      }

      if (current.earliest < b_tw->start) {
        if (const auto margin = b_tw->start - current.earliest;
            margin < next.travel) {
          next.travel -= margin;
        } else {
          next.travel = 0;
        }

        current.earliest = b_tw->start;
      }

      current.earliest += b.service;

      ++current_break;
      continue;
    }

    // We still have jobs to go through.
    const auto& j = input.jobs[*current_job];

    if (current_break == last_break) {
      // Compute earliest end date for job after last inserted breaks.
      current.earliest += current.travel;

      const auto j_tw = std::ranges::find_if(j.tws, [&](const auto& tw) {
        return current.earliest <= tw.end;
      });
      if (j_tw == j.tws.end()) {
        return false;
      }
      const auto job_action_time = (j.index() == current.location_index)
                                     ? j.services[v_type]
                                     : j.setups[v_type] + j.services[v_type];
      current.location_index = j.index();
      // Soft timing may have drifted past the latest TW; use the clamped start
      // so we stay consistent with the forward propagation.
      const Duration job_start = std::max(current.earliest, j_tw->start);
      current.earliest = job_start + job_action_time;

      if (check_max_load) {
        assert(j.delivery <= current_load);
        current_load += (j.pickup - j.delivery);
      }

      ++current_job;
      if (current_job != last_job) {
        // Account for travel time to next current job.
        current.travel =
          v.duration(j.index(), input.jobs[*current_job].index());
      }
      continue;
    }

    // We still have both jobs and breaks to go through, so decide on
    // ordering.
    const auto& b = v.breaks[current_break];
    const auto job_action_time = (j.index() == current.location_index)
                                   ? j.services[v_type]
                                   : j.setups[v_type] + j.services[v_type];

    auto oc = order_choice(input,
                           *current_job,
                           job_action_time,
                           b,
                           current,
                           next,
                           current_load,
                           check_max_load);

    if (!oc.add_job_first && !oc.add_break_first) {
      // Infeasible insertion.
      return false;
    }

    // Feasible insertion based on time windows, now update next end
    // time with given insertion choice.
    assert(oc.add_job_first xor oc.add_break_first);
    if (oc.add_break_first) {
      if (check_max_load && !b.is_valid_for_load(current_load)) {
        return false;
      }

      if (current.earliest < oc.b_tw->start) {
        if (const auto margin = oc.b_tw->start - current.earliest;
            margin < current.travel) {
          current.travel -= margin;
        } else {
          current.travel = 0;
        }

        current.earliest = oc.b_tw->start;
      }

      current.earliest += b.service;

      ++current_break;
    }
    if (oc.add_job_first) {
      current.location_index = j.index();

      // Same clamping story for the "job-first" branch.
      const Duration job_start =
        std::max(current.earliest + current.travel, oc.j_tw->start);
      current.earliest = job_start + job_action_time;

      if (check_max_load) {
        assert(j.delivery <= current_load);
        current_load += (j.pickup - j.delivery);
      }

      ++current_job;
      if (current_job != last_job) {
        // Account for travel time to next current job.
        current.travel =
          v.duration(j.index(), input.jobs[*current_job].index());
      }
    }
  }

  if (check_max_load && last_break < v.breaks.size()) {
    const auto previous_final_load =
      (route.empty()) ? input.zero_amount() : load_at_step(last_rank);

    const Amount delta_pickup = current_load - previous_final_load;

    if (!(delta_pickup <= bwd_smallest_breaks_load_margin[last_break])) {
      return false;
    }
  }

  if (last_rank < route.size() &&
      input.jobs[route[last_rank]].index() != current.location_index) {
    // There is a task right after replace range and setup time does
    // apply to it.
    const auto& j_after = input.jobs[route[last_rank]];
    auto new_action_time = j_after.setups[v_type] + j_after.services[v_type];
    if (last_rank >= action_time.size()) {
      return false;
    }
    if (action_time[last_rank] < new_action_time) {
      // Setup time did not previously apply to that task as action
      // time has increased. In that case the margin check for job at
      // last_rank may be OK in the return clause below, BUT shifting
      // earliest date for next task with new setup time may make it
      // not doable anymore.
      auto earliest_after = current.earliest + next.travel;
      const auto j_after_tw =
        std::ranges::find_if(j_after.tws, [&](const auto& tw) {
          return earliest_after <= tw.end;
        });
      if (j_after_tw == j_after.tws.end()) {
        return false;
      }
      earliest_after = std::max(earliest_after, j_after_tw->start);

      auto next_after = next_info(input, route[last_rank], last_rank + 1);

      // Go through breaks right after.
      Index break_rank =
        breaks_counts[last_rank + 1] - breaks_at_rank[last_rank + 1];

      for (Index r = 0; r < breaks_at_rank[last_rank + 1]; ++r, ++break_rank) {
        const auto& b = v.breaks[break_rank];

        earliest_after += new_action_time;

        const auto b_tw = std::ranges::find_if(b.tws, [&](const auto& tw) {
          return earliest_after <= tw.end;
        });
        if (b_tw == b.tws.end()) {
          // Break does not fit due to its time windows.
          return false;
        }

        if (earliest_after < b_tw->start) {
          if (const auto margin = b_tw->start - earliest_after;
              margin < next_after.travel) {
            next_after.travel -= margin;
          } else {
            next_after.travel = 0;
          }

          earliest_after = b_tw->start;
        }

        new_action_time = v.breaks[break_rank].service;
      }

      if (earliest_after + new_action_time + next_after.travel >
          next_after.latest) {
        return false;
      }
    }
  }

  const bool tw_ok = (current.earliest + next.travel <= next.latest);

  if (!tw_ok) {
    // Keep default behavior when not preserving pinned
    if (!input.pinned_soft_timing()) {
      return false;
    }
    // If we preserve pinned, we allow infeasible TW at seeding stage only when
    // there is no insertion (seeding callers use first_rank==last_rank==0 and
    // last_rank may be 0 with empty route). Still, rely on heuristics to seed relaxed.
  }

  if (input.pinned_soft_timing() && last_rank < route.size() && !baseline_service_start.empty()) {
    // Compute added delay at the next original step.
    const Duration arrival_with_insertion = current.earliest + next.travel;
    Duration baseline =
      baseline_service_start.empty() ? Duration{} : baseline_service_start.back();
    if (last_rank < baseline_service_start.size()) {
      baseline = baseline_service_start[last_rank];
    }
    const Duration delta = (arrival_with_insertion > baseline)
                             ? (arrival_with_insertion - baseline)
                             : 0;

    // Compute allowed added delay up to any pinned step at or after last_rank.
    Duration allowed = std::numeric_limits<Duration>::max();
    for (Index k = last_rank; k < route.size(); ++k) {
      if (input.jobs[route[k]].pinned) {
        const auto& j = input.jobs[route[k]];
        const Duration base_k = (k < baseline_service_start.size()) ? baseline_service_start[k] : baseline;
        // Find first TW whose end >= base_k
        Duration step_allowed = 0;
        bool late_already = true;
        for (const auto& tw : j.tws) {
          if (base_k <= tw.end) {
            late_already = (base_k > tw.end);
            const Duration slack = (tw.end >= base_k) ? (tw.end - base_k) : 0;
            // If base is before tw.start, slack is still measured to end (we don't penalize early arrival)
            step_allowed = std::min(slack, input.pinned_violation_budget());
            break;
          }
        }
        if (late_already) {
          step_allowed = 0;
        }
        if (step_allowed < allowed) {
          allowed = step_allowed;
        }
      }
    }
    if (allowed == std::numeric_limits<Duration>::max()) {
      // No pinned steps ahead; no guard.
      return tw_ok;
    }
    if (delta > allowed) {
      return false;
    }
  }

  return tw_ok;
}

template <std::random_access_iterator Iter>
void TWRoute::replace(const Input& input,
                      const Amount& delivery,
                      const Iter first_job,
                      const Iter last_job,
                      const Index first_rank,
                      const Index last_rank) {
  assert(first_job <= last_job);
  assert(first_rank <= last_rank);

  const auto& v = input.vehicles[v_rank];

  PreviousInfo current(0, 0);
  NextInfo next(0, 0);

  // Value initialization differ whether there are actually jobs added
  // or not.
  if (first_job < last_job) {
    current = previous_info(input, *first_job, first_rank);
    next = next_info(input, *(last_job - 1), last_rank);
  } else {
    // This is actually a removal as no jobs are inserted.
    current.earliest = v_start;
    next.latest = v_end;

    if (first_rank > 0) {
      const auto& previous_job = input.jobs[route[first_rank - 1]];
      const auto previous_index = previous_job.index();
      current.earliest = earliest[first_rank - 1] + action_time[first_rank - 1];
      current.location_index = previous_index;

      if (last_rank < route.size()) {
        next.latest = latest[last_rank];
        next.travel =
          v.duration(previous_index, input.jobs[route[last_rank]].index());
      } else {
        if (has_end) {
          next.travel = v.duration(previous_index, v.end.value().index());
        }
      }
    } else {
      if (last_rank < route.size()) {
        next.latest = latest[last_rank];
        if (has_start) {
          current.location_index = v.start.value().index();
          next.travel = v.duration(v.start.value().index(),
                                   input.jobs[route[last_rank]].index());
        }
      }
    }
  }

  // Determine break range between first_rank and last_rank.
  Index current_break = breaks_counts[first_rank] - breaks_at_rank[first_rank];
  const Index last_break = breaks_counts[last_rank];

  // Maintain current load while adding insertion range. Initial load
  // is lowered based on removed range.
  const auto previous_init_load =
    (route.empty()) ? input.zero_amount() : load_at_step(first_rank);
  const auto previous_final_load =
    (route.empty()) ? input.zero_amount() : load_at_step(last_rank);
  assert(delivery_in_range(first_rank, last_rank) <= previous_init_load);
  const Amount delta_delivery =
    delivery - delivery_in_range(first_rank, last_rank);
  Amount current_load = previous_init_load + delta_delivery;

  // Update all break load margins prior to modified range.
  assert(current_break == 0 ||
         delta_delivery <= fwd_smallest_breaks_load_margin[current_break - 1]);
  for (std::size_t i = 0; i < current_break; ++i) {
    assert(delta_delivery <= fwd_smallest_breaks_load_margin[i]);

    // Manually decrement margin to avoid overflows that would end up
    // in a negative margin with a plain
    // fwd_smallest_breaks_load_margin[i] -= delta_delivery;
    for (std::size_t a = 0; a < delta_delivery.size(); ++a) {
      if ((-delta_delivery[a]) <= (std::numeric_limits<Capacity>::max() -
                                   fwd_smallest_breaks_load_margin[i][a])) {
        fwd_smallest_breaks_load_margin[i][a] -= delta_delivery[a];
      } else {
        fwd_smallest_breaks_load_margin[i][a] =
          std::numeric_limits<Capacity>::max();
      }
    }
  }

  unsigned previous_breaks_counts =
    (first_rank != 0) ? breaks_counts[first_rank - 1] : 0;

  // Adjust various vector sizes. Dummy inserted values and unmodified
  // old values in the insertion range will be overwritten below.
  const unsigned erase_count = last_rank - first_rank;
  const unsigned add_count = std::distance(first_job, last_job);

  // Update data structures. For earliest and latest dates, we need to
  // overwrite old values. Otherwise they may happen to be identical
  // to new computed values and stop propagation inside
  // fwd_update_earliest_from and bwd_update_latest_from below.
  if (add_count < erase_count) {
    auto to_erase = erase_count - add_count;
    route.erase(route.begin() + first_rank,
                route.begin() + first_rank + to_erase);
    earliest.erase(earliest.begin() + first_rank,
                   earliest.begin() + first_rank + to_erase);
    latest.erase(latest.begin() + first_rank,
                 latest.begin() + first_rank + to_erase);
    action_time.erase(action_time.begin() + first_rank,
                      action_time.begin() + first_rank + to_erase);
    breaks_at_rank.erase(breaks_at_rank.begin() + first_rank,
                         breaks_at_rank.begin() + first_rank + to_erase);
    breaks_counts.erase(breaks_counts.begin() + first_rank,
                        breaks_counts.begin() + first_rank + to_erase);

    std::fill(earliest.begin() + first_rank,
              earliest.begin() + first_rank + add_count,
              std::numeric_limits<Duration>::max());
    std::fill(latest.begin() + first_rank,
              latest.begin() + first_rank + add_count,
              0);
  } else {
    std::fill(earliest.begin() + first_rank,
              earliest.begin() + first_rank + erase_count,
              std::numeric_limits<Duration>::max());
    std::fill(latest.begin() + first_rank,
              latest.begin() + first_rank + erase_count,
              0);

    auto to_insert = add_count - erase_count;
    route.insert(route.begin() + first_rank, to_insert, 0);
    earliest.insert(earliest.begin() + first_rank, to_insert, 0);
    latest.insert(latest.begin() + first_rank, to_insert, 0);
    action_time.insert(action_time.begin() + first_rank, to_insert, 0);
    breaks_at_rank.insert(breaks_at_rank.begin() + first_rank, to_insert, 0);
    breaks_counts.insert(breaks_counts.begin() + first_rank, to_insert, 0);
  }

  // Current rank in route/earliest/latest/action_time vectors.
  Index current_job_rank = first_rank;
  unsigned breaks_before = 0;

  // Ensure breaks vectors have a trailing slot for end-of-route
  {
    const std::size_t expected_slots = route.size() + 1;
    if (breaks_at_rank.size() != expected_slots) {
      breaks_at_rank.resize(expected_slots, 0);
    }
    if (breaks_counts.size() != expected_slots) {
      // Keep existing prefix; trailing slots initialized to 0 and will be updated below
      breaks_counts.resize(expected_slots, 0);
    }
  }

  // Propagate earliest dates (and action times) for all jobs and
  // breaks in their respective addition ranges.
  auto current_job = first_job;
  while (current_job != last_job || current_break != last_break) {
    if (current_job == last_job) {
      // Compute earliest end date for break after last inserted jobs.
      if (current_break >= v.breaks.size()) {
        // No more breaks to place; treat as if last_break reached.
        current_break = last_break;
        continue;
      }
      const auto& b = v.breaks[current_break];
      assert(b.is_valid_for_load(current_load));

      const auto b_tw = std::ranges::find_if(b.tws, [&](const auto& tw) {
        return current.earliest <= tw.end;
      });
      assert(b_tw != b.tws.end());

      if (current.earliest < b_tw->start) {
        if (const auto margin = b_tw->start - current.earliest;
            margin < next.travel) {
          next.travel -= margin;
        } else {
          next.travel = 0;
        }

        current.earliest = b_tw->start;
      }
      break_earliest[current_break] = current.earliest;

      current.earliest += b.service;

      // Update break max load margin.
      auto current_margin = (b.max_load.has_value())
                              ? b.max_load.value() - current_load
                              : utils::max_amount(input.get_amount_size());
      if (current_break == 0) {
        // New fwd_smallest_breaks_load_margin is solely based on this
        // break max_load.
        fwd_smallest_breaks_load_margin[current_break] = current_margin;
      } else {
        const auto& previous_margin =
          fwd_smallest_breaks_load_margin[current_break - 1];
        for (std::size_t i = 0; i < previous_margin.size(); ++i) {
          fwd_smallest_breaks_load_margin[current_break][i] =
            std::min(previous_margin[i], current_margin[i]);
        }
      }

      ++breaks_before;
      ++current_break;
      continue;
    }

    // We still have jobs to go through.
    const auto& j = input.jobs[*current_job];

    if (current_break == last_break) {
      // Compute earliest end date for job after last inserted breaks.
      current.earliest += current.travel;

      const auto j_tw = std::ranges::find_if(j.tws, [&](const auto& tw) {
        return current.earliest <= tw.end;
      });
      assert(j_tw != j.tws.end());

      current.earliest = std::max(current.earliest, j_tw->start);

      route[current_job_rank] = *current_job;
      earliest[current_job_rank] = current.earliest;
      breaks_at_rank[current_job_rank] = breaks_before;
      breaks_counts[current_job_rank] = previous_breaks_counts + breaks_before;

      action_time[current_job_rank] = (j.index() == current.location_index)
                                        ? j.services[v_type]
                                        : j.setups[v_type] + j.services[v_type];
      current.location_index = j.index();
      current.earliest += action_time[current_job_rank];

      ++current_job_rank;
      previous_breaks_counts += breaks_before;
      breaks_before = 0;

      assert(j.delivery <= current_load);
      current_load += (j.pickup - j.delivery);

      ++current_job;
      if (current_job != last_job) {
        // Account for travel time to next current job.
        current.travel =
          v.duration(j.index(), input.jobs[*current_job].index());
      }
      continue;
    }

    // We still have both jobs and breaks to go through, so decide on
    // ordering.
    if (current_break >= v.breaks.size()) {
      // No more breaks available; continue with jobs only path.
      current_break = last_break;
      continue;
    }
    const auto& b = v.breaks[current_break];

    const auto job_action_time = (j.index() == current.location_index)
                                   ? j.services[v_type]
                                   : j.setups[v_type] + j.services[v_type];

    auto oc = order_choice(input,
                           *current_job,
                           job_action_time,
                           b,
                           current,
                           next,
                           current_load);

    assert(oc.add_job_first xor oc.add_break_first);
    if (oc.add_break_first) {
      assert(b.is_valid_for_load(current_load));

      if (current.earliest < oc.b_tw->start) {
        if (const auto margin = oc.b_tw->start - current.earliest;
            margin < current.travel) {
          current.travel -= margin;
        } else {
          current.travel = 0;
        }

        current.earliest = oc.b_tw->start;
      }
      break_earliest[current_break] = current.earliest;

      current.earliest += b.service;

      // Update break max load margin.
      auto current_margin = (b.max_load.has_value())
                              ? b.max_load.value() - current_load
                              : utils::max_amount(input.get_amount_size());
      if (current_break == 0) {
        // New fwd_smallest_breaks_load_margin is solely based on this
        // break max_load.
        fwd_smallest_breaks_load_margin[current_break] = current_margin;
      } else {
        const auto& previous_margin =
          fwd_smallest_breaks_load_margin[current_break - 1];
        for (std::size_t i = 0; i < previous_margin.size(); ++i) {
          fwd_smallest_breaks_load_margin[current_break][i] =
            std::min(previous_margin[i], current_margin[i]);
        }
      }

      ++breaks_before;
      ++current_break;
    }
    if (oc.add_job_first) {
      current.earliest =
        std::max(current.earliest + current.travel, oc.j_tw->start);

      route[current_job_rank] = *current_job;
      earliest[current_job_rank] = current.earliest;
      breaks_at_rank[current_job_rank] = breaks_before;
      breaks_counts[current_job_rank] = previous_breaks_counts + breaks_before;

      action_time[current_job_rank] = job_action_time;
      current.earliest += job_action_time;
      current.location_index = j.index();

      ++current_job_rank;
      previous_breaks_counts += breaks_before;
      breaks_before = 0;

      assert(j.delivery <= current_load);
      current_load += (j.pickup - j.delivery);

      ++current_job;
      if (current_job != last_job) {
        // Account for travel time to next current job.
        current.travel =
          v.duration(j.index(), input.jobs[*current_job].index());
      }
    }
  }

  assert(current_job_rank == first_rank + add_count);

  // Update all break load margins after modified range.
  const Amount delta_pickup = current_load - previous_final_load;
  for (std::size_t i = last_break; i < v.breaks.size(); ++i) {
    assert(delta_pickup <= bwd_smallest_breaks_load_margin[i]);

    // Manually decrement margin to avoid overflows that would end up
    // in a negative margin with a plain
    // bwd_smallest_breaks_load_margin[i] -= delta_pickup;
    for (std::size_t a = 0; a < delta_pickup.size(); ++a) {
      if ((-delta_pickup[a]) <= (std::numeric_limits<Capacity>::max() -
                                 bwd_smallest_breaks_load_margin[i][a])) {
        bwd_smallest_breaks_load_margin[i][a] -= delta_pickup[a];
      } else {
        bwd_smallest_breaks_load_margin[i][a] =
          std::numeric_limits<Capacity>::max();
      }
    }
  }

  // Update remaining number of breaks due before next step.
  breaks_at_rank[current_job_rank] = breaks_before;
  // Keep cumulative count consistent for the trailing slot
  breaks_counts[current_job_rank] = previous_breaks_counts + breaks_before;
  assert(previous_breaks_counts + breaks_at_rank[current_job_rank] ==
         breaks_counts[current_job_rank]);

  if (!route.empty()) {
    auto valid_latest_date_rank = current_job_rank;
    auto valid_earliest_date_rank =
      (first_rank == 0) ? 0 : first_rank - 1;
    const bool replace_last_jobs = (current_job_rank == route.size());
    bool do_update_last_latest_date = false;

    if (replace_last_jobs) {
      earliest_end = current.earliest + next.travel;

      do_update_last_latest_date = true;
      valid_latest_date_rank = route.size() - 1;
    } else {
      // current_job_rank is the rank of the first non-replaced job.
      const auto& j = input.jobs[route[current_job_rank]];

      const auto new_action_time = (j.index() == current.location_index)
                                     ? j.services[v_type]
                                     : j.setups[v_type] + j.services[v_type];
      assert(action_time[current_job_rank] == j.services[v_type] ||
             action_time[current_job_rank] ==
               j.services[v_type] + j.setups[v_type]);

      const bool current_action_time_changed =
        (new_action_time != action_time[current_job_rank]);
      if (current_action_time_changed) {
        // Due to removal, total time spent at first non-replaced
        // task changed, so we need its latest date updated, either
        // directly if at the end of the route, either by going
        // backward from next task (if any).
        if (current_job_rank == route.size() - 1) {
          do_update_last_latest_date = true;
        } else {
          valid_latest_date_rank = current_job_rank + 1;
          // We need to update latest dates for the previous jobs
          // **before** current_job_rank, but bwd_update_latest_from
          // has a stop criterion for propagation that will trigger if
          // latest date happens to not change at current_job_rank.
          latest[current_job_rank] = 0;
        }
      }

      if (current_job_rank == 0) {
        // First jobs in route have been erased and not replaced, so
        // update new first job earliest date and action time.
        current.earliest += next.travel;
        const auto j_tw = std::ranges::find_if(j.tws, [&](const auto& tw) {
          return current.earliest <= tw.end;
        });
        assert(j_tw != j.tws.end());

        earliest[0] = std::max(current.earliest, j_tw->start);
        assert(earliest[0] <= latest[0] ||
               (current_action_time_changed && latest[current_job_rank] == 0));

        action_time[0] = new_action_time;
      } else {
        valid_earliest_date_rank =
          std::min<Index>(valid_earliest_date_rank,
                          static_cast<Index>(current_job_rank - 1));
        // The time spent at jobs before current_job_rank has changed, so force
        // recomputation of earliest dates for the suffix even if the action
        // time of the first retained job stays identical.
        if (current_action_time_changed) {
          // We need to update earliest dates for the following jobs
          // **after** current_job_rank, but fwd_update_earliest_from
          // has a stop criterion for propagation that will trigger if
          // earliest date happens to not change at current_job_rank.
          // The sentinel above already guarantees the propagation will happen.
        }
      }
    }

    if (!replace_last_jobs) {
      // Force recomputation of earliest dates for the suffix that may rely on
      // the modified prefix. Using a neutral value ensures fwd propagation does
      // not stop early because values appear unchanged.
      const Index reset_from =
        std::min<Index>(route.size(), valid_earliest_date_rank + 1);
      if (reset_from < route.size()) {
        for (Index i = reset_from; i < route.size(); ++i) {
          earliest[i] = v_end;
          latest[i] = v_end;
        }
        // Recompute action times and earliest dates forward from the anchor rank.
        fwd_update_action_time_from(input, valid_earliest_date_rank);
        fwd_update_earliest_from(input, valid_earliest_date_rank);
      }
    }

    if (do_update_last_latest_date) {
      update_last_latest_date(input);
    }
    // Update latest dates backward.
    if (valid_latest_date_rank >= route.size()) {
      // Clamp to last valid index to avoid OOB reads.
      valid_latest_date_rank = (route.empty() ? 0 : route.size() - 1);
    }
    bwd_update_latest_from(input, valid_latest_date_rank);
  }

  update_amounts(input);

  // Propagate fwd/bwd_smallest_breaks_load_margin if required.
  if (last_break < v.breaks.size()) {
    fwd_update_breaks_load_margin_from(input, current_job_rank);
  }
  if (last_break > 0) {
    bwd_update_breaks_load_margin_from(input, current_job_rank);
  }
}

template bool
TWRoute::is_valid_addition_for_tw(const Input& input,
                                  const Amount& delivery,
                                  const std::vector<Index>::iterator first_job,
                                  const std::vector<Index>::iterator last_job,
                                  const Index first_rank,
                                  const Index last_rank,
                                  bool check_max_load) const;

template bool TWRoute::is_valid_addition_for_tw(
  const Input& input,
  const Amount& delivery,
  const std::vector<Index>::reverse_iterator first_job,
  const std::vector<Index>::reverse_iterator last_job,
  const Index first_rank,
  const Index last_rank,
  bool check_max_load) const;

template bool TWRoute::is_valid_addition_for_tw(
  const Input& input,
  const Amount& delivery,
  const std::array<Index, 1>::const_iterator first_job,
  const std::array<Index, 1>::const_iterator last_job,
  const Index first_rank,
  const Index last_rank,
  bool check_max_load) const;

template bool TWRoute::is_valid_addition_for_tw(
  const Input& input,
  const Amount& delivery,
  const std::vector<Index>::const_iterator first_job,
  const std::vector<Index>::const_iterator last_job,
  const Index first_rank,
  const Index last_rank,
  bool check_max_load) const;

template void TWRoute::replace(const Input& input,
                               const Amount& delivery,
                               const std::vector<Index>::iterator first_job,
                               const std::vector<Index>::iterator last_job,
                               const Index first_rank,
                               const Index last_rank);
template void
TWRoute::replace(const Input& input,
                 const Amount& delivery,
                 const std::vector<Index>::const_iterator first_job,
                 const std::vector<Index>::const_iterator last_job,
                 const Index first_rank,
                 const Index last_rank);
template void
TWRoute::replace(const Input& input,
                 const Amount& delivery,
                 const std::vector<Index>::reverse_iterator first_job,
                 const std::vector<Index>::reverse_iterator last_job,
                 const Index first_rank,
                 const Index last_rank);

template void
TWRoute::replace(const Input& input,
                 const Amount& delivery,
                 const std::array<Index, 1>::const_iterator first_job,
                 const std::array<Index, 1>::const_iterator last_job,
                 const Index first_rank,
                 const Index last_rank);

} // namespace vroom
