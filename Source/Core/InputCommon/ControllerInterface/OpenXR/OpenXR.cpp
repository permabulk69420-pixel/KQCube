// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/OpenXR/OpenXR.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "Common/VR/OpenXRInputState.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace ciface::OpenXR
{
namespace
{
constexpr std::string_view SOURCE_NAME = "OpenXR";

enum class Hand
{
  Left = 0,
  Right = 1,
};

class OpenXRDevice final : public Core::Device
{
public:
  OpenXRDevice()
  {
    AddHandInputs(Hand::Left);
    AddHandInputs(Hand::Right);
    AddOutput(new RumbleOutput(this, RumbleTarget::Both));
    AddOutput(new RumbleOutput(this, RumbleTarget::Left));
    AddOutput(new RumbleOutput(this, RumbleTarget::Right));
  }

  ~OpenXRDevice() override { Common::VR::OpenXRInputState::SetRumble(0.0f); }

  std::string GetName() const override { return "OpenXR Controller"; }
  std::string GetSource() const override { return std::string(SOURCE_NAME); }
  bool IsVirtualDevice() const override { return true; }
  int GetSortPriority() const override { return -10; }

  Core::DeviceRemoval UpdateInput() override
  {
    m_snapshot = Common::VR::OpenXRInputState::GetSnapshot();
    return Core::DeviceRemoval::Keep;
  }

  const Common::VR::OpenXRControllerState& GetControllerState(Hand hand) const
  {
    return m_snapshot.controllers[static_cast<std::size_t>(hand)];
  }

private:
  enum class DigitalControl
  {
    Primary,
    Secondary,
    Menu,
    Trigger,
    Squeeze,
    Thumbstick,
  };

  enum class AnalogControl
  {
    Trigger,
    Squeeze,
  };

  enum class AxisControl
  {
    ThumbstickX,
    ThumbstickY,
  };

  enum class RumbleTarget
  {
    Both,
    Left,
    Right,
  };

  class DigitalInput final : public Core::Device::Input
  {
  public:
    DigitalInput(const OpenXRDevice* device, Hand hand, DigitalControl control)
        : m_device(*device), m_hand(hand), m_control(control)
    {
    }

    std::string GetName() const override
    {
      const std::string prefix = m_hand == Hand::Left ? "Left" : "Right";
      switch (m_control)
      {
      case DigitalControl::Primary:
        return prefix + (m_hand == Hand::Left ? " Button X" : " Button A");
      case DigitalControl::Secondary:
        return prefix + (m_hand == Hand::Left ? " Button Y" : " Button B");
      case DigitalControl::Menu:
        return prefix + " Button Menu";
      case DigitalControl::Trigger:
        return prefix + " Button Trigger";
      case DigitalControl::Squeeze:
        return prefix + " Button Squeeze";
      case DigitalControl::Thumbstick:
        return prefix + " Button Thumbstick";
      }
      return {};
    }

    ControlState GetState() const override
    {
      const auto& state = m_device.GetControllerState(m_hand);
      if (!state.connected)
        return 0.0;

      switch (m_control)
      {
      case DigitalControl::Primary:
        return state.primary_button ? 1.0 : 0.0;
      case DigitalControl::Secondary:
        return state.secondary_button ? 1.0 : 0.0;
      case DigitalControl::Menu:
        return state.menu_button ? 1.0 : 0.0;
      case DigitalControl::Trigger:
        return state.trigger_button ? 1.0 : 0.0;
      case DigitalControl::Squeeze:
        return state.squeeze_button ? 1.0 : 0.0;
      case DigitalControl::Thumbstick:
        return state.thumbstick_button ? 1.0 : 0.0;
      }
      return 0.0;
    }

  private:
    const OpenXRDevice& m_device;
    const Hand m_hand;
    const DigitalControl m_control;
  };

  class AnalogInput final : public Core::Device::Input
  {
  public:
    AnalogInput(const OpenXRDevice* device, Hand hand, AnalogControl control)
        : m_device(*device), m_hand(hand), m_control(control)
    {
    }

    std::string GetName() const override
    {
      const std::string prefix = m_hand == Hand::Left ? "Left" : "Right";
      return prefix + (m_control == AnalogControl::Trigger ? " Trigger" : " Squeeze");
    }

    ControlState GetState() const override
    {
      const auto& state = m_device.GetControllerState(m_hand);
      if (!state.connected)
        return 0.0;
      return m_control == AnalogControl::Trigger ? state.trigger_value : state.squeeze_value;
    }

  private:
    const OpenXRDevice& m_device;
    const Hand m_hand;
    const AnalogControl m_control;
  };

  class AxisInput final : public Core::Device::Input
  {
  public:
    AxisInput(const OpenXRDevice* device, Hand hand, AxisControl axis, bool positive)
        : m_device(*device), m_hand(hand), m_axis(axis), m_positive(positive)
    {
    }

    std::string GetName() const override
    {
      const std::string prefix = m_hand == Hand::Left ? "Left" : "Right";
      const char axis = m_axis == AxisControl::ThumbstickX ? 'X' : 'Y';
      return prefix + " Thumbstick " + axis + (m_positive ? '+' : '-');
    }

    ControlState GetState() const override
    {
      const auto& state = m_device.GetControllerState(m_hand);
      if (!state.connected)
        return 0.0;

      const float value =
          m_axis == AxisControl::ThumbstickX ? state.thumbstick_x : state.thumbstick_y;
      return m_positive ? std::max(0.0f, value) : std::max(0.0f, -value);
    }

  private:
    const OpenXRDevice& m_device;
    const Hand m_hand;
    const AxisControl m_axis;
    const bool m_positive;
  };

  class RumbleOutput final : public Core::Device::Output
  {
  public:
    RumbleOutput(OpenXRDevice* device, RumbleTarget target) : m_device(*device), m_target(target) {}

    std::string GetName() const override
    {
      switch (m_target)
      {
      case RumbleTarget::Both:
        return "Motor";
      case RumbleTarget::Left:
        return "Motor Left";
      case RumbleTarget::Right:
        return "Motor Right";
      }
      return "Motor";
    }

    void SetState(ControlState state) override
    {
      m_device.SetRumbleState(static_cast<float>(state), m_target);
    }

  private:
    OpenXRDevice& m_device;
    const RumbleTarget m_target;
  };

  void AddHandInputs(Hand hand)
  {
    AddInput(new DigitalInput(this, hand, DigitalControl::Primary));
    AddInput(new DigitalInput(this, hand, DigitalControl::Secondary));
    AddInput(new DigitalInput(this, hand, DigitalControl::Menu));
    AddInput(new DigitalInput(this, hand, DigitalControl::Trigger));
    AddInput(new DigitalInput(this, hand, DigitalControl::Squeeze));
    AddInput(new DigitalInput(this, hand, DigitalControl::Thumbstick));
    AddInput(new AnalogInput(this, hand, AnalogControl::Trigger));
    AddInput(new AnalogInput(this, hand, AnalogControl::Squeeze));
    AddInput(new AxisInput(this, hand, AxisControl::ThumbstickX, false));
    AddInput(new AxisInput(this, hand, AxisControl::ThumbstickX, true));
    AddInput(new AxisInput(this, hand, AxisControl::ThumbstickY, false));
    AddInput(new AxisInput(this, hand, AxisControl::ThumbstickY, true));
  }

  void SetRumbleState(float value, RumbleTarget target)
  {
    const float amplitude = std::clamp(value, 0.0f, 1.0f);
    switch (target)
    {
    case RumbleTarget::Both:
      Common::VR::OpenXRInputState::SetRumble(amplitude);
      break;
    case RumbleTarget::Left:
      Common::VR::OpenXRInputState::SetRumbleForHand(0, amplitude);
      break;
    case RumbleTarget::Right:
      Common::VR::OpenXRInputState::SetRumbleForHand(1, amplitude);
      break;
    }
  }

  Common::VR::OpenXRInputSnapshot m_snapshot{};
};

class InputBackend final : public ciface::InputBackend
{
public:
  using ciface::InputBackend::InputBackend;

  void PopulateDevices() override
  {
    GetControllerInterface().RemoveDevice(
        [](const auto* device) { return device->GetSource() == std::string(SOURCE_NAME); });
    GetControllerInterface().AddDevice(std::make_shared<OpenXRDevice>());
  }
};
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}
}  // namespace ciface::OpenXR
