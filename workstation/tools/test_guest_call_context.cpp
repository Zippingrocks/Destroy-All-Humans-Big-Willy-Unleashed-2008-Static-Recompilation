#include <cassert>
#include <cstring>
#include <iostream>
#include "Core/PowerPC/StaticRecomp/GuestCallContext.h"

// Use the real module ABI, not a mock CPU layout. The owning object survives
// two simulated Run invocations, as it must across CPUThreadGuard pauses.
int main()
{
  GuestCallContext context;
  CPUState cpu{};
  std::array<u8,64> stack;
  for (unsigned i=0;i<stack.size();++i) stack[i]=static_cast<u8>(i+7);
  const auto original_stack=stack;
  cpu.pc=0x80012340; cpu.lr=0x80045678; cpu.gpr[1]=0x90010000;
  cpu.gpr[3]=0x12345678; cpu.fpr[1]=123.5; cpu.cr=0xABCDE;
  cpu.downcount=-17; cpu.timebase=100;
  const CPUState original=cpu;
  {
    context.Begin(cpu,stack.data());
    cpu.pc=0x80036114; cpu.lr=original.pc;
    std::memset(stack.data(),0xCC,stack.size());
  }
  assert(context.active && !context.HasReturned(cpu));
  cpu.pc=original.pc; cpu.gpr[1]-=0x100;
  assert(!context.HasReturned(cpu)); // Same PC in a nested callback is not a return.
  cpu.gpr[1]=original.gpr[1]; cpu.gpr[3]=0; cpu.fpr[1]=0; cpu.cr=0;
  cpu.downcount=-90; cpu.timebase=173;
  assert(context.HasReturned(cpu));
  context.Finish(cpu,stack.data());
  assert(!context.active && stack==original_stack);
  CPUState expected=original; expected.downcount=-90; expected.timebase=173;
  assert(std::memcmp(&cpu,&expected,sizeof(cpu))==0);
  context.Begin(cpu,stack.data());
  context.active=false; // Loading another checkpoint cancels the host-only call.
  assert(!context.HasReturned(cpu));
  std::cout << "Guest callback pause, nested return, ABI restoration, linkage and timing tests passed\n";
}
