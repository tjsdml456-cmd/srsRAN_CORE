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

#include "rlc_bearer_logger.h"
#include "rlc_hol_pdb_helper.h"
#include "rlc_sdu_queue_lockfree.h"
#include "srsran/ran/du_types.h"
#include "srsran/ran/rb_id.h"
#include "srsran/srslog/srslog.h"
#include "fmt/format.h"
#include <array>

namespace srsran {

inline unsigned drop_pdb_expired_hol_sdus(rlc_sdu_queue_lockfree& sdu_queue,
                                          rlc_bearer_logger&      logger,
                                          du_ue_index_t           ue_index,
                                          rb_id_t                 rb_id,
                                          unsigned&               drop_total)
{
  if (not RLC_PDB_AQM_ENABLED) {
    return 0;
  }

  const uint32_t                mapper_ue   = rlc_mapper_ue_index(ue_index);
  const std::optional<unsigned> current_pdb = resolve_rlc_hol_pdb_ms_for_drop(ue_index, rb_id);
  static srslog::basic_logger&  aqm_logger  = srslog::fetch_basic_logger("RLC", false);

  static std::array<unsigned, MAX_NOF_DU_UES> aqm_probe_ctr{};
  const rlc_sdu_queue_lockfree::state_t       qstate = sdu_queue.get_state();
  if (qstate.n_sdus > 0 && ++aqm_probe_ctr[mapper_ue % MAX_NOF_DU_UES] % 256 == 0) {
    const rlc_sdu* hol    = sdu_queue.front();
    const double   hol_ms = hol != nullptr ? compute_hol_sojourn_ms(hol->time_of_arrival) : 0.0;
    aqm_logger.warning("[AQM-PROBE] UE{} RB{} kind=QUEUE hol_ms={:.1f} drop_pdb={} queue_sdus={} expired={}",
                       fmt::underlying(ue_index),
                       rb_id,
                       hol_ms,
                       current_pdb.value_or(0),
                       qstate.n_sdus,
                       hol != nullptr && rlc_sdu_pdb_expired(*hol, ue_index, rb_id));
  }

  unsigned dropped = 0;
  while (dropped < RLC_PDB_AQM_MAX_DROPS_PER_PASS) {
    const rlc_sdu* hol = sdu_queue.front();
    if (hol == nullptr) {
      break;
    }
    if (not rlc_sdu_pdb_expired(*hol, ue_index, rb_id)) {
      break;
    }

    rlc_sdu expired_sdu;
    if (not sdu_queue.read(expired_sdu)) {
      break;
    }

    const double sojourn_ms = compute_hol_sojourn_ms(expired_sdu.time_of_arrival);
    drop_total++;
    dropped++;
    const unsigned pdb_log = current_pdb.value_or(expired_sdu.pdb_ms.value_or(0));
    const bool     phase_shrink =
        rlc_pdb_phase_shrank(current_pdb, expired_sdu.pdb_ms) && current_pdb.has_value() &&
        sojourn_ms >= static_cast<double>(current_pdb.value()) * RLC_PDB_AQM_DROP_FRACTION;
    logger.log_warning("[SOJOURN-DROP] UE{} {} kind=QUEUE sojourn_ms={:.3f} PDB={}ms stamped_pdb={}ms "
                       "phase_shrink={} pdcp_sn={} sdu_len={} drop_total={}",
                       fmt::underlying(ue_index),
                       rb_id,
                       sojourn_ms,
                       pdb_log,
                       expired_sdu.pdb_ms.value_or(0),
                       phase_shrink,
                       expired_sdu.pdcp_sn.has_value() ? static_cast<unsigned>(expired_sdu.pdcp_sn.value()) : 0U,
                       expired_sdu.buf.length(),
                       drop_total);
    aqm_logger.warning("[SOJOURN-DROP] UE{} {} kind=QUEUE sojourn_ms={:.3f} PDB={}ms stamped_pdb={}ms "
                       "phase_shrink={} pdcp_sn={} sdu_len={} drop_total={}",
                       fmt::underlying(ue_index),
                       rb_id,
                       sojourn_ms,
                       pdb_log,
                       expired_sdu.pdb_ms.value_or(0),
                       phase_shrink,
                       expired_sdu.pdcp_sn.has_value() ? static_cast<unsigned>(expired_sdu.pdcp_sn.value()) : 0U,
                       expired_sdu.buf.length(),
                       drop_total);
  }
  return dropped;
}

} // namespace srsran
