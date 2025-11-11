#ifndef EVAL_H
#define EVAL_H

/*

This file is part of VROOM.

Copyright (c) 2015-2025, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include <tuple>

#include "structures/typedefs.h"

namespace vroom {

struct Eval {
  Cost cost;
  Duration duration;
  Distance distance;

  template <typename T>
  static constexpr T saturating_add(T lhs, T rhs) {
      // ASAN/UBSAN will flag the sentinel math we use in heuristics if we let
      // these overflow. Clamp so debug tooling stays quiet while preserving the
      // intent (treat NO_EVAL / NO_GAIN as infinities).
    if (rhs > 0 &&
        lhs > std::numeric_limits<T>::max() - rhs) {
      return std::numeric_limits<T>::max();
    }
    if (rhs < 0 &&
        lhs < std::numeric_limits<T>::min() - rhs) {
      return std::numeric_limits<T>::min();
    }
    return lhs + rhs;
  }

  template <typename T>
  static constexpr T saturating_sub(T lhs, T rhs) {
    return saturating_add(lhs, static_cast<T>(-rhs));
  }

  template <typename T>
  static constexpr T saturating_neg(T value) {
    if (value == std::numeric_limits<T>::min()) {
      return std::numeric_limits<T>::max();
    }
    if (value == std::numeric_limits<T>::max()) {
      return std::numeric_limits<T>::min();
    }
    return static_cast<T>(-value);
  }

  constexpr Eval() : cost(0), duration(0), distance(0){};

  constexpr Eval(Cost cost, Duration duration = 0, Distance distance = 0)
    : cost(cost), duration(duration), distance(distance){};

  Eval& operator+=(const Eval& rhs) {
    cost = saturating_add(cost, rhs.cost);
    duration = saturating_add(duration, rhs.duration);
    distance = saturating_add(distance, rhs.distance);

    return *this;
  }

  Eval& operator-=(const Eval& rhs) {
    cost = saturating_sub(cost, rhs.cost);
    duration = saturating_sub(duration, rhs.duration);
    distance = saturating_sub(distance, rhs.distance);

    return *this;
  }

  Eval operator-() const {
    return {saturating_neg(cost),
            saturating_neg(duration),
            saturating_neg(distance)};
  }

  friend Eval operator+(Eval lhs, const Eval& rhs) {
    lhs += rhs;
    return lhs;
  }

  friend Eval operator-(Eval lhs, const Eval& rhs) {
    lhs -= rhs;
    return lhs;
  }

  friend bool operator<(const Eval& lhs, const Eval& rhs) {
    return std::tie(lhs.cost, lhs.duration, lhs.distance) <
           std::tie(rhs.cost, rhs.duration, rhs.distance);
  }

  friend bool operator<=(const Eval& lhs, const Eval& rhs) {
    return lhs.cost <= rhs.cost;
  }

  friend bool operator==(const Eval& lhs, const Eval& rhs) = default;
};

constexpr Eval NO_EVAL = {std::numeric_limits<Cost>::max(), 0, 0};
constexpr Eval NO_GAIN = {std::numeric_limits<Cost>::min(), 0, 0};

} // namespace vroom

#endif
