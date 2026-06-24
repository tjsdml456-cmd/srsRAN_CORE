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
#include "srsran/srslog/srslog.h"
#include "srsran/support/math/moving_averager.h"
#include "fmt/format.h"
#include <chrono>
#include <cstdint>
#include <limits>

namespace srsran {

/// Per-LC DL MAC SDU payload throughput tracker (same payload as MAC-THP-DL, fixed time windows).
/// Used for GBR gbr_weight: averages completed windows and emits zero-throughput windows on tick.
class lc_dl_delivered_rate_tracker
{
public:
  void set_window(std::chrono::milliseconds window) { window_ms = window; }

  void reset()
  {
    tracking         = false;
    win_bytes        = 0;
    last_sample_time = {};
    completed_windows.resize(0);
  }

  bool is_active() const { return tracking; }

  void on_payload(unsigned payload_bytes, std::chrono::steady_clock::time_point t)
  {
    if (payload_bytes == 0) {
      return;
    }
    ensure_tracking(t);
    win_bytes += payload_bytes;
    flush_windows(t);
    last_sample_time = t;
  }

  void on_tick(std::chrono::steady_clock::time_point t)
  {
    if (not tracking) {
      tracking         = true;
      win_start        = t;
      last_sample_time = t;
      return;
    }
    flush_windows(t);
    last_sample_time = t;
  }

  /// Max additional MAC SDU payload bytes allowed in the current window without exceeding MFBR.
  unsigned mfbr_remaining_payload_budget(uint64_t mbr_bps, std::chrono::steady_clock::time_point t)
  {
    if (mbr_bps == 0 or window_ms.count() <= 0) {
      return std::numeric_limits<unsigned>::max();
    }
    ensure_tracking(t);
    flush_windows(t);
    const uint64_t budget_bytes =
        (mbr_bps * static_cast<uint64_t>(window_ms.count()) + 7999ULL) / 8000ULL;
    if (win_bytes >= budget_bytes) {
      return 0;
    }
    const uint64_t rem = budget_bytes - win_bytes;
    return rem > static_cast<uint64_t>(std::numeric_limits<unsigned>::max())
               ? std::numeric_limits<unsigned>::max()
               : static_cast<unsigned>(rem);
  }

  /// Average delivered bit rate in bps over completed measurement windows.
  double average_bps() const
  {
    if (not tracking) {
      return 0.0;
    }
    if (completed_windows.size() > 0) {
      return static_cast<double>(completed_windows.average()) * 8.0 * 1000.0 /
             static_cast<double>(window_ms.count());
    }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(last_sample_time - win_start).count();
    if (elapsed_ms <= 0 or win_bytes == 0) {
      return 0.0;
    }
    return static_cast<double>(win_bytes) * 8.0 * 1000.0 / static_cast<double>(elapsed_ms);
  }

private:
  void ensure_tracking(std::chrono::steady_clock::time_point t)
  {
    if (not tracking) {
      tracking  = true;
      win_start = t;
      win_bytes = 0;
    }
  }

  void flush_windows(std::chrono::steady_clock::time_point t)
  {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t - win_start);
    while (elapsed >= window_ms) {
      push_completed_window(win_bytes);
      win_bytes = 0;
      win_start += window_ms;
      elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t - win_start);
    }
  }

  void push_completed_window(uint64_t bytes)
  {
    if (completed_windows.size() == 0) {
      completed_windows.resize(1);
    }
    const unsigned clamped =
        bytes > static_cast<uint64_t>(std::numeric_limits<unsigned>::max()) ? std::numeric_limits<unsigned>::max()
                                                                            : static_cast<unsigned>(bytes);
    completed_windows.push(clamped);
  }

  std::chrono::milliseconds             window_ms{100};
  bool                                  tracking = false;
  std::chrono::steady_clock::time_point win_start{};
  std::chrono::steady_clock::time_point last_sample_time{};
  uint64_t                              win_bytes = 0;
  moving_averager<unsigned>             completed_windows;
};

/// Fixed-window DL MAC payload throughput tracker (allocate_mac_sdu \c sdu_size).
/// Call \c on_tick from \c slot_indication so throttled (zero-allocation) windows emit vol_bytes=0.
class dl_mac_shaped_thp_tracker
{
public:
  explicit dl_mac_shaped_thp_tracker(du_ue_index_t                       ue_index_,
                                     std::chrono::milliseconds           window = std::chrono::milliseconds(10))
    : ue_index(ue_index_), window_ms(window)
  {
  }

  void on_shaped_payload(uint32_t payload_bytes, std::chrono::steady_clock::time_point t)
  {
    if (payload_bytes == 0) {
      return;
    }

    ensure_active(t);
    win_bytes += payload_bytes;
    flush_windows(t);
  }

  /// Advance the window clock; emit zero-throughput windows when no SDU was allocated.
  void on_tick(std::chrono::steady_clock::time_point now)
  {
    if (not active) {
      active    = true;
      win_start = now;
      return;
    }
    flush_windows(now);
  }

private:
  void ensure_active(std::chrono::steady_clock::time_point t)
  {
    if (not active) {
      active    = true;
      win_start = t;
      win_bytes = 0;
    }
  }

  void flush_windows(std::chrono::steady_clock::time_point t)
  {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t - win_start);
    while (elapsed >= window_ms) {
      emit_window(win_bytes);
      win_bytes = 0;
      win_start += window_ms;
      elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t - win_start);
    }
  }

  void emit_window(uint64_t bytes)
  {
    static srslog::basic_logger& logger = srslog::fetch_basic_logger("SCHED");
    const double win_ms               = static_cast<double>(window_ms.count());
    const double vol_kbit             = static_cast<double>(bytes) * 8.0 / 1000.0;
    const double thp_kbps             = win_ms > 0.0 ? vol_kbit / win_ms * 1000.0 : 0.0;
    logger.info("UE{} [MAC-THP-DL] window_ms={:.0f} vol_bytes={} thp_kbps={:.3f}",
                fmt::underlying(ue_index),
                win_ms,
                bytes,
                thp_kbps);
  }

  du_ue_index_t                         ue_index;
  std::chrono::milliseconds             window_ms;
  bool                                  active    = false;
  std::chrono::steady_clock::time_point win_start{};
  uint64_t                              win_bytes = 0;
};

} // namespace srsran

