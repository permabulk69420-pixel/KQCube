// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/Wiimote.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#ifdef ANDROID
#include "Common/VR/OpenXRInputState.h"
#endif

#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/InputConfig.h"

// Limit the amount of wiimote connect requests, when a button is pressed in disconnected state
static std::array<u8, MAX_BBMOTES> s_last_connect_request_counter;

namespace
{
static std::array<std::atomic<WiimoteSource>, MAX_BBMOTES> s_wiimote_sources;
static std::optional<Config::ConfigChangedCallbackID> s_config_callback_id = std::nullopt;

bool IsEmulatedSource(WiimoteSource source)
{
  return source == WiimoteSource::Emulated || source == WiimoteSource::OpenXR;
}

#ifdef ANDROID
Common::Quaternion ToQuaternion(const std::array<float, 4>& quaternion)
{
  return {quaternion[3], quaternion[0], quaternion[1], quaternion[2]};
}

Common::Vec3 ToVec3(const std::array<float, 3>& vector)
{
  return {vector[0], vector[1], vector[2]};
}

struct OpenXRVelocityHistory
{
  bool has_pose_sample = false;
  bool has_velocity_sample = false;
  Common::Vec3 previous_position{};
  Common::Vec3 previous_velocity{};
  s64 previous_time_ns = 0;
};

struct OpenXRWiimoteState
{
  u64 generation = std::numeric_limits<u64>::max();
  bool primary_button = false;
  bool trigger_button = false;
  bool squeeze_button = false;
  bool home_button = false;
  float thumbstick_x = 0.0f;
  float thumbstick_y = 0.0f;
  Common::Vec3 acceleration{0.0f, 0.0f, float(MathUtil::GRAVITY_ACCELERATION)};
  Common::Vec3 angular_velocity{};
  float ir_x = std::numeric_limits<float>::quiet_NaN();
  float ir_y = 0.0f;
  float ir_z = 0.0f;
};

// A real Wii Remote's IR camera remains useful beyond the configured cursor range. Keep brief
// tracking spikes from hiding the pointer, but still let a sustained off-screen aim lose the bar.
constexpr float OPENXR_IR_HIDE_MARGIN_U = 1.9f;
constexpr float OPENXR_IR_HIDE_MARGIN_V = 1.5f;
constexpr int OPENXR_IR_HIDE_DELAY_SNAPSHOTS = 9;

s64 OpenXRSampleTimeNs(const Common::VR::OpenXRInputSnapshot& snapshot)
{
  if (snapshot.sample_time_ns != 0)
    return snapshot.sample_time_ns;
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

OpenXRWiimoteState BuildOpenXRState(const Common::VR::OpenXRControllerState& controller,
                                    OpenXRVelocityHistory* velocity_history, s64 sample_time_ns)
{
  OpenXRWiimoteState out;
  out.primary_button = controller.primary_button;
  out.trigger_button = controller.trigger_button;
  out.squeeze_button = controller.squeeze_button;
  out.thumbstick_x = controller.thumbstick_x;
  out.thumbstick_y = controller.thumbstick_y;

  const bool has_grip_pose = controller.grip_pose.valid;
  const bool has_aim_pose = controller.aim_pose.valid;
  if (!has_grip_pose && !has_aim_pose)
    return out;

  // Use aim pose for the Wii Remote local frame. Grip and aim orientations differ substantially
  // on Touch controllers; using grip here makes gravity and wrist rotation disagree with the
  // direction the player is visibly pointing the remote.
  const Common::Quaternion reference_orientation =
      (has_aim_pose ? ToQuaternion(controller.aim_pose.orientation) :
                      ToQuaternion(controller.grip_pose.orientation))
          .Normalized();
  const Common::Matrix33 world_to_local =
      Common::Matrix33::FromQuaternion(reference_orientation).Transposed();
  const auto get_matrix = [&world_to_local](int row, int column) {
    return world_to_local.data[row * 3 + column];
  };

  Common::Vec3 relative_acceleration{};
  const float dt = velocity_history->has_pose_sample ?
                       float(sample_time_ns - velocity_history->previous_time_ns) * 1e-9f :
                       0.0f;

  Common::Vec3 current_position{};
  std::optional<Common::Vec3> pose_velocity;
  if (has_grip_pose)
  {
    current_position = ToVec3(controller.grip_pose.position);
    if (velocity_history->has_pose_sample && dt > 0.001f)
      pose_velocity = (current_position - velocity_history->previous_position) / dt;
  }

  std::optional<Common::Vec3> current_velocity;
  if (controller.grip_velocity.linear_valid)
    current_velocity = ToVec3(controller.grip_velocity.linear);

  if (pose_velocity)
  {
    // Blend pose-derived and runtime velocity. This preserves fast thrusts on runtimes that smooth
    // their reported linear velocity heavily while still using the runtime's higher-rate estimate.
    if (current_velocity)
      current_velocity = (*current_velocity + *pose_velocity) * 0.5f;
    else
      current_velocity = *pose_velocity;
  }

  if (current_velocity && velocity_history->has_velocity_sample && dt > 0.001f)
  {
    const Common::Vec3 world_acceleration =
        (*current_velocity - velocity_history->previous_velocity) / dt;
    relative_acceleration = world_to_local * world_acceleration;
  }

  if (has_grip_pose)
  {
    velocity_history->previous_position = current_position;
    velocity_history->previous_time_ns = sample_time_ns;
    velocity_history->has_pose_sample = true;
  }
  else
  {
    velocity_history->has_pose_sample = false;
    velocity_history->has_velocity_sample = false;
  }

  if (current_velocity)
  {
    velocity_history->previous_velocity = *current_velocity;
    velocity_history->has_velocity_sample = true;
  }
  else if (!velocity_history->has_pose_sample)
  {
    velocity_history->has_velocity_sample = false;
  }

  // OpenXR is right-handed (+X right, +Y up, -Z forward). These deliberately reordered signs map
  // world gravity into Dolphin's Wii Remote accelerometer convention.
  float gx = -get_matrix(0, 1);
  float gz = get_matrix(1, 1);
  float gy = get_matrix(2, 1);

  gx -= relative_acceleration.x / float(MathUtil::GRAVITY_ACCELERATION);
  gz += relative_acceleration.y / float(MathUtil::GRAVITY_ACCELERATION);
  gy += relative_acceleration.z / float(MathUtil::GRAVITY_ACCELERATION);

  out.acceleration = Common::Vec3(gx, gy, gz) * float(MathUtil::GRAVITY_ACCELERATION);

  if (controller.grip_velocity.angular_valid)
  {
    const Common::Vec3 world_angular_velocity = ToVec3(controller.grip_velocity.angular);
    const Common::Vec3 local_angular_velocity = world_to_local * world_angular_velocity;
    // Aim-local (X=right, Y=up, Z=back) to Dolphin's gyro convention
    // (+x=pitch down, +y=roll left, +z=yaw left). Pitch up is +X in aim-local and therefore -X
    // for the emulated Wii Remote.
    out.angular_velocity =
        Common::Vec3(-local_angular_velocity.x, local_angular_velocity.z, local_angular_velocity.y);
  }

  const Common::VR::OpenXRScreenHit& hit = controller.screen_hit;
  if (hit.valid)
  {
    out.ir_x = hit.u;
    out.ir_y = hit.v;
    // EmulatePoint uses a two-metre neutral sensor-bar distance. Feed the actual perpendicular
    // distance as its offset so leaning toward or away from the screen changes dot spacing.
    out.ir_z = std::clamp(hit.distance_m, 0.1f, 8.0f) - 2.0f;
  }

  return out;
}

ControllerEmu::InputOverrideFunction CreateOpenXRInputOverrideFunction(unsigned int wiimote_index,
                                                                       bool prefer_left_hand)
{
  OpenXRWiimoteState cached_state;
  OpenXRVelocityHistory left_velocity_history;
  OpenXRVelocityHistory right_velocity_history;
  int offscreen_snapshots = 0;
  float held_ir_x = std::numeric_limits<float>::quiet_NaN();
  float held_ir_y = 0.0f;
  float held_ir_z = 0.0f;

  return [wiimote_index, prefer_left_hand, cached_state, left_velocity_history,
          right_velocity_history, offscreen_snapshots, held_ir_x, held_ir_y,
          held_ir_z](std::string_view group_name, std::string_view control_name,
                     ControlState) mutable -> std::optional<ControlState> {
    if (s_wiimote_sources[wiimote_index].load() != WiimoteSource::OpenXR)
      return std::nullopt;

    const Common::VR::OpenXRInputSnapshot snapshot = Common::VR::OpenXRInputState::GetSnapshot();
    if (!snapshot.runtime_active)
      return std::nullopt;

    if (cached_state.generation != snapshot.generation)
    {
      const auto& left = snapshot.controllers[0];
      const auto& right = snapshot.controllers[1];
      const bool right_valid = right.connected || right.grip_pose.valid || right.aim_pose.valid;
      const bool left_valid = left.connected || left.grip_pose.valid || left.aim_pose.valid;
      const s64 sample_time_ns = OpenXRSampleTimeNs(snapshot);

      if (prefer_left_hand && left_valid)
        cached_state = BuildOpenXRState(left, &left_velocity_history, sample_time_ns);
      else if (!prefer_left_hand && right_valid)
        cached_state = BuildOpenXRState(right, &right_velocity_history, sample_time_ns);
      else if (left_valid)
        cached_state = BuildOpenXRState(left, &left_velocity_history, sample_time_ns);
      else if (right_valid)
        cached_state = BuildOpenXRState(right, &right_velocity_history, sample_time_ns);
      else
        cached_state = {};
      // Quest exposes the menu button on the left controller. Keep Home reachable even while the
      // right controller is acting as the Wii Remote.
      cached_state.home_button = left.menu_button || right.menu_button;

      const bool on_screen = !std::isnan(cached_state.ir_x) &&
                             std::abs(cached_state.ir_x) <= OPENXR_IR_HIDE_MARGIN_U &&
                             std::abs(cached_state.ir_y) <= OPENXR_IR_HIDE_MARGIN_V;
      if (on_screen)
      {
        offscreen_snapshots = 0;
        held_ir_x = cached_state.ir_x;
        held_ir_y = cached_state.ir_y;
        held_ir_z = cached_state.ir_z;
      }
      else if (++offscreen_snapshots < OPENXR_IR_HIDE_DELAY_SNAPSHOTS)
      {
        if (std::isnan(cached_state.ir_x))
        {
          cached_state.ir_x = held_ir_x;
          cached_state.ir_y = held_ir_y;
          cached_state.ir_z = held_ir_z;
        }
      }
      else
      {
        cached_state.ir_x = std::numeric_limits<float>::quiet_NaN();
        cached_state.ir_y = 0.0f;
        cached_state.ir_z = 0.0f;
      }

      cached_state.generation = snapshot.generation;
    }

    if (group_name == WiimoteEmu::Wiimote::BUTTONS_GROUP)
    {
      constexpr float stick_button_threshold = 0.5f;
      if (control_name == WiimoteEmu::Wiimote::A_BUTTON)
        return cached_state.primary_button;
      if (control_name == WiimoteEmu::Wiimote::B_BUTTON)
        return cached_state.trigger_button;
      if (control_name == WiimoteEmu::Wiimote::ONE_BUTTON)
        return cached_state.thumbstick_y > stick_button_threshold;
      if (control_name == WiimoteEmu::Wiimote::TWO_BUTTON)
        return cached_state.thumbstick_y < -stick_button_threshold;
      if (control_name == WiimoteEmu::Wiimote::MINUS_BUTTON)
        return cached_state.thumbstick_x < -stick_button_threshold;
      if (control_name == WiimoteEmu::Wiimote::PLUS_BUTTON)
        return cached_state.thumbstick_x > stick_button_threshold;
      if (control_name == WiimoteEmu::Wiimote::HOME_BUTTON)
        return cached_state.home_button;
      if (control_name == WiimoteEmu::Nunchuk::C_BUTTON)
        return cached_state.squeeze_button;
      if (control_name == WiimoteEmu::Nunchuk::Z_BUTTON)
        return cached_state.trigger_button;
    }
    else if (group_name == WiimoteEmu::Nunchuk::STICK_GROUP)
    {
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return cached_state.thumbstick_x;
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return cached_state.thumbstick_y;
    }
    else if (group_name == WiimoteEmu::Wiimote::ACCELEROMETER_GROUP)
    {
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return cached_state.acceleration.x;
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return cached_state.acceleration.y;
      if (control_name == ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE)
        return cached_state.acceleration.z;
    }
    else if (group_name == WiimoteEmu::Wiimote::GYROSCOPE_GROUP)
    {
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return cached_state.angular_velocity.x;
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return cached_state.angular_velocity.y;
      if (control_name == ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE)
        return cached_state.angular_velocity.z;
    }
    else if (group_name == WiimoteEmu::Wiimote::IR_GROUP)
    {
      // This is absolute aim-vs-screen pointing, so Dolphin's relative cursor recenter control has
      // nothing to reset. NaN means the emulated camera has lost the virtual sensor bar.
      if (control_name == "Recenter")
        return std::nullopt;
      if (control_name == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE)
        return static_cast<double>(cached_state.ir_x);
      if (control_name == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE)
        return static_cast<double>(cached_state.ir_y);
      if (control_name == ControllerEmu::ReshapableInput::Z_INPUT_OVERRIDE)
        return static_cast<double>(cached_state.ir_z);
    }

    return std::nullopt;
  };
}

std::array<bool, MAX_BBMOTES> s_openxr_overrides_enabled{};
std::array<u32, MAX_BBMOTES> s_pre_openxr_attachments{};
std::array<bool, MAX_BBMOTES> s_pre_openxr_motion_plus{};

void UpdateOpenXRInputOverride(unsigned int index, WiimoteSource source)
{
  auto* wiimote = static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(index));
  if (!wiimote)
    return;

  auto* attachments = static_cast<ControllerEmu::Attachments*>(
      wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Attachments));

  const auto apply_to_attachments =
      [attachments](const ControllerEmu::InputOverrideFunction& override_function) {
        if (!attachments)
          return;

        for (const auto& attachment : attachments->GetAttachmentList())
        {
          if (!attachment)
            continue;
          if (override_function)
            attachment->SetInputOverrideFunction(override_function);
          else
            attachment->ClearInputOverrideFunction();
        }
      };

  if (source == WiimoteSource::OpenXR)
  {
    if (!s_openxr_overrides_enabled[index])
    {
      if (attachments)
        s_pre_openxr_attachments[index] = attachments->GetSelectedAttachment();
      s_pre_openxr_motion_plus[index] = wiimote->GetMotionPlusSetting().GetValue();
    }

    // Selecting the OpenXR source is intentionally self-contained: Skyward Sword needs both a
    // Nunchuk and MotionPlus, and requiring a separate profile import left them silently disabled.
    if (attachments)
      attachments->SetSelectedAttachment(WiimoteEmu::ExtensionNumber::NUNCHUK);
    wiimote->GetMotionPlusSetting().SetValue(true);

    const bool prefer_left_hand = Config::Get(
        Config::Info<bool>{{Config::System::Main, "Android", "QuestLeftHanded"}, false});
    wiimote->SetInputOverrideFunction(CreateOpenXRInputOverrideFunction(index, prefer_left_hand));
    apply_to_attachments(CreateOpenXRInputOverrideFunction(index, !prefer_left_hand));
    s_openxr_overrides_enabled[index] = true;
  }
  else if (s_openxr_overrides_enabled[index])
  {
    wiimote->ClearInputOverrideFunction();
    apply_to_attachments({});
    if (attachments)
      attachments->SetSelectedAttachment(s_pre_openxr_attachments[index]);
    wiimote->GetMotionPlusSetting().SetValue(s_pre_openxr_motion_plus[index]);
    Common::VR::OpenXRInputState::SetRumble(0.0f);
    s_openxr_overrides_enabled[index] = false;
  }
}
#else
void UpdateOpenXRInputOverride(unsigned int, WiimoteSource)
{
}
#endif

WiimoteSource GetSource(unsigned int index)
{
  return s_wiimote_sources[index];
}

void OnSourceChanged(unsigned int index, WiimoteSource source)
{
  const WiimoteSource previous_source = s_wiimote_sources[index].exchange(source);

  if (previous_source == source)
  {
    // Reapply the override in case hand preference changed while the source stayed OpenXR.
    UpdateOpenXRInputOverride(index, source);
    return;
  }

  UpdateOpenXRInputOverride(index, source);

  WiimoteReal::HandleWiimoteSourceChange(index);

  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  WiimoteCommon::UpdateSource(index);
}

void RefreshConfig()
{
  for (int i = 0; i < MAX_BBMOTES; ++i)
    OnSourceChanged(i, Config::Get(Config::GetInfoForWiimoteSource(i)));
}

}  // namespace

namespace WiimoteCommon
{
void UpdateSource(unsigned int index)
{
  const auto bluetooth = WiiUtils::GetBluetoothEmuDevice();
  if (bluetooth == nullptr)
    return;

  bluetooth->AccessWiimoteByIndex(index)->SetSource(GetHIDWiimoteSource(index));
}

HIDWiimote* GetHIDWiimoteSource(unsigned int index)
{
  HIDWiimote* hid_source = nullptr;

  switch (GetSource(index))
  {
  case WiimoteSource::Emulated:
  case WiimoteSource::OpenXR:
    hid_source = static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(index));
    break;

  case WiimoteSource::Real:
    hid_source = WiimoteReal::g_wiimotes[index].get();
    break;

  default:
    break;
  }

  return hid_source;
}

}  // namespace WiimoteCommon

namespace Wiimote
{
static InputConfig s_config(WIIMOTE_INI_NAME, _trans("Wii Remote"), "Wiimote", "Wiimote");

InputConfig* GetConfig()
{
  return &s_config;
}

ControllerEmu::ControlGroup* GetWiimoteGroup(int number, WiimoteEmu::WiimoteGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetWiimoteGroup(group);
}

ControllerEmu::ControlGroup* GetNunchukGroup(int number, WiimoteEmu::NunchukGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetNunchukGroup(group);
}

ControllerEmu::ControlGroup* GetClassicGroup(int number, WiimoteEmu::ClassicGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetClassicGroup(group);
}

ControllerEmu::ControlGroup* GetGuitarGroup(int number, WiimoteEmu::GuitarGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetGuitarGroup(group);
}

ControllerEmu::ControlGroup* GetDrumsGroup(int number, WiimoteEmu::DrumsGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetDrumsGroup(group);
}

ControllerEmu::ControlGroup* GetTurntableGroup(int number, WiimoteEmu::TurntableGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetTurntableGroup(group);
}

ControllerEmu::ControlGroup* GetUDrawTabletGroup(int number, WiimoteEmu::UDrawTabletGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetUDrawTabletGroup(group);
}

ControllerEmu::ControlGroup* GetDrawsomeTabletGroup(int number,
                                                    WiimoteEmu::DrawsomeTabletGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetDrawsomeTabletGroup(group);
}

ControllerEmu::ControlGroup* GetTaTaConGroup(int number, WiimoteEmu::TaTaConGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))->GetTaTaConGroup(group);
}

ControllerEmu::ControlGroup* GetShinkansenGroup(int number, WiimoteEmu::ShinkansenGroup group)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(number))
      ->GetShinkansenGroup(group);
}

void Shutdown()
{
  s_config.UnregisterHotplugCallback();

  s_config.ClearControllers();

#ifdef ANDROID
  // Controller objects are recreated for later game boots in the same Android process. Do not
  // carry override ownership or saved attachment settings across those object lifetimes.
  s_openxr_overrides_enabled.fill(false);
  s_pre_openxr_attachments.fill(0);
  s_pre_openxr_motion_plus.fill(false);
  Common::VR::OpenXRInputState::SetRumble(0.0f);
#endif

  WiimoteReal::Stop();

  if (s_config_callback_id)
  {
    Config::RemoveConfigChangedCallback(*s_config_callback_id);
    s_config_callback_id = std::nullopt;
  }
}

void Initialize(InitializeMode init_mode)
{
  if (s_config.ControllersNeedToBeCreated())
  {
    for (unsigned int i = WIIMOTE_CHAN_0; i < MAX_BBMOTES; ++i)
      s_config.CreateController<WiimoteEmu::Wiimote>(i);
  }

  s_config.RegisterHotplugCallback();

  LoadConfig();

  if (!s_config_callback_id)
    s_config_callback_id = Config::AddConfigChangedCallback(RefreshConfig);
  RefreshConfig();

  WiimoteReal::Initialize(init_mode);

  // Reload Wiimotes with our settings
  auto& movie = Core::System::GetInstance().GetMovie();
  if (movie.IsMovieActive())
    movie.ChangeWiiPads();
}

void ResetAllWiimotes()
{
  for (int i = WIIMOTE_CHAN_0; i < MAX_BBMOTES; ++i)
    static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(i))->Reset();
}

void LoadConfig()
{
  s_config.LoadConfig();
  s_last_connect_request_counter.fill(0);
}

void GenerateDynamicInputTextures()
{
  s_config.GenerateControllerTextures();
}

void Resume()
{
  WiimoteReal::Resume();
}

void Pause()
{
  WiimoteReal::Pause();
}

void DoState(PointerWrap& p)
{
  for (int i = 0; i < MAX_BBMOTES; ++i)
  {
    const WiimoteSource source = GetSource(i);
    auto state_wiimote_source = u8(source);
    p.Do(state_wiimote_source);

    if (IsEmulatedSource(WiimoteSource(state_wiimote_source)))
    {
      // Sync complete state of emulated wiimotes.
      static_cast<WiimoteEmu::Wiimote*>(s_config.GetController(i))->DoState(p);
    }

    if (p.IsReadMode())
    {
      // If using a real wiimote or the save-state source does not match the current source,
      // then force a reconnection on load.
      if (source == WiimoteSource::Real || source != WiimoteSource(state_wiimote_source))
        WiimoteCommon::UpdateSource(i);
    }
  }
}
}  // namespace Wiimote
