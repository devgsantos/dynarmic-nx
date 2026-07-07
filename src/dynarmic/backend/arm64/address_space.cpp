/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <array>
#include <cstdint>
#include <cstdio>

#include <mcl/bit_cast.hpp>

#include "dynarmic/backend/arm64/a64_address_space.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace Dynarmic::Backend::Arm64 {

#if defined(__SWITCH__)
extern "C" void azahar_switch_dynarmic_jit_log_message(
    const char* tag, const char* message) noexcept;

namespace {

thread_local std::uint32_t switch_emit_log_count = 0;

void LogSwitchEmit(const char* phase, std::uintptr_t guest_pc, std::uintptr_t entry_point,
                   std::size_t size) {
    if (switch_emit_log_count >= 64) {
        return;
    }
    ++switch_emit_log_count;
    std::array<char, 192> buffer{};
    std::snprintf(buffer.data(), buffer.size(),
                  "%s guest_pc=0x%08x entry=0x%016llx size=%zu",
                  phase != nullptr ? phase : "unknown",
                  static_cast<unsigned>(guest_pc),
                  static_cast<unsigned long long>(entry_point), size);
    azahar_switch_dynarmic_jit_log_message("Dynarmic.Emit", buffer.data());
}

} // namespace
#endif

AddressSpace::AddressSpace(size_t code_cache_size)
        : code_cache_size(code_cache_size)
        , mem(code_cache_size)
        , code(mem.wptr(), mem.xptr())
        , fastmem_manager(exception_handler) {
    ASSERT_MSG(code_cache_size <= 128 * 1024 * 1024, "code_cache_size > 128 MiB not currently supported");

    // Horizon does not use Dynarmic's POSIX signal/fastmem exception path.
    // Switch builds use page-table/callback memory access instead.
#if !defined(__SWITCH__)
    exception_handler.Register(mem, code_cache_size);
    exception_handler.SetFastmemCallback([this](u64 host_pc) {
        return FastmemCallback(host_pc);
    });
#endif
}

AddressSpace::~AddressSpace() = default;

CodePtr AddressSpace::Get(IR::LocationDescriptor descriptor) {
    if (const auto iter = block_entries.find(descriptor); iter != block_entries.end()) {
        return iter->second;
    }
    return nullptr;
}

std::optional<IR::LocationDescriptor> AddressSpace::ReverseGetLocation(CodePtr host_pc) {
    if (auto iter = reverse_block_entries.upper_bound(host_pc); iter != reverse_block_entries.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->second;
    }
    return std::nullopt;
}

CodePtr AddressSpace::ReverseGetEntryPoint(CodePtr host_pc) {
    if (auto iter = reverse_block_entries.upper_bound(host_pc); iter != reverse_block_entries.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->first;
    }
    return nullptr;
}

CodePtr AddressSpace::GetOrEmit(IR::LocationDescriptor descriptor) {
    if (CodePtr block_entry = Get(descriptor)) {
#if defined(__SWITCH__)
        LogSwitchEmit("cache-hit", 0, reinterpret_cast<std::uintptr_t>(block_entry), 0);
#endif
        return block_entry;
    }

#if defined(__SWITCH__)
    LogSwitchEmit("emit-begin", 0, 0, 0);
#endif
    IR::Block ir_block = GenerateIR(descriptor);
    const EmittedBlockInfo block_info = Emit(std::move(ir_block));
#if defined(__SWITCH__)
    LogSwitchEmit("emit-end", 0, reinterpret_cast<std::uintptr_t>(block_info.entry_point),
                  block_info.size);
#endif
    return block_info.entry_point;
}

void AddressSpace::InvalidateBasicBlocks(const tsl::robin_set<IR::LocationDescriptor>& descriptors) {
    UnprotectCodeMemory();

    for (const auto& descriptor : descriptors) {
        const auto iter = block_entries.find(descriptor);
        if (iter == block_entries.end()) {
            continue;
        }

        // Unlink before removal because InvalidateBasicBlocks can be called within a fastmem callback,
        // and the currently executing block may have references to itself which need to be unlinked.
        RelinkForDescriptor(descriptor, nullptr);

        block_entries.erase(iter);
    }

    ProtectCodeMemory();
}

void AddressSpace::ClearCache() {
    block_entries.clear();
    reverse_block_entries.clear();
    block_infos.clear();
    block_references.clear();
    code.set_offset(prelude_info.end_of_prelude);
}

void AddressSpace::DumpDisassembly() const {
    u32* write_ptr = mem.wptr();
    for (u32* exec_ptr = mem.xptr(); exec_ptr < code.xptr<u32*>();
         ++write_ptr, ++exec_ptr) {
        std::printf("%s", Common::DisassembleAArch64(*write_ptr,
                                                       mcl::bit_cast<u64>(exec_ptr))
                              .c_str());
    }
}

size_t AddressSpace::GetRemainingSize() {
    return code_cache_size - static_cast<size_t>(code.offset());
}

EmittedBlockInfo AddressSpace::Emit(IR::Block block) {
    if (GetRemainingSize() < 1024 * 1024) {
        ClearCache();
    }

    UnprotectCodeMemory();

    EmittedBlockInfo block_info = EmitArm64(code, std::move(block), GetEmitConfig(), fastmem_manager);

    Link(block_info);

    mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);
    ProtectCodeMemory();

    const auto rx_base = reinterpret_cast<std::uintptr_t>(mem.xptr());
    const auto entry = reinterpret_cast<std::uintptr_t>(block_info.entry_point);
    ASSERT(entry >= rx_base);
    ASSERT(entry < rx_base + code_cache_size);

    ASSERT(block_entries.insert({block.Location(), block_info.entry_point}).second);
    ASSERT(reverse_block_entries.insert({block_info.entry_point, block.Location()}).second);
    ASSERT(block_infos.insert({block_info.entry_point, block_info}).second);

    UnprotectCodeMemory();
    RelinkForDescriptor(block.Location(), block_info.entry_point);
    ProtectCodeMemory();

    RegisterNewBasicBlock(block, block_info);

#if defined(__SWITCH__)
    LogSwitchEmit("registered", 0, reinterpret_cast<std::uintptr_t>(block_info.entry_point),
                  block_info.size);
#endif

    return block_info;
}

void AddressSpace::Link(EmittedBlockInfo& block_info) {
    using namespace oaknut;
    using namespace oaknut::util;

    for (auto [ptr_offset, target] : block_info.relocations) {
        CodeGenerator c{mem.wptr(), mem.xptr()};
        c.set_xptr(reinterpret_cast<u32*>(block_info.entry_point + ptr_offset));

        switch (target) {
        case LinkTarget::ReturnToDispatcher:
            c.B(prelude_info.return_to_dispatcher);
            break;
        case LinkTarget::ReturnFromRunCode:
            c.B(prelude_info.return_from_run_code);
            break;
        case LinkTarget::ReadMemory8:
            c.BL(prelude_info.read_memory_8);
            break;
        case LinkTarget::ReadMemory16:
            c.BL(prelude_info.read_memory_16);
            break;
        case LinkTarget::ReadMemory32:
            c.BL(prelude_info.read_memory_32);
            break;
        case LinkTarget::ReadMemory64:
            c.BL(prelude_info.read_memory_64);
            break;
        case LinkTarget::ReadMemory128:
            c.BL(prelude_info.read_memory_128);
            break;
        case LinkTarget::WrappedReadMemory8:
            c.BL(prelude_info.wrapped_read_memory_8);
            break;
        case LinkTarget::WrappedReadMemory16:
            c.BL(prelude_info.wrapped_read_memory_16);
            break;
        case LinkTarget::WrappedReadMemory32:
            c.BL(prelude_info.wrapped_read_memory_32);
            break;
        case LinkTarget::WrappedReadMemory64:
            c.BL(prelude_info.wrapped_read_memory_64);
            break;
        case LinkTarget::WrappedReadMemory128:
            c.BL(prelude_info.wrapped_read_memory_128);
            break;
        case LinkTarget::ExclusiveReadMemory8:
            c.BL(prelude_info.exclusive_read_memory_8);
            break;
        case LinkTarget::ExclusiveReadMemory16:
            c.BL(prelude_info.exclusive_read_memory_16);
            break;
        case LinkTarget::ExclusiveReadMemory32:
            c.BL(prelude_info.exclusive_read_memory_32);
            break;
        case LinkTarget::ExclusiveReadMemory64:
            c.BL(prelude_info.exclusive_read_memory_64);
            break;
        case LinkTarget::ExclusiveReadMemory128:
            c.BL(prelude_info.exclusive_read_memory_128);
            break;
        case LinkTarget::WriteMemory8:
            c.BL(prelude_info.write_memory_8);
            break;
        case LinkTarget::WriteMemory16:
            c.BL(prelude_info.write_memory_16);
            break;
        case LinkTarget::WriteMemory32:
            c.BL(prelude_info.write_memory_32);
            break;
        case LinkTarget::WriteMemory64:
            c.BL(prelude_info.write_memory_64);
            break;
        case LinkTarget::WriteMemory128:
            c.BL(prelude_info.write_memory_128);
            break;
        case LinkTarget::WrappedWriteMemory8:
            c.BL(prelude_info.wrapped_write_memory_8);
            break;
        case LinkTarget::WrappedWriteMemory16:
            c.BL(prelude_info.wrapped_write_memory_16);
            break;
        case LinkTarget::WrappedWriteMemory32:
            c.BL(prelude_info.wrapped_write_memory_32);
            break;
        case LinkTarget::WrappedWriteMemory64:
            c.BL(prelude_info.wrapped_write_memory_64);
            break;
        case LinkTarget::WrappedWriteMemory128:
            c.BL(prelude_info.wrapped_write_memory_128);
            break;
        case LinkTarget::ExclusiveWriteMemory8:
            c.BL(prelude_info.exclusive_write_memory_8);
            break;
        case LinkTarget::ExclusiveWriteMemory16:
            c.BL(prelude_info.exclusive_write_memory_16);
            break;
        case LinkTarget::ExclusiveWriteMemory32:
            c.BL(prelude_info.exclusive_write_memory_32);
            break;
        case LinkTarget::ExclusiveWriteMemory64:
            c.BL(prelude_info.exclusive_write_memory_64);
            break;
        case LinkTarget::ExclusiveWriteMemory128:
            c.BL(prelude_info.exclusive_write_memory_128);
            break;
        case LinkTarget::CallSVC:
            c.BL(prelude_info.call_svc);
            break;
        case LinkTarget::ExceptionRaised:
            c.BL(prelude_info.exception_raised);
            break;
        case LinkTarget::InstructionSynchronizationBarrierRaised:
            c.BL(prelude_info.isb_raised);
            break;
        case LinkTarget::InstructionCacheOperationRaised:
            c.BL(prelude_info.ic_raised);
            break;
        case LinkTarget::DataCacheOperationRaised:
            c.BL(prelude_info.dc_raised);
            break;
        case LinkTarget::GetCNTPCT:
            c.BL(prelude_info.get_cntpct);
            break;
        case LinkTarget::AddTicks:
            c.BL(prelude_info.add_ticks);
            break;
        case LinkTarget::GetTicksRemaining:
            c.BL(prelude_info.get_ticks_remaining);
            break;
        default:
            ASSERT_FALSE("Invalid relocation target");
        }
    }

    for (auto [target_descriptor, list] : block_info.block_relocations) {
        block_references[target_descriptor].insert(block_info.entry_point);
        LinkBlockLinks(block_info.entry_point, Get(target_descriptor), list);
    }
}

void AddressSpace::LinkBlockLinks(const CodePtr entry_point, const CodePtr target_ptr, const std::vector<BlockRelocation>& block_relocations_list) {
    using namespace oaknut;
    using namespace oaknut::util;

    for (auto [ptr_offset, type] : block_relocations_list) {
        CodeGenerator c{mem.wptr(), mem.xptr()};
        c.set_xptr(reinterpret_cast<u32*>(entry_point + ptr_offset));

        switch (type) {
        case BlockRelocationType::Branch:
            if (target_ptr) {
                c.B((void*)target_ptr);
            } else {
                c.NOP();
            }
            break;
        case BlockRelocationType::MoveToScratch1:
            if (target_ptr) {
                c.ADRL(Xscratch1, (void*)target_ptr);
            } else {
                c.ADRL(Xscratch1, prelude_info.return_to_dispatcher);
            }
            break;
        default:
            ASSERT_FALSE("Invalid BlockRelocationType");
        }
    }
}

void AddressSpace::RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr) {
    for (auto code_ptr : block_references[target_descriptor]) {
        if (auto block_iter = block_infos.find(code_ptr); block_iter != block_infos.end()) {
            const EmittedBlockInfo& block_info = block_iter->second;

            if (auto relocation_iter = block_info.block_relocations.find(target_descriptor); relocation_iter != block_info.block_relocations.end()) {
                LinkBlockLinks(block_info.entry_point, target_ptr, relocation_iter->second);
            }

            mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);
        }
    }
}

FakeCall AddressSpace::FastmemCallback(u64 host_pc) {
    {
        const auto host_ptr = mcl::bit_cast<CodePtr>(host_pc);

        const auto entry_point = ReverseGetEntryPoint(host_ptr);
        if (!entry_point) {
            goto fail;
        }

        const auto block_info = block_infos.find(entry_point);
        if (block_info == block_infos.end()) {
            goto fail;
        }

        const auto patch_entry = block_info->second.fastmem_patch_info.find(host_ptr - entry_point);
        if (patch_entry == block_info->second.fastmem_patch_info.end()) {
            goto fail;
        }

        const auto fc = patch_entry->second.fc;

        if (patch_entry->second.recompile) {
            const auto marker = patch_entry->second.marker;
            fastmem_manager.MarkDoNotFastmem(marker);
            InvalidateBasicBlocks({std::get<0>(marker)});
        }

        return fc;
    }

fail:
    fmt::print("dynarmic: Segfault happened within JITted code at host_pc = {:016x}\n", host_pc);
    fmt::print("Segfault wasn't at a fastmem patch location!\n");
    ASSERT_FALSE("segfault");
}

}  // namespace Dynarmic::Backend::Arm64
