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
#include "fmt/format.h"
#include <chrono>
#include <cstdint>

namespace srsran {

/// Fixed-window DL MAC payload throughput after token-bucket shaping (allocate_mac_sdu \c sdu_size).
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
