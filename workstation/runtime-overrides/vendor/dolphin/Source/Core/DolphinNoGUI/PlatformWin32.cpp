// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Core.h"
#include "Core/SavestateLayout.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/PowerPC/StaticRecomp/BigWillyConsole.h"
#include "Core/PowerPC/StaticRecomp/BigWillyControls.h"

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#include <string>
#include <vector>
#include <windows.h>
#include <climits>
#include <dbghelp.h>
#include <dwmapi.h>
#include <thread>

#include "VideoCommon/Present.h"
#include "resource.h"

// ── Crash handler ────────────────────────────────────────────────────────────
// Installed once during Init(). Writes a minidump + text log to
// <exe_dir>\crashes\ on any unhandled exception and forwards the one-line
// summary to stderr (furonlog) and the in-game dev console.

using MiniDumpWriteDump_t =
    BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                  PMINIDUMP_EXCEPTION_INFORMATION,
                  PMINIDUMP_USER_STREAM_INFORMATION,
                  PMINIDUMP_CALLBACK_INFORMATION);
using SymInitialize_t = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using SymFromAddr_t   = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);

static MiniDumpWriteDump_t s_MiniDumpWriteDump = nullptr;
static SymInitialize_t     s_SymInitialize     = nullptr;
static SymFromAddr_t       s_SymFromAddr       = nullptr;
static wchar_t s_crash_dir[MAX_PATH]           = {};

static const char* ExceptionCodeName(DWORD code)
{
  switch (code)
  {
  case EXCEPTION_ACCESS_VIOLATION:       return "ACCESS_VIOLATION";
  case EXCEPTION_STACK_OVERFLOW:         return "STACK_OVERFLOW";
  case EXCEPTION_ILLEGAL_INSTRUCTION:    return "ILLEGAL_INSTRUCTION";
  case EXCEPTION_INT_DIVIDE_BY_ZERO:     return "INT_DIVIDE_BY_ZERO";
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:     return "FLT_DIVIDE_BY_ZERO";
  case EXCEPTION_BREAKPOINT:             return "BREAKPOINT";
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:  return "ARRAY_BOUNDS_EXCEEDED";
  case EXCEPTION_DATATYPE_MISALIGNMENT:  return "DATATYPE_MISALIGNMENT";
  case EXCEPTION_IN_PAGE_ERROR:          return "IN_PAGE_ERROR";
  case EXCEPTION_PRIV_INSTRUCTION:       return "PRIVILEGED_INSTRUCTION";
  case 0xC0000409u:                      return "STACK_BUFFER_OVERRUN";
  default:                               return nullptr;
  }
}

static LONG WINAPI BigWillyCrashHandler(EXCEPTION_POINTERS* ep)
{
  // Skip first-chance breakpoints so the debugger still gets them.
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT)
    return EXCEPTION_CONTINUE_SEARCH;

  const DWORD     code = ep->ExceptionRecord->ExceptionCode;
  const ULONG_PTR addr =
      reinterpret_cast<ULONG_PTR>(ep->ExceptionRecord->ExceptionAddress);
  const char* name = ExceptionCodeName(code);

  SYSTEMTIME st{};
  GetLocalTime(&st);

  wchar_t stem[64];
  _snwprintf_s(stem, _countof(stem), _TRUNCATE, L"crash_%04u%02u%02u_%02u%02u%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

  CreateDirectoryW(s_crash_dir, nullptr);

  // ── Minidump ───────────────────────────────────────────────────────────────
  wchar_t dmp_path[MAX_PATH];
  _snwprintf_s(dmp_path, _countof(dmp_path), _TRUNCATE, L"%s\\%s.dmp", s_crash_dir, stem);

  bool wrote_dmp = false;
  if (s_MiniDumpWriteDump)
  {
    HANDLE hf = CreateFileW(dmp_path, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE)
    {
      MINIDUMP_EXCEPTION_INFORMATION mei{};
      mei.ThreadId          = GetCurrentThreadId();
      mei.ExceptionPointers = ep;
      mei.ClientPointers    = FALSE;
      wrote_dmp = s_MiniDumpWriteDump(
          GetCurrentProcess(), GetCurrentProcessId(), hf,
          static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs |
                                     MiniDumpWithUnloadedModules |
                                     MiniDumpWithProcessThreadData),
          &mei, nullptr, nullptr) != FALSE;
      CloseHandle(hf);
    }
  }

  // ── Text crash log ─────────────────────────────────────────────────────────
  wchar_t log_path[MAX_PATH];
  _snwprintf_s(log_path, _countof(log_path), _TRUNCATE, L"%s\\%s.log", s_crash_dir, stem);

  HANDLE hlog = CreateFileW(log_path, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hlog != INVALID_HANDLE_VALUE)
  {
    char buf[4096];
    int  n = 0;

    n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
        "=== Big Willy Unleashed — Crash Report ===\n"
        "Date/time     : %04u-%02u-%02u %02u:%02u:%02u\n"
        "Exception     : 0x%08X",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        static_cast<unsigned>(code));
    if (name)
      n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, " (%s)", name);
    n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "\n");

    n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
        "Fault address : 0x%016llX\n"
        "Thread ID     : %u\n"
        "Process ID    : %u\n",
        static_cast<unsigned long long>(addr),
        static_cast<unsigned>(GetCurrentThreadId()),
        static_cast<unsigned>(GetCurrentProcessId()));

    // For access violations include read/write direction and target address.
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
    {
      const ULONG_PTR rw  = ep->ExceptionRecord->ExceptionInformation[0];
      const ULONG_PTR tgt = ep->ExceptionRecord->ExceptionInformation[1];
      n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
          "AV operation  : %s\n"
          "AV target     : 0x%016llX\n",
          rw == 1 ? "WRITE" : rw == 8 ? "DEP" : "READ",
          static_cast<unsigned long long>(tgt));
    }

    // Module containing the faulting address.
    HMODULE hmod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &hmod))
    {
      char mod_path[MAX_PATH] = {};
      GetModuleFileNameA(hmod, mod_path, sizeof(mod_path));
      n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
          "Module        : %s\n", mod_path);
    }

    n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
        "Minidump      : %ls\n",
        wrote_dmp ? dmp_path : L"(not written)");

    DWORD written = 0;
    WriteFile(hlog, buf, static_cast<DWORD>(n), &written, nullptr);

    // ── Stack trace ────────────────────────────────────────────────────────
    VOID* frames[64] = {};
    const USHORT nframes = CaptureStackBackTrace(0, 64, frames, nullptr);
    char frame_buf[256];
    int fn = _snprintf_s(frame_buf, sizeof(frame_buf), _TRUNCATE,
                         "Stack trace (%u frames):\n", nframes);
    WriteFile(hlog, frame_buf, static_cast<DWORD>(fn), &written, nullptr);

    for (USHORT i = 0; i < nframes; ++i)
    {
      const DWORD64 frame_addr =
          static_cast<DWORD64>(reinterpret_cast<uintptr_t>(frames[i]));
      char sym_name[256] = "";
      if (s_SymFromAddr)
      {
        alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 255];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;
        DWORD64 disp = 0;
        if (s_SymFromAddr(GetCurrentProcess(), frame_addr, &disp, sym))
          _snprintf_s(sym_name, sizeof(sym_name), _TRUNCATE,
                      "  %s+0x%llx", sym->Name,
                      static_cast<unsigned long long>(disp));
      }
      fn = _snprintf_s(frame_buf, sizeof(frame_buf), _TRUNCATE,
                       "  #%02u  0x%016llX%s\n",
                       i, static_cast<unsigned long long>(frame_addr), sym_name);
      WriteFile(hlog, frame_buf, static_cast<DWORD>(fn), &written, nullptr);
    }

    const char footer[] = "==========================================\n";
    WriteFile(hlog, footer, static_cast<DWORD>(sizeof(footer) - 1), &written, nullptr);
    CloseHandle(hlog);
  }

  // ── Forward summary to stderr (furonlog) and in-game console ──────────────
  char msg[256];
  if (name)
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "[CRASH] %s (0x%08X) at 0x%llX",
                name, static_cast<unsigned>(code),
                static_cast<unsigned long long>(addr));
  else
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "[CRASH] 0x%08X at 0x%llX",
                static_cast<unsigned>(code),
                static_cast<unsigned long long>(addr));

  fprintf(stderr, "%s\n", msg);
  fflush(stderr);
  BigWillyConsole::Log(msg);

  return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallCrashHandler()
{
  // Derive crash dir from the running exe: <exe_dir>\crashes
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t* sep = wcsrchr(exe, L'\\');
  if (sep)
    *sep = L'\0';
  _snwprintf_s(s_crash_dir, _countof(s_crash_dir), _TRUNCATE, L"%s\\crashes", exe);

  // Load MiniDumpWriteDump dynamically — dbghelp.dll ships with Windows 10+.
  // Keep the module handle open so dbghelp stays resident and is safe to call
  // from inside the exception handler.
  if (HMODULE hdbg = LoadLibraryW(L"dbghelp.dll"))
  {
    s_MiniDumpWriteDump = reinterpret_cast<MiniDumpWriteDump_t>(
        GetProcAddress(hdbg, "MiniDumpWriteDump"));
    s_SymInitialize = reinterpret_cast<SymInitialize_t>(
        GetProcAddress(hdbg, "SymInitialize"));
    s_SymFromAddr   = reinterpret_cast<SymFromAddr_t>(
        GetProcAddress(hdbg, "SymFromAddr"));
    if (s_SymInitialize)
      s_SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  }

  // Reserve 64 KB of guaranteed stack so the handler can run even after a
  // stack overflow.
  ULONG stack_guarantee = 64u * 1024u;
  SetThreadStackGuarantee(&stack_guarantee);

  SetUnhandledExceptionFilter(BigWillyCrashHandler);
}

// ─────────────────────────────────────────────────────────────────────────────

namespace
{
// Menu command ids. Load State entries are allocated a contiguous range,
// since the list is rebuilt from disk each time the menu opens.
constexpr UINT ID_SAVE_STATE = 41001;
constexpr UINT ID_PAUSE = 41002;
constexpr UINT ID_MUTE = 41003;
constexpr UINT ID_FULLSCREEN = 41004;
constexpr UINT ID_LOAD_STATE_FIRST = 41100;
constexpr UINT ID_LOAD_STATE_LAST = 41199;

// Hold-to-fast-forward target. 2x is fast enough to skip a cutscene without
// outrunning what most hosts can actually emulate.
constexpr float FAST_FORWARD_SPEED = 2.0f;

bool AgentControlsGame()
{
  static const bool enabled = [] {
    const char* value = std::getenv("STATICRECOMP_AUTOMATION_EXCLUSIVE");
    return value && std::string_view(value) == "1";
  }();
  return enabled;
}

// Forward-declared so ExecuteConsoleCommand can call RequestShutdown.
class PlatformWin32;

void StartStdoutTee();
void ExecuteConsoleCommand(PlatformWin32* platform, const std::string& cmd_str);

class PlatformWin32 final : public Platform
{
public:
  ~PlatformWin32() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

private:
  static constexpr TCHAR WINDOW_CLASS_NAME[] = _T("DolphinNoGUI");

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  static bool RegisterRenderWindowClass();
  bool CreateRenderWindow();
  bool CreateMenus();
  void RefreshMenu(HMENU menu);
  void SaveStateToStatesDirectory();
  void ToggleFullscreen();
  void UpdateWindowPosition();
  void ProcessEvents();

  HWND m_hwnd{};
  HMENU m_menu{};
  HMENU m_file_menu{};
  HMENU m_load_menu{};
  HMENU m_view_menu{};
  // Parallel to the Load State menu entries, rebuilt whenever it opens.
  std::vector<std::string> m_load_state_paths;
  std::time_t m_last_save_time{};
  std::size_t m_save_sequence{};
  bool m_fullscreen = false;
  LONG m_windowed_style = 0;
  RECT m_windowed_rect{};

  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
};

// Redirects the C runtime's stdout through an anonymous pipe so a background
// thread can tee each line to both the original stdout handle (preserving the
// launcher's furonlog capture) and BigWillyConsole::Log() for in-game display.
void StartStdoutTee()
{
  // Grab the current stdout HANDLE before we replace it.
  const HANDLE orig_raw = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
  if (orig_raw == INVALID_HANDLE_VALUE || orig_raw == nullptr)
    return;

  // Keep a durable duplicate so the tee thread can write to it after we
  // reassign fd 1.
  HANDLE orig = INVALID_HANDLE_VALUE;
  if (!DuplicateHandle(GetCurrentProcess(), orig_raw, GetCurrentProcess(), &orig,
                       0, FALSE, DUPLICATE_SAME_ACCESS))
    return;

  // Create the pipe. A 64 KiB buffer avoids blocking bursts of output.
  HANDLE pipe_r = INVALID_HANDLE_VALUE;
  HANDLE pipe_w = INVALID_HANDLE_VALUE;
  {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    if (!CreatePipe(&pipe_r, &pipe_w, &sa, 65536))
    {
      CloseHandle(orig);
      return;
    }
  }

  // Open an fd around the write end and dup2 it over fd 1 (stdout).
  // _dup2 makes fd 1 its own DuplicateHandle copy of pipe_w, so the
  // subsequent _close of pipe_w_fd is safe and does not break fd 1.
  const int pipe_w_fd = _open_osfhandle(reinterpret_cast<intptr_t>(pipe_w),
                                        _O_WRONLY | _O_TEXT);
  if (pipe_w_fd == -1)
  {
    CloseHandle(orig);
    CloseHandle(pipe_r);
    CloseHandle(pipe_w);
    return;
  }
  if (_dup2(pipe_w_fd, _fileno(stdout)) != 0)
  {
    CloseHandle(orig);
    CloseHandle(pipe_r);
    _close(pipe_w_fd);  // also closes pipe_w
    return;
  }
  _close(pipe_w_fd);           // fd 1 owns its own dup; release the extra fd
  setvbuf(stdout, nullptr, _IONBF, 0);  // flush immediately on every write

  // Keep Win32's view in sync with the new fd 1 handle.
  SetStdHandle(STD_OUTPUT_HANDLE,
               reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));

  // Background tee thread: reads lines from pipe_r and dispatches to both
  // the original stdout handle and BigWillyConsole::Log().
  std::thread([pipe_r, orig]() {
    const int read_fd = _open_osfhandle(reinterpret_cast<intptr_t>(pipe_r),
                                        _O_RDONLY | _O_TEXT);
    if (read_fd == -1)
    {
      CloseHandle(pipe_r);
      CloseHandle(orig);
      return;
    }
    FILE* const f = _fdopen(read_fd, "r");
    if (!f)
    {
      _close(read_fd);
      CloseHandle(orig);
      return;
    }

    char buf[4096];
    while (fgets(buf, static_cast<int>(sizeof(buf)), f))
    {
      const std::size_t len_nl = std::strlen(buf);

      // Forward the line (with its newline) to the original stdout so the
      // launcher's furonlog pipe continues to receive everything.
      DWORD written = 0;
      WriteFile(orig, buf, static_cast<DWORD>(len_nl), &written, nullptr);

      // Strip trailing newline(s) for single-line console display.
      std::size_t len = len_nl;
      while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

      if (len > 0)
        BigWillyConsole::Log(std::string(buf, len));
    }

    fclose(f);       // closes read_fd and pipe_r
    CloseHandle(orig);
  }).detach();
}

void ExecuteConsoleCommand(PlatformWin32* platform, const std::string& cmd_str)
{
  BigWillyConsole::Log("> " + cmd_str);

  const auto space = cmd_str.find(' ');
  const std::string cmd = (space == std::string::npos) ? cmd_str : cmd_str.substr(0, space);
  const std::string args = (space == std::string::npos) ? std::string{} : cmd_str.substr(space + 1);

  if (cmd.empty())
    return;

  if (cmd == "help")
  {
    BigWillyConsole::Log("Commands:");
    BigWillyConsole::Log("  help        - show this message");
    BigWillyConsole::Log("  clear       - clear the log");
    BigWillyConsole::Log("  quit/exit   - quit the game");
    BigWillyConsole::Log("  fps         - toggle FPS display hint");
    BigWillyConsole::Log("  enter_bigwilly - instantly pilot Big Willy during gameplay");
    BigWillyConsole::Log("  giveallweapons - unlock all Crypto, saucer and mech weapons");
    BigWillyConsole::Log("  list_levels - list installed single-player level names");
    BigWillyConsole::Log("  load level_<name> - travel to a listed level during gameplay");
    BigWillyConsole::Log("  osd <msg>   - post an OSD notification");
  }
  else if (cmd == "enter_bigwilly")
  {
    BigWillyConsole::Log(BigWillyControls::RequestEnterBigWilly() ? "Big Willy entry requested." : "A command is already queued.");
  }
  else if (cmd == "giveallweapons")
  {
    BigWillyConsole::Log(BigWillyControls::RequestGiveAllWeapons() ? "Weapon unlock requested." : "A command is already queued.");
  }
  else if (cmd == "list_levels")
  {
    for (auto level : BigWillyControls::LEVELS)
      BigWillyConsole::Log("  load level_" + std::string(level));
  }
  else if (cmd == "load")
  {
    const int level = BigWillyControls::FindLevel(args);
    if (level < 0)
      BigWillyConsole::Log("Unknown level. Use list_levels and copy a load command.");
    else
      BigWillyConsole::Log(BigWillyControls::RequestLoadLevel(level) ? "Level travel requested." : "A command is already queued.");
  }
  else if (cmd == "clear")
  {
    std::lock_guard<std::mutex> lock(BigWillyConsole::g_mutex);
    BigWillyConsole::g_log.clear();
  }
  else if (cmd == "quit" || cmd == "exit")
  {
    BigWillyConsole::Log("Shutting down...");
    if (platform)
      platform->RequestShutdown();
  }
  else if (cmd == "osd")
  {
    if (args.empty())
      BigWillyConsole::Log("[osd] usage: osd <message>");
    else
      BigWillyConsole::Log("[osd] " + args);
  }
  else if (cmd == "fps")
  {
    BigWillyConsole::Log("[fps] FPS display is in the window title bar.");
  }
  else
  {
    BigWillyConsole::Log("Unknown command '" + cmd + "' -- type 'help' for help.");
  }
}

PlatformWin32::~PlatformWin32()
{
  if (m_hwnd)
  {
    DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
  }
}

bool PlatformWin32::RegisterRenderWindowClass()
{
  WNDCLASSEX wc = {};
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = 0;
  wc.lpfnWndProc = WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hIcon = LoadIcon(nullptr, IDI_ICON1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszMenuName = nullptr;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  wc.hIconSm = LoadIcon(nullptr, IDI_ICON1);

  if (!RegisterClassEx(&wc))
  {
    MessageBox(nullptr, _T("Window registration failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  return true;
}

bool PlatformWin32::CreateRenderWindow()
{
  // A client edge adds a sunken border around the rendered image and survives
  // the fullscreen toggle, which removes only the normal window styles.
  m_hwnd = CreateWindowEx(0, WINDOW_CLASS_NAME, _T("Dolphin"), WS_OVERLAPPEDWINDOW,
                          m_window_x < 0 ? CW_USEDEFAULT : m_window_x,
                          m_window_y < 0 ? CW_USEDEFAULT : m_window_y, m_window_width,
                          m_window_height, nullptr, nullptr, GetModuleHandle(nullptr), this);
  if (!m_hwnd)
  {
    MessageBox(nullptr, _T("CreateWindowEx failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  ShowWindow(m_hwnd, SW_SHOW);
  UpdateWindow(m_hwnd);
  return true;
}

bool PlatformWin32::CreateMenus()
{
  m_menu = CreateMenu();
  m_file_menu = CreatePopupMenu();
  m_load_menu = CreatePopupMenu();
  m_view_menu = CreatePopupMenu();
  if (!m_menu || !m_file_menu || !m_load_menu || !m_view_menu)
    return false;

  AppendMenuW(m_file_menu, MF_STRING, ID_SAVE_STATE, L"&Save State\tF1");
  AppendMenuW(m_file_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(m_load_menu), L"&Load State");
  AppendMenuW(m_file_menu, MF_SEPARATOR, 0, nullptr);
  // Checked state is refreshed from the core when the menu opens, so it cannot
  // drift out of step with an emulation that was paused some other way.
  AppendMenuW(m_file_menu, MF_STRING, ID_PAUSE, L"&Pause");

  AppendMenuW(m_view_menu, MF_STRING, ID_FULLSCREEN, L"&Fullscreen\tAlt+Enter");
  AppendMenuW(m_view_menu, MF_STRING, ID_MUTE, L"&Mute Audio");

  AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(m_file_menu), L"&File");
  AppendMenuW(m_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(m_view_menu), L"&View");
  return SetMenu(m_hwnd, m_menu) != FALSE;
}

// Rebuilt on open rather than cached: states are written by this process while
// the menu is closed, and by the launcher between sessions.
void PlatformWin32::RefreshMenu(const HMENU menu)
{
  if (menu == m_file_menu)
  {
    auto& system = Core::System::GetInstance();
    const bool paused = Core::GetState(system) == Core::State::Paused;
    CheckMenuItem(m_file_menu, ID_PAUSE, MF_BYCOMMAND | (paused ? MF_CHECKED : MF_UNCHECKED));
    return;
  }

  if (menu == m_view_menu)
  {
    CheckMenuItem(m_view_menu, ID_MUTE,
                  MF_BYCOMMAND |
                      (Config::Get(Config::MAIN_AUDIO_MUTED) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m_view_menu, ID_FULLSCREEN,
                  MF_BYCOMMAND | (m_fullscreen ? MF_CHECKED : MF_UNCHECKED));
    return;
  }

  if (menu != m_load_menu)
    return;

  while (DeleteMenu(m_load_menu, 0, MF_BYPOSITION))
  {
  }
  m_load_state_paths.clear();

  // Location, extension and order all come from State::Layout, so this menu and
  // any frontend listing the same directory cannot disagree.
  const std::vector<std::filesystem::path> states =
      State::Layout::List(StringToPath(File::GetUserPath(D_STATESAVES_IDX)));

  if (states.empty())
  {
    AppendMenuW(m_load_menu, MF_STRING | MF_GRAYED, 0, L"(no savestates)");
    return;
  }

  const std::size_t limit = std::min<std::size_t>(
      states.size(), ID_LOAD_STATE_LAST - ID_LOAD_STATE_FIRST + 1);
  for (std::size_t i = 0; i < limit; ++i)
  {
    AppendMenuW(m_load_menu, MF_STRING, ID_LOAD_STATE_FIRST + i,
                states[i].filename().wstring().c_str());
    m_load_state_paths.push_back(PathToString(states[i]));
  }
}

void PlatformWin32::SaveStateToStatesDirectory()
{
  const std::string directory = File::GetUserPath(D_STATESAVES_IDX);
  File::CreateFullPath(directory);
  const std::time_t now = std::time(nullptr);
  if (now == m_last_save_time)
    ++m_save_sequence;
  else
  {
    m_last_save_time = now;
    m_save_sequence = 0;
  }

  const std::filesystem::path path =
      StringToPath(directory) /
      State::Layout::TimestampedName(now, m_save_sequence, State::Layout::MANUAL_PREFIX);
  State::SaveAs(Core::System::GetInstance(), PathToString(path));
}

void PlatformWin32::ToggleFullscreen()
{
  if (!m_fullscreen)
  {
    GetWindowRect(m_hwnd, &m_windowed_rect);
    m_windowed_style = GetWindowLong(m_hwnd, GWL_STYLE);

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfo(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
      return;

    SetMenu(m_hwnd, nullptr);
    SetWindowLong(m_hwnd, GWL_STYLE, m_windowed_style & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(m_hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    m_fullscreen = true;
    return;
  }

  SetWindowLong(m_hwnd, GWL_STYLE, m_windowed_style);
  SetMenu(m_hwnd, m_menu);
  SetWindowPos(m_hwnd, nullptr, m_windowed_rect.left, m_windowed_rect.top,
               m_windowed_rect.right - m_windowed_rect.left,
               m_windowed_rect.bottom - m_windowed_rect.top,
               SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOZORDER);
  m_fullscreen = false;
}

bool PlatformWin32::Init()
{
  if (!RegisterRenderWindowClass() || !CreateRenderWindow() || !CreateMenus())
    return false;

  // TODO: Enter fullscreen if enabled.
  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    ProcessEvents();
  }

  if (Config::Get(Config::MAIN_DISABLE_SCREENSAVER))
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

  InstallCrashHandler();

  // Tee stdout to the dev console so OSReport output appears in-game.
  // The original stdout handle is preserved, keeping furonlog capture intact.
  StartStdoutTee();

  UpdateWindowPosition();
  return true;
}

void PlatformWin32::SetTitle(const std::string& string)
{
  SetWindowTextW(m_hwnd, UTF8ToWString(string).c_str());
}

void PlatformWin32::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    UpdateWindowPosition();

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformWin32::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Windows;
  wsi.render_window = reinterpret_cast<void*>(m_hwnd);
  wsi.render_surface = reinterpret_cast<void*>(m_hwnd);
  return wsi;
}

void PlatformWin32::UpdateWindowPosition()
{
  if (m_window_fullscreen)
    return;

  RECT rc = {};
  if (!GetWindowRect(m_hwnd, &rc))
    return;

  m_window_x = rc.left;
  m_window_y = rc.top;
  m_window_width = rc.right - rc.left;
  m_window_height = rc.bottom - rc.top;
}

void PlatformWin32::ProcessEvents()
{
  MSG msg;
  while (PeekMessage(&msg, m_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

LRESULT PlatformWin32::WndProc(const HWND hwnd, const UINT msg, const WPARAM wParam,
                               const LPARAM lParam)
{
  PlatformWin32* platform = reinterpret_cast<PlatformWin32*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  switch (msg)
  {
  case WM_NCCREATE:
  {
    platform = static_cast<PlatformWin32*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(platform));
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  case WM_NCDESTROY:
    // Clear the stored pointer before the HWND becomes invalid so that any
    // stray message dispatched after this point cannot dereference a freed or
    // partially-destroyed PlatformWin32 object.
    SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
    return DefWindowProc(hwnd, msg, wParam, lParam);

  case WM_CREATE:
  {
    if (hwnd)
    {
      // Remove rounded corners from the render window on Windows 11
      constexpr DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DONOTROUND;
      DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_preference,
                            sizeof(corner_preference));
    }
  }
  break;

  case WM_SIZE:
  {
    if (g_presenter)
      g_presenter->ResizeSurface();
  }
  break;

  case WM_CHAR:
    // Feed printable characters into the dev console input when it is open.
    // The backtick (0x60) is excluded: it is the toggle key and we don't want
    // it to appear in the input buffer.
    if (BigWillyConsole::IsOpen() && !AgentControlsGame())
    {
      const wchar_t wch = static_cast<wchar_t>(wParam);
      if (wch >= 0x20 && wch != L'`')
        BigWillyConsole::AppendInputChar(wch);
      return 0;
    }
    break;

  case WM_KEYDOWN:
    if (AgentControlsGame())
      return 0;

    // Backtick (VK_OEM_3) toggles the dev console. Ignore auto-repeat.
    if (wParam == VK_OEM_3 && (static_cast<ULONG_PTR>(lParam) & (1u << 30)) == 0)
    {
      BigWillyConsole::Toggle();
      if (BigWillyConsole::IsOpen())
        BigWillyConsole::Log("Furon Dev Console -- type 'help' for commands");
      return 0;
    }

    // When the console is open, intercept edit/nav keys before the normal
    // game bindings. F11 is allowed to fall through so fullscreen still works.
    if (BigWillyConsole::IsOpen())
    {
      if (wParam == VK_ESCAPE)
      {
        BigWillyConsole::Close();
        return 0;
      }
      if (wParam == VK_BACK)
      {
        BigWillyConsole::BackspaceInput();
        return 0;
      }
      if (wParam == VK_RETURN && (static_cast<ULONG_PTR>(lParam) & (1u << 30)) == 0)
      {
        ExecuteConsoleCommand(platform, BigWillyConsole::GetInput());
        BigWillyConsole::ClearInput();
        return 0;
      }
      if (wParam != VK_F11)
        return 0;
    }

    // Standard key bindings (console closed, or VK_F11 passthrough).
    // Bit 30 of lParam is the previous key state: ignore auto-repeat, or holding
    // F1 down would write a state every few milliseconds.
    if (!platform)
      break;
    if (wParam == VK_F1 && (static_cast<ULONG_PTR>(lParam) & (1u << 30)) == 0)
    {
      platform->SaveStateToStatesDirectory();
      return 0;
    }
    else if (wParam == VK_F11)
    {
      platform->ToggleFullscreen();
      return 0;
    }
    break;

  case WM_KEYUP:
    if (AgentControlsGame())
      return 0;
    break;

  case WM_KILLFOCUS:
    // Space no longer changes emulation speed. During DestroyWindow the
    // current Config layer can already be gone; do not write it here.
    break;

  case WM_SYSKEYDOWN:
    if (AgentControlsGame())
      return DefWindowProc(hwnd, msg, wParam, lParam);
    if (platform && wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000) != 0)
    {
      platform->ToggleFullscreen();
      return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);

  case WM_INITMENUPOPUP:
    if (platform)
      platform->RefreshMenu(reinterpret_cast<HMENU>(wParam));
    break;

  case WM_COMMAND:
  {
    if (AgentControlsGame())
      return 0;
    if (!platform)
      break;
    const UINT command = LOWORD(wParam);
    auto& system = Core::System::GetInstance();
    if (command == ID_SAVE_STATE)
    {
      platform->SaveStateToStatesDirectory();
    }
    else if (command == ID_PAUSE)
    {
      const bool paused = Core::GetState(system) == Core::State::Paused;
      Core::SetState(system, paused ? Core::State::Running : Core::State::Paused);
    }
    else if (command == ID_MUTE)
    {
      Config::SetCurrent(Config::MAIN_AUDIO_MUTED, !Config::Get(Config::MAIN_AUDIO_MUTED));
    }
    else if (command == ID_FULLSCREEN)
    {
      platform->ToggleFullscreen();
    }
    else if (command >= ID_LOAD_STATE_FIRST && command <= ID_LOAD_STATE_LAST)
    {
      const std::size_t index = command - ID_LOAD_STATE_FIRST;
      if (index < platform->m_load_state_paths.size())
        State::LoadAs(system, platform->m_load_state_paths[index]);
    }
    break;
  }

  case WM_CLOSE:
    if (platform)
      platform->RequestShutdown();
    break;

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  return 0;
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWin32Platform()
{
  return std::make_unique<PlatformWin32>();
}
