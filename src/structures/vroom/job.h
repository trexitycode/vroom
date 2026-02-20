#ifndef JOB_H
#define JOB_H

/*

This file is part of VROOM.

Copyright (c) 2015-2025, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include <string>

#include "structures/typedefs.h"
#include "structures/vroom/amount.h"
#include "structures/vroom/location.h"
#include "structures/vroom/time_window.h"

namespace vroom {

enum class PinnedPosition {
  NONE = 0,
  FIRST,
  LAST
};

struct Job {
  Location location;
  const Id id;
  const JOB_TYPE type;
  const Duration default_setup;
  const Duration default_service;
  const Amount delivery;
  const Amount pickup;
  const Skills skills;
  const Priority priority;
  const std::vector<TimeWindow> tws;
  const std::string description;
  const TypeToDurationMap setup_per_type;
  const TypeToDurationMap service_per_type;
  // Optional per-vehicle objective penalties (signed, internal cost units),
  // keyed by vehicle id. Applied when this job is assigned to that vehicle.
  // For shipments, store penalties on the pickup only (delivery penalties empty).
  const std::vector<std::pair<Id, Cost>> vehicle_penalties;
  // Optional exclusive tags: at most one task per tag may appear in a route.
  // For shipments, tags should be set on the pickup only (delivery tags empty).
  const std::vector<ExclusiveTag> exclusive_tags;
  // Optional budget used for route-level budget feasibility.
  // For shipments, budget should be set on the pickup only (delivery budget=0).
  const Cost budget;
  // Optional hard-constraint flags/filters
  bool pinned{false};
  PinnedPosition pinned_position{PinnedPosition::NONE};
  // If non-empty, only these vehicle ids are eligible for this job/shipment step
  std::vector<Id> allowed_vehicles;
  std::vector<Duration> setups;
  std::vector<Duration> services;

  // Constructor for regular one-stop job (JOB_TYPE::SINGLE).
  Job(Id id,
      const Location& location,
      UserDuration default_setup = 0,
      UserDuration default_service = 0,
      Amount delivery = Amount(0),
      Amount pickup = Amount(0),
      Skills skills = Skills(),
      Priority priority = 0,
      const std::vector<TimeWindow>& tws =
        std::vector<TimeWindow>(1, TimeWindow()),
      std::string description = "",
      const TypeToUserDurationMap& setup_per_type = TypeToUserDurationMap(),
      const TypeToUserDurationMap& service_per_type = TypeToUserDurationMap(),
      const std::vector<std::pair<Id, Cost>>& vehicle_penalties =
        std::vector<std::pair<Id, Cost>>(),
      const std::vector<ExclusiveTag>& exclusive_tags = std::vector<ExclusiveTag>(),
      UserCost budget = 0,
      bool pinned = false,
      PinnedPosition pinned_position = PinnedPosition::NONE,
      const std::vector<Id>& allowed_vehicles = std::vector<Id>());

  // Constructor for pickup and delivery jobs (JOB_TYPE::PICKUP or
  // JOB_TYPE::DELIVERY).
  Job(Id id,
      JOB_TYPE type,
      const Location& location,
      UserDuration default_setup = 0,
      UserDuration default_service = 0,
      const Amount& amount = Amount(0),
      Skills skills = Skills(),
      Priority priority = 0,
      const std::vector<TimeWindow>& tws =
        std::vector<TimeWindow>(1, TimeWindow()),
      std::string description = "",
      const TypeToUserDurationMap& setup_per_type = TypeToUserDurationMap(),
      const TypeToUserDurationMap& service_per_type = TypeToUserDurationMap(),
      const std::vector<std::pair<Id, Cost>>& vehicle_penalties =
        std::vector<std::pair<Id, Cost>>(),
      const std::vector<ExclusiveTag>& exclusive_tags = std::vector<ExclusiveTag>(),
      UserCost budget = 0,
      bool pinned = false,
      PinnedPosition pinned_position = PinnedPosition::NONE,
      const std::vector<Id>& allowed_vehicles = std::vector<Id>());

  Index index() const {
    return location.index();
  }

  bool is_valid_start(Duration time) const;
};

} // namespace vroom

#endif
