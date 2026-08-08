// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace Common::VR
{
struct OpenXRPoseState
{
  bool valid = false;
  std::array<float, 3> position{};
  std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

struct OpenXRVelocityState
{
  bool linear_valid = false;
  bool angular_valid = false;
  std::array<float, 3> linear{};
  std::array<float, 3> angular{};
};

// Where the controller's aim ray intersects KQCube's fixed virtual cinema screen. The OpenXR
// presenter computes this from the same LOCAL-space pose and dimensions used for the submitted
// quad, making u/v an absolute pointing target rather than a relative cursor delta.
struct OpenXRScreenHit
{
  bool valid = false;
  float u = 0.0f;           // -1..+1 across the screen width, +right
  float v = 0.0f;           // -1..+1 across the screen height, +up
  float distance_m = 0.0f;  // Perpendicular controller-to-screen distance in metres
};

struct OpenXRControllerState
{
  bool connected = false;
  bool primary_button = false;
  bool secondary_button = false;
  bool menu_button = false;
  bool trigger_button = false;
  bool squeeze_button = false;
  bool thumbstick_button = false;
  float trigger_value = 0.0f;
  float squeeze_value = 0.0f;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;
  OpenXRPoseState aim_pose;
  OpenXRPoseState grip_pose;
  OpenXRVelocityState grip_velocity;
  OpenXRScreenHit screen_hit;
};

struct OpenXRInputSnapshot
{
  std::array<OpenXRControllerState, 2> controllers{};
  bool runtime_active = false;
  bool session_focused = false;
  std::uint64_t generation = 0;
  std::int64_t sample_time_ns = 0;
  std::array<std::string, 2> interaction_profiles;
};

struct OpenXRHapticsState
{
  std::array<float, 2> amplitude{};
};

// OpenXR is sampled on Dolphin's render thread while ControllerInterface and Wiimote emulation can
// consume it elsewhere. Keep the hand-off deliberately small and copy-based so no OpenXR handles
// or renderer-owned objects cross that boundary.
class OpenXRInputState final
{
public:
  static OpenXRInputSnapshot GetSnapshot()
  {
    std::lock_guard lk(s_state_mutex);
    return s_state;
  }

  static void SetControllers(const std::array<OpenXRControllerState, 2>& controllers,
                             bool runtime_active,
                             const std::array<std::string, 2>& interaction_profiles = {},
                             bool session_focused = false, std::int64_t sample_time_ns = 0)
  {
    std::lock_guard lk(s_state_mutex);
    s_state.controllers = controllers;
    s_state.runtime_active = runtime_active;
    s_state.session_focused = session_focused;
    s_state.sample_time_ns = sample_time_ns;
    s_state.interaction_profiles = interaction_profiles;
    ++s_state.generation;
  }

  static std::string GetDiagnosticString()
  {
    std::lock_guard lk(s_state_mutex);
    std::string result = s_state.session_focused ? "Session: FOCUSED\n" : "Session: NOT FOCUSED\n";
    for (std::size_t i = 0; i < s_state.controllers.size(); ++i)
    {
      const char* const hand = i == 0 ? "Left" : "Right";
      const std::string& profile = s_state.interaction_profiles[i];
      result += std::string(hand) + " Profile: " + (profile.empty() ? "<none>" : profile) + "\n";
      result += std::string(hand) + ": " +
                (s_state.controllers[i].connected ? "Connected" : "Not connected") + "\n";
    }
    return result;
  }

  static void Reset()
  {
    std::lock_guard lk(s_state_mutex);
    const std::uint64_t next_generation = s_state.generation + 1;
    s_state = {};
    s_state.generation = next_generation;
    s_haptics = {};
  }

  static OpenXRHapticsState GetHaptics()
  {
    std::lock_guard lk(s_state_mutex);
    return s_haptics;
  }

  static void SetRumble(float amplitude) { SetRumble(amplitude, amplitude); }

  static void SetRumble(float left_amplitude, float right_amplitude)
  {
    std::lock_guard lk(s_state_mutex);
    s_haptics.amplitude[0] = Clamp01(left_amplitude);
    s_haptics.amplitude[1] = Clamp01(right_amplitude);
  }

  static void SetRumbleForHand(std::size_t hand_index, float amplitude)
  {
    if (hand_index >= s_haptics.amplitude.size())
      return;

    std::lock_guard lk(s_state_mutex);
    s_haptics.amplitude[hand_index] = Clamp01(amplitude);
  }

private:
  static float Clamp01(float value)
  {
    if (value < 0.0f)
      return 0.0f;
    if (value > 1.0f)
      return 1.0f;
    return value;
  }

  static inline std::mutex s_state_mutex;
  static inline OpenXRInputSnapshot s_state{};
  static inline OpenXRHapticsState s_haptics{};
};
}  // namespace Common::VR
