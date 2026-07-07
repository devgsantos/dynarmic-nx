/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include "dynarmic/backend/arm64/address_space.h"
#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/interface/A32/config.h"

#if defined(__SWITCH__)
#include "dynarmic/backend/arm64/devirtualize.h"
#endif

namespace Dynarmic::Backend::Arm64 {

struct EmittedBlockInfo;

class A32AddressSpace final : public AddressSpace {
public:
    explicit A32AddressSpace(const A32::UserConfig& conf);

    IR::Block GenerateIR(IR::LocationDescriptor) const override;

    void InvalidateCacheRanges(const boost::icl::interval_set<u32>& ranges);

protected:
    friend class A32Core;

    void EmitPrelude();
    EmitConfig GetEmitConfig() override;
    void RegisterNewBasicBlock(IR::LocationDescriptor location,
                               IR::LocationDescriptor end_location,
                               const EmittedBlockInfo& block_info) override;

    const A32::UserConfig conf;
#if defined(__SWITCH__)
    DevirtualizedCall host_get_ticks_remaining;
    DevirtualizedCall host_add_ticks;

    u64 HostGetTicksRemaining() const;
    void HostAddTicks(u64 ticks) const;
    u64 HostGetTicksRemainingTarget() const;
    u64 HostAddTicksTarget() const;
#endif
    BlockRangeInformation<u32> block_ranges;
};

}  // namespace Dynarmic::Backend::Arm64
