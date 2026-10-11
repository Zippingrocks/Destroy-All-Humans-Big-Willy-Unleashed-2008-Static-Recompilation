// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/StaticRecomp/BigWillyControls.h"
#include "Core/PowerPC/StaticRecomp/BigWillyConsole.h"
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/MMU.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/PixelEngine.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <bit>
#include <array>
#include <chrono>
#include <cstring>
#include <unordered_set>

namespace
{
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);

struct FileCloser
{
  void operator()(std::FILE* file) const
  {
    if (file)
      std::fclose(file);
  }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr OpenDispatchTrace()
{
  const char* path = std::getenv("STATICRECOMP_TRACE_FILE");
  if (!path || !*path)
    return {};

  FilePtr file(std::fopen(path, "w"));
  if (file)
  {
    std::fprintf(file.get(), "dispatch,pc,lr,ctr,cr,timebase,ppc_downcount,sp,r3,r13,r30,r31,cp_read,cp_write,cp_distance,cp_breakpoint,cp_bp_hit,cp_bp_enabled,cp_read_enabled,pe_token,pe_control,pi_cause,pi_mask\n");
    std::fflush(file.get());
  }
  return file;
}
}

void StaticRecompCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();
  FilePtr dispatch_trace = OpenDispatchTrace();
  const char* callback_audit_path = std::getenv("STATICRECOMP_CALLBACK_AUDIT");
  FilePtr callback_audit(callback_audit_path && *callback_audit_path ?
                           std::fopen(callback_audit_path, "a") : nullptr);
  std::unordered_set<u64> callback_edges;
  if (callback_audit)
  {
    std::fseek(callback_audit.get(), 0, SEEK_END);
    if (std::ftell(callback_audit.get()) == 0)
      std::fprintf(callback_audit.get(), "kind,site,target,native_verified\n");
  }

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);
  const bool lockstep_enabled = m_lockstep_verifier->IsEnabled();
  // Native-dispatch numbering is part of lockstep's windowing and the two
  // sampling modes. In a normal gameplay run none of those consumers is
  // active, so fast-stats mode can omit the counter store as well.
  const bool count_native_dispatches = !m_fast_diagnostics || lockstep_enabled ||
                                      dispatch_trace || m_collect_dispatch_samples;
  const auto fast_dispatchable_at = [this](u32 address) {
    if (m_has_rel_modules || !m_forced_fallback_ranges.empty())
      return FastDispatchableAt(address);
    if (!m_module_active || m_chunk_lookup_table.empty())
      return false;

    int lookup_index = -1;
    if (address >= 0x80000000u && address < 0x80000000u + m_lookup_ram_size)
    {
      lookup_index = static_cast<int>((address - 0x80000000u) >> 2);
    }
    else if (address >= 0x90000000u && address < 0x90000000u + m_lookup_exram_size)
    {
      lookup_index = static_cast<int>((m_lookup_ram_size >> 2) + ((address - 0x90000000u) >> 2));
    }
    if (lookup_index < 0 || lookup_index >= static_cast<int>(m_chunk_lookup_table.size()))
      return false;
    const int chunk = m_chunk_lookup_table[lookup_index];
    return chunk >= 0 && m_chunk_state[chunk] == CHUNK_VERIFIED;
  };

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  bool recover_completed_token = initial_game_id == "RDHE78" &&
      std::getenv("BIGWILLY_RECOVER_COMPLETED_TOKEN") != nullptr;
  const bool bigwilly_poll_yield = initial_game_id == "RDHE78" &&
      std::getenv("BIGWILLY_DISABLE_POLL_YIELD") == nullptr;
  const bool bigwilly_menu_60 = initial_game_id == "RDHE78" &&
      std::getenv("BIGWILLY_DISABLE_MENU_60") == nullptr;
  // Diagnostic escape hatch for the Wii dynamic-vertex-buffer lock poll. The
  // exact call site is guarded below and remains disabled unless explicitly
  // requested, so normal runs preserve the original GPU-resource semantics.
  const bool bigwilly_skip_lock_poll = initial_game_id == "RDHE78" &&
      std::getenv("BIGWILLY_SKIP_LOCK_POLL") != nullptr;
  // RDHE78 calls these two tiny privileged helpers extremely frequently. Keep
  // the shortcut opt-in while it is being validated against the generated
  // code and interrupt timing; the normal module dispatch remains the default.
  const bool bigwilly_fast_os_interrupts = initial_game_id == "RDHE78" &&
      std::getenv("BIGWILLY_FAST_OS_INTERRUPTS") != nullptr;
  bool bigwilly_timing_applied = false;
  u64 bigwilly_poll_wake_tick = 0;
  u32 bigwilly_poll_stack = 0;
  u32 bigwilly_lock_debug_hits = 0;
  u32 bigwilly_video_debug_hits = 0;
  u32 bigwilly_other_sync_debug_hits = 0;
  const auto read_ram_word = [this](u32 address) {
    const u8* p = m_guest.ram + address - 0x80000000u;
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
  };
  const auto read_guest_word = [this](u32 address) {
    const u8* p = nullptr;
    if (address >= 0x80000000u &&
        address - 0x80000000u + 4u <= m_guest.ram_size)
    {
      p = m_guest.ram + address - 0x80000000u;
    }
    else if (address >= 0x90000000u &&
             address - 0x90000000u + 4u <= m_guest.exram_size)
    {
      p = m_guest.exram + address - 0x90000000u;
    }
    if (!p)
      return 0u;
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
  };
  const auto read_guest_byte = [this](u32 address) {
    if (address >= 0x80000000u && address - 0x80000000u < m_guest.ram_size)
      return m_guest.ram[address - 0x80000000u];
    if (address >= 0x90000000u && address - 0x90000000u < m_guest.exram_size)
      return m_guest.exram[address - 0x90000000u];
    return static_cast<u8>(0);
  };
  const auto write_ram_word = [this](u32 address, u32 value) {
    u8* p = m_guest.ram + address - 0x80000000u;
    p[0] = static_cast<u8>(value >> 24);
    p[1] = static_cast<u8>(value >> 16);
    p[2] = static_cast<u8>(value >> 8);
    p[3] = static_cast<u8>(value);
  };
  const auto bigwilly_frontend_mode = [&] {
    // ELF symbols: Game::spGameMode, CGameModeNull and CDAHGameModeShell.
    const u32 mode = read_ram_word(0x8053A640u);
    if (mode == 0)
      return true;
    const u8* object = nullptr;
    if (mode >= 0x80000000u && mode - 0x80000000u + 4u <= m_guest.ram_size)
      object = m_guest.ram + mode - 0x80000000u;
    else if (mode >= 0x90000000u && mode - 0x90000000u + 4u <= m_guest.exram_size)
      object = m_guest.exram + mode - 0x90000000u;
    if (!object)
      return false;
    const u32 vtable = (u32(object[0]) << 24) | (u32(object[1]) << 16) |
                       (u32(object[2]) << 8) | object[3];
    return vtable == 0x803FD4F8u || vtable == 0x80442244u;
  };
  if (initial_game_id == "RDHE78" && m_idle_pc == 0 &&
      read_ram_word(0x803654F0u) == 0x800DB9F0u)
    m_idle_pc = 0x803654F0u;
  const auto after_mtmsr = [this](u32 pc) {
    if (pc < 4u || (pc & 3u) != 0)
      return false;

    const u32 instruction_pc = pc - 4u;
    const u8* code = nullptr;
    if (instruction_pc >= 0x80000000u &&
        instruction_pc - 0x80000000u + 4u <= m_guest.ram_size)
    {
      code = m_guest.ram + instruction_pc - 0x80000000u;
    }
    else if (instruction_pc >= 0x90000000u &&
             instruction_pc - 0x90000000u + 4u <= m_guest.exram_size)
    {
      code = m_guest.exram + instruction_pc - 0x90000000u;
    }
    if (!code)
      return false;

    const u32 raw = static_cast<u32>(code[0]) << 24 | static_cast<u32>(code[1]) << 16 |
                    static_cast<u32>(code[2]) << 8 | code[3];
    return (raw & 0xFC0007FEu) == 0x7C000124u;
  };

  // Escape requests a guest-level skip rather than shutting down the host.
  // These are the game's verified RDHE78 entry points:
  //   0x80036114: Lua cutscene_skip_to_end()
  //   0x8031190C: Pcpl::VideoPlayer::Stop()
  // The call is injected only at a game/video update entry, and the full
  // CPU register state is restored after the guest call. This preserves the
  // interrupted game instruction stream while retaining teardown side effects.
  constexpr u32 bigwilly_cutscene_skip = 0x80036114u;
  constexpr u32 bigwilly_video_stop = 0x8031190Cu;
  auto& bigwilly_skip_active = m_bigwilly_skip.active;
  auto& bigwilly_skip_phase = m_bigwilly_skip_phase;
  auto& bigwilly_skip_resume_pc = m_bigwilly_skip.saved.pc;
  auto& bigwilly_video_player = m_bigwilly_video_player;
  auto& bigwilly_stop_video = m_bigwilly_stop_video;
  // Inject at the local player's update entry, on the game's own thread.
  // Preserve the caller's linkage/argument area as well as registers: guest
  // callees may write above SP even when they allocate their own stack frame.
  constexpr u32 bigwilly_player_update = 0x801617BCu;
  constexpr u32 bigwilly_set_avatar = 0x80161878u;
  constexpr std::array<u32, 14> weapon_ids{0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, 19, 20, 21};
  const bool bigwilly_pc_controls = initial_game_id == "RDHE78" &&
      std::getenv("BIGWILLY_PC_CONTROLS") && std::string_view(std::getenv("BIGWILLY_PC_CONTROLS")) == "1";
  const bool bigwilly_full_world = initial_game_id == "RDHE78" &&
      std::getenv("BIGWILLY_FULL_WORLD") && std::string_view(std::getenv("BIGWILLY_FULL_WORLD")) == "1";
  bool full_world_logged = false;
  const bool bigwilly_max_detail = initial_game_id == "RDHE78" &&
      std::getenv("BIGWILLY_MAX_DETAIL") && std::string_view(std::getenv("BIGWILLY_MAX_DETAIL")) == "1";
  bool max_detail_logged = false;
  auto& bigwilly_enter_pending = m_bigwilly_enter_pending;
  auto& bigwilly_enter_active = m_bigwilly_enter_active;
  auto& bigwilly_enter_saved = m_bigwilly_enter_saved;
  auto& bigwilly_enter_linkage = m_bigwilly_enter_linkage;
  auto& bigwilly_enter_deadline = m_bigwilly_enter_deadline;
  const auto bigwilly_guest_span = [this](u32 address, std::size_t count) -> u8* {
    if (address >= 0x80000000u && address - 0x80000000u <= m_guest.ram_size &&
        count <= m_guest.ram_size - (address - 0x80000000u))
      return m_guest.ram + address - 0x80000000u;
    if (address >= 0x90000000u && address - 0x90000000u <= m_guest.exram_size &&
        count <= m_guest.exram_size - (address - 0x90000000u))
      return m_guest.exram + address - 0x90000000u;
    return nullptr;
  };
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (!m_module_active && m_fallback_jit && !m_guest.host_call)
  {
    m_fallback_jit->Run();
    return;
  }

  const FastDispatchFn native_dispatch =
      m_fast_dispatch ? m_fast_dispatch : (m_module ? m_module->dispatch : nullptr);

  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();
    const std::string current_game_id = SConfig::GetInstance().GetGameID();
    m_module_active = m_module && (current_game_id.empty() || current_game_id == m_module->game_id);

    do
    {
      // Poll the verified frontend timer at most 1 kHz.
      // Hardware events and their interrupt handlers still run normally.
      if (bigwilly_poll_wake_tick && ppc.pc == 0x80022440u &&
          ppc.gpr[1] == bigwilly_poll_stack && ppc.msr.EE)
      {
        if (core_timing.GetTicks() < bigwilly_poll_wake_tick)
        {
          core_timing.Idle();
          continue;
        }
        bigwilly_poll_wake_tick = 0;
      }
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && DispatchableAt(ppc.pc) &&
          !(m_guest.host_call && IsHostCallAddress(ppc.pc)))
      {
        SyncIn();
        if (!bigwilly_enter_active && !bigwilly_enter_pending &&
            (m_bigwilly_command = BigWillyControls::g_game_command.exchange(0)) != 0)
        {
          bigwilly_enter_pending = m_module && std::string_view(m_module->game_id) == "RDHE78" && !bigwilly_enter_active;
          std::fprintf(stderr, "[bigwilly] game command=%u queued=%d game=%s\n", m_bigwilly_command, bigwilly_enter_pending ? 1 : 0, current_game_id.c_str());
          bigwilly_enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        if (bigwilly_enter_pending && std::chrono::steady_clock::now() > bigwilly_enter_deadline)
        {
          bigwilly_enter_pending = false;
          BigWillyConsole::Log("Command unavailable here: enter single-player gameplay first.");
          std::fprintf(stderr, "[bigwilly] mech request expired before player update\n");
        }
        if (recover_completed_token && m_guest.pc == 0x802F8C28u &&
            m_system.GetCommandProcessor().GetFifo().bFF_Breakpoint.load())
        {
          auto& pe = m_system.GetPixelEngine();
          const u16 completed = pe.GetTokenForDiagnostics();
          const u32 last_address = m_guest.gpr[13] - 0x4A3Cu;
          const u16 acknowledged = static_cast<u16>(read_guest_word(last_address) >> 16);
          if (static_cast<u16>(completed - acknowledged) > 0 &&
              static_cast<u16>(completed - acknowledged) < 16)
          {
            // Diagnostic replay only: redeliver an already completed hardware
            // token from a checkpoint captured after its interrupt was lost.
            // No GPU work is marked complete and no resource wait is bypassed.
            std::fprintf(stderr, "[bigwilly] replay completed GPU token %04x (acknowledged %04x)\n",
                         completed, acknowledged);
            pe.SetToken(completed, true, 0);
            recover_completed_token = false;
          }
        }
        if (!m_fast_diagnostics)
          ++m_bursts;
        do
        {
        // Inject before Game::_update (after NetGame::Update), or at VideoPlayer::Update.
        // Interrupting Play/Render can tear down a movie while its stack is live.
        if (initial_game_id == "RDHE78" && !bigwilly_skip_active && !bigwilly_enter_active &&
            (m_guest.pc == 0x80021EF8u || m_guest.pc == 0x80311BACu) &&
            (m_guest.msr & 0x8000u) && bigwilly_guest_span(m_guest.gpr[1], 64) &&
            BigWillyControls::ConsumeSkipMovieRequest())
        {
          const u32 cutscene = read_guest_word(m_guest.gpr[13] - 22720u);
          bigwilly_video_player = read_guest_word(m_guest.gpr[13] - 21944u);
          bigwilly_stop_video = bigwilly_video_player != 0 &&
                                read_guest_byte(bigwilly_video_player + 4u) != 0;

          if (cutscene != 0 || bigwilly_stop_video)
          {
            m_bigwilly_skip.Begin(m_guest, bigwilly_guest_span(m_guest.gpr[1], 64));
            bigwilly_skip_phase = cutscene != 0 ? 1u : 2u;
            m_guest.lr = bigwilly_skip_resume_pc;
            if (bigwilly_skip_phase == 1u)
            {
              m_guest.pc = bigwilly_cutscene_skip;
              std::fprintf(stderr, "[bigwilly] Escape: ending active cutscene\n");
            }
            else
            {
              m_guest.gpr[3] = bigwilly_video_player;
              m_guest.pc = bigwilly_video_stop;
              std::fprintf(stderr, "[bigwilly] Escape: stopping active intro movie\n");
            }
          }
          else
          {
            std::fprintf(stderr, "[bigwilly] Escape: no active movie/cutscene\n");
          }
        }
        // CMesh::PostCull, after Camera::GetForward. Zero only its local
        // camera-to-mesh distance vector so the original selector chooses LOD 0.
        // Keep native index switching, shadow attachment and animation updates.
        if (bigwilly_max_detail && m_guest.pc == 0x80038260u)
        {
          if (u8* distance = bigwilly_guest_span(m_guest.gpr[1] + 0x10u, 12))
          {
            std::memset(distance, 0, 12);
            if (!max_detail_logged)
            {
              std::fprintf(stderr, "[bigwilly] distance-based mesh selection: highest detail enabled\n");
              max_detail_logged = true;
            }
          }
        }
        if (bigwilly_full_world)
        {
          if (m_guest.pc == 0x802F9C90u && m_guest.lr == 0x8004017Cu &&
              bigwilly_guest_span(m_guest.gpr[3], 0x74u))
          {
            const auto far_plane = bigwilly_guest_span(m_guest.gpr[3] + 0x70u, 4);
            if (!full_world_logged)
            {
              std::fprintf(stderr, "[bigwilly] full world camera=%08x far clip %.1f -> 20000\n",
                           m_guest.gpr[3], std::bit_cast<float>(read_guest_word(m_guest.gpr[3] + 0x70u)));
              full_world_logged = true;
            }
            constexpr u8 distance[4] = {0x46, 0x9C, 0x40, 0x00};  // 20000.0f, big endian
            std::memcpy(far_plane, distance, sizeof(distance));
          }
          // Return from the distance sqrt inside CalculateObjectFade: retain
          // normal frustum and visibility rules, remove distance-only fading.
          if (m_guest.pc == 0x80050558u)
            m_guest.fpr[1] = 0.0;
        }
        if (bigwilly_pc_controls && m_guest.pc == 0x8016E500u && m_guest.gpr[3] == 0)
          m_guest.pc = 0x8016E334u;  // UFOTurnLeftRight -> Crypto's pointer camera axis
        if (initial_game_id == "RDHE78" && m_guest.pc == 0x8014789Cu &&
            (m_guest.gpr[4] == 0x3Du ||
             m_guest.gpr[4] == 0x2Bu || m_guest.gpr[4] == 0x2Du) &&
            BigWillyControls::ConsumeInteract())
        {
          // Contextual enter/exit action; the caller retains its proximity,
          // landing-pad, and animation checks. One E press is consumed once.
          std::fprintf(stderr, "[bigwilly] E interaction action=%u\n", m_guest.gpr[4]);
          m_guest.gpr[3] = 1;
          m_guest.pc = m_guest.lr & ~3u;
          m_guest.downcount -= 2;
        }
        if (!bigwilly_enter_active && !bigwilly_enter_pending && !bigwilly_skip_active &&
            m_guest.pc == bigwilly_player_update && read_guest_word(m_guest.gpr[3] + 4u) == 2u &&
            BigWillyControls::ConsumeInteract())
        {
          // A forced mech entry may occur before the tutorial enables its
          // regular exit action. E still uses the native dismount sequence.
          m_bigwilly_command = 4u;
          bigwilly_enter_pending = true;
          bigwilly_enter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        if (bigwilly_enter_pending && !bigwilly_skip_active &&
            m_guest.pc == bigwilly_player_update && (m_guest.msr & 0x8000u))
        {
          bigwilly_enter_pending = false;
          const u32 player = m_guest.gpr[3];
          const u32 manager = read_guest_word(m_guest.gpr[13] - 0x4E08u);
          const u32 willy = manager ? read_guest_word(manager + 4u) : 0u;
          const u32 mode = read_guest_word(player + 4u);
          const u32 stack = m_guest.gpr[1];
          const unsigned command_kind = m_bigwilly_command & 0xFFu;
          const bool in_cutscene = read_guest_word(m_guest.gpr[13] - 22720u) != 0;
          m_bigwilly_inventory = read_guest_word(m_guest.gpr[13] - 0x5240u);
          const bool valid = bigwilly_guest_span(player, 0x50u) &&
                             ((command_kind != 1u && command_kind != 4u) || bigwilly_guest_span(willy, 0x830u)) &&
                             (command_kind != 2u || bigwilly_guest_span(m_bigwilly_inventory, 0x3520u)) &&
                             bigwilly_guest_span(stack - 0x1000u, 0x1000u + bigwilly_enter_linkage.size()) &&
                             read_guest_word(m_guest.gpr[13] - 0x4E48u) <= 1u && mode <= 2u;
          if (in_cutscene)
          {
            BigWillyConsole::Log("Finish or skip the cutscene before using this command.");
            std::fprintf(stderr, "[bigwilly] command rejected during cutscene\n");
          }
          else if (!valid)
          {
            BigWillyConsole::Log("Command unavailable: player or requested game objects are not ready.");
            std::fprintf(stderr, "[bigwilly] entry rejected player=%08x manager=%08x willy=%08x mode=%u\n", player, manager, willy, mode);
          }
          else if (command_kind == 1u && mode == 2u)
          {
            BigWillyConsole::Log("Already piloting Big Willy.");
          }
          else if (command_kind == 4u && (read_guest_word(willy + 0x1ECu) & 0x20000008u))
          {
            BigWillyConsole::Log("Big Willy is already exiting or unavailable.");
          }
          else
          {
            bigwilly_enter_saved = m_guest;
            std::memcpy(bigwilly_enter_linkage.data(), bigwilly_guest_span(stack, bigwilly_enter_linkage.size()), bigwilly_enter_linkage.size());
            bigwilly_enter_active = true;
            m_bigwilly_enter_phase = 1u;
            m_bigwilly_enter_willy = willy;
            // Use the game's own availability and charge functions, then its
            // normal avatar switch to establish controls, camera, and HUD.
            m_guest.gpr[3] = manager;
            m_guest.gpr[4] = 1u;
            m_guest.lr = bigwilly_player_update;
            m_guest.pc = 0x8016EBCCu;  // SetSwitchToWillyAllowed(true)
            if (command_kind == 2u)
            {
              m_bigwilly_enter_phase = 0;
              m_guest.gpr[3] = m_bigwilly_inventory;
              m_guest.gpr[4] = weapon_ids[0];
              m_guest.gpr[5] = 1;
              m_guest.gpr[6] = 0;
              m_guest.pc = 0x800A1248u;
            }
            else if (command_kind == 3u)
            {
              // Immutable names from CMapID::GetStreamNameFromMapID; RequestLoadLevel
              // copies the string and queues travel for the game's next frame.
              constexpr u32 level_names[] = {0x80423968u, 0x80423988u, 0x804239A8u, 0x804239C8u};
              m_guest.gpr[3] = level_names[(m_bigwilly_command >> 8) & 3u];
              m_guest.gpr[4] = 0x804426B0u;  // loading_screenNavicom.stream
              m_guest.gpr[5] = 0;
              m_guest.pc = 0x80021DF0u;
              m_bigwilly_enter_phase = 4;
            }
            else if (command_kind == 4u)
            {
              m_guest.gpr[3] = willy;
              m_guest.pc = 0x8015EF14u;  // _startExitFromWilly()
              m_bigwilly_enter_phase = 4;
            }
            std::fprintf(stderr, "[bigwilly] executing command=%u player=%08x willy=%08x inventory=%08x\n", m_bigwilly_command, player, willy, m_bigwilly_inventory);
          }
        }
          if (dispatch_trace && (m_native_dispatches & 0xFFFFFu) == 0)
          {
            auto& fifo = m_system.GetCommandProcessor().GetFifo();
            std::fprintf(dispatch_trace.get(), "%llu,%08x,%08x,%08x,%08x,%llu,%d,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%u,%08x,%u,%u,%u,%04x,%04x,%08x,%08x\n",
                         static_cast<unsigned long long>(m_native_dispatches), m_guest.pc,
                         m_guest.lr, m_guest.ctr, m_guest.cr,
                         static_cast<unsigned long long>(m_guest.timebase), ppc.downcount,
                         m_guest.gpr[1], m_guest.gpr[3], m_guest.gpr[13], m_guest.gpr[30],
                         m_guest.gpr[31], fifo.CPReadPointer.load(), fifo.CPWritePointer.load(),
                         fifo.CPReadWriteDistance.load(), fifo.CPBreakpoint.load(),
                         fifo.bFF_Breakpoint.load(), fifo.bFF_BPEnable.load(),
                         fifo.bFF_GPReadEnable.load(),
                         m_system.GetPixelEngine().GetTokenForDiagnostics(),
                         m_system.GetMMU().Read<u16>(0xCC00100Au),
                         m_system.GetProcessorInterface().GetCause(),
                         m_system.GetProcessorInterface().GetMask());
            std::fflush(dispatch_trace.get());
          }
          const bool do_ls = lockstep_enabled && !bigwilly_skip_active &&
                             m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          if (m_collect_dispatch_samples && (m_native_dispatches & 4095u) == 0)
            ++m_dispatch_samples[m_guest.pc];
          const u32 runtime_dispatch_address = m_guest.pc;
          // Halve the front end's step and flip interval together. Identify the
          // mode by its symbol-verified vtable, since the main menu's clock is
          // not paused. Restore the original step before gameplay updates;
          // do not alter the physics clock or emulation speed.
          if (bigwilly_menu_60 && runtime_dispatch_address == 0x802CBC78u &&
              m_guest.gpr[3] == 0x8047B668u && m_guest.ram_size >= 0x00539E40u &&
              read_ram_word(0x802CBC78u) == 0x9421FFD0u)
          {
            constexpr u32 timer = 0x8047B668u;
            const u32 step = read_ram_word(timer + 8u);
            const bool frontend = m_guest.ram[timer - 0x80000000u + 52u] == 1 &&
                                  bigwilly_frontend_mode();
            if (frontend && (step == 0x3D08AB86u || step == 0x3C88AB86u))
            {
              write_ram_word(timer + 8u, 0x3C88AB86u);
              write_ram_word(timer + 12u, 0x3E08AB86u);
              write_ram_word(0x80539E3Cu, 1u);
              if (!bigwilly_timing_applied)
                std::fprintf(stderr, "[bigwilly] frontend timing: 59.94 FPS\n");
              bigwilly_timing_applied = true;
            }
            else if (!frontend && bigwilly_timing_applied)
            {
              if (step == 0x3C88AB86u)
              {
                write_ram_word(timer + 8u, 0x3D08AB86u);
                write_ram_word(timer + 12u, 0x3E88AB86u);
                write_ram_word(0x80539E3Cu, 2u);
              }
              std::fprintf(stderr, "[bigwilly] original gameplay timing restored\n");
              bigwilly_timing_applied = false;
            }
          }
          u32 linked_dispatch_address = runtime_dispatch_address;
          if (m_has_rel_modules)
            ResolveNativeAddress(runtime_dispatch_address, &linked_dispatch_address, nullptr);
          // The stack-LR probe is only needed by the opt-in frame-sync
          // diagnostic/override.  Avoid a RAM read on every native dispatch
          // during normal runs, where the override is disabled.
          u32 bigwilly_saved_lr = 0;
          if (bigwilly_skip_lock_poll)
            bigwilly_saved_lr = read_guest_word(m_guest.gpr[1] + 0x14u);
          const bool bigwilly_lock_poll_caller = bigwilly_skip_lock_poll &&
                                                  bigwilly_saved_lr == 0x802F7484u;
          const bool bigwilly_video_sync_caller = bigwilly_skip_lock_poll &&
                                                  (bigwilly_saved_lr == 0x80311BF0u ||
                                                   bigwilly_saved_lr == 0x80311944u);
          if (bigwilly_skip_lock_poll && runtime_dispatch_address == 0x802F8C7Cu &&
              ((bigwilly_lock_poll_caller && bigwilly_lock_debug_hits < 16u) ||
               (bigwilly_video_sync_caller && bigwilly_video_debug_hits < 32u) ||
               (!bigwilly_lock_poll_caller && !bigwilly_video_sync_caller &&
                bigwilly_other_sync_debug_hits < 16u)))
          {
            std::fprintf(stderr,
                         "[bigwilly] frame-sync return sp=%08x saved_lr=%08x lock=%u video=%u\n",
                         m_guest.gpr[1], bigwilly_saved_lr, bigwilly_lock_poll_caller ? 1u : 0u,
                         bigwilly_video_sync_caller ? 1u : 0u);
            if (bigwilly_lock_poll_caller)
              ++bigwilly_lock_debug_hits;
            else if (bigwilly_video_sync_caller)
              ++bigwilly_video_debug_hits;
            else
              ++bigwilly_other_sync_debug_hits;
          }
          if (bigwilly_skip_lock_poll && runtime_dispatch_address == 0x802F8C7Cu &&
              (bigwilly_lock_poll_caller || bigwilly_video_sync_caller))
          {
            // DynamicVertexBuffer::Lock and the VideoPlayer cutscene handoff
            // both poll the Wii frame-sync window. Force only those known
            // RDHE78 callers to observe a completed frame and keep the normal
            // function epilogue, stack restoration, and cycle accounting
            // intact. The live LR is overwritten by OSRestoreInterrupts
            // before this return point, so the caller LR must be read from the
            // stack.
            m_guest.gpr[31] = 1;
          }
          bool bigwilly_fast_interrupt_helper = false;
          if (bigwilly_fast_os_interrupts && (m_guest.msr & 0x4000u) == 0)
          {
            if (runtime_dispatch_address == 0x803625B0u)
            {
              // OSDisableInterrupts: mfmsr r3; clear EE; return old EE.
              const u32 old_msr = m_guest.msr;
              m_guest.gpr[3] = (old_msr & 0x8000u) != 0 ? 1u : 0u;
              m_guest.gpr[4] = old_msr & ~0x8000u;
              m_guest.msr = m_guest.gpr[4];
              m_guest.pc = m_guest.lr & ~3u;
              m_guest.downcount -= 5;
              bigwilly_fast_interrupt_helper = true;
            }
            else if (runtime_dispatch_address == 0x803625D8u)
            {
              // OSRestoreInterrupts: preserve cmpwi r3,0's CR0 result,
              // update EE from the signed argument, then return old EE.
              const u32 old_msr = m_guest.msr;
              const u32 arg = m_guest.gpr[3];
              m_guest.gpr[4] = old_msr;
              const s32 signed_arg = static_cast<s32>(arg);
              const u32 cr_bits = signed_arg < 0 ? 0x8u :
                                  signed_arg > 0 ? 0x4u : 0x2u;
              m_guest.cr = (m_guest.cr & 0x0FFFFFFFu) |
                           ((cr_bits | ((m_guest.xer >> 31) & 1u)) << 28);
              const u32 new_msr = arg == 0 ? old_msr & ~0x8000u : old_msr | 0x8000u;
              m_guest.gpr[5] = new_msr;
              m_guest.msr = new_msr;
              m_guest.gpr[3] = (old_msr & 0x8000u) != 0 ? 1u : 0u;
              m_guest.pc = m_guest.lr & ~3u;
              m_guest.downcount -= arg == 0 ? 7 : 8;
              bigwilly_fast_interrupt_helper = true;
            }
          }
          if (!bigwilly_fast_interrupt_helper)
          {
            m_guest.pc = linked_dispatch_address;
            native_dispatch(&m_guest, linked_dispatch_address);
          }
          if (m_has_rel_modules)
            m_guest.pc = TranslateRelAddress(m_guest.pc);

          // bctrl returns to the chassis with CTR as its target and LR naming
          // the continuation. Record unique observed edges without guessing
          // or replacing any target. Disabled in ordinary flagship launches.
          if (callback_audit && m_guest.pc == (m_guest.ctr & ~3u) && m_guest.lr >= 4u)
          {
            const u32 site = m_guest.lr - 4u;
            if ((read_guest_word(site) & 0xFC0007FFu) == 0x4C000421u &&
                callback_edges.size() < 100000 &&
                callback_edges.insert((u64(site) << 32) | m_guest.pc).second)
            {
              std::fprintf(callback_audit.get(), "ctr_call,%08x,%08x,%d\n",
                           site, m_guest.pc, fast_dispatchable_at(m_guest.pc) ? 1 : 0);
            }
          }

          if (bigwilly_enter_active && m_guest.pc == bigwilly_player_update &&
              m_guest.gpr[1] == bigwilly_enter_saved.gpr[1])
          {
            const unsigned command_kind = m_bigwilly_command & 0xFFu;
            if (command_kind == 2u && m_bigwilly_enter_phase < weapon_ids.size() * 2u - 1u)
            {
              ++m_bigwilly_enter_phase;
              m_guest.lr = bigwilly_player_update;
              m_guest.gpr[3] = m_bigwilly_inventory;
              m_guest.gpr[4] = weapon_ids[m_bigwilly_enter_phase / 2u];
              m_guest.gpr[5] = (m_bigwilly_enter_phase & 1u) ? 0 : 1;
              m_guest.gpr[6] = 0;
              m_guest.pc = (m_bigwilly_enter_phase & 1u) ? 0x800A1284u : 0x800A1248u;
            }
            else if (command_kind == 1u && m_bigwilly_enter_phase < 4u)
            {
              m_guest.lr = bigwilly_player_update;
              m_guest.gpr[3] = m_bigwilly_enter_willy;
              if (m_bigwilly_enter_phase == 1u)
                m_guest.pc = 0x80157E98u;  // GetMaxCharge(), returns f1
              else if (m_bigwilly_enter_phase == 2u)
              {
                m_guest.gpr[4] = 1u;
                m_guest.pc = 0x80157F58u;  // AddCharge(max, true), clamps to max
              }
              else
              {
                m_guest.gpr[3] = bigwilly_enter_saved.gpr[3];
                m_guest.gpr[4] = 2u;
                m_guest.gpr[5] = 0u;
                m_guest.gpr[6] = 0u;
                m_guest.pc = bigwilly_set_avatar;
              }
              ++m_bigwilly_enter_phase;
            }
            else
            {
            const auto remaining_cycles = m_guest.downcount;
            const auto current_timebase = m_guest.timebase;
            std::memcpy(bigwilly_guest_span(bigwilly_enter_saved.gpr[1], bigwilly_enter_linkage.size()),
                        bigwilly_enter_linkage.data(), bigwilly_enter_linkage.size());
            m_guest = bigwilly_enter_saved;
            m_guest.downcount = remaining_cycles;
            m_guest.timebase = current_timebase;
            // Restoring FPSCR also requires restoring the host FP environment.
            if (m_module && m_module->on_state_loaded)
              m_module->on_state_loaded(&m_guest);
            bigwilly_enter_active = false;
            const u32 mode = read_guest_word(m_guest.gpr[3] + 4u);
            if (command_kind == 1u)
            {
              BigWillyConsole::Log(mode == 2u ? "Now piloting Big Willy." : "Big Willy entry did not complete.");
              std::fprintf(stderr, "[bigwilly] mech entry complete mode=%u\n", mode);
            }
            else if (command_kind == 2u)
              BigWillyConsole::Log("All Crypto, saucer and mech weapons unlocked.");
            else if (command_kind == 4u)
              BigWillyConsole::Log("Exiting Big Willy.");
            else
              BigWillyConsole::Log("Level travel queued by the game.");
            std::fprintf(stderr, "[bigwilly] game command=%u complete\n", m_bigwilly_command);
            }
          }
          if (m_bigwilly_skip.HasReturned(m_guest))
          {
            if (bigwilly_skip_phase == 1u && bigwilly_stop_video)
            {
              // The cutscene helper has returned. Stop a simultaneously
              // active Bink movie before resuming the interrupted game code.
              m_guest.lr = bigwilly_skip_resume_pc;
              m_guest.gpr[3] = bigwilly_video_player;
              m_guest.pc = bigwilly_video_stop;
              bigwilly_skip_phase = 2u;
            }
            else
            {
              m_bigwilly_skip.Finish(m_guest,
                  bigwilly_guest_span(m_bigwilly_skip.saved.gpr[1], 64));
              if (m_module && m_module->on_state_loaded)
                m_module->on_state_loaded(&m_guest);
              bigwilly_skip_phase = 0;
              bigwilly_video_player = 0;
              bigwilly_stop_video = false;
              std::fprintf(stderr, "[bigwilly] Escape: movie/cutscene skip complete\n");
            }
          }
          if (count_native_dispatches)
            ++m_native_dispatches;

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }

          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher back-edge timing check — CoreTiming regains control
          // with at least CachedInterpreter's per-block frequency, so
          // external-interrupt latency matches stock.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          const u64 effective_charge = static_cast<u64>(charge > 0 ? charge : 1);
          ppc.downcount -= static_cast<int>(effective_charge);
          if (!m_fast_diagnostics)
            m_charged_cycles += effective_charge;
          const u64 total_cycles = m_timebase_cycle_remainder + effective_charge;
          m_guest.timebase += total_cycles / SystemTimers::TIMER_RATIO;
          m_timebase_cycle_remainder = total_cycles % SystemTimers::TIMER_RATIO;

          // Idle loop skipping for configured target loops (e.g. Wii Menu OSIdleThread)
          if (m_guest.pc == m_idle_pc && m_idle_pc != 0)
          {
            m_system.GetCoreTiming().Idle();
          }

          // RDHE78 Game::_update: TimeSync has just been polled. If neither
          // a simulation turn nor a rendered frame is due, yield only to the
          // next hardware event. Never yield across work that is already due.
          if (bigwilly_poll_yield && m_guest.pc == 0x80022440u &&
              (m_guest.msr & 0x8000u) &&
              read_ram_word(0x80022440u) == 0x881D0036u)
          {
            const u32 timer = m_guest.gpr[29];
            if (timer >= 0x80000000u && timer - 0x80000000u + 56u <= m_guest.ram_size &&
                m_guest.ram[timer - 0x80000000u + 52u] == 1 &&
                read_ram_word(timer + 24u) == 0)
            {
              const float step = std::bit_cast<float>(read_ram_word(timer + 8u));
              const float elapsed = std::bit_cast<float>(read_ram_word(timer + 44u));
              if (step > 0.0f && elapsed < step)
              {
                if (bigwilly_poll_wake_tick == 0)
                {
                  const double wait_seconds = std::min<double>(0.001, step - elapsed);
                  bigwilly_poll_wake_tick = core_timing.GetTicks() +
                      static_cast<u64>(wait_seconds * m_system.GetSystemTimers().GetTicksPerSecond());
                  bigwilly_poll_stack = m_guest.gpr[1];
                }
                core_timing.Idle();
              }
            }
          }

          // ctx->timebase is refreshed at burst start (SyncIn), and here we
          // incrementally advance it by the exact block cycle charges to
          // prevent guest busy-wait loops from spinning on a stale timebase.
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            if (!m_fast_diagnostics)
              ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
          if ((ppc.Exceptions & EXCEPTION_EXTERNAL_INT) != 0 &&
              (m_guest.msr & 0x8000u) != 0 && after_mtmsr(m_guest.pc))
            break;
        } while (m_module_active && fast_dispatchable_at(m_guest.pc) &&
                 !(m_guest.host_call && IsHostCallAddress(m_guest.pc)) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
        else if ((ppc.Exceptions & EXCEPTION_EXTERNAL_INT) != 0 && ppc.msr.EE &&
                 after_mtmsr(ppc.pc))
          power_pc.CheckExternalExceptions();
      }
      else
      {
        if (m_guest.host_call && IsHostCallAddress(ppc.pc))
        {
          SyncIn();
          bool handled = m_guest.host_call(&m_guest, m_guest.pc);
          if (!handled && m_guest.pc < m_guest.ram_size)
            handled = m_guest.host_call(&m_guest, m_guest.pc | 0x80000000u);
          if (m_fallback_jit && IsHostCallAddress(m_guest.lr))
            m_fallback_jit->GetBlockCache()->InvalidateICache(m_guest.lr, 4, true);
          if (handled)
          {
            const s64 charge = -m_guest.downcount;
            m_guest.downcount = 0;
            const u64 effective_charge = static_cast<u64>(charge > 0 ? charge : 1);
            ppc.downcount -= static_cast<int>(effective_charge);
            AdvanceGuestTimebase(effective_charge);
            SyncOut();
            continue;
          }
          SyncOut();
          if (m_fallback_jit)
          {
            m_host_call_passthrough_pc = ppc.pc;
            m_host_call_passthrough = true;
          }
        }
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        if (m_module_active && IsForcedFallbackAddress(ppc.pc))
        {
          ppc.downcount -= interpreter.SingleStepInner();
          if (!m_fast_diagnostics)
            ++m_fallback_steps;
        }
        else if (m_fallback_jit)
        {
          m_fallback_jit->Run();
        }
        else
        {
          do
          {
            ppc.downcount -= interpreter.SingleStepInner();
            if (!m_fast_diagnostics)
              ++m_fallback_steps;
          } while (!(m_module_active && DispatchableAt(ppc.pc)) &&
                   !IsHostCallAddress(ppc.pc) && ppc.downcount > 0 &&
                   *state_ptr == CPU::State::Running);
        }
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
