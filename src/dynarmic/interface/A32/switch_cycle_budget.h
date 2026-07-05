/* This file is part of the dynarmic project.
 * Copyright (c) 2026
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstdint>

namespace Dynarmic::A32::SwitchCycleBudget {

#if defined(__SWITCH__)
// Azahar currently schedules the emulated ARM cores sequentially on the
// emulation thread. Ordinary program storage avoids Horizon TLS resolution and
// its indirect call veneer while preserving the required run-to-run handoff.
inline std::uint64_t requested_ticks = 0;
inline std::uint64_t executed_ticks = 0;

inline void SetRequestedTicks(std::uint64_t ticks) noexcept {
    requested_ticks = ticks;
}

inline std::uint64_t GetRequestedTicks() noexcept {
    return requested_ticks;
}

inline void SetExecutedTicks(std::uint64_t ticks) noexcept {
    executed_ticks = ticks;
}

inline std::uint64_t TakeExecutedTicks() noexcept {
    const std::uint64_t ticks = executed_ticks;
    executed_ticks = 0;
    return ticks;
}
#endif

} // namespace Dynarmic::A32::SwitchCycleBudget
