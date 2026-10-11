// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstring>
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"

// Host-injected calls must survive CPU pauses and preserve the interrupted
// caller's linkage area, which PPC callees may overwrite above their own frame.
struct GuestCallContext
{
  bool active = false;
  CPUState saved{};
  std::array<u8, 64> linkage{};

  void Begin(const CPUState& cpu, const u8* caller_linkage)
  {
    saved = cpu;
    std::memcpy(linkage.data(), caller_linkage, linkage.size());
    active = true;
  }

  bool HasReturned(const CPUState& cpu) const
  {
    return active && cpu.pc == saved.pc && cpu.gpr[1] == saved.gpr[1];
  }

  void Finish(CPUState& cpu, u8* caller_linkage)
  {
    const auto downcount = cpu.downcount;
    const auto timebase = cpu.timebase;
    cpu = saved;
    cpu.downcount = downcount;
    cpu.timebase = timebase;
    std::memcpy(caller_linkage, linkage.data(), linkage.size());
    active = false;
  }
};
