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

#include "ul_logical_channel_manager.h"

using namespace srsran;

// Initial capacity for the slice_lcid_list_lookup vector.
static constexpr unsigned INITIAL_SLICE_CAPACITY = 4;

// GBR gbr_weight uses ul_avg_rate over this window (stock 5QI table: 2000 ms).
static constexpr unsigned GBR_RATE_AVG_WINDOW_MS = 100;

static std::optional<unsigned> get_qos_rate_avg_window_msec(const logical_channel_config::qos_info& qos)
{
  if (not qos.gbr_qos_info.has_value()) {
    return std::nullopt;
  }
  return GBR_RATE_AVG_WINDOW_MS;
}

ul_logical_channel_manager::ul_logical_channel_manager(subcarrier_spacing              scs,
                                                       logical_channel_config_list_ptr log_channels_configs) :
  slots_per_sec(get_nof_slots_per_subframe(scs) * 1000)
{
  slice_lcgid_list_lookup.reserve(INITIAL_SLICE_CAPACITY);

  configure(log_channels_configs);
}

void ul_logical_channel_manager::set_lcg_ran_slice(lcg_id_t lcgid, ran_slice_id_t slice_id)
{
  if (groups[lcgid].slice_id == slice_id) {
    // No-op.
    return;
  }

  // Remove LCID from previous slice.
  reset_lcg_ran_slice(lcgid);

  // Add LCID to new slice.
  unsigned slice_idx = slice_id.value();
  if (slice_lcgid_list_lookup.size() <= slice_idx) {
    slice_lcgid_list_lookup.resize(slice_idx + 1);
  }
  slice_lcgid_list_lookup[slice_idx].push_back(&groups[lcgid]);
  groups[lcgid].slice_id = slice_id;
}

void ul_logical_channel_manager::reset_lcg_ran_slice(lcg_id_t lcgid)
{
  if (not groups[lcgid].slice_id.has_value()) {
    // LCID has no slice.
    return;
  }

  // Pop LCG-ID from the slice linked list.
  unsigned slice_idx = groups[lcgid].slice_id.value().value();
  slice_lcgid_list_lookup[slice_idx].pop(&groups[lcgid]);
  groups[lcgid].slice_id.reset();
}

void ul_logical_channel_manager::deregister_ran_slice(ran_slice_id_t slice_id)
{
  if (not has_slice(slice_id)) {
    return;
  }
  for (channel_group_context& ch : slice_lcgid_list_lookup[slice_id.value()]) {
    ch.slice_id.reset();
  }
  slice_lcgid_list_lookup[slice_id.value()].clear();
}

void ul_logical_channel_manager::slot_indication()
{
  // Update the bit rates of the UE logical channel groups with tracked bit rates.
  for (const auto& ch : *lc_channels_configs) {
    auto& group = groups[ch->lc_group];
    if (group.avg_bytes_per_slot.size() > 0) {
      group.avg_bytes_per_slot.push(group.last_sched_bytes);
      group.last_sched_bytes = 0;
    }
  }
}

/// \brief Update the configurations of the provided lists of bearers.
void ul_logical_channel_manager::configure(logical_channel_config_list_ptr lc_channel_configs_)
{
  if (lc_channels_configs == lc_channel_configs_) {
    // No-op.
    return;
  }

  // Detect QoS changes for logical channels and arm a one-shot flag for the corresponding LCG.
  //
  // This enables correlating "QoS modification" with the very first UL grant that uses the updated QoS.
  const logical_channel_config_list_ptr old_lc_channels_configs = lc_channels_configs;
  if (old_lc_channels_configs.has_value() and old_lc_channels_configs->size() > 0 and
      lc_channel_configs_.has_value() and lc_channel_configs_->size() > 0) {
    for (logical_channel_config_ptr new_lc : *lc_channel_configs_) {
      const lcid_t lcid = new_lc->lcid;
      const lcg_id_t lcg_id = new_lc->lc_group;

      bool qos_changed = false;
      if (not old_lc_channels_configs->contains(lcid)) {
        qos_changed = true;
      } else {
        const logical_channel_config_ptr old_lc = (*old_lc_channels_configs)[lcid];
        qos_changed                               = not(old_lc->qos == new_lc->qos);
      }

      if (qos_changed) {
        groups[lcg_id].first_ul_grant_after_qos_change_pending = true;
      }
    }
  }

  lc_channels_configs = lc_channel_configs_;

  for (unsigned i = 1; i != groups.size(); ++i) {
    groups[i].active = false;
  }
  for (logical_channel_config_ptr lc_ch : *lc_channels_configs) {
    groups[lc_ch->lc_group].active = true;
    if (lc_ch->qos.has_value() and lc_ch->qos->gbr_qos_info.has_value()) {
      if (const auto win_size_msec = get_qos_rate_avg_window_msec(lc_ch->qos.value())) {
        const unsigned win_size_slots = std::max(1U, *win_size_msec * slots_per_sec / 1000);
        groups[lc_ch->lc_group].avg_bytes_per_slot.resize(win_size_slots);
      }
    }
  }
  for (unsigned i = 0; i != groups.size(); ++i) {
    if (not groups[i].active and groups[i].slice_id.has_value() and has_slice(*groups[i].slice_id)) {
      reset_lcg_ran_slice(uint_to_lcg_id(i));
      groups[i].first_ul_grant_after_qos_change_pending = false;
    }
  }
}

void ul_logical_channel_manager::deactivate()
{
  // Disable UL SRBs and DRBs.
  for (unsigned lcg_id = 0; lcg_id <= MAX_LCG_ID; lcg_id++) {
    groups[lcg_id].active = false;
  }

  // Reset any pending SR.
  reset_sr_indication();
}

unsigned ul_logical_channel_manager::pending_bytes() const
{
  unsigned bytes = 0;
  for (const auto& ch : *lc_channels_configs) {
    bytes += pending_bytes(ch->lc_group);
  }
  return bytes;
}

unsigned ul_logical_channel_manager::pending_bytes(ran_slice_id_t slice_id) const
{
  if (not has_slice(slice_id)) {
    return 0;
  }
  unsigned bytes = 0;
  for (const channel_group_context& ch : slice_lcgid_list_lookup[slice_id.value()]) {
    lcg_id_t lcgid = uint_to_lcg_id(std::distance(groups.data(), &ch));
    bytes += pending_bytes(lcgid);
  }
  return bytes;
}

void ul_logical_channel_manager::handle_ul_grant(unsigned grant_size)
{
  // Reset any pending SR indication.
  reset_sr_indication();

  // Update estimates of logical channel bit rates.
  for (unsigned i = 0, e = groups.size(); i != e and grant_size > 0; ++i) {
    if (groups[i].active and groups[i].buf_st > groups[i].sched_bytes_accum) {
      unsigned bytes_sched = std::min(groups[i].buf_st - groups[i].sched_bytes_accum, grant_size);
      groups[i].last_sched_bytes += bytes_sched;
      groups[i].sched_bytes_accum += bytes_sched;
      grant_size -= bytes_sched;
    }
  }
}

bool ul_logical_channel_manager::consume_first_ul_grant_after_qos_change(lcg_id_t lcg_id)
{
  if (lcg_id >= groups.size()) {
    return false;
  }
  const bool pending = groups[lcg_id].first_ul_grant_after_qos_change_pending;
  groups[lcg_id].first_ul_grant_after_qos_change_pending = false;
  return pending;
}


