#include "Input/Core/InputSystem.h"
#include "ImGui/Gui.h"
#include <SDL.h>
namespace Comfy::Input
{
    template<size_t Size> using BitArray = std::array<bool, Size>;
	struct SimplifiedCombinedState
	{
		BitArray<KeyCode_Count> KeysDown;
		BitArray<KeyCode_Count> KeysPress;
		BitArray<KeyCode_Count> KeysRepeat;
		BitArray<KeyCode_Count> KeysRelease;
		KeyModifiers ModifiersDown;

		BitArray<EnumCount<Button>()> ButtonsDown;
		BitArray<EnumCount<Button>()> ButtonsPress;
		BitArray<EnumCount<Button>()> ButtonsRepeat;
		BitArray<EnumCount<Button>()> ButtonsRelease;
		std::array<f32, EnumCount<Axis>()> Axes;
		std::array<vec2, EnumCount<Stick>()> Sticks;
	};

	struct SimplifiedCombinedTimingState
	{
		std::array<f32, EnumCount<Button>()> ButtonHoldDurationSeconds;
		std::array<i32, EnumCount<Button>()> ButtonHoldDurationFrames;
		f32 TimeSinceLastModifiersChange;
		f32 FrameTimeSeconds;
	};

    struct GlobalInputSystemState
    {
        SimplifiedCombinedState ThisFrameState = {}, LastFrameState = {};
        SimplifiedCombinedTimingState ThisFrameTiming = {}, LastFrameTiming = {};
    };
    GlobalInputSystemState Global;
    namespace Detail
    {
        constexpr bool IsValidKey(KeyCode key) { return key > KeyCode_None && key < KeyCode_Count; }
        constexpr bool IsValidButton(Button button) { return button > Button::None && button < Button::Count; }
        constexpr bool IsValidAxis(Axis axis) { return axis > Axis::None && axis < Axis::Count; }
        constexpr bool IsValidStick(Stick stick) { return stick > Stick::None && stick < Stick::Count; }
		bool IsKeyDownAfterAllModifiers(const GlobalInputSystemState& global, KeyCode keyCode)
		{
			const f32 keyDuration = IsValidKey(keyCode) ? GImGui->IO.KeysDownDuration[keyCode] : 0.0f;
			return global.ThisFrameTiming.TimeSinceLastModifiersChange >= keyDuration;
		}

		bool WasKeyDownAfterAllModifiers(const GlobalInputSystemState& global, KeyCode keyCode)
		{
			const f32 keyDuration = IsValidKey(keyCode) ? GImGui->IO.KeysDownDurationPrev[keyCode] : 0.0f;
			return global.LastFrameTiming.TimeSinceLastModifiersChange >= keyDuration;
		}

		bool AreModifiersDownFirst(const GlobalInputSystemState& global, KeyCode keyCode, KeyModifiers modifiers)
		{
			const f32 keyDuration = IsValidKey(keyCode) ? GImGui->IO.KeysDownDuration[keyCode] : 0.0f;
			bool allLonger = true;

			ForEachKeyCodeInKeyModifiers(modifiers, [&](KeyCode modifierKey) { allLonger &= (GImGui->IO.KeysDownDuration[modifierKey] >= keyDuration); });
			return allLonger;
		}

		bool WereModifiersDownFirst(const GlobalInputSystemState& global, KeyCode keyCode, KeyModifiers modifiers)
		{
			const f32 keyDuration = IsValidKey(keyCode) ? GImGui->IO.KeysDownDurationPrev[keyCode] : 0.0f;
			bool allLonger = true;

			ForEachKeyCodeInKeyModifiers(modifiers, [&](KeyCode modifierKey) { allLonger &= (GImGui->IO.KeysDownDurationPrev[modifierKey] >= keyDuration); });
			return allLonger;
		}
	}
}

namespace Comfy::Input
{
    struct ControllerDevice
    {
        SDL_GameController* GameController = nullptr;
        SDL_Joystick* Joystick = nullptr;
        ControllerID InstanceID = {}, ProductID = {};
        std::string Name;
    };
    std::vector<ControllerDevice> Controllers;
    const StandardControllerLayoutMappings* ExternalMappings = nullptr;
    void GlobalSystemDispose(void*)
    {
        for (auto& device : Controllers)
        {
            if (device.GameController)
                SDL_GameControllerClose(device.GameController);
            else
                SDL_JoystickClose(device.Joystick);
        }
        Controllers.clear();
    }
    void GlobalSystemRefreshDevices()
    {
        GlobalSystemDispose(nullptr);
        for (int index = 0; index < SDL_NumJoysticks(); index++)
        {
            ControllerDevice device;
            if (SDL_IsGameController(index))
            {
                device.GameController = SDL_GameControllerOpen(index);
                if (device.GameController)
                    device.Joystick = SDL_GameControllerGetJoystick(device.GameController);
            }
            else
                device.Joystick = SDL_JoystickOpen(index);
            if (!device.Joystick)
                continue;
            const auto guid = SDL_JoystickGetGUID(device.Joystick);
            std::copy(std::begin(guid.data), std::end(guid.data), device.ProductID.GUID.begin());
            device.InstanceID = device.ProductID;
            const auto instance = SDL_JoystickInstanceID(device.Joystick);
            std::memcpy(device.InstanceID.GUID.data() + 12, &instance, sizeof(instance));
            const char* name = SDL_JoystickName(device.Joystick);
            device.Name = name ? name : "Controller";
            Controllers.push_back(std::move(device));
        }
    }
    void GlobalSystemInitialize(void*) { GlobalSystemRefreshDevices(); }
    void GlobalSystemSetExternalLayoutMappingsSource(const StandardControllerLayoutMappings* value) { ExternalMappings = value; }
    size_t GlobalSystemGetConnectedControllerCount() { return Controllers.size(); }
    ControllerInfoView GlobalSystemGetConnectedControllerInfoAt(size_t index)
    {
        if (index >= Controllers.size())
            return {};
        const auto& device = Controllers[index];
        return {device.InstanceID, device.ProductID, device.Name, device.Name,
                SDL_JoystickNumButtons(device.Joystick), SDL_JoystickNumHats(device.Joystick), SDL_JoystickNumAxes(device.Joystick)};
    }
    std::pair<const StandardControllerLayoutMapping*, size_t> GetKnownDS4LayoutMappingsView() { return {nullptr, 0}; }
    TimeSpan GlobalSystemGetUpdateFrameProcessDuration() { return TimeSpan::Zero(); }
    ControllerDevice* FindDevice(const ControllerID& id)
    {
        for (auto& device : Controllers)
            if (device.InstanceID.GUID == id.GUID)
                return &device;
        return nullptr;
    }
    f32 GetNativeAxis(const ControllerID& id, NativeAxis axis)
    {
        const auto* device = FindDevice(id);
        const int index = int(axis) - int(NativeAxis::First);
        if (!device || index < 0 || index >= SDL_JoystickNumAxes(device->Joystick))
            return 0;
        return Clamp(float(SDL_JoystickGetAxis(device->Joystick, index)) / 32767.0f, -1.0f, 1.0f);
    }
    bool IsNativeButtonDown(const ControllerID& id, NativeButton button)
    {
        const auto* device = FindDevice(id);
        if (!device)
            return false;
        if (button >= NativeButton::FirstButton && button <= NativeButton::LastButton)
        {
            const int index = int(button) - int(NativeButton::FirstButton);
            return index < SDL_JoystickNumButtons(device->Joystick) && SDL_JoystickGetButton(device->Joystick, index);
        }
        if (button >= NativeButton::FirstDPad && button <= NativeButton::LastDPad)
        {
            const int index = int(button) - int(NativeButton::FirstDPad);
            const Uint8 directions[] = {SDL_HAT_UP, SDL_HAT_LEFT, SDL_HAT_DOWN, SDL_HAT_RIGHT};
            return index / 4 < SDL_JoystickNumHats(device->Joystick)
                && (SDL_JoystickGetHat(device->Joystick, index / 4) & directions[index % 4]);
        }
        if (button >= NativeButton::FirstAxis && button <= NativeButton::LastAxis)
        {
            const int index = int(button) - int(NativeButton::FirstAxis);
            const float value = GetNativeAxis(id, NativeAxis(int(NativeAxis::First) + index / 2));
            return index % 2 == 0 ? value < -0.5f : value > 0.5f;
        }
        return false;
    }
    void PollControllers(bool focused, float elapsed)
    {
        SDL_GameControllerUpdate();
        SDL_JoystickUpdate();
        auto& state = Global.ThisFrameState;
        state.ButtonsDown.fill(false);
        state.ButtonsPress.fill(false);
        state.ButtonsRelease.fill(false);
        state.ButtonsRepeat.fill(false);
        state.Axes.fill(0);
        state.Sticks.fill(vec2(0));
        if (focused)
        {
            for (const auto& device : Controllers)
            {
                const StandardControllerLayoutMapping* mapping = nullptr;
                if (ExternalMappings)
                    for (const auto& candidate : *ExternalMappings)
                        if (candidate.ProductID.GUID == device.ProductID.GUID)
                            mapping = &candidate;
                if (mapping)
                {
                    for (size_t index = 1; index < state.ButtonsDown.size(); index++)
                        state.ButtonsDown[index] |= IsNativeButtonDown(device.InstanceID, mapping->StandardToNativeButtons[index]);
                    for (size_t index = 1; index < state.Axes.size(); index++)
                        state.Axes[index] += GetNativeAxis(device.InstanceID, mapping->StandardToNativeAxes[index]);
                }
                else if (device.GameController)
                {
                    auto held = [&](Button button, SDL_GameControllerButton native)
                    {
                        state.ButtonsDown[size_t(button)] |= SDL_GameControllerGetButton(device.GameController, native) != 0;
                    };
                    held(Button::FaceDown, SDL_CONTROLLER_BUTTON_A);
                    held(Button::FaceRight, SDL_CONTROLLER_BUTTON_B);
                    held(Button::FaceLeft, SDL_CONTROLLER_BUTTON_X);
                    held(Button::FaceUp, SDL_CONTROLLER_BUTTON_Y);
                    held(Button::DPadUp, SDL_CONTROLLER_BUTTON_DPAD_UP);
                    held(Button::DPadLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
                    held(Button::DPadDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
                    held(Button::DPadRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
                    held(Button::LeftBumper, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
                    held(Button::RightBumper, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
                    held(Button::LeftStickClick, SDL_CONTROLLER_BUTTON_LEFTSTICK);
                    held(Button::RightStickClick, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
                    held(Button::Select, SDL_CONTROLLER_BUTTON_BACK);
                    held(Button::Start, SDL_CONTROLLER_BUTTON_START);
                    held(Button::Home, SDL_CONTROLLER_BUTTON_GUIDE);
                    const SDL_GameControllerAxis axes[] = {SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
                        SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, SDL_CONTROLLER_AXIS_TRIGGERLEFT,
                        SDL_CONTROLLER_AXIS_TRIGGERRIGHT};
                    for (size_t index = 0; index < std::size(axes); index++)
                        state.Axes[index + 1] += float(SDL_GameControllerGetAxis(device.GameController, axes[index])) / 32767.0f;
                }
            }
            for (auto& axis : state.Axes)
                axis = Clamp(axis, -1.0f, 1.0f);
            auto directions = [&](Button first, float x, float y)
            {
                state.ButtonsDown[size_t(first)] |= y < -0.5f;
                state.ButtonsDown[size_t(first) + 1] |= x < -0.5f;
                state.ButtonsDown[size_t(first) + 2] |= y > 0.5f;
                state.ButtonsDown[size_t(first) + 3] |= x > 0.5f;
            };
            directions(Button::LeftStickUp, state.Axes[size_t(Axis::LeftStickX)], state.Axes[size_t(Axis::LeftStickY)]);
            directions(Button::RightStickUp, state.Axes[size_t(Axis::RightStickX)], state.Axes[size_t(Axis::RightStickY)]);
            state.ButtonsDown[size_t(Button::LeftTrigger)] |= state.Axes[size_t(Axis::LeftTrigger)] > 0.5f;
            state.ButtonsDown[size_t(Button::RightTrigger)] |= state.Axes[size_t(Axis::RightTrigger)] > 0.5f;
            state.Sticks[size_t(Stick::LeftStick)] = {state.Axes[size_t(Axis::LeftStickX)], state.Axes[size_t(Axis::LeftStickY)]};
            state.Sticks[size_t(Stick::RightStick)] = {state.Axes[size_t(Axis::RightStickX)], state.Axes[size_t(Axis::RightStickY)]};
        }
        for (size_t index = 1; index < state.ButtonsDown.size(); index++)
        {
            const bool down = state.ButtonsDown[index], previous = Global.LastFrameState.ButtonsDown[index];
            state.ButtonsPress[index] = down && !previous;
            state.ButtonsRelease[index] = !down && previous;
            auto& duration = Global.ThisFrameTiming.ButtonHoldDurationSeconds[index];
            const float oldDuration = duration;
            duration = down ? (previous ? duration + elapsed : 0) : 0;
            const auto& io = Gui::GetIO();
            state.ButtonsRepeat[index] = state.ButtonsPress[index] || (down && duration >= io.KeyRepeatDelay
                && int((duration - io.KeyRepeatDelay) / io.KeyRepeatRate) > int((oldDuration - io.KeyRepeatDelay) / io.KeyRepeatRate));
        }
    }
    FormatBuffer ControllerIDToString(const ControllerID& id)
    {
        FormatBuffer result = {};
        const auto* bytes = reinterpret_cast<const u8*>(&id);
        for (size_t index = 0; index < sizeof(id); index++)
            sprintf_s(result.data() + index * 2, result.size() - index * 2, "%02x", bytes[index]);
        return result;
    }
    ControllerID ControllerIDFromString(std::string_view input)
    {
        ControllerID result = {};
        auto* bytes = reinterpret_cast<u8*>(&result);
        if (input.size() != sizeof(result) * 2)
            return result;
        for (size_t index = 0; index < sizeof(result); index++)
        {
            unsigned value = 0;
            const std::string pair(input.substr(index * 2, 2));
            if (std::sscanf(pair.c_str(), "%02x", &value) != 1)
                return {};
            bytes[index] = value;
        }
        return result;
    }
    void GlobalSystemUpdateFrame(TimeSpan elapsed, bool focused)
    {
        Global.LastFrameState = Global.ThisFrameState;
        Global.LastFrameTiming = Global.ThisFrameTiming;
        PollControllers(focused, float(elapsed.TotalSeconds()));
        const auto& io = Gui::GetIO();
        for (KeyCode key = 0; key < KeyCode_Count; key++)
        {
            Global.ThisFrameState.KeysDown[key] = focused && io.KeysDown[key];
            Global.ThisFrameState.KeysPress[key] = focused && Gui::IsKeyPressed(key, false);
            Global.ThisFrameState.KeysRepeat[key] = focused && Gui::IsKeyPressed(key, true);
        }
        auto& modifiers = Global.ThisFrameState.ModifiersDown;
        modifiers = (io.KeyCtrl ? KeyModifiers_Ctrl : 0) | (io.KeyShift ? KeyModifiers_Shift : 0) | (io.KeyAlt ? KeyModifiers_Alt : 0);
        Global.ThisFrameTiming.TimeSinceLastModifiersChange = modifiers == Global.LastFrameState.ModifiersDown
            ? Global.LastFrameTiming.TimeSinceLastModifiersChange + float(elapsed.TotalSeconds()) : 0;
    }
	bool AreAllModifiersDown(const KeyModifiers modifiers)
	{
		return ((Global.ThisFrameState.ModifiersDown & modifiers) == modifiers);
	}

	bool WereAllModifiersDown(const KeyModifiers modifiers)
	{
		return ((Global.LastFrameState.ModifiersDown & modifiers) == modifiers);
	}

	bool AreAllModifiersUp(const KeyModifiers modifiers)
	{
		return (Global.ThisFrameState.ModifiersDown & modifiers) == 0;
	}

	bool WereAllModifiersUp(const KeyModifiers modifiers)
	{
		return (Global.LastFrameState.ModifiersDown & modifiers) == 0;
	}

	bool AreOnlyModifiersDown(const KeyModifiers modifiers)
	{
		return (Global.ThisFrameState.ModifiersDown == modifiers);
	}

	bool WereOnlyModifiersDown(const KeyModifiers modifiers)
	{
		return (Global.LastFrameState.ModifiersDown == modifiers);
	}

	bool IsKeyDown(const KeyCode keyCode)
	{
		if (!Detail::IsValidKey(keyCode))
			return false;

		return Global.ThisFrameState.KeysDown[keyCode];
	}

	bool WasKeyDown(const KeyCode keyCode)
	{
		if (!Detail::IsValidKey(keyCode))
			return false;

		return Global.LastFrameState.KeysDown[keyCode];
	}

	bool IsKeyPressed(const KeyCode keyCode, bool repeat)
	{
		if (!Detail::IsValidKey(keyCode))
			return false;

		if (repeat)
			return Global.ThisFrameState.KeysRepeat[keyCode];
		else
			return Global.ThisFrameState.KeysPress[keyCode];
	}

	bool IsKeyReleased(const KeyCode keyCode)
	{
		if (!Detail::IsValidKey(keyCode))
			return false;

		return (!Global.ThisFrameState.KeysDown[keyCode] && Global.LastFrameState.KeysDown[keyCode]);
	}

	bool IsButtonDown(const Button button)
	{
		if (!Detail::IsValidButton(button))
			return false;

		return Global.ThisFrameState.ButtonsDown[static_cast<u8>(button)];
	}

	bool WasButtonDown(const Button button)
	{
		if (!Detail::IsValidButton(button))
			return false;

		return Global.LastFrameState.ButtonsDown[static_cast<u8>(button)];
	}

	bool IsButtonPressed(const Button button, bool repeat)
	{
		if (!Detail::IsValidButton(button))
			return false;

		const auto index = static_cast<u8>(button);
		if (repeat)
			return Global.ThisFrameState.ButtonsRepeat[index];
		else
			return Global.ThisFrameState.ButtonsPress[index];
	}

	bool IsButtonReleased(const Button button)
	{
		if (!Detail::IsValidButton(button))
			return false;

		const auto index = static_cast<u8>(button);
		return Global.ThisFrameState.ButtonsRelease[index];
	}

	f32 GetAxis(const Axis axis)
	{
		if (!Detail::IsValidAxis(axis))
			return false;

		return Global.ThisFrameState.Axes[static_cast<u8>(axis)];
	}

	vec2 GetStick(const Stick stick)
	{
		if (!Detail::IsValidStick(stick))
			return vec2(0.0f);

		return Global.ThisFrameState.Sticks[static_cast<u8>(stick)];
	}

	// BUG: Regular bindings with modifier keys as primary keys aren't triggered correctly
	bool IsDown(const Binding& binding, ModifierBehavior behavior)
	{
		if (binding.Type == BindingType::Keyboard)
		{
			if (behavior == ModifierBehavior_Strict)
				return IsKeyDown(binding.Keyboard.Key) && AreOnlyModifiersDown(binding.Keyboard.Modifiers) && Detail::IsKeyDownAfterAllModifiers(Global, binding.Keyboard.Key);
			else
				return IsKeyDown(binding.Keyboard.Key) && AreAllModifiersDown(binding.Keyboard.Modifiers) && Detail::AreModifiersDownFirst(Global, binding.Keyboard.Key, binding.Keyboard.Modifiers);
		}
		else if (binding.Type == BindingType::Controller)
		{
			return IsButtonDown(binding.Controller.Button);
		}
		else
		{
			return false;
		}
	}

	bool WasDown(const Binding& binding, ModifierBehavior behavior)
	{
		if (binding.Type == BindingType::Keyboard)
		{
			if (behavior == ModifierBehavior_Strict)
				return WasKeyDown(binding.Keyboard.Key) && WereOnlyModifiersDown(binding.Keyboard.Modifiers) && Detail::WasKeyDownAfterAllModifiers(Global, binding.Keyboard.Key);
			else
				return WasKeyDown(binding.Keyboard.Key) && WereAllModifiersDown(binding.Keyboard.Modifiers) && Detail::WereModifiersDownFirst(Global, binding.Keyboard.Key, binding.Keyboard.Modifiers);
		}
		else if (binding.Type == BindingType::Controller)
		{
			return WasButtonDown(binding.Controller.Button);
		}
		else
		{
			return false;
		}
	}

	bool IsPressed(const Binding& binding, bool repeat, ModifierBehavior behavior)
	{
		if (binding.Type == BindingType::Keyboard)
		{
			// NOTE: Still have to explictily check the modifier hold durations here in case of repeat
			if (behavior == ModifierBehavior_Strict)
				return IsKeyPressed(binding.Keyboard.Key, repeat) && AreOnlyModifiersDown(binding.Keyboard.Modifiers) && Detail::IsKeyDownAfterAllModifiers(Global, binding.Keyboard.Key);
			else
				return IsKeyPressed(binding.Keyboard.Key, repeat) && AreAllModifiersDown(binding.Keyboard.Modifiers) && Detail::AreModifiersDownFirst(Global, binding.Keyboard.Key, binding.Keyboard.Modifiers);
		}
		else if (binding.Type == BindingType::Controller)
		{
			return IsButtonPressed(binding.Controller.Button, repeat);
		}
		else
		{
			return false;
		}
	}

	bool IsReleased(const Binding& binding, ModifierBehavior behavior)
	{
		return !IsDown(binding, behavior) && WasDown(binding, behavior);
	}

	bool IsAnyDown(const MultiBinding& binding, ModifierBehavior behavior)
	{
		return std::any_of(binding.begin(), binding.end(), [&](auto& b) { return IsDown(b, behavior); });
	}

	bool IsAnyPressed(const MultiBinding& binding, bool repeat, ModifierBehavior behavior)
	{
		return std::any_of(binding.begin(), binding.end(), [&](auto& b) { return IsPressed(b, repeat, behavior); });
	}

	bool IsAnyReleased(const MultiBinding& binding, ModifierBehavior behavior)
	{
		return std::any_of(binding.begin(), binding.end(), [&](auto& b) { return IsReleased(b, behavior); });
	}

	bool IsLastReleased(const MultiBinding& binding, ModifierBehavior behavior)
	{
		const bool allUp = std::all_of(binding.begin(), binding.end(), [&](auto& b) { return !IsDown(b, behavior); });
		const bool anyReleased = std::any_of(binding.begin(), binding.end(), [&](auto& b) { return IsReleased(b, behavior); });

		return (allUp && anyReleased);
	}
}
