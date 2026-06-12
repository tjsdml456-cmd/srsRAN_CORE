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

#include "srsran/ran/du_types.h"
#include "srsran/ran/rb_id.h"
#include "fmt/format.h"
#include <array>
#include <optional>

namespace srsran {

/// Per-UE runtime PDB cache (scheduler core 5QI updates).
inline std::array<std::optional<unsigned>, MAX_NOF_DU_UES>& rlc_ue_runtime_pdb_cache()
{
  static std::array<std::optional<unsigned>, MAX_NOF_DU_UES> cache;
  return cache;
}

/// Per-(UE, bearer) runtime PDB for multi-DRB UEs.
inline std::optional<unsigned>& rlc_bearer_runtime_pdb_cache(du_ue_index_t ue_index, rb_id_t rb_id)
{
  static std::array<std::array<std::optional<unsigned>, MAX_NOF_DRBS>, MAX_NOF_DU_UES> cache;
  rb_id_t        rb_copy = rb_id;
  const unsigned drb_idx = rb_copy.is_drb() ? drb_id_to_uint(rb_id.get_drb_id()) - 1U : 0U;
  return cache[static_cast<unsigned>(fmt::underlying(ue_index))][drb_idx];
}

inline void cache_rlc_ue_runtime_pdb_ms(uint32_t ue_index, unsigned pdb_ms)
{
  if (ue_index < MAX_NOF_DU_UES) {
    rlc_ue_runtime_pdb_cache()[ue_index] = pdb_ms;
  }
}

inline void cache_rlc_bearer_runtime_pdb_ms(du_ue_index_t ue_index, rb_id_t rb_id, unsigned pdb_ms)
{
  if (fmt::underlying(ue_index) < MAX_NOF_DU_UES) {
    rlc_bearer_runtime_pdb_cache(ue_index, rb_id) = pdb_ms;
    cache_rlc_ue_runtime_pdb_ms(static_cast<uint32_t>(fmt::underlying(ue_index)), pdb_ms);
  }
}

} // namespace srsran
