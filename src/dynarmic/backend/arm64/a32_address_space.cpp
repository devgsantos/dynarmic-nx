/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/a32_address_space.h"

#include <cstdint>

#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/opt/passes.h"

#if defined(__SWITCH__)
#include <cstdio>
#include <array>
#endif

namespace Dynarmic::Backend::Arm64 {

#if defined(__SWITCH__)
extern "C" bool azahar_switch_dynarmic_jit_is_rx_address(
    std::uintptr_t address) noexcept;
extern "C" std::uint32_t azahar_switch_dynarmic_jit_get_range_id(
    std::uintptr_t address) noexcept;
extern "C" void azahar_switch_dynarmic_jit_log_prelude_target(
    const char* name, std::uintptr_t address) noexcept;
extern "C" void azahar_switch_dynarmic_jit_log_a32_svc(
    std::uint32_t svc, std::uintptr_t callback_target, std::uintptr_t continuation,
    std::uint32_t guest_pc) noexcept;
extern "C" void azahar_switch_dynarmic_jit_log_message(
    const char* tag, const char* message) noexcept;
#endif

template<auto mfp, typename T>
static void* EmitCallTrampoline(oaknut::CodeGenerator& code, T* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this;

    constexpr RegisterList save_lr = ToRegList(X30);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_lr, 0);
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    ABI_PopRegisters(code, save_lr, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

#if defined(__SWITCH__)
static void* EmitSwitchCallSvcTrampoline(oaknut::CodeGenerator& code, A32::UserCallbacks* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<&A32::UserCallbacks::CallSVC>(this_);
    using LogA32SvcFunc = void (*)(std::uint32_t, std::uintptr_t, std::uintptr_t,
                                   std::uint32_t);
    const LogA32SvcFunc log_a32_svc = azahar_switch_dynarmic_jit_log_a32_svc;
    constexpr RegisterList save_svc_and_lr = ToRegList(X1) | ToRegList(X30);

    oaknut::Label l_addr, l_log_a32_svc, l_this;

    void* target = code.xptr<void*>();

    ABI_PushRegisters(code, save_svc_and_lr, 0);
    code.MOV(W0, W1);
    code.LDR(X1, l_addr);
    code.MOV(X2, X30);
    code.LDR(W3, Xstate, offsetof(A32JitState, regs) + 15 * sizeof(u32));
    code.LDR(Xscratch0, l_log_a32_svc);
    code.BLR(Xscratch0);

    code.LDR(W1, SP, 0);
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.LDR(W1, SP, 0);
    code.LDR(X30, SP, 8);
    ABI_PopRegisters(code, save_svc_and_lr, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);
    code.l(l_log_a32_svc);
    code.dx(mcl::bit_cast<u64>(log_a32_svc));

    return target;
}
#endif

template<auto mfp, typename T>
static void* EmitWrappedReadCallTrampoline(oaknut::CodeGenerator& code, T* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this;

    constexpr u64 save_regs = ABI_CALLER_SAVE & ~ToRegList(Xscratch0);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.MOV(Xscratch0, X0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

template<auto callback, typename T>
static void* EmitExclusiveReadCallTrampoline(oaknut::CodeGenerator& code, const A32::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A32::UserConfig& conf, A32::VAddr vaddr) -> T {
        return conf.global_monitor->ReadAndMark<T>(conf.processor_id, vaddr, [&]() -> T {
            return (conf.callbacks->*callback)(vaddr);
        });
    };

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(mcl::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

template<auto mfp, typename T>
static void* EmitWrappedWriteCallTrampoline(oaknut::CodeGenerator& code, T* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this;

    constexpr u64 save_regs = ABI_CALLER_SAVE;

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.MOV(X2, Xscratch1);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

template<auto callback, typename T>
static void* EmitExclusiveWriteCallTrampoline(oaknut::CodeGenerator& code, const A32::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A32::UserConfig& conf, A32::VAddr vaddr, T value) -> u32 {
        return conf.global_monitor->DoExclusiveOperation<T>(conf.processor_id, vaddr,
                                                            [&](T expected) -> bool {
                                                                return (conf.callbacks->*callback)(vaddr, value, expected);
                                                            })
                 ? 0
                 : 1;
    };

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(mcl::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

A32AddressSpace::A32AddressSpace(const A32::UserConfig& conf)
        : AddressSpace(conf.code_cache_size)
        , conf(conf)
#if defined(__SWITCH__)
        , host_get_ticks_remaining(Devirtualize<&A32::UserCallbacks::GetTicksRemaining>(conf.callbacks))
        , host_add_ticks(Devirtualize<&A32::UserCallbacks::AddTicks>(conf.callbacks))
#endif
{
    EmitPrelude();
}

#if defined(__SWITCH__)
u64 A32AddressSpace::HostGetTicksRemaining() const {
    using HostGetTicksRemainingFunc = u64 (*)(void*);
    const auto fn = mcl::bit_cast<HostGetTicksRemainingFunc>(host_get_ticks_remaining.fn_ptr);
    return fn(reinterpret_cast<void*>(host_get_ticks_remaining.this_ptr));
}

void A32AddressSpace::HostAddTicks(u64 ticks) const {
    using HostAddTicksFunc = void (*)(void*, u64);
    const auto fn = mcl::bit_cast<HostAddTicksFunc>(host_add_ticks.fn_ptr);
    fn(reinterpret_cast<void*>(host_add_ticks.this_ptr), ticks);
}

u64 A32AddressSpace::HostGetTicksRemainingTarget() const {
    return host_get_ticks_remaining.fn_ptr;
}

u64 A32AddressSpace::HostAddTicksTarget() const {
    return host_add_ticks.fn_ptr;
}
#endif

IR::Block A32AddressSpace::GenerateIR(IR::LocationDescriptor descriptor) const {
    IR::Block ir_block = A32::Translate(A32::LocationDescriptor{descriptor}, conf.callbacks, {conf.arch_version, conf.define_unpredictable_behaviour, conf.hook_hint_instructions});

    Optimization::PolyfillPass(ir_block, {});
    Optimization::NamingPass(ir_block);
    if (conf.HasOptimization(OptimizationFlag::GetSetElimination)) {
        Optimization::A32GetSetElimination(ir_block, {.convert_nzc_to_nz = true});
        Optimization::DeadCodeElimination(ir_block);
    }
    if (conf.HasOptimization(OptimizationFlag::ConstProp)) {
        Optimization::A32ConstantMemoryReads(ir_block, conf.callbacks);
        Optimization::ConstantPropagation(ir_block);
        Optimization::DeadCodeElimination(ir_block);
    }
    Optimization::IdentityRemovalPass(ir_block);
    Optimization::VerificationPass(ir_block);

    return ir_block;
}

void A32AddressSpace::InvalidateCacheRanges(const boost::icl::interval_set<u32>& ranges) {
    InvalidateBasicBlocks(block_ranges.InvalidateRanges(ranges));
}

void A32AddressSpace::EmitPrelude() {
    using namespace oaknut::util;

    UnprotectCodeMemory();

    prelude_info.read_memory_128 = nullptr;
    prelude_info.wrapped_read_memory_128 = nullptr;
    prelude_info.exclusive_read_memory_128 = nullptr;
    prelude_info.write_memory_128 = nullptr;
    prelude_info.wrapped_write_memory_128 = nullptr;
    prelude_info.exclusive_write_memory_128 = nullptr;
    prelude_info.dc_raised = nullptr;
    prelude_info.ic_raised = nullptr;
    prelude_info.get_cntpct = nullptr;

    prelude_info.read_memory_8 = EmitCallTrampoline<&A32::UserCallbacks::MemoryRead8>(code, conf.callbacks);
    prelude_info.read_memory_16 = EmitCallTrampoline<&A32::UserCallbacks::MemoryRead16>(code, conf.callbacks);
    prelude_info.read_memory_32 = EmitCallTrampoline<&A32::UserCallbacks::MemoryRead32>(code, conf.callbacks);
    prelude_info.read_memory_64 = EmitCallTrampoline<&A32::UserCallbacks::MemoryRead64>(code, conf.callbacks);
    prelude_info.wrapped_read_memory_8 = EmitWrappedReadCallTrampoline<&A32::UserCallbacks::MemoryRead8>(code, conf.callbacks);
    prelude_info.wrapped_read_memory_16 = EmitWrappedReadCallTrampoline<&A32::UserCallbacks::MemoryRead16>(code, conf.callbacks);
    prelude_info.wrapped_read_memory_32 = EmitWrappedReadCallTrampoline<&A32::UserCallbacks::MemoryRead32>(code, conf.callbacks);
    prelude_info.wrapped_read_memory_64 = EmitWrappedReadCallTrampoline<&A32::UserCallbacks::MemoryRead64>(code, conf.callbacks);
    prelude_info.exclusive_read_memory_8 = EmitExclusiveReadCallTrampoline<&A32::UserCallbacks::MemoryRead8, u8>(code, conf);
    prelude_info.exclusive_read_memory_16 = EmitExclusiveReadCallTrampoline<&A32::UserCallbacks::MemoryRead16, u16>(code, conf);
    prelude_info.exclusive_read_memory_32 = EmitExclusiveReadCallTrampoline<&A32::UserCallbacks::MemoryRead32, u32>(code, conf);
    prelude_info.exclusive_read_memory_64 = EmitExclusiveReadCallTrampoline<&A32::UserCallbacks::MemoryRead64, u64>(code, conf);
    prelude_info.write_memory_8 = EmitCallTrampoline<&A32::UserCallbacks::MemoryWrite8>(code, conf.callbacks);
    prelude_info.write_memory_16 = EmitCallTrampoline<&A32::UserCallbacks::MemoryWrite16>(code, conf.callbacks);
    prelude_info.write_memory_32 = EmitCallTrampoline<&A32::UserCallbacks::MemoryWrite32>(code, conf.callbacks);
    prelude_info.write_memory_64 = EmitCallTrampoline<&A32::UserCallbacks::MemoryWrite64>(code, conf.callbacks);
    prelude_info.wrapped_write_memory_8 = EmitWrappedWriteCallTrampoline<&A32::UserCallbacks::MemoryWrite8>(code, conf.callbacks);
    prelude_info.wrapped_write_memory_16 = EmitWrappedWriteCallTrampoline<&A32::UserCallbacks::MemoryWrite16>(code, conf.callbacks);
    prelude_info.wrapped_write_memory_32 = EmitWrappedWriteCallTrampoline<&A32::UserCallbacks::MemoryWrite32>(code, conf.callbacks);
    prelude_info.wrapped_write_memory_64 = EmitWrappedWriteCallTrampoline<&A32::UserCallbacks::MemoryWrite64>(code, conf.callbacks);
    prelude_info.exclusive_write_memory_8 = EmitExclusiveWriteCallTrampoline<&A32::UserCallbacks::MemoryWriteExclusive8, u8>(code, conf);
    prelude_info.exclusive_write_memory_16 = EmitExclusiveWriteCallTrampoline<&A32::UserCallbacks::MemoryWriteExclusive16, u16>(code, conf);
    prelude_info.exclusive_write_memory_32 = EmitExclusiveWriteCallTrampoline<&A32::UserCallbacks::MemoryWriteExclusive32, u32>(code, conf);
    prelude_info.exclusive_write_memory_64 = EmitExclusiveWriteCallTrampoline<&A32::UserCallbacks::MemoryWriteExclusive64, u64>(code, conf);
#if defined(__SWITCH__)
    prelude_info.call_svc = EmitSwitchCallSvcTrampoline(code, conf.callbacks);
#else
    prelude_info.call_svc = EmitCallTrampoline<&A32::UserCallbacks::CallSVC>(code, conf.callbacks);
#endif
    prelude_info.exception_raised = EmitCallTrampoline<&A32::UserCallbacks::ExceptionRaised>(code, conf.callbacks);
    prelude_info.isb_raised = EmitCallTrampoline<&A32::UserCallbacks::InstructionSynchronizationBarrierRaised>(code, conf.callbacks);
#if defined(__SWITCH__)
    prelude_info.add_ticks = nullptr;
    prelude_info.get_ticks_remaining = nullptr;
#else
    prelude_info.add_ticks = EmitCallTrampoline<&A32::UserCallbacks::AddTicks>(code, conf.callbacks);
    prelude_info.get_ticks_remaining = EmitCallTrampoline<&A32::UserCallbacks::GetTicksRemaining>(code, conf.callbacks);
#endif

    oaknut::Label return_from_run_code, l_return_to_dispatcher;

    prelude_info.run_code = code.xptr<PreludeInfo::RunCodeFuncType>();
    {
        ABI_PushRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));

        code.MOV(X19, X0);
        code.MOV(Xstate, X1);
        code.MOV(Xhalt, X2);
#if defined(__SWITCH__)
        code.STR(X4, SP, offsetof(StackLayout, ticks_executed_out));
#endif
        if (conf.page_table) {
            code.MOV(Xpagetable, mcl::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.MOV(Xfastmem, *conf.fastmem_pointer);
        }

        if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
            code.LDR(Xscratch0, l_return_to_dispatcher);
            for (size_t i = 0; i < RSBCount; i++) {
                code.STR(Xscratch0, SP, offsetof(StackLayout, rsb) + offsetof(RSBEntry, code_ptr) + i * sizeof(RSBEntry));
            }
        }

        if (conf.enable_cycle_counting) {
#if defined(__SWITCH__)
            code.MOV(Xticks, X3);
#else
            code.BL(prelude_info.get_ticks_remaining);
            code.MOV(Xticks, X0);
#endif
            code.STR(Xticks, SP, offsetof(StackLayout, cycles_to_run));
        }

        code.LDR(Wscratch0, Xstate, offsetof(A32JitState, upper_location_descriptor));
        code.AND(Wscratch0, Wscratch0, 0xffff0000);
        code.MRS(Xscratch1, oaknut::SystemReg::FPCR);
        code.STR(Wscratch1, SP, offsetof(StackLayout, save_host_fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);

        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);

        code.BR(X19);
    }

    prelude_info.step_code = code.xptr<PreludeInfo::RunCodeFuncType>();
    {
        ABI_PushRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));

        code.MOV(X19, X0);
        code.MOV(Xstate, X1);
        code.MOV(Xhalt, X2);
#if defined(__SWITCH__)
        code.STR(X4, SP, offsetof(StackLayout, ticks_executed_out));
#endif
        if (conf.page_table) {
            code.MOV(Xpagetable, mcl::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.MOV(Xfastmem, *conf.fastmem_pointer);
        }

        if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
            code.LDR(Xscratch0, l_return_to_dispatcher);
            for (size_t i = 0; i < RSBCount; i++) {
                code.STR(Xscratch0, SP, offsetof(StackLayout, rsb) + offsetof(RSBEntry, code_ptr) + i * sizeof(RSBEntry));
            }
        }

        if (conf.enable_cycle_counting) {
            code.MOV(Xticks, 1);
            code.STR(Xticks, SP, offsetof(StackLayout, cycles_to_run));
        }

        code.LDR(Wscratch0, Xstate, offsetof(A32JitState, upper_location_descriptor));
        code.AND(Wscratch0, Wscratch0, 0xffff0000);
        code.MRS(Xscratch1, oaknut::SystemReg::FPCR);
        code.STR(Wscratch1, SP, offsetof(StackLayout, save_host_fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);

        oaknut::Label step_hr_loop;
        code.l(step_hr_loop);
        code.LDAXR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);
        code.ORR(Wscratch0, Wscratch0, static_cast<u32>(HaltReason::Step));
        code.STLXR(Wscratch1, Wscratch0, Xhalt);
        code.CBNZ(Wscratch1, step_hr_loop);

        code.BR(X19);
    }

    prelude_info.return_to_dispatcher = code.xptr<void*>();
    {
        oaknut::Label l_this, l_addr;

        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);

        if (conf.enable_cycle_counting) {
            code.CMP(Xticks, 0);
            code.B(LE, return_from_run_code);
        }

        code.LDR(X0, l_this);
        code.MOV(X1, Xstate);
        code.LDR(Xscratch0, l_addr);
        code.BLR(Xscratch0);
        code.BR(X0);

        const auto fn = [](A32AddressSpace& self, A32JitState& context) -> CodePtr {
            return self.GetOrEmit(context.GetLocationDescriptor());
        };

        code.align(8);
        code.l(l_this);
        code.dx(mcl::bit_cast<u64>(this));
        code.l(l_addr);
        code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));
    }

    prelude_info.return_from_run_code = code.xptr<void*>();
    {
        code.l(return_from_run_code);

        if (conf.enable_cycle_counting) {
            code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
            code.SUB(X1, X1, Xticks);
#if defined(__SWITCH__)
            oaknut::Label skip_store_ticks;
            code.LDR(X2, SP, offsetof(StackLayout, ticks_executed_out));
            code.CBZ(X2, skip_store_ticks);
            code.STR(X1, X2);
            code.l(skip_store_ticks);
#else
            code.BL(prelude_info.add_ticks);
#endif
        }

        code.LDR(Wscratch0, SP, offsetof(StackLayout, save_host_fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);

        oaknut::Label exit_hr_loop;
        code.l(exit_hr_loop);
        code.LDAXR(W0, Xhalt);
        code.STLXR(Wscratch0, WZR, Xhalt);
        code.CBNZ(Wscratch0, exit_hr_loop);

        ABI_PopRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));
        code.RET();
    }

    code.align(8);
    code.l(l_return_to_dispatcher);
    code.dx(mcl::bit_cast<u64>(prelude_info.return_to_dispatcher));

    prelude_info.end_of_prelude = code.offset();

    mem.invalidate_all();
    ProtectCodeMemory();

#if defined(__SWITCH__)
    const auto run_code_range = azahar_switch_dynarmic_jit_get_range_id(
        reinterpret_cast<std::uintptr_t>(prelude_info.run_code));
    const auto assert_rx_entry = [run_code_range](const char* name, const void* entry,
                                                  bool offset_zero_ok = false) {
        const auto address = reinterpret_cast<std::uintptr_t>(entry);
        azahar_switch_dynarmic_jit_log_prelude_target(name, address);
        ASSERT(azahar_switch_dynarmic_jit_is_rx_address(address));
        ASSERT(azahar_switch_dynarmic_jit_get_range_id(address) == run_code_range);
        (void)offset_zero_ok;
    };
    const auto assert_optional_rx_entry = [&assert_rx_entry](const char* name,
                                                            const void* entry) {
        if (entry != nullptr) {
            assert_rx_entry(name, entry);
        } else {
            azahar_switch_dynarmic_jit_log_prelude_target(name, 0);
        }
    };

    assert_rx_entry("run_code", reinterpret_cast<const void*>(prelude_info.run_code), true);
    assert_rx_entry("step_code", reinterpret_cast<const void*>(prelude_info.step_code));
    assert_rx_entry("return_to_dispatcher", prelude_info.return_to_dispatcher);
    assert_rx_entry("return_from_run_code", prelude_info.return_from_run_code);
    assert_optional_rx_entry("read_memory_8", prelude_info.read_memory_8);
    assert_optional_rx_entry("read_memory_16", prelude_info.read_memory_16);
    assert_optional_rx_entry("read_memory_32", prelude_info.read_memory_32);
    assert_optional_rx_entry("read_memory_64", prelude_info.read_memory_64);
    assert_optional_rx_entry("read_memory_128", prelude_info.read_memory_128);
    assert_optional_rx_entry("wrapped_read_memory_8", prelude_info.wrapped_read_memory_8);
    assert_optional_rx_entry("wrapped_read_memory_16", prelude_info.wrapped_read_memory_16);
    assert_optional_rx_entry("wrapped_read_memory_32", prelude_info.wrapped_read_memory_32);
    assert_optional_rx_entry("wrapped_read_memory_64", prelude_info.wrapped_read_memory_64);
    assert_optional_rx_entry("wrapped_read_memory_128", prelude_info.wrapped_read_memory_128);
    assert_optional_rx_entry("exclusive_read_memory_8", prelude_info.exclusive_read_memory_8);
    assert_optional_rx_entry("exclusive_read_memory_16", prelude_info.exclusive_read_memory_16);
    assert_optional_rx_entry("exclusive_read_memory_32", prelude_info.exclusive_read_memory_32);
    assert_optional_rx_entry("exclusive_read_memory_64", prelude_info.exclusive_read_memory_64);
    assert_optional_rx_entry("exclusive_read_memory_128", prelude_info.exclusive_read_memory_128);
    assert_optional_rx_entry("write_memory_8", prelude_info.write_memory_8);
    assert_optional_rx_entry("write_memory_16", prelude_info.write_memory_16);
    assert_optional_rx_entry("write_memory_32", prelude_info.write_memory_32);
    assert_optional_rx_entry("write_memory_64", prelude_info.write_memory_64);
    assert_optional_rx_entry("write_memory_128", prelude_info.write_memory_128);
    assert_optional_rx_entry("wrapped_write_memory_8", prelude_info.wrapped_write_memory_8);
    assert_optional_rx_entry("wrapped_write_memory_16", prelude_info.wrapped_write_memory_16);
    assert_optional_rx_entry("wrapped_write_memory_32", prelude_info.wrapped_write_memory_32);
    assert_optional_rx_entry("wrapped_write_memory_64", prelude_info.wrapped_write_memory_64);
    assert_optional_rx_entry("wrapped_write_memory_128", prelude_info.wrapped_write_memory_128);
    assert_optional_rx_entry("exclusive_write_memory_8", prelude_info.exclusive_write_memory_8);
    assert_optional_rx_entry("exclusive_write_memory_16", prelude_info.exclusive_write_memory_16);
    assert_optional_rx_entry("exclusive_write_memory_32", prelude_info.exclusive_write_memory_32);
    assert_optional_rx_entry("exclusive_write_memory_64", prelude_info.exclusive_write_memory_64);
    assert_optional_rx_entry("exclusive_write_memory_128", prelude_info.exclusive_write_memory_128);
    assert_optional_rx_entry("call_svc", prelude_info.call_svc);
    assert_optional_rx_entry("exception_raised", prelude_info.exception_raised);
    assert_optional_rx_entry("dc_raised", prelude_info.dc_raised);
    assert_optional_rx_entry("ic_raised", prelude_info.ic_raised);
    assert_optional_rx_entry("isb_raised", prelude_info.isb_raised);
    assert_optional_rx_entry("get_cntpct", prelude_info.get_cntpct);
    assert_optional_rx_entry("add_ticks", prelude_info.add_ticks);
    assert_optional_rx_entry("get_ticks_remaining", prelude_info.get_ticks_remaining);
#endif
}

EmitConfig A32AddressSpace::GetEmitConfig() {
    return EmitConfig{
        .optimizations = conf.unsafe_optimizations ? conf.optimizations : conf.optimizations & all_safe_optimizations,

        .hook_isb = conf.hook_isb,

        .cntfreq_el0{},
        .ctr_el0{},
        .dczid_el0{},
        .tpidrro_el0{},
        .tpidr_el0{},

        .check_halt_on_memory_access = conf.check_halt_on_memory_access,

        .page_table_pointer = mcl::bit_cast<u64>(conf.page_table),
        .page_table_address_space_bits = 32,
        .page_table_pointer_mask_bits = conf.page_table_pointer_mask_bits,
        .silently_mirror_page_table = true,
        .absolute_offset_page_table = conf.absolute_offset_page_table,
        .detect_misaligned_access_via_page_table = conf.detect_misaligned_access_via_page_table,
        .only_detect_misalignment_via_page_table_on_page_boundary = conf.only_detect_misalignment_via_page_table_on_page_boundary,

        .fastmem_pointer = conf.fastmem_pointer,
        .recompile_on_fastmem_failure = conf.recompile_on_fastmem_failure,
        .fastmem_address_space_bits = 32,
        .silently_mirror_fastmem = true,

        .wall_clock_cntpct = conf.wall_clock_cntpct,
        .enable_cycle_counting = conf.enable_cycle_counting,

        .always_little_endian = conf.always_little_endian,

        .descriptor_to_fpcr = [](const IR::LocationDescriptor& location) { return FP::FPCR{A32::LocationDescriptor{location}.FPSCR().Value()}; },
        .emit_cond = EmitA32Cond,
        .emit_condition_failed_terminal = EmitA32ConditionFailedTerminal,
        .emit_terminal = EmitA32Terminal,
        .emit_check_memory_abort = EmitA32CheckMemoryAbort,

        .state_nzcv_offset = offsetof(A32JitState, cpsr_nzcv),
        .state_fpsr_offset = offsetof(A32JitState, fpsr),
        .state_exclusive_state_offset = offsetof(A32JitState, exclusive_state),

        .coprocessors = conf.coprocessors,

        .very_verbose_debugging_output = conf.very_verbose_debugging_output,
    };
}

void A32AddressSpace::RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) {
    const A32::LocationDescriptor descriptor{block.Location()};
    const A32::LocationDescriptor end_location{block.EndLocation()};
    const auto range = boost::icl::discrete_interval<u32>::closed(descriptor.PC(), end_location.PC() - 1);
#if defined(__SWITCH__) && defined(AZAHAR_SWITCH_DYNARMIC_VERBOSE_TEXT_LOGS)
    std::array<char, 160> buffer{};
    std::snprintf(buffer.data(), buffer.size(),
                  "guest_pc=0x%08x end_pc=0x%08x entry=0x%016llx size=%zu",
                  descriptor.PC(), end_location.PC(),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(block_info.entry_point)),
                  block_info.size);
    azahar_switch_dynarmic_jit_log_message("Dynarmic.Block", buffer.data());
#endif
    block_ranges.AddRange(range, descriptor);
}

}  // namespace Dynarmic::Backend::Arm64
