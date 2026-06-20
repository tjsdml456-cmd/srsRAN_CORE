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
#include "srsran/ran/qos/arp_prio_level.h"
#include "srsran/ran/qos/five_qi_qos_mapping.h"
#include "srsran/srslog/srslog.h"
#include <algorithm>
#include <limits>

using namespace srsran;

// GBR/DC-GBR air cap: allow up to N slots of GBR bytes per grant (TBS is discrete; 1 slot undershoots target).
static constexpr unsigned AIR_RATE_CAP_GRANT_SLOT_FACTOR = 2;

// 5M DC-GBR / 7M GBR use TBS-level air cap; non-GBR has no MAC token bucket.
static constexpr uint64_t DL_AIR_TBS_CAP_MIN_GBR_BPS = 5'000'000;

static bool dl_air_tbs_cap_applies(uint64_t gbr_bps)
{
  return gbr_bps >= DL_AIR_TBS_CAP_MIN_GBR_BPS;
}

namespace {

unsigned gbr_bytes_per_slot_ceil(uint64_t gbr_bps, unsigned slots_per_sec)
{
  const uint64_t denom = 8ULL * static_cast<uint64_t>(slots_per_sec);
  return static_cast<unsigned>(std::max<uint64_t>(1ULL, (gbr_bps + denom - 1ULL) / denom));
}

unsigned gbr_air_cap_grant_bytes(uint64_t gbr_bps, unsigned slots_per_sec)
{
  const unsigned per_slot = gbr_bytes_per_slot_ceil(gbr_bps, slots_per_sec);
  // 2 slots of GBR bytes: enough headroom for discrete TBS (~7M/5M) without cell-peak overshoot.
  return per_slot * AIR_RATE_CAP_GRANT_SLOT_FACTOR;
}

} // namespace

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

// [Implementation-defined] Averaging window for LC bit-rate tracking (GFBR/MFBR scheduling weights).
static constexpr unsigned QOS_RATE_AVG_WINDOW_MS = 300;

// Max burst stored in the token bucket (seconds of MBR). Smaller = flatter throughput near GBR.
static constexpr double TOKEN_BUCKET_BURST_SEC = 0.005;

// Defer DL scheduling only when token cannot fun d a minimal MAC SDU (priority gate, not rate cap).
static constexpr unsigned MIN_TOKEN_TO_SCHED = 500;

static unsigned get_qos_rate_avg_window_msec(const logical_channel_config::qos_info& qos)
{
  if (qos.qos.average_window_ms.has_value()) {
    return qos.qos.average_window_ms.value();
  }
  return QOS_RATE_AVG_WINDOW_MS;
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
  const double slot_dur_s = 1.0 / static_cast<double>(slots_per_sec);

  // Update the bit rates of the UE logical channels with tracked bit rates.
  for (lcid_t lcid : sorted_channels) {
    if (not is_srb(lcid)) {
      auto& ch = channels[lcid];
      // Check if avg_bytes_per_slot needs to be initialized for GBR flows
      // This handles cases where runtime_qos.res_type changed to GBR after configure() was called
      if (channels[lcid].avg_bytes_per_slot.size() == 0 and channel_configs.has_value() and
          channel_configs->contains(lcid)) {
        logical_channel_config_ptr lc_cfg = (*channel_configs)[lcid];
        if (lc_cfg->qos.has_value()) {
          const unsigned win_size_msec  = get_qos_rate_avg_window_msec(lc_cfg->qos.value());
          const unsigned win_size_slots = win_size_msec * slots_per_sec / 1000;
          if (channels[lcid].avg_bytes_per_slot.size() != win_size_slots) {
            channels[lcid].avg_bytes_per_slot.resize(win_size_slots);
          }
        }
      }
      
      // Push last_sched_bytes if avg_bytes_per_slot is initialized
      if (ch.avg_bytes_per_slot.size() > 0) {
        ch.avg_bytes_per_slot.push(ch.last_sched_bytes);
        ch.last_sched_bytes = 0;
      }

      // Token bucket refill (GBR/DC-GBR only).
      if (ch.tb_gbr_bps > 0) {
        ch.token_bytes += (static_cast<double>(ch.tb_gbr_bps) / 8.0) * slot_dur_s;
        const double cap = (static_cast<double>(ch.tb_mbr_bps) / 8.0) * TOKEN_BUCKET_BURST_SEC;
        if (ch.token_bytes > cap) {
          ch.token_bytes = cap;
        }
      }
    }
  }

  shaped_thp_tracker.on_tick(std::chrono::steady_clock::now());
}

void dl_logical_channel_manager::sync_lc_token_rates_from_config(lcid_t lcid)
{
  if (not channel_configs.has_value() or not channel_configs->contains(lcid)) {
    return;
  }
  const logical_channel_config_ptr lc_cfg = (*channel_configs)[lcid];
  if (not lc_cfg->qos.has_value()) {
    return;
  }
  const auto* qos_chars = get_5qi_to_qos_characteristics_mapping(lc_cfg->qos->five_qi);
  const bool  is_gbr_5qi =
      qos_chars != nullptr and (qos_chars->res_type == qos_flow_resource_type::gbr or
                                qos_chars->res_type == qos_flow_resource_type::delay_critical_gbr);

  auto& ch = channels[lcid];

  // Only drop shaping when the configured 5QI is definitively non-GBR.
  if (not is_gbr_5qi) {
    if (ch.tb_gbr_bps != 0 or ch.air_rate_cap) {
      set_token_rates(lcid, 0, 0, false);
    }
    return;
  }

  // GBR/DC-GBR 5QI but PCF/NGAP has not populated bitrates yet — keep current cap (do not open to full rate).
  if (not lc_cfg->qos->gbr_qos_info.has_value()) {
    return;
  }

  uint64_t gbr_bps = lc_cfg->qos->gbr_qos_info->gbr_dl;
  uint64_t mbr_bps = lc_cfg->qos->gbr_qos_info->max_br_dl;
  if (mbr_bps == 0) {
    mbr_bps = gbr_bps;
  }
  const bool air_rate_cap = dl_air_tbs_cap_applies(gbr_bps);
  if (ch.tb_gbr_bps != gbr_bps or ch.tb_mbr_bps != mbr_bps or ch.air_rate_cap != air_rate_cap) {
    set_token_rates(lcid, gbr_bps, mbr_bps, air_rate_cap);
  }
}

void dl_logical_channel_manager::set_token_rates(lcid_t lcid, uint64_t gbr_bps, uint64_t mbr_bps, bool air_rate_cap_en)
{
  srsran_assert(lcid < MAX_NOF_RB_LCIDS, "Max LCID value 32 exceeded");
  auto&        ch            = channels[lcid];
  const bool   rates_changed = (ch.tb_gbr_bps != gbr_bps or ch.tb_mbr_bps != mbr_bps or ch.air_rate_cap != air_rate_cap_en);
  ch.tb_gbr_bps              = gbr_bps;
  ch.tb_mbr_bps              = mbr_bps;
  ch.air_rate_cap            = air_rate_cap_en and dl_air_tbs_cap_applies(gbr_bps);
  if (gbr_bps == 0) {
    ch.token_bytes   = 0.0;
    ch.air_rate_cap  = false;
    if (rates_changed) {
      static srslog::basic_logger& logger = srslog::fetch_basic_logger("SCHED");
      logger.info("[TOKEN-BUCKET] SET LCID{} gbr=0 mbr=0 (disabled)", static_cast<unsigned>(lcid));
    }
    return;
  }
  if (rates_changed) {
    const double cap = (static_cast<double>(mbr_bps) / 8.0) * TOKEN_BUCKET_BURST_SEC;
    ch.token_bytes   = cap;
  }
  if (rates_changed) {
    static srslog::basic_logger& logger = srslog::fetch_basic_logger("SCHED");
    logger.info("[TOKEN-BUCKET] SET LCID{} gbr={} mbr={} air_cap={} token={:.0f} cap={:.0f}",
                static_cast<unsigned>(lcid),
                gbr_bps,
                mbr_bps,
                ch.air_rate_cap,
                ch.token_bytes,
                (static_cast<double>(mbr_bps) / 8.0) * TOKEN_BUCKET_BURST_SEC);
  }
  if (gbr_bps > 0) {
    apply_lc_rate_avg_window(lcid);
  }
}

bool dl_logical_channel_manager::is_token_throttled(ran_slice_id_t slice_id) const
{
  // Token gate disabled: deferring the whole UE on low token_bytes caused burst/idle
  // throughput artifacts. GBR shaping uses per-grant TBS air cap when enabled.
  (void)slice_id;
  return false;
}

unsigned dl_logical_channel_manager::get_dl_token_budget(lcid_t lcid) const
{
  if (not is_active(lcid)) {
    return 0;
  }
  const auto& ch = channels[lcid];
  if (ch.tb_gbr_bps == 0) {
    return std::numeric_limits<unsigned>::max();
  }
  return static_cast<unsigned>(ch.token_bytes);
}

unsigned dl_logical_channel_manager::get_dl_grant_byte_budget(ran_slice_id_t slice_id) const
{
  if (fallback_state or not has_slice(slice_id)) {
    return 0;
  }

  unsigned lc_bytes = 0;
  for (const channel_context& ch : slice_lcid_list_lookup[slice_id.value()]) {
    if (not ch.active or ch.buf_st == 0) {
      continue;
    }
    const unsigned pending = get_mac_sdu_required_bytes(ch.buf_st);
    if (ch.tb_gbr_bps > 0) {
      if (ch.air_rate_cap) {
        lc_bytes += std::min(pending, gbr_air_cap_grant_bytes(ch.tb_gbr_bps, slots_per_sec));
      } else {
        unsigned budget = std::min(pending, static_cast<unsigned>(ch.token_bytes));
        lc_bytes += budget;
      }
    } else {
      lc_bytes += pending;
    }
  }

  const unsigned ce_bytes = pending_ce_bytes();
  if (ce_bytes == 0) {
    return lc_bytes;
  }
  if (lc_bytes > 0) {
    return lc_bytes + ce_bytes;
  }
  if (slice_id == SRB_RAN_SLICE_ID) {
    for (lcid_t lcid : sorted_channels) {
      if (lcid > LCID_SRB1 and pending_bytes(lcid) > 0) {
        return 0;
      }
    }
    return ce_bytes;
  }
  return lc_bytes;
}

bool dl_logical_channel_manager::dl_air_rate_cap_enabled(ran_slice_id_t slice_id) const
{
  if (fallback_state or not has_slice(slice_id)) {
    return false;
  }
  for (const channel_context& ch : slice_lcid_list_lookup[slice_id.value()]) {
    if (ch.active and ch.air_rate_cap) {
      return true;
    }
  }
  return false;
}

unsigned dl_logical_channel_manager::get_dl_gbr_air_cap_grant_bytes(ran_slice_id_t slice_id) const
{
  if (fallback_state or not has_slice(slice_id)) {
    return std::numeric_limits<unsigned>::max();
  }

  unsigned cap = std::numeric_limits<unsigned>::max();
  for (const channel_context& ch : slice_lcid_list_lookup[slice_id.value()]) {
    if (not ch.active or not ch.air_rate_cap) {
      continue;
    }
    cap = std::min(cap, gbr_air_cap_grant_bytes(ch.tb_gbr_bps, slots_per_sec));
  }
  return cap;
}

void dl_logical_channel_manager::debit_dl_grant_tokens(ran_slice_id_t slice_id, unsigned tbs_bytes)
{
  if (fallback_state or tbs_bytes == 0 or not has_slice(slice_id)) {
    return;
  }

  for (channel_context& ch : slice_lcid_list_lookup[slice_id.value()]) {
    if (not ch.active or not ch.air_rate_cap) {
      continue;
    }
    // Air cap sizes each PDSCH TBS; debiting full TBS here double-limits below GBR.
    return;
  }
}

void dl_logical_channel_manager::apply_lc_rate_avg_window(lcid_t lcid)
{
  if (is_srb(lcid) or not channel_configs.has_value() or not channel_configs->contains(lcid)) {
    return;
  }
  const logical_channel_config_ptr lc_cfg = (*channel_configs)[lcid];
  if (not lc_cfg->qos.has_value()) {
    return;
  }
  const unsigned win_size_msec  = get_qos_rate_avg_window_msec(lc_cfg->qos.value());
  const unsigned win_size_slots = std::max(1U, win_size_msec * slots_per_sec / 1000);
  if (channels[lcid].avg_bytes_per_slot.size() != win_size_slots) {
    channels[lcid].avg_bytes_per_slot.resize(win_size_slots);
  }
}

void dl_logical_channel_manager::reset_drbs_rate_averages()
{
  for (lcid_t lcid : sorted_channels) {
    if (is_srb(lcid)) {
      continue;
    }
    auto& ch = channels[lcid];
    const unsigned win = ch.avg_bytes_per_slot.size();
    if (win == 0) {
      ch.last_sched_bytes = 0;
      continue;
    }
    for (unsigned i = 0; i < win; ++i) {
      ch.avg_bytes_per_slot.push(0);
    }
    ch.last_sched_bytes = 0;
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
    if (not is_srb(ch_cfg->lcid) and ch_cfg->qos.has_value()) {
      const unsigned win_size_msec  = get_qos_rate_avg_window_msec(ch_cfg->qos.value());
      const unsigned win_size_slots = win_size_msec * slots_per_sec / 1000;
      channels[ch_cfg->lcid].avg_bytes_per_slot.resize(win_size_slots);
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

  // Apply token/air-cap shaping once per QoS reconfiguration (not every slot).
  for (lcid_t lcid : sorted_channels) {
    if (not is_srb(lcid)) {
      sync_lc_token_rates_from_config(lcid);
    }
  }
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

  // GBR token ceiling at MAC (non-GBR 10M path). GBR/DC-GBR air cap uses grant-time TBS debit instead.
  if (ch.tb_gbr_bps > 0 and not ch.air_rate_cap) {
    const unsigned token_limit = static_cast<unsigned>(ch.token_bytes);
    if (alloc_bytes > token_limit) {
      alloc_bytes = token_limit;
    }
    if (alloc_bytes <= MIN_MAC_SDU_SUBHEADER_SIZE) {
      return 0;
    }
  }

  unsigned sdu_size = get_mac_sdu_size(alloc_bytes);

  // Update DL Buffer Status to avoid reallocating the same LCID bytes.
  ch.last_sched_bytes = std::min(sdu_size, ch.buf_st);
  ch.buf_st -= ch.last_sched_bytes;

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

  if (sdu_size > 0 and not is_srb(lcid)) {
    shaped_thp_tracker.on_shaped_payload(sdu_size, std::chrono::steady_clock::now());
  }

  if (ch.tb_gbr_bps > 0 and not ch.air_rate_cap) {
    ch.token_bytes -= static_cast<double>(alloc_bytes);
    if (ch.token_bytes < 0) {
      ch.token_bytes = 0;
    }
  }

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
  active           = false;
  buf_st           = 0;
  last_sched_bytes = 0;
  token_bytes      = 0.0;
  tb_gbr_bps       = 0;
  tb_mbr_bps       = 0;
  air_rate_cap     = false;
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




