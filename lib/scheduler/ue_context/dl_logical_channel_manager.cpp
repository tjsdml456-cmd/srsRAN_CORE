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

#include "dl_logical_channel_manager.h"
#include "srsran/srslog/srslog.h"
#include "srsran/ran/qos/arp_prio_level.h"
#include <algorithm>
#include <chrono>

using namespace srsran;

static unsigned get_mac_sdu_size(unsigned sdu_and_subheader_bytes)
{
  if (sdu_and_subheader_bytes == 0) {
    return 0;
  }
  const unsigned sdu_size = sdu_and_subheader_bytes - MIN_MAC_SDU_SUBHEADER_SIZE;
  return sdu_size < MAC_SDU_SUBHEADER_LENGTH_THRES ? sdu_size : sdu_size - 1;
}

// Initial capacity for the sorted_channels vector.
static constexpr unsigned INITIAL_CHANNEL_VEC_CAPACITY = 8;

// Initial capacity for the slice_lcid_list_lookup vector.
static constexpr unsigned INITIAL_SLICE_CAPACITY = 4;

// Number of MAC CEs supported by the implementation.
// - SCELL_ACTIV_4_OCTET || SCELL_ACTIV_4_OCTET
// - DRX_CMD || LONG_DRX_CMD
// - TA_CMD
// - UE_CON_RES_ID
// - PADDING
static constexpr unsigned MAX_CES_PER_UE = 5;

// Size of the pending_ces queue.
static constexpr unsigned MAX_PENDING_CES = MAX_NOF_DU_UES * MAX_CES_PER_UE;

// GBR gbr_weight and MFBR policer share this window (MFBR policing at MAC grant time).
// 300ms: smoother than 100ms cliff; fits ~250ms steady state per 0.5s QoS phase after QRT.
static constexpr unsigned GBR_RATE_AVG_WINDOW_MS = 2000;

static std::optional<unsigned> get_qos_rate_avg_window_msec(const logical_channel_config::qos_info& qos)
{
  if (not qos.gbr_qos_info.has_value()) {
    return std::nullopt;
  }
  return GBR_RATE_AVG_WINDOW_MS;
}

static void sync_lc_mbr_bps(uint64_t& mbr_bps_out, const logical_channel_config& lc_cfg)
{
  if (lc_cfg.qos.has_value() and lc_cfg.qos->gbr_qos_info.has_value()) {
    mbr_bps_out = lc_cfg.qos->gbr_qos_info->max_br_dl;
    if (mbr_bps_out == 0) {
      mbr_bps_out = lc_cfg.qos->gbr_qos_info->gbr_dl;
    }
    return;
  }
  mbr_bps_out = 0;
}

dl_logical_channel_manager::dl_logical_channel_manager(subcarrier_spacing              scs_common_,
                                                       du_ue_index_t                   ue_index_,
                                                       bool                            starts_in_fallback,
                                                       logical_channel_config_list_ptr log_channels_configs) :
  slots_per_sec(get_nof_slots_per_subframe(scs_common_) * 1000),
  fallback_state(starts_in_fallback),
  pending_ces(MAX_PENDING_CES),
  ue_index(ue_index_),
  shaped_thp_tracker(ue_index_)
{
  // Reserve entries to avoid allocating in hot path.
  sorted_channels.reserve(INITIAL_CHANNEL_VEC_CAPACITY);
  slice_lcid_list_lookup.reserve(INITIAL_SLICE_CAPACITY);

  configure(log_channels_configs);
}

void dl_logical_channel_manager::slot_indication()
{
  const auto now = std::chrono::steady_clock::now();
  for (lcid_t lcid : sorted_channels) {
    if (not is_srb(lcid) and channels[lcid].track_delivered_rate) {
      channels[lcid].delivered_rate.on_tick(now);
    }
  }

  shaped_thp_tracker.on_tick(now);
}

void dl_logical_channel_manager::apply_lc_rate_avg_window(lcid_t lcid)
{
  if (is_srb(lcid) or not channel_configs.has_value() or not channel_configs->contains(lcid)) {
    return;
  }
  const logical_channel_config_ptr lc_cfg = (*channel_configs)[lcid];
  auto&                            ch     = channels[lcid];
  if (not lc_cfg->qos.has_value() or not lc_cfg->qos->gbr_qos_info.has_value()) {
    ch.track_delivered_rate = false;
    ch.mbr_bps              = 0;
    ch.delivered_rate.reset();
    ch.avg_bytes_per_slot.resize(0);
    return;
  }
  sync_lc_mbr_bps(ch.mbr_bps, *lc_cfg);
  if (const auto win_size_msec = get_qos_rate_avg_window_msec(lc_cfg->qos.value())) {
    const unsigned win_size_slots = std::max(1U, *win_size_msec * slots_per_sec / 1000);
    ch.avg_bytes_per_slot.resize(win_size_slots);
    ch.track_delivered_rate = true;
    ch.delivered_rate.set_window(std::chrono::milliseconds(*win_size_msec));
  } else {
    ch.track_delivered_rate = false;
    ch.delivered_rate.reset();
    ch.avg_bytes_per_slot.resize(0);
  }
}

void dl_logical_channel_manager::reset_drbs_rate_averages()
{
  for (lcid_t lcid : sorted_channels) {
    if (is_srb(lcid)) {
      continue;
    }
    auto& ch = channels[lcid];
    ch.last_sched_bytes = 0;
    ch.delivered_rate.reset();
    if (ch.track_delivered_rate) {
      ch.delivered_rate.set_window(std::chrono::milliseconds(GBR_RATE_AVG_WINDOW_MS));
    }
    const unsigned win = ch.avg_bytes_per_slot.size();
    if (win == 0) {
      continue;
    }
    for (unsigned i = 0; i < win; ++i) {
      ch.avg_bytes_per_slot.push(0);
    }
  }
}

void dl_logical_channel_manager::deactivate()
{
  for (unsigned lcid = LCID_SRB0; lcid <= LCID_MAX_DRB; lcid++) {
    channels[lcid].active = false;
  }
  sorted_channels.clear();
  pending_ces.clear();
}

void dl_logical_channel_manager::set_fallback_state(bool enter_fallback)
{
  if (fallback_state == enter_fallback) {
    // no-op.
    return;
  }

  fallback_state = enter_fallback;
}

void dl_logical_channel_manager::reset_lcid_ran_slice(lcid_t lcid)
{
  if (not channels[lcid].slice_id.has_value()) {
    // LCID has no slice.
    return;
  }

  // Pop LCID from the slice linked list.
  unsigned slice_idx = channels[lcid].slice_id.value().value();
  slice_lcid_list_lookup[slice_idx].pop(&channels[lcid]);
  channels[lcid].slice_id.reset();
}

void dl_logical_channel_manager::deregister_ran_slice(ran_slice_id_t slice_id)
{
  if (not has_slice(slice_id)) {
    return;
  }
  for (channel_context& ch : slice_lcid_list_lookup[slice_id.value()]) {
    ch.slice_id.reset();
  }
  slice_lcid_list_lookup[slice_id.value()].clear();
}

void dl_logical_channel_manager::register_ran_slice(ran_slice_id_t slice_id)
{
  unsigned slice_index = slice_id.value();
  if (slice_lcid_list_lookup.size() <= slice_index) {
    slice_lcid_list_lookup.resize(slice_index + 1);
  }
}

void dl_logical_channel_manager::set_lcid_ran_slice(lcid_t lcid, ran_slice_id_t slice_id)
{
  unsigned slice_idx = slice_id.value();
  srsran_assert(slice_idx < slice_lcid_list_lookup.size(), "Invalid slice ID");
  if (channels[lcid].slice_id == slice_id) {
    // No-op.
    return;
  }

  // Remove LCID from previous slice.
  reset_lcid_ran_slice(lcid);

  // Add LCID to new slice.
  slice_lcid_list_lookup[slice_idx].push_back(&channels[lcid]);
  channels[lcid].slice_id = slice_id;
}

static uint16_t get_lc_prio(const logical_channel_config& cfg)
{
  uint16_t prio = 0;
  if (is_srb(cfg.lcid)) {
    prio = cfg.lcid <= LCID_SRB1 ? 0 : 1;
  } else if (cfg.qos.has_value()) {
    prio = cfg.qos->qos.priority.value() * cfg.qos->arp_priority.value();
  } else {
    prio = qos_prio_level_t::max() * arp_prio_level_t::max();
  }
  return prio;
}

void dl_logical_channel_manager::configure(logical_channel_config_list_ptr log_channels_configs)
{
  auto old_cfgs   = channel_configs;
  channel_configs = log_channels_configs;

  // If a previously custom configured LC is not in the list of new configs, we delete it.
  // Note: LCID will be removed from sorted_channels later.
  if (old_cfgs.has_value()) {
    for (const auto& old_lc : *old_cfgs) {
      if (not channel_configs->contains(old_lc->lcid)) {
        reset_lcid_ran_slice(old_lc->lcid);
        channels[old_lc->lcid].reset();
      }
    }
  }
  if (not channel_configs.has_value()) {
    return;
  }

  // Set new LC configurations.
  for (logical_channel_config_ptr ch_cfg : *channel_configs) {
    channels[ch_cfg->lcid].active = true;
    auto& ch                     = channels[ch_cfg->lcid];
    if (not is_srb(ch_cfg->lcid) and ch_cfg->qos.has_value() and ch_cfg->qos->gbr_qos_info.has_value()) {
      sync_lc_mbr_bps(ch.mbr_bps, *ch_cfg);
      if (const auto win_size_msec = get_qos_rate_avg_window_msec(ch_cfg->qos.value())) {
        const unsigned win_size_slots = std::max(1U, *win_size_msec * slots_per_sec / 1000);
        ch.avg_bytes_per_slot.resize(win_size_slots);
        ch.track_delivered_rate = true;
        ch.delivered_rate.set_window(std::chrono::milliseconds(*win_size_msec));
      } else {
        ch.track_delivered_rate = false;
        ch.mbr_bps              = 0;
        ch.delivered_rate.reset();
        ch.avg_bytes_per_slot.resize(0);
      }
    } else if (not is_srb(ch_cfg->lcid)) {
      ch.track_delivered_rate = false;
      ch.mbr_bps              = 0;
      ch.delivered_rate.reset();
      ch.avg_bytes_per_slot.resize(0);
    }
    // buffer state stays the same when configuration is updated.
  }

  // Refresh sorted channels list.
  sorted_channels.clear();
  for (const auto& lc_cfg : *channel_configs) {
    sorted_channels.push_back(lc_cfg->lcid);
  }
  std::sort(sorted_channels.begin(), sorted_channels.end(), [this](lcid_t lhs, lcid_t rhs) {
    return get_lc_prio(*channel_configs.value()[lhs]) < get_lc_prio(*channel_configs.value()[rhs]);
  });
}

bool dl_logical_channel_manager::has_pending_bytes(ran_slice_id_t slice_id) const
{
  if (fallback_state or not has_slice(slice_id)) {
    return false;
  }

  // Iterate through bearers of the slice until we find one with pending data.
  unsigned slice_idx = slice_id.value();
  for (const channel_context& ch : slice_lcid_list_lookup[slice_idx]) {
    if (ch.active and ch.buf_st > 0) {
      return true;
    }
  }

  // In case SRB slice was selected (but with no data) and there are pending CE bytes.
  if (slice_id == SRB_RAN_SLICE_ID and has_pending_ces()) {
    // Check if any other bearers have pending data. If they do, CE is not considered.
    // Note: This extra check is to avoid multiple slices report pending data.
    for (lcid_t lcid : sorted_channels) {
      if (channels[lcid].slice_id != SRB_RAN_SLICE_ID and has_pending_bytes(lcid)) {
        return false;
      }
    }
    return true;
  }

  return false;
}

unsigned dl_logical_channel_manager::total_pending_bytes() const
{
  unsigned bytes = pending_ce_bytes();
  for (lcid_t lcid : sorted_channels) {
    bytes += pending_bytes(lcid);
  }
  return bytes;
}

unsigned dl_logical_channel_manager::pending_bytes() const
{
  if (fallback_state) {
    return pending_con_res_ce_bytes() + pending_bytes(LCID_SRB0) + pending_bytes(LCID_SRB1);
  }
  unsigned bytes = pending_ce_bytes();
  for (lcid_t lcid : sorted_channels) {
    bytes += lcid != LCID_SRB0 ? pending_bytes(lcid) : 0;
  }
  return bytes;
}

unsigned dl_logical_channel_manager::pending_bytes(ran_slice_id_t slice_id) const
{
  if (fallback_state or not has_slice(slice_id)) {
    return 0;
  }

  // Compute pending bytes for the given slice bearers.
  unsigned total_bytes = 0;
  for (const channel_context& ch : slice_lcid_list_lookup[slice_id.value()]) {
    total_bytes += ch.active ? get_mac_sdu_required_bytes(ch.buf_st) : 0;
  }

  unsigned ce_bytes = pending_ce_bytes();
  if (ce_bytes > 0) {
    // There are also pending CE bytes.
    if (total_bytes > 0) {
      // In case the UE has pending bearer bytes, we also include the CE bytes.
      total_bytes += ce_bytes;
    } else if (slice_id == SRB_RAN_SLICE_ID) {
      // In case SRB1 was selected, and there are no pending bytes in the selected bearers, we return the pending CE
      // bytes iff the UE has no pending data on the remaining, non-selected bearers.
      // This is to avoid the situation where a UE, for instance, has DRB data to transmit, but the CE is allocated in
      // the SRB slice instead.
      for (lcid_t lcid : sorted_channels) {
        if (lcid > LCID_SRB1 and pending_bytes(lcid) > 0) {
          return 0;
        }
      }
      return ce_bytes;
    }
  }

  return total_bytes;
}

[[nodiscard]] bool dl_logical_channel_manager::handle_mac_ce_indication(const mac_ce_info& ce)
{
  if (ce.ce_lcid == lcid_dl_sch_t::UE_CON_RES_ID) {
    // CON RES is a special case, as it needs to be always scheduled first.
    pending_con_res_id = true;
    return true;
  }
  if (ce.ce_lcid == lcid_dl_sch_t::TA_CMD) {
    auto ce_it = std::find_if(pending_ces.begin(), pending_ces.end(), [](const mac_ce_info& c) {
      return c.ce_lcid == lcid_dl_sch_t::TA_CMD;
    });
    if (ce_it != pending_ces.end()) {
      ce_it->ce_payload = ce.ce_payload;
      return true;
    }
  }
  return pending_ces.try_push(ce);
}

unsigned dl_logical_channel_manager::allocate_mac_sdu(dl_msg_lc_info& subpdu, unsigned rem_bytes, lcid_t lcid)
{
  subpdu.lcid        = lcid_dl_sch_t::MIN_RESERVED;
  subpdu.sched_bytes = 0;

  lcid_t lcid_with_prio = lcid == INVALID_LCID ? get_max_prio_lcid() : lcid;
  if (lcid_with_prio == INVALID_LCID) {
    return 0;
  }

  // Update Buffer Status of allocated LCID.
  return allocate_mac_sdu(subpdu, lcid_with_prio, rem_bytes);
}

lcid_t dl_logical_channel_manager::get_max_prio_lcid() const
{
  for (const auto lcid : sorted_channels) {
    if (has_pending_bytes(lcid)) {
      return lcid;
    }
  }
  return INVALID_LCID;
}

unsigned dl_logical_channel_manager::allocate_mac_sdu(dl_msg_lc_info& subpdu, lcid_t lcid, unsigned rem_bytes)
{
  srsran_sanity_check(lcid < MAX_NOF_RB_LCIDS, "Max LCID value 32 exceeded");

  auto& ch = channels[lcid];

  unsigned lch_bytes = pending_bytes(lcid);
  if (lch_bytes == 0 or rem_bytes <= MIN_MAC_SDU_SUBHEADER_SIZE) {
    return 0;
  }

  // Account for available space and MAC subheader to decide the number of bytes to allocate.
  unsigned alloc_bytes = std::min(rem_bytes, get_mac_sdu_required_bytes(lch_bytes));

  // Allocate all leftover bytes in following cases:
  //  - If it is last PDU of the TBS.
  //  - [Implementation-defined] If \c leftover_bytes is < 5 bytes, as it results in small SDU size.
  unsigned leftover_bytes = rem_bytes - alloc_bytes;
  if (leftover_bytes > 0 and ((leftover_bytes <= MAX_MAC_SDU_SUBHEADER_SIZE + 1) or total_pending_bytes() == 0)) {
    alloc_bytes += leftover_bytes;
  }
  if (alloc_bytes == MAC_SDU_SUBHEADER_LENGTH_THRES + MIN_MAC_SDU_SUBHEADER_SIZE) {
    // Avoid invalid combination of MAC subPDU and subheader size.
    alloc_bytes--;
  }

  unsigned sdu_size = get_mac_sdu_size(alloc_bytes);

  // MFBR policing: clamp MAC SDU payload to remaining bytes in the current measurement window.
  if (not is_srb(lcid) and ch.mbr_bps > 0 and ch.track_delivered_rate) {
    const auto     now            = std::chrono::steady_clock::now();
    const unsigned payload_budget = ch.delivered_rate.mfbr_remaining_payload_budget(ch.mbr_bps, now);
    if (payload_budget == 0) {
      return 0;
    }
    if (sdu_size > payload_budget) {
      sdu_size    = payload_budget;
      alloc_bytes = get_mac_sdu_required_bytes(sdu_size);
      if (alloc_bytes > rem_bytes) {
        alloc_bytes = rem_bytes;
        sdu_size    = get_mac_sdu_size(alloc_bytes);
        if (sdu_size > payload_budget) {
          sdu_size    = payload_budget;
          alloc_bytes = get_mac_sdu_required_bytes(sdu_size);
        }
      }
      if (alloc_bytes <= MIN_MAC_SDU_SUBHEADER_SIZE or sdu_size == 0) {
        return 0;
      }
    }
  }

  // Update DL Buffer Status to avoid reallocating the same LCID bytes.
  ch.last_sched_bytes = std::min(sdu_size, ch.buf_st);
  ch.buf_st -= ch.last_sched_bytes;

  if (sdu_size > 0 and not is_srb(lcid)) {
    const auto now = std::chrono::steady_clock::now();
    if (ch.track_delivered_rate) {
      ch.delivered_rate.on_payload(sdu_size, now);
    }
    shaped_thp_tracker.on_shaped_payload(sdu_size, now);
  }

  if (lcid != LCID_SRB0 and ch.buf_st > 0) {
    static constexpr unsigned RLC_SEGMENTATION_OVERHEAD = 4;
    // Allocation was not enough to empty the logical channel. In this specific case, we add some bytes to account
    // for the RLC segmentation overhead.
    // Note: This update is only relevant for PDSCH allocations for slots > slot_tx. For the case of PDSCH
    // slot==slot_tx, there will be an RLC Buffer Occupancy update right away, which will set a new buffer value.
    ch.buf_st += RLC_SEGMENTATION_OVERHEAD;
  }

  subpdu.lcid        = (lcid_dl_sch_t::options)lcid;
  subpdu.sched_bytes = sdu_size;

  return alloc_bytes;
}

unsigned dl_logical_channel_manager::allocate_mac_ce(dl_msg_lc_info& subpdu, unsigned rem_bytes)
{
  subpdu.lcid        = lcid_dl_sch_t::MIN_RESERVED;
  subpdu.sched_bytes = 0;

  // Allocate ConRes CE ID if pending.
  unsigned alloc_bytes = allocate_ue_con_res_id_mac_ce(subpdu, rem_bytes);
  if (alloc_bytes > 0) {
    return alloc_bytes;
  }

  if (pending_ces.empty()) {
    return 0;
  }
  const lcid_dl_sch_t lcid = pending_ces.top().ce_lcid;

  // Derive space needed for CE subheader + payload.
  const unsigned ce_size = lcid.sizeof_ce();
  if (lcid.is_var_len_ce()) {
    alloc_bytes = get_mac_sdu_required_bytes(ce_size);
  } else {
    alloc_bytes = ce_size + FIXED_SIZED_MAC_CE_SUBHEADER_SIZE;
  }

  // Verify there is space for both MAC CE and subheader.
  if (rem_bytes < alloc_bytes) {
    return 0;
  }

  subpdu.lcid        = lcid;
  subpdu.sched_bytes = ce_size;
  subpdu.ce_payload  = pending_ces.top().ce_payload;

  pending_ces.pop();

  return alloc_bytes;
}

void dl_logical_channel_manager::channel_context::reset()
{
  active                = false;
  buf_st                = 0;
  last_sched_bytes      = 0;
  track_delivered_rate  = false;
  mbr_bps               = 0;
  delivered_rate.reset();
  avg_bytes_per_slot.resize(0);
}

unsigned
srsran::allocate_mac_sdus(dl_msg_tb_info& tb_info, dl_logical_channel_manager& lch_mng, unsigned total_tbs, lcid_t lcid)
{
  static constexpr unsigned min_mac_sdu_space = 4; // Needs to fit at least MAC SDU subheader and RLC header.
  unsigned                  rem_tbs           = total_tbs;

  // If we do not have enough bytes to fit MAC subheader, skip MAC SDU allocation.
  // Note: We assume upper layer accounts for its own subheaders when updating the buffer state.
  while (rem_tbs > min_mac_sdu_space and not tb_info.lc_chs_to_sched.full()) {
    dl_msg_lc_info subpdu;
    unsigned       alloc_bytes = lch_mng.allocate_mac_sdu(subpdu, rem_tbs, lcid);
    if (alloc_bytes == 0) {
      break;
    }

    // Add new subPDU.
    tb_info.lc_chs_to_sched.push_back(subpdu);

    // Update remaining space taking into account the MAC SDU subheader.
    rem_tbs -= alloc_bytes;
  }

  return total_tbs - rem_tbs;
}

unsigned srsran::allocate_mac_ces(dl_msg_tb_info& tb_info, dl_logical_channel_manager& lch_mng, unsigned total_tbs)
{
  unsigned rem_tbs = total_tbs;

  while (lch_mng.has_pending_ces() and not tb_info.lc_chs_to_sched.full()) {
    dl_msg_lc_info subpdu;
    unsigned       alloc_bytes = lch_mng.allocate_mac_ce(subpdu, rem_tbs);
    if (alloc_bytes == 0) {
      break;
    }

    // Add new subPDU.
    tb_info.lc_chs_to_sched.push_back(subpdu);

    // Update remaining space taking into account the MAC CE subheader.
    rem_tbs -= alloc_bytes;
  }
  return total_tbs - rem_tbs;
}

unsigned dl_logical_channel_manager::allocate_ue_con_res_id_mac_ce(dl_msg_lc_info& subpdu, unsigned rem_bytes)
{
  if (not is_con_res_id_pending()) {
    return 0;
  }

  subpdu.lcid        = lcid_dl_sch_t::MIN_RESERVED;
  subpdu.sched_bytes = 0;

  static const auto ce_size     = lcid_dl_sch_t{lcid_dl_sch_t::UE_CON_RES_ID}.sizeof_ce();
  unsigned          alloc_bytes = ce_size + FIXED_SIZED_MAC_CE_SUBHEADER_SIZE;

  // Verify there is space for both MAC CE and subheader.
  if (rem_bytes < alloc_bytes) {
    alloc_bytes = 0;
  }

  if (alloc_bytes > 0) {
    pending_con_res_id = false;

    subpdu.lcid        = lcid_dl_sch_t{lcid_dl_sch_t::UE_CON_RES_ID};
    subpdu.sched_bytes = ce_size;
  }

  return alloc_bytes;
}

unsigned
srsran::allocate_ue_con_res_id_mac_ce(dl_msg_tb_info& tb_info, dl_logical_channel_manager& lch_mng, unsigned total_tbs)
{
  unsigned rem_tbs = total_tbs;

  if (not tb_info.lc_chs_to_sched.full()) {
    dl_msg_lc_info subpdu;
    unsigned       alloc_bytes = lch_mng.allocate_ue_con_res_id_mac_ce(subpdu, rem_tbs);
    if (alloc_bytes != 0) {
      // Add new subPDU.
      tb_info.lc_chs_to_sched.push_back(subpdu);

      // Update remaining space taking into account the MAC CE subheader.
      rem_tbs -= alloc_bytes;
    }
  }
  return total_tbs - rem_tbs;
}

unsigned srsran::build_dl_fallback_transport_block_info(dl_msg_tb_info&             tb_info,
                                                        dl_logical_channel_manager& lch_mng,
                                                        unsigned                    tb_size_bytes)
{
  unsigned total_subpdu_bytes = 0;
  total_subpdu_bytes += allocate_ue_con_res_id_mac_ce(tb_info, lch_mng, tb_size_bytes);
  // Since SRB0 PDU cannot be segmented, skip SRB0 if remaining TB size is not enough to fit entire PDU.
  if (lch_mng.has_pending_bytes(LCID_SRB0) and
      ((tb_size_bytes - total_subpdu_bytes) >= lch_mng.pending_bytes(LCID_SRB0))) {
    total_subpdu_bytes += allocate_mac_sdus(tb_info, lch_mng, tb_size_bytes - total_subpdu_bytes, LCID_SRB0);
    return total_subpdu_bytes;
  }
  total_subpdu_bytes += allocate_mac_sdus(tb_info, lch_mng, tb_size_bytes - total_subpdu_bytes, LCID_SRB1);
  return total_subpdu_bytes;
}

unsigned srsran::build_dl_transport_block_info(dl_msg_tb_info&             tb_info,
                                               dl_logical_channel_manager& lch_mng,
                                               unsigned                    tb_size_bytes,
                                               ran_slice_id_t              slice_id)
{
  unsigned total_subpdu_bytes = 0;
  total_subpdu_bytes += allocate_mac_ces(tb_info, lch_mng, tb_size_bytes);
  for (const auto lcid : lch_mng.get_prioritized_logical_channels()) {
    if (lch_mng.get_slice_id(lcid) == slice_id) {
      total_subpdu_bytes += allocate_mac_sdus(tb_info, lch_mng, tb_size_bytes - total_subpdu_bytes, uint_to_lcid(lcid));
    }
  }
  return total_subpdu_bytes;
}






