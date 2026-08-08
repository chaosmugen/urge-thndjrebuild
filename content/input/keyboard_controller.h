// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_INPUT_KEYBOARD_CONTROLLER_H_
#define CONTENT_INPUT_KEYBOARD_CONTROLLER_H_

#include "content/context/engine_object.h"
#include "content/profile/content_profile.h"
#include "content/profile/i18n_profile.h"
#include "content/public/engine_input.h"
#include "SDL3/SDL_gamepad.h"
#include "ui/widget/widget.h"

namespace content {

class KeyboardControllerImpl : public Input, public EngineObject {
 public:
  struct KeyBinding {
    std::string sym;
    SDL_Scancode scancode;

    bool operator==(const KeyBinding& other) const {
      return sym == other.sym && scancode == other.scancode;
    }
  };

  using KeySymMap = std::vector<KeyBinding>;
  using KeyState = struct {
    bool pressed;
    bool trigger;
    bool repeat;
    int32_t repeat_count;
  };

  KeyboardControllerImpl(ExecutionContext* execution_context);
  ~KeyboardControllerImpl() override;

  KeyboardControllerImpl(const KeyboardControllerImpl&) = delete;
  KeyboardControllerImpl& operator=(const KeyboardControllerImpl&) = delete;

  void ApplyKeySymBinding(const KeySymMap& keysyms);
  bool CreateButtonGUISettings();

 public:
  void Update(ExceptionState& exception_state) override;
  bool IsPressed(const std::string& sym,
                 ExceptionState& exception_state) override;
  bool IsTriggered(const std::string& sym,
                   ExceptionState& exception_state) override;
  bool IsRepeated(const std::string& sym,
                  ExceptionState& exception_state) override;
  int32_t Dir4(ExceptionState& exception_state) override;
  int32_t Dir8(ExceptionState& exception_state) override;

  bool KeyPressed(int32_t scancode, ExceptionState& exception_state) override;
  bool KeyTriggered(int32_t scancode, ExceptionState& exception_state) override;
  bool KeyRepeated(int32_t scancode, ExceptionState& exception_state) override;
  std::string GetKeyName(int32_t scancode,
                         ExceptionState& exception_state) override;
  std::vector<int32_t> GetKeysFromFlag(
      const std::string& flag,
      ExceptionState& exception_state) override;
  void SetKeysFromFlag(const std::string& flag,
                       const std::vector<int32_t>& keys,
                       ExceptionState& exception_state) override;

  std::vector<int32_t> GetRecentPressed(
      ExceptionState& exception_state) override;
  std::vector<int32_t> GetRecentTriggered(
      ExceptionState& exception_state) override;
  std::vector<int32_t> GetRecentRepeated(
      ExceptionState& exception_state) override;

  bool Emulate(int32_t scancode,
               bool down,
               int32_t modifier,
               bool repeat,
               ExceptionState& exception_state) override;

  void BindGamepad(int32_t button,
                   int32_t scancode,
                   ExceptionState& exception_state) override;
  void UnbindGamepad(int32_t button,
                     ExceptionState& exception_state) override;

  void OpenGamepad();
  void CloseGamepad();
  void FeedGamepadButton(SDL_GamepadButton button, bool down);
  void FeedGamepadAxis(SDL_GamepadAxis axis, int16_t value);

 private:
  void UpdateDir4Internal();
  void UpdateDir8Internal();

  void TryReadBindingsInternal();
  void StorageBindingsInternal();

  // Gamepad support. Buttons are bound to existing symbols via BindGamepad and
  // merged into the keyboard state on Update(). D-pad and left stick always map
  // to the arrow keys regardless of configuration.
  static constexpr int16_t kGamepadDeadzone = 9830;
  static constexpr int16_t kGamepadTriggerThreshold = 8192;

  // SDL3 exposes the triggers (LT/RT) only as axes, never as buttons, so they
  // get two "virtual" slots above the physical SDL_GamepadButton range. Scripts
  // bind them as GP_LT / GP_RT (26 / 27) and they map to a scancode just like
  // any button slot; the merge loop drives them from the trigger axes.
  static constexpr int32_t kGamepadVtLT = SDL_GAMEPAD_BUTTON_COUNT;      // 26
  static constexpr int32_t kGamepadVtRT = SDL_GAMEPAD_BUTTON_COUNT + 1;  // 27
  static constexpr int32_t kGamepadSlotCount = SDL_GAMEPAD_BUTTON_COUNT + 2;

  KeySymMap key_bindings_;
  KeySymMap setting_bindings_;
  bool disable_gui_key_input_;

  std::array<KeyState, SDL_SCANCODE_COUNT> key_states_;
  std::array<KeyState, SDL_SCANCODE_COUNT> recent_key_states_;

  // Physical keyboard state from the previous frame. Keyboard trigger
  // detection uses this (not the merged key_states_, which also carries
  // gamepad input) so a held gamepad button bound to the same scancode cannot
  // suppress a keyboard keypress.
  std::array<bool, SDL_SCANCODE_COUNT> keyboard_prev_state_{};

  struct {
    int32_t active = 0;
    int32_t previous = 0;
  } dir4_state_;

  struct {
    int32_t active = 0;
  } dir8_state_;

  SDL_Gamepad* gamepad_ = nullptr;
  // Holds physical button state (slots 0..SDL_GAMEPAD_BUTTON_COUNT-1); the two
  // virtual trigger slots are driven from gamepad_trigger_state_ instead.
  std::array<bool, kGamepadSlotCount> gamepad_button_state_{};
  std::array<bool, 2> gamepad_trigger_state_{};  // [0]=LT, [1]=RT (axis-based)
  int16_t gamepad_axis_[SDL_GAMEPAD_AXIS_COUNT]{};

  // Per-slot (physical button or virtual trigger) -> SDL scancode mapping, set
  // via BindGamepad and persisted in the cfg's GPND block.
  std::array<SDL_Scancode, kGamepadSlotCount> gamepad_scancode_map_{};
  std::array<KeyState, kGamepadSlotCount> gamepad_button_prev_{};

  // Independent repeat counters per gamepad input slot, so a held gamepad
  // button repeats on the same schedule as a held keyboard key without being
  // reset every frame by the keyboard pass over key_states_.
  std::array<int32_t, kGamepadSlotCount> gamepad_button_repeat_count_{};

  // Repeat counters for the four D-pad / left-stick directions (DOWN, UP,
  // LEFT, RIGHT), so gamepad direction input drives `repeat` instead of a
  // continuous `pressed` hold.
  std::array<int32_t, 4> gamepad_dir_repeat_count_{};
};

}  // namespace content

#endif  //! CONTENT_INPUT_CONTROLLER_KEYBOARD_H_
