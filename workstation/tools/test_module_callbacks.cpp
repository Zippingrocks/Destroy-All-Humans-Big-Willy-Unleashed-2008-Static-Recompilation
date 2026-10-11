#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fstream>
#include <iostream>
#include <cstring>
#include <xmmintrin.h>
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"

// Execute each indirect-call/return instruction in the actual shipped module.
// The deliberately external target makes the chassis regain control immediately.
int main(int argc, char** argv)
{
  if (argc != 3 && argc != 4) return 2;
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  std::cerr << "Loading game module\n";
  HMODULE library = LoadLibraryA(argv[1]);
  if (!library) return 3;
  auto get = reinterpret_cast<const StaticRecompModuleDesc*(*)()>(
      GetProcAddress(library, "staticrecomp_get_module"));
  if (!get) return 4;
  const auto* module = get();
  std::cerr << "Module loaded; checking call sites\n";
  if (module->abi_version != STATICRECOMP_ABI_VERSION || module->cpu_state_size != sizeof(CPUState))
    return 5;
  if (argc == 4)
  {
    const unsigned original_csr=_mm_getcsr();
    const unsigned rounding[4]={0,0x6000,0x4000,0x2000};
    for (unsigned mode=0;mode<8;++mode)
    {
      CPUState cpu{};
      cpu.fpscr=mode;
      _mm_setcsr((original_csr&~0xE040u) | 0x6000u);
      module->on_state_loaded(&cpu);
      const unsigned expected=rounding[mode&3u] | ((mode&4u) ? 0x8040u : 0u);
      const unsigned actual=_mm_getcsr()&0xE040u;
      _mm_setcsr(original_csr);
      if (actual != expected)
      {
        std::cerr << "FP callback mismatch: FPSCR=" << mode << " expected=" << std::hex
                  << expected << " actual=" << actual << '\n';
        return 9;
      }
    }
    std::cout << "All 8 FPSCR rounding/denormal mode callbacks passed\n";
    return 0;
  }
  std::ifstream sites(argv[2]);
  unsigned site, raw, count=0;
  while (sites >> std::hex >> site >> raw)
  {
    if (count%1000 == 0) std::cerr << "Checking site " << std::hex << site << '\n';
    CPUState cpu{};
    cpu.ctr=0x81234567u;
    cpu.lr=0x8124567bu;
    cpu.cr=0x12345678u;
    const unsigned bo=(raw>>21)&31u, bi=(raw>>16)&31u;
    const bool lr_branch=((raw>>1)&1023u)==16u;
    // Exercise the taken path of conditional returns. All RDHE78 indirect
    // branches use BO=4,12,20; none decrement CTR.
    if (bo != 4 && bo != 12 && bo != 20) return 8;
    if (!(bo&16u))
      cpu.cr=(cpu.cr&~(1u<<(31-bi))) | ((bo&8u) ? (1u<<(31-bi)) : 0u);
    for (unsigned i=0;i<32;++i) cpu.gpr[i]=0x90000000u+i*4;
    const auto original=cpu;
    const u32 target=(lr_branch ? original.lr : original.ctr)&~3u;
    const u32 expected_lr=(raw&1u) ? site+4 : original.lr;
    if (module->dispatch(&cpu,site) != 1 || cpu.pc != target || cpu.lr != expected_lr ||
        cpu.ctr != original.ctr || cpu.cr != original.cr ||
        std::memcmp(cpu.gpr,original.gpr,sizeof(cpu.gpr)) != 0)
    {
      std::cerr << "Failed call semantics at " << std::hex << site << ": pc=" << cpu.pc
                << " lr=" << cpu.lr << '\n';
      return 6;
    }
    ++count;
  }
  FreeLibrary(library);
  if (!sites.eof() || count != 29260) return 7;
  std::cout << "Verified " << count << " indirect-call/return sites (taken path) in the actual game DLL\n";
}
