/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#if defined(__SWITCH__)
#include <cassert>
#include <cstdint>
#endif

#include "dynarmic/backend/arm64/a32_address_space.h"
#include "dynarmic/backend/arm64/a32_jitstate.h"

#if defined(__SWITCH__)
extern "C" bool azahar_switch_dynarmic_jit_is_rx_address(
    std::uintptr_t address) noexcept;
extern "C" void azahar_switch_dynarmic_jit_log_run_entry(
    std::uintptr_t run_entry) noexcept;
extern "C" void azahar_switch_dynarmic_jit_set_breadcrumb_phase(
    std::uint32_t phase, std::uintptr_t block_entry, std::uint32_t guest_pc) noexcept;
extern "C" void azahar_switch_dynarmic_jit_log_host_timing(
    const char* phase, std::uint32_t guest_pc, std::uint64_t ticks_to_run,
    std::uint64_t ticks_executed, std::uintptr_t run_entry,
    std::uint32_t run_entry_range_id) noexcept;
extern "C" std::uint32_t azahar_switch_dynarmic_jit_get_range_id(
    std::uintptr_t address) noexcept;
#endif

namespace Dynarmic::Backend::Arm64 {

class A32Core final {
public:
    explicit A32Core(const A32::UserConfig&) {}

    HaltReason Run(A32AddressSpace& process, A32JitState& thread_ctx, volatile u32* halt_reason) {
        const auto location_descriptor = thread_ctx.GetLocationDescriptor();
        const auto entry_point = process.GetOrEmit(location_descriptor);
#if defined(__SWITCH__)
        const auto run_entry = reinterpret_cast<std::uintptr_t>(entry_point);
        const auto guest_pc = A32::LocationDescriptor{location_descriptor}.PC();
        const auto run_entry_range_id = azahar_switch_dynarmic_jit_get_range_id(run_entry);
        u64 ticks_executed = 0;

        azahar_switch_dynarmic_jit_log_run_entry(run_entry);
        assert(azahar_switch_dynarmic_jit_is_rx_address(run_entry));
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(6, run_entry, guest_pc);
        azahar_switch_dynarmic_jit_log_host_timing(
            "BeforeHostGetTicks", guest_pc, 0, 0, run_entry, run_entry_range_id);
        const u64 ticks_to_run = process.conf.enable_cycle_counting
                                     ? process.conf.callbacks->GetTicksRemaining()
                                     : 0;
        azahar_switch_dynarmic_jit_log_host_timing(
            "AfterHostGetTicks", guest_pc, ticks_to_run, 0, run_entry,
            run_entry_range_id);
        azahar_switch_dynarmic_jit_log_host_timing(
            "BeforeGeneratedRun", guest_pc, ticks_to_run, 0, run_entry,
            run_entry_range_id);
        const HaltReason result = process.prelude_info.run_code(
            entry_point, &thread_ctx, halt_reason, ticks_to_run, &ticks_executed);
        azahar_switch_dynarmic_jit_log_host_timing(
            "AfterGeneratedRun", guest_pc, ticks_to_run, ticks_executed, run_entry,
            run_entry_range_id);
        if (process.conf.enable_cycle_counting) {
            azahar_switch_dynarmic_jit_log_host_timing(
                "BeforeHostAddTicks", guest_pc, ticks_to_run, ticks_executed,
                run_entry, run_entry_range_id);
            process.conf.callbacks->AddTicks(ticks_executed);
            azahar_switch_dynarmic_jit_log_host_timing(
                "AfterHostAddTicks", guest_pc, ticks_to_run, ticks_executed,
                run_entry, run_entry_range_id);
        }
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(
            7, run_entry, A32::LocationDescriptor{thread_ctx.GetLocationDescriptor()}.PC());
        return result;
#else
        return process.prelude_info.run_code(entry_point, &thread_ctx, halt_reason);
#endif
    }

    HaltReason Step(A32AddressSpace& process, A32JitState& thread_ctx, volatile u32* halt_reason) {
        const auto location_descriptor = A32::LocationDescriptor{thread_ctx.GetLocationDescriptor()}.SetSingleStepping(true);
        const auto entry_point = process.GetOrEmit(location_descriptor);
#if defined(__SWITCH__)
        const auto run_entry = reinterpret_cast<std::uintptr_t>(entry_point);
        const auto run_entry_range_id = azahar_switch_dynarmic_jit_get_range_id(run_entry);
        u64 ticks_executed = 0;

        azahar_switch_dynarmic_jit_log_run_entry(run_entry);
        assert(azahar_switch_dynarmic_jit_is_rx_address(run_entry));
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(6, run_entry, location_descriptor.PC());
        azahar_switch_dynarmic_jit_log_host_timing(
            "BeforeGeneratedRun", location_descriptor.PC(), 1, 0, run_entry,
            run_entry_range_id);
        const HaltReason result = process.prelude_info.step_code(
            entry_point, &thread_ctx, halt_reason, 1, &ticks_executed);
        azahar_switch_dynarmic_jit_log_host_timing(
            "AfterGeneratedRun", location_descriptor.PC(), 1, ticks_executed,
            run_entry, run_entry_range_id);
        if (process.conf.enable_cycle_counting) {
            azahar_switch_dynarmic_jit_log_host_timing(
                "BeforeHostAddTicks", location_descriptor.PC(), 1, ticks_executed,
                run_entry, run_entry_range_id);
            process.conf.callbacks->AddTicks(ticks_executed);
            azahar_switch_dynarmic_jit_log_host_timing(
                "AfterHostAddTicks", location_descriptor.PC(), 1, ticks_executed,
                run_entry, run_entry_range_id);
        }
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(
            7, run_entry, A32::LocationDescriptor{thread_ctx.GetLocationDescriptor()}.PC());
        return result;
#else
        return process.prelude_info.step_code(entry_point, &thread_ctx, halt_reason);
#endif
    }
};

}  // namespace Dynarmic::Backend::Arm64
