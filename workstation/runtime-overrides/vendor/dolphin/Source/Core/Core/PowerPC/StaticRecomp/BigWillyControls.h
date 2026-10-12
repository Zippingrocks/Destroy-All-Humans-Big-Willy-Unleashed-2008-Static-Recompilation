// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <array>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace BigWillyControls
{
// One pending request; an active command can finish before the next is consumed.
inline std::atomic_uint g_game_command{0};
inline constexpr std::array<std::string_view, 4> LEVELS = {
    "MAP01_EastCoastCity", "MAP03_MidwestTown", "MAP04_Jungle", "MAP05_Island"};
inline int FindLevel(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  if (value.starts_with("level_")) value.erase(0, 6);
  if (value.ends_with(".stream")) value.resize(value.size() - 7);
  for (unsigned i = 0; i < LEVELS.size(); ++i)
  {
    std::string name(LEVELS[i]);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    if (name == value) return static_cast<int>(i);
  }
  return -1;
}
inline bool RequestGameCommand(unsigned value)
{
  unsigned empty = 0;
  return g_game_command.compare_exchange_strong(empty, value);
}
inline bool RequestEnterBigWilly() { return RequestGameCommand(1); }
inline bool RequestGiveAllWeapons() { return RequestGameCommand(2); }
inline bool RequestLoadLevel(unsigned index) { return index < LEVELS.size() && RequestGameCommand(3 | (index << 8)); }
// Host-window input is delivered on the UI thread. The static recompiler
// consumes this request at a safe native-dispatch boundary on the emulation
// thread, where it can call the game's own cutscene/video teardown code.
extern std::atomic_bool g_skip_movie_requested;

inline void RequestSkipMovie()
{
  g_skip_movie_requested.store(true, std::memory_order_release);
}

inline bool ConsumeSkipMovieRequest()
{
  return g_skip_movie_requested.exchange(false, std::memory_order_acq_rel);
}
}  // namespace BigWillyControls
