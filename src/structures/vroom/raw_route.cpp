/*

This file is part of VROOM.

Copyright (c) 2015-2025, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include "structures/vroom/raw_route.h"

namespace vroom {

RawRoute::RawRoute(const Input& input, Index i, unsigned amount_size)
  : _zero(amount_size),
    _fwd_peaks(2, _zero),
    _bwd_peaks(2, _zero),
    _delivery_margin(input.vehicles[i].capacity),
    _pickup_margin(input.vehicles[i].capacity),
    v_rank(i),
    v_type(input.vehicles[i].type),
    has_start(input.vehicles[i].has_start()),
    has_end(input.vehicles[i].has_end()),
    capacity(input.vehicles[i].capacity) {
}

void RawRoute::set_route(const Input& input, const std::vector<Index>& r) {
  route = r;
  update_amounts(input);
}

void RawRoute::update_amounts(const Input& input) {
  auto step_size = route.size() + 2;
  _fwd_pickups.resize(route.size());
  _fwd_deliveries.resize(route.size());
  _bwd_deliveries.resize(route.size());
  _bwd_pickups.resize(route.size());
  _pd_loads.resize(route.size());
  _nb_pickups.resize(route.size());
  _nb_deliveries.resize(route.size());

  _current_loads.resize(step_size);
  _fwd_peaks.resize(step_size);
  _bwd_peaks.resize(step_size);

  if (route.empty()) {
    // So that check in is_valid_addition_for_capacity is consistent
    // with empty routes.
    std::ranges::fill(_fwd_peaks, _zero);
    std::ranges::fill(_bwd_peaks, _zero);
    // So that check against break max_load and margins computations
    // are consistent with empty routes.
    std::ranges::fill(_current_loads, _zero);
    return;
  }

  Amount current_pickups(_zero);
  Amount current_deliveries(_zero);
  Amount current_pd_load(_zero);
  unsigned current_nb_pickups = 0;
  unsigned current_nb_deliveries = 0;

  for (std::size_t i = 0; i < route.size(); ++i) {
    switch (const auto& job = input.jobs[route[i]]; job.type) {
      using enum JOB_TYPE;
    case SINGLE:
      current_pickups += job.pickup;
      current_deliveries += job.delivery;
      break;
    case PICKUP:
      current_pd_load += job.pickup;
      current_nb_pickups += 1;
      break;
    case DELIVERY:
      assert(job.delivery <= current_pd_load);
      current_pd_load -= job.delivery;
      current_nb_deliveries += 1;
      break;
    }
    _fwd_pickups[i] = current_pickups;
    _fwd_deliveries[i] = current_deliveries;
    _pd_loads[i] = current_pd_load;
    assert(current_nb_deliveries <= current_nb_pickups);
    _nb_pickups[i] = current_nb_pickups;
    _nb_deliveries[i] = current_nb_deliveries;
  }
  assert(_pd_loads.back() == _zero);

  current_deliveries = _zero;
  current_pickups = _zero;

  _current_loads.back() = _fwd_pickups.back();
  assert(_current_loads.back() <= capacity);

  for (std::size_t i = 0; i < route.size(); ++i) {
    auto bwd_i = route.size() - i - 1;

    _bwd_deliveries[bwd_i] = current_deliveries;
    _bwd_pickups[bwd_i] = current_pickups;
    _current_loads[bwd_i + 1] =
      _fwd_pickups[bwd_i] + _pd_loads[bwd_i] + current_deliveries;
    assert(_current_loads[bwd_i + 1] <= capacity);
    const auto& job = input.jobs[route[bwd_i]];
    if (job.type == JOB_TYPE::SINGLE) {
      current_deliveries += job.delivery;
      current_pickups += job.pickup;
    }
  }
  _current_loads[0] = current_deliveries;
  assert(_current_loads[0] <= capacity);

  auto peak = _current_loads[0];
  _fwd_peaks[0] = peak;
  for (std::size_t s = 1; s < _fwd_peaks.size(); ++s) {
    // Handle max component-wise.
    for (std::size_t r = 0; r < _zero.size(); ++r) {
      peak[r] = std::max(peak[r], _current_loads[s][r]);
    }
    _fwd_peaks[s] = peak;
  }

  peak = _current_loads.back();
  _bwd_peaks.back() = peak;
  for (std::size_t s = 1; s < _bwd_peaks.size(); ++s) {
    auto bwd_s = _bwd_peaks.size() - s - 1;
    // Handle max component-wise.
    for (std::size_t r = 0; r < _zero.size(); ++r) {
      peak[r] = std::max(peak[r], _current_loads[bwd_s][r]);
    }
    _bwd_peaks[bwd_s] = peak;
  }

  if (route.empty()) {
    _delivery_margin = capacity;
    _pickup_margin = capacity;
  } else {
    assert(!_fwd_pickups.empty());
    const auto& pickups_sum = _fwd_pickups.back();

    for (unsigned i = 0; i < _zero.size(); ++i) {
      _delivery_margin[i] = capacity[i] - _current_loads[0][i];
      _pickup_margin[i] = capacity[i] - pickups_sum[i];
    }
  }
}

bool RawRoute::has_pending_delivery_after_rank(const Index rank) const {
  return _nb_deliveries[rank] < _nb_pickups[rank];
}

bool RawRoute::has_delivery_after_rank(const Index rank) const {
  assert(rank < _nb_deliveries.size());
  return _nb_deliveries[rank] < _nb_deliveries.back();
}

bool RawRoute::has_pickup_up_to_rank(const Index rank) const {
  assert(rank < _nb_pickups.size());
  return 0 < _nb_pickups[rank];
}

bool RawRoute::is_valid_addition_for_capacity(const Input&,
                                              const Amount& pickup,
                                              const Amount& delivery,
                                              const Index rank) const {
  assert(rank <= route.size());

  return (_fwd_peaks[rank] + delivery <= capacity) &&
         (_bwd_peaks[rank] + pickup <= capacity);
}

bool RawRoute::is_valid_addition_for_load(const Input&,
                                          const Amount& pickup,
                                          const Index rank) const {
  assert(rank <= route.size());

  const auto& load = route.empty() ? _zero : _current_loads[rank];
  return load + pickup <= capacity;
}

bool RawRoute::is_valid_addition_for_capacity_margins(
  const Input&,
  const Amount& pickup,
  const Amount& delivery,
  const Index first_rank,
  const Index last_rank) const {
  assert(1 <= last_rank);
  assert(last_rank <= route.size() + 1);

  const auto& first_deliveries =
    (first_rank == 0) ? _current_loads[0] : _bwd_deliveries[first_rank - 1];

  const auto& first_pickups =
    (first_rank == 0) ? _zero : _fwd_pickups[first_rank - 1];

  auto replaced_deliveries = first_deliveries - _bwd_deliveries[last_rank - 1];

  return (_fwd_peaks[first_rank] + delivery <=
          capacity + replaced_deliveries) &&
         (_bwd_peaks[last_rank] + pickup <=
          capacity + _fwd_pickups[last_rank - 1] - first_pickups);
}

template <std::forward_iterator Iter>
bool RawRoute::is_valid_addition_for_capacity_inclusion(
  const Input& input,
  Amount delivery,
  const Iter first_job,
  const Iter last_job,
  const Index first_rank,
  const Index last_rank) const {
  assert(first_rank <= last_rank);
  assert(last_rank <= route.size() + 1);

  // Enforce pinned first/last boundary constraints regardless of capacity
  const auto v = v_rank;
  const auto insert_len = static_cast<unsigned>(std::distance(first_job, last_job));

  if (const auto pf = input.pinned_first_for_vehicle(v); pf.has_value()) {
    const auto& req = pf.value();
    if (req.job_rank.has_value()) {
      if (first_rank == 0) {
        Index new_first;
        if (insert_len > 0) {
          new_first = *first_job;
        } else {
          if (last_rank < route.size()) {
            new_first = route[last_rank];
          } else {
            return false;
          }
        }
        if (new_first != req.job_rank.value()) {
          return false;
        }
      }
    } else if (req.pickup_rank.has_value() && req.delivery_rank.has_value()) {
      if (first_rank == 0) {
        std::optional<Index> n0;
        std::optional<Index> n1;
        if (insert_len >= 2) {
          auto it = first_job;
          n0 = *it;
          ++it;
          n1 = *it;
        } else if (insert_len == 1) {
          n0 = *first_job;
          if (last_rank < route.size()) {
            n1 = route[last_rank];
          }
        } else {
          if (last_rank < route.size()) {
            n0 = route[last_rank];
          }
          if (last_rank + 1 < route.size()) {
            n1 = route[last_rank + 1];
          }
        }
        if (!n0.has_value() || !n1.has_value() ||
            n0.value() != req.pickup_rank.value() ||
            n1.value() != req.delivery_rank.value()) {
          return false;
        }
      }
      if (first_rank == 1 && insert_len > 0) {
        if (route.size() >= 2 && route[0] == req.pickup_rank.value() &&
            route[1] == req.delivery_rank.value()) {
          return false;
        }
      }
    }
  }

  if (const auto pl = input.pinned_last_for_vehicle(v); pl.has_value()) {
    const auto& req = pl.value();
    if (req.job_rank.has_value()) {
      if (last_rank == route.size()) {
        std::optional<Index> new_last;
        if (insert_len > 0) {
          auto it = first_job;
          std::advance(it, insert_len - 1);
          new_last = *it;
        } else {
          if (first_rank > 0) {
            new_last = route[first_rank - 1];
          }
        }
        if (!new_last.has_value() || new_last.value() != req.job_rank.value()) {
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
        const Index n0 = *it;
        ++it;
        const Index n1 = *it;
        if (n0 != req.pickup_rank.value() || n1 != req.delivery_rank.value()) {
          return false;
        }
      }
      if (first_rank == (route.size() >= 1 ? route.size() - 1 : 0) && insert_len > 0) {
        if (route.size() >= 2 && route[route.size() - 2] == req.pickup_rank.value() &&
            route.back() == req.delivery_rank.value()) {
          return false;
        }
      }
    }
  }

  const auto& init_load = (route.empty()) ? _zero : _current_loads[0];

  const auto& first_deliveries =
    (first_rank == 0) ? init_load : _bwd_deliveries[first_rank - 1];

  const auto& last_deliveries =
    (last_rank == 0) ? init_load : _bwd_deliveries[last_rank - 1];

  auto replaced_deliveries = first_deliveries - last_deliveries;

  delivery += ((route.empty()) ? _zero : _current_loads[first_rank]) -
              replaced_deliveries;

  bool valid = (delivery <= capacity);

  for (auto job_iter = first_job; job_iter != last_job; ++job_iter) {
    if (!valid) {
      break;
    }

    auto& job = input.jobs[*job_iter];
    delivery += job.pickup;
    delivery -= job.delivery;

    valid = (delivery <= capacity);
  }

  return valid;
}

const Amount& RawRoute::job_deliveries_sum() const {
  return route.empty() ? _zero : _current_loads[0];
}

const Amount& RawRoute::job_pickups_sum() const {
  return route.empty() ? _zero : _fwd_pickups.back();
}

const Amount& RawRoute::delivery_margin() const {
  return _delivery_margin;
}

const Amount& RawRoute::pickup_margin() const {
  return _pickup_margin;
}

Amount RawRoute::pickup_in_range(Index i, Index j) const {
  assert(i <= j && j <= _fwd_pickups.size());
  if (i == j || route.empty()) {
    return _zero;
  }
  if (i == 0) {
    return _fwd_pickups[j - 1];
  }
  return _fwd_pickups[j - 1] - _fwd_pickups[i - 1];
}

Amount RawRoute::delivery_in_range(Index i, Index j) const {
  assert(i <= j && j <= _bwd_deliveries.size());
  if (i == j || route.empty()) {
    return _zero;
  }
  const auto& before_deliveries =
    (i == 0) ? _current_loads[0] : _bwd_deliveries[i - 1];
  return before_deliveries - _bwd_deliveries[j - 1];
}

void RawRoute::add(const Input& input, const Index job_rank, const Index rank) {
  route.insert(route.begin() + rank, job_rank);
  update_amounts(input);
}

void RawRoute::remove(const Input& input,
                      const Index rank,
                      const unsigned count) {
  route.erase(route.begin() + rank, route.begin() + rank + count);
  update_amounts(input);
}

template <std::forward_iterator Iter>
void RawRoute::replace(const Input& input,
                       const Iter first_job,
                       const Iter last_job,
                       const Index first_rank,
                       const Index last_rank) {
  assert(first_rank <= last_rank);

  route.erase(route.begin() + first_rank, route.begin() + last_rank);
  route.insert(route.begin() + first_rank, first_job, last_job);

  update_amounts(input);
}

template bool RawRoute::is_valid_addition_for_capacity_inclusion(
  const Input& input,
  Amount delivery,
  const std::vector<Index>::iterator first_job,
  const std::vector<Index>::iterator last_job,
  const Index first_rank,
  const Index last_rank) const;
template bool RawRoute::is_valid_addition_for_capacity_inclusion(
  const Input& input,
  Amount delivery,
  const std::vector<Index>::const_iterator first_job,
  const std::vector<Index>::const_iterator last_job,
  const Index first_rank,
  const Index last_rank) const;
template bool RawRoute::is_valid_addition_for_capacity_inclusion(
  const Input& input,
  Amount delivery,
  const std::vector<Index>::reverse_iterator first_job,
  const std::vector<Index>::reverse_iterator last_job,
  const Index first_rank,
  const Index last_rank) const;
template void RawRoute::replace(const Input& input,
                                std::vector<Index>::iterator first_job,
                                std::vector<Index>::iterator last_job,
                                const Index first_rank,
                                const Index last_rank);
template void RawRoute::replace(const Input& input,
                                std::vector<Index>::const_iterator first_job,
                                std::vector<Index>::const_iterator last_job,
                                const Index first_rank,
                                const Index last_rank);
} // namespace vroom
