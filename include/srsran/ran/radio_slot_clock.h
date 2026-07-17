/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#pragma once

#include "srsran/ran/slot_point.h"
#include <atomic>

namespace srsran {

/// \brief Process-wide approximate radio slot clock for CU-CP QRT logs.
/// Updated from MAC/DU slot indication; read from RRC/NGAP (no native slot there).
inline std::atomic<slot_point>& radio_slot_clock_storage()
{
  static std::atomic<slot_point> clock;
  return clock;
}

inline void radio_slot_clock_update(slot_point sl)
{
  if (sl.valid()) {
    radio_slot_clock_storage().store(sl, std::memory_order_relaxed);
  }
}

inline slot_point radio_slot_clock_now()
{
  return radio_slot_clock_storage().load(std::memory_order_relaxed);
}

} // namespace srsran

