/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#if defined(__SWITCH__)
#include <cassert>
#include <cstdint>
#include <cstdio>
#endif

#include "dynarmic/backend/arm64/a32_address_space.h"
#include "dynarmic/backend/arm64/a32_jitstate.h"
#if defined(__SWITCH__)
#include "dynarmic/interface/A32/switch_cycle_budget.h"
#endif

#if defined(__SWITCH__)
extern "C" bool azahar_switch_dynarmic_jit_is_rx_address(
    std::uintptr_t address) noexcept;
extern "C" void azahar_switch_dynarmic_jit_log_run_entry(
    std::uintptr_t run_entry) noexcept;
extern "C" void azahar_switch_dynarmic_jit_set_breadcrumb_phase(
    std::uint32_t phase, std::uintptr_t block_entry, std::uint32_t guest_pc) noexcept;
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
        u64 ticks_executed = 0;

        azahar_switch_dynarmic_jit_log_run_entry(run_entry);
        assert(azahar_switch_dynarmic_jit_is_rx_address(run_entry));

        azahar_switch_dynarmic_jit_set_breadcrumb_phase(6, run_entry, guest_pc);
        const u64 ticks_to_run = A32::SwitchCycleBudget::GetRequestedTicks();
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(7, run_entry, guest_pc);

        std::fprintf(stderr,
                     "[Dynarmic.Core] Run enter entry=0x%016llx guest_pc=0x%08x ticks_to_run=%llu\n",
                     static_cast<unsigned long long>(run_entry), guest_pc,
                     static_cast<unsigned long long>(ticks_to_run));
        std::fflush(stderr);

        const HaltReason result = RunWithTicks(process, thread_ctx, halt_reason,
                                               ticks_to_run, ticks_executed, false);

        const auto guest_pc_after =
            A32::LocationDescriptor{thread_ctx.GetLocationDescriptor()}.PC();
        std::fprintf(stderr,
                     "[Dynarmic.Core] Run leave entry=0x%016llx guest_pc_after=0x%08x ticks_executed=%llu result=%d\n",
                     static_cast<unsigned long long>(run_entry), guest_pc_after,
                     static_cast<unsigned long long>(ticks_executed), static_cast<int>(result));
        std::fflush(stderr);
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(10, run_entry, guest_pc_after);
        A32::SwitchCycleBudget::SetExecutedTicks(ticks_executed);
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(11, run_entry, guest_pc_after);
        return result;
#else
        return process.prelude_info.run_code(entry_point, &thread_ctx, halt_reason);
#endif
    }

#if defined(__SWITCH__)
    HaltReason RunWithTicks(A32AddressSpace& process, A32JitState& thread_ctx,
                            volatile u32* halt_reason, u64 ticks_to_run,
                            u64& ticks_executed, bool log_entry = true) {
        const auto location_descriptor = thread_ctx.GetLocationDescriptor();
        const auto entry_point = process.GetOrEmit(location_descriptor);
        const auto run_entry = reinterpret_cast<std::uintptr_t>(entry_point);
        const auto guest_pc = A32::LocationDescriptor{location_descriptor}.PC();

        if (log_entry) {
            azahar_switch_dynarmic_jit_log_run_entry(run_entry);
            assert(azahar_switch_dynarmic_jit_is_rx_address(run_entry));
        }

        azahar_switch_dynarmic_jit_set_breadcrumb_phase(8, run_entry, guest_pc);
        std::fprintf(stderr,
                     "[Dynarmic.Core] RunWithTicks enter entry=0x%016llx guest_pc=0x%08x ticks_to_run=%llu\n",
                     static_cast<unsigned long long>(run_entry), guest_pc,
                     static_cast<unsigned long long>(ticks_to_run));
        std::fflush(stderr);
        const HaltReason result = process.prelude_info.run_code(
            entry_point, &thread_ctx, halt_reason, ticks_to_run, &ticks_executed);
        const auto guest_pc_after =
            A32::LocationDescriptor{thread_ctx.GetLocationDescriptor()}.PC();
        std::fprintf(stderr,
                     "[Dynarmic.Core] RunWithTicks leave entry=0x%016llx guest_pc_after=0x%08x ticks_executed=%llu result=%d\n",
                     static_cast<unsigned long long>(run_entry), guest_pc_after,
                     static_cast<unsigned long long>(ticks_executed), static_cast<int>(result));
        std::fflush(stderr);
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(9, run_entry, guest_pc_after);
        return result;
    }
#endif

    HaltReason Step(A32AddressSpace& process, A32JitState& thread_ctx, volatile u32* halt_reason) {
        const auto location_descriptor = A32::LocationDescriptor{thread_ctx.GetLocationDescriptor()}.SetSingleStepping(true);
        const auto entry_point = process.GetOrEmit(location_descriptor);
#if defined(__SWITCH__)
        const auto run_entry = reinterpret_cast<std::uintptr_t>(entry_point);
        u64 ticks_executed = 0;

        azahar_switch_dynarmic_jit_log_run_entry(run_entry);
        assert(azahar_switch_dynarmic_jit_is_rx_address(run_entry));

        std::fprintf(stderr,
                     "[Dynarmic.Core] Step enter entry=0x%016llx guest_pc=0x%08x\n",
                     static_cast<unsigned long long>(run_entry),
                     location_descriptor.PC());
        std::fflush(stderr);

        const HaltReason result = StepWithTicks(process, thread_ctx, halt_reason,
                                                ticks_executed, false);
        const auto guest_pc_after =
            A32::LocationDescriptor{thread_ctx.GetLocationDescriptor()}.PC();
        std::fprintf(stderr,
                     "[Dynarmic.Core] Step leave entry=0x%016llx guest_pc_after=0x%08x ticks_executed=%llu result=%d\n",
                     static_cast<unsigned long long>(run_entry), guest_pc_after,
                     static_cast<unsigned long long>(ticks_executed), static_cast<int>(result));
        std::fflush(stderr);
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(10, run_entry, guest_pc_after);
        A32::SwitchCycleBudget::SetExecutedTicks(ticks_executed);
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(11, run_entry, guest_pc_after);
        return result;
#else
        return process.prelude_info.step_code(entry_point, &thread_ctx, halt_reason);
#endif
    }

#if defined(__SWITCH__)
    HaltReason StepWithTicks(A32AddressSpace& process, A32JitState& thread_ctx,
                             volatile u32* halt_reason, u64& ticks_executed,
                             bool log_entry = true) {
        const auto location_descriptor =
            A32::LocationDescriptor{thread_ctx.GetLocationDescriptor()}.SetSingleStepping(true);
        const auto entry_point = process.GetOrEmit(location_descriptor);
        const auto run_entry = reinterpret_cast<std::uintptr_t>(entry_point);

        if (log_entry) {
            azahar_switch_dynarmic_jit_log_run_entry(run_entry);
            assert(azahar_switch_dynarmic_jit_is_rx_address(run_entry));
        }

        azahar_switch_dynarmic_jit_set_breadcrumb_phase(
            8, run_entry, location_descriptor.PC());
        std::fprintf(stderr,
                     "[Dynarmic.Core] StepWithTicks enter entry=0x%016llx guest_pc=0x%08x\n",
                     static_cast<unsigned long long>(run_entry), location_descriptor.PC());
        std::fflush(stderr);
        const HaltReason result = process.prelude_info.step_code(
            entry_point, &thread_ctx, halt_reason, 1, &ticks_executed);
        const auto guest_pc_after =
            A32::LocationDescriptor{thread_ctx.GetLocationDescriptor()}.PC();
        std::fprintf(stderr,
                     "[Dynarmic.Core] StepWithTicks leave entry=0x%016llx guest_pc_after=0x%08x ticks_executed=%llu result=%d\n",
                     static_cast<unsigned long long>(run_entry), guest_pc_after,
                     static_cast<unsigned long long>(ticks_executed), static_cast<int>(result));
        std::fflush(stderr);
        azahar_switch_dynarmic_jit_set_breadcrumb_phase(9, run_entry, guest_pc_after);
        return result;
    }
#endif
};

}  // namespace Dynarmic::Backend::Arm64
