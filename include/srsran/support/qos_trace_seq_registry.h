/*
 *
 * Copyright 2021-2026 Software Radio Systems Limited
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

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace srsran {
namespace qos_trace_seq_registry {

inline std::mutex& registry_mutex()
{
  static std::mutex mtx;
  return mtx;
}

inline std::unordered_map<unsigned, uint64_t>& registry_map()
{
  static std::unordered_map<unsigned, uint64_t> by_ue;
  return by_ue;
}

inline void set_last_seq(unsigned ue_index, uint64_t seq)
{
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry_map()[ue_index] = seq;
}

inline uint64_t get_last_seq(unsigned ue_index)
{
  std::lock_guard<std::mutex> lock(registry_mutex());
  auto                         it = registry_map().find(ue_index);
  return it == registry_map().end() ? 0 : it->second;
}

} // namespace qos_trace_seq_registry
} // namespace srsran


