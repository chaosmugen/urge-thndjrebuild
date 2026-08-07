// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/input/keyboard_controller.h"

#include "components/filesystem/io_service.h"

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_gamepad.h"
#include "SDL3/SDL_timer.h"
#include "imgui/imgui.h"

#include "content/context/execution_context.h"
#include "content/profile/command_ids.h"

#define INPUT_CONFIG_SUBFIX ".cfg"

namespace content {

namespace {

const KeyboardControllerImpl::KeyBinding kDefaultKeyboardBindings[] = {
    {"DOWN", SDL_SCANCODE_DOWN},    {"LEFT", SDL_SCANCODE_LEFT},
    {"RIGHT", SDL_SCANCODE_RIGHT},  {"UP", SDL_SCANCODE_UP},

    {"F1", SDL_SCANCODE_F1},        {"F2", SDL_SCANCODE_F2},
    {"F3", SDL_SCANCODE_F3},        {"F4", SDL_SCANCODE_F4},
    {"F5", SDL_SCANCODE_F5},        {"F6", SDL_SCANCODE_F6},
    {"F7", SDL_SCANCODE_F7},        {"F8", SDL_SCANCODE_F8},
    {"F9", SDL_SCANCODE_F9},        {"F10", SDL_SCANCODE_F10},
    {"F11", SDL_SCANCODE_F11},      {"F12", SDL_SCANCODE_F12},

    {"SHIFT", SDL_SCANCODE_LSHIFT}, {"SHIFT", SDL_SCANCODE_RSHIFT},
    {"CTRL", SDL_SCANCODE_LCTRL},   {"CTRL", SDL_SCANCODE_RCTRL},
    {"ALT", SDL_SCANCODE_LALT},     {"ALT", SDL_SCANCODE_RALT},

    {"C", SDL_SCANCODE_C},          {"C", SDL_SCANCODE_SPACE},
    {"C", SDL_SCANCODE_RETURN},     {"C", SDL_SCANCODE_KP_ENTER},
    {"L", SDL_SCANCODE_Q},          {"L", SDL_SCANCODE_PAGEUP},
    {"R", SDL_SCANCODE_W},          {"R", SDL_SCANCODE_PAGEDOWN},

    /* RGSS button symbols. These were dropped from the default table at some
       point and must be present so games can query "A"/"B"/"C"/"X"/"Y"/"Z"/
       "L"/"R" out of the box. Key choices follow the original defaults. */
    {"A", SDL_SCANCODE_LSHIFT},
    {"B", SDL_SCANCODE_ESCAPE},    {"B", SDL_SCANCODE_KP_0},
    {"B", SDL_SCANCODE_X},
    {"X", SDL_SCANCODE_A},          {"Y", SDL_SCANCODE_S},
    {"Z", SDL_SCANCODE_D},
};

const int32_t kDefaultKeyboardBindingsSize =
    sizeof(kDefaultKeyboardBindings) / sizeof(kDefaultKeyboardBindings[0]);

const KeyboardControllerImpl::KeyBinding kKeyboardBindings1[] = {
    {"C", SDL_SCANCODE_C},
};

const int32_t kKeyboardBindings1Size =
    sizeof(kKeyboardBindings1) / sizeof(kKeyboardBindings1[0]);

const KeyboardControllerImpl::KeyBinding kKeyboardBindings2[] = {
    {"C", SDL_SCANCODE_Z},
};

const int32_t kKeyboardBindings2Size =
    sizeof(kKeyboardBindings2) / sizeof(kKeyboardBindings2[0]);

const std::string kArrowDirsSymbol[] = {
    "DOWN",
    "LEFT",
    "RIGHT",
    "UP",
};

const int32_t kArrowDirsSymbolSize =
    sizeof(kArrowDirsSymbol) / sizeof(kArrowDirsSymbol[0]);

const std::array<std::string, 12> kButtonItems = {
    "A", "B", "C", "X", "Y", "Z", "L", "R", "DOWN", "LEFT", "RIGHT", "UP",
};

}  // namespace

///////////////////////////////////////////////////////////////////////////////
// KeyboardControllerImpl Implement

KeyboardControllerImpl::KeyboardControllerImpl(
    ExecutionContext* execution_context)
    : EngineObject(execution_context), disable_gui_key_input_(false) {
  std::memset(key_states_.data(), 0, key_states_.size() * sizeof(KeyState));
  std::memset(recent_key_states_.data(), 0,
              recent_key_states_.size() * sizeof(KeyState));

  /* Apply default keyboard bindings */
  for (int32_t i = 0; i < kDefaultKeyboardBindingsSize; ++i)
    key_bindings_.push_back(kDefaultKeyboardBindings[i]);

  if (execution_context->engine_profile->api_version ==
      ContentProfile::APIVersion::RGSS1)
    for (int32_t i = 0; i < kKeyboardBindings1Size; ++i)
      key_bindings_.push_back(kKeyboardBindings1[i]);

  if (execution_context->engine_profile->api_version >=
      ContentProfile::APIVersion::RGSS2)
    for (int32_t i = 0; i < kKeyboardBindings2Size; ++i)
      key_bindings_.push_back(kKeyboardBindings2[i]);

  TryReadBindingsInternal();
  setting_bindings_ = key_bindings_;

  OpenGamepad();
}

KeyboardControllerImpl::~KeyboardControllerImpl() = default;

void KeyboardControllerImpl::ApplyKeySymBinding(const KeySymMap& keysyms) {
  key_bindings_ = keysyms;
}

bool KeyboardControllerImpl::CreateButtonGUISettings() {
  static int32_t selected_button = 0, selected_binding = -1;
  disable_gui_key_input_ = (selected_binding != -1);

  if (ImGui::CollapsingHeader(
          context()
              ->i18n_profile->GetI18NString(IDS_SETTINGS_BUTTON, "Button")
              .c_str())) {
    auto list_height = 6 * ImGui::GetTextLineHeightWithSpacing();
    // Button name list box
    if (ImGui::BeginListBox(
            "##button_list",
            ImVec2(ImGui::CalcItemWidth() / 2.0f, list_height + 64))) {
      for (size_t i = 0; i < kButtonItems.size(); ++i) {
        if (ImGui::Selectable(kButtonItems[i].c_str(),
                              static_cast<int32_t>(i) == selected_button)) {
          selected_button = i;
          selected_binding = -1;
        }
      }

      ImGui::EndListBox();
    }

    ImGui::SameLine();
    std::string button_name = kButtonItems[selected_button];

    // Binding list box
    {
      ImGui::BeginGroup();
      if (ImGui::BeginListBox("##binding_list",
                              ImVec2(-FLT_MIN, list_height))) {
        for (size_t i = 0; i < setting_bindings_.size(); ++i) {
          auto& it = setting_bindings_[i];
          if (it.sym == button_name) {
            const bool is_select =
                (selected_binding == static_cast<int32_t>(i));

            // Generate button sign
            std::string display_button_name = SDL_GetScancodeName(it.scancode);
            if (it.scancode == SDL_SCANCODE_UNKNOWN)
              display_button_name = "<x>";
            if (is_select)
              display_button_name = "<...>";
            display_button_name += "##";
            display_button_name.push_back(i);

            // Find conflict bindings
            int32_t conflict_count = -1;
            for (auto& item : setting_bindings_)
              if (item == it)
                conflict_count++;

            // Draw selectable
            const bool is_conflict = !!conflict_count;
            if (is_conflict)
              ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));

            if (ImGui::Selectable(display_button_name.c_str(), is_select)) {
              selected_binding = (is_select ? -1 : i);
            }

            if (is_conflict)
              ImGui::PopStyleColor();
          }
        }

        ImGui::EndListBox();
      }

      // Get any keys state for binding
      if (disable_gui_key_input_) {
        for (int32_t code = 0; code < SDL_SCANCODE_COUNT; ++code) {
          bool key_pressed =
              context()->window->GetKeyState(static_cast<SDL_Scancode>(code));
          if (key_pressed) {
            setting_bindings_[selected_binding].scancode =
                static_cast<SDL_Scancode>(code);
            selected_binding = -1;
          }
        }
      }

      // Add binding
      if (ImGui::Button(context()
                            ->i18n_profile->GetI18NString(IDS_BUTTON_ADD, "Add")
                            .c_str())) {
        selected_binding = -1;
        setting_bindings_.push_back(
            KeyBinding{button_name, SDL_SCANCODE_UNKNOWN});
      }

      ImGui::SameLine();

      // Remove binding
      if (selected_binding >= 0 &&
          ImGui::Button(
              context()
                  ->i18n_profile->GetI18NString(IDS_BUTTON_REMOVE, "Remove")
                  .c_str())) {
        auto it = setting_bindings_.begin();
        for (int32_t i = 0; i < selected_binding; ++i)
          it++;

        setting_bindings_.erase(it);
        selected_binding = -1;
      }

      ImGui::EndGroup();
    }

    if (ImGui::Button(
            context()
                ->i18n_profile
                ->GetI18NString(IDS_BUTTON_SAVE_SETTINGS, "Save Settings")
                .c_str())) {
      key_bindings_ = setting_bindings_;
      selected_binding = -1;

      StorageBindingsInternal();
    }

    ImGui::SameLine();
    if (ImGui::Button(
            context()
                ->i18n_profile
                ->GetI18NString(IDS_BUTTON_RESET_SETTINGS, "Reset Settings")
                .c_str())) {
      setting_bindings_ = key_bindings_;
      selected_binding = -1;

      StorageBindingsInternal();
    }
  }

  return disable_gui_key_input_;
}

void KeyboardControllerImpl::Update(ExceptionState& exception_state) {
  for (int32_t i = 0; i < SDL_SCANCODE_COUNT; ++i) {
    bool key_pressed =
        context()->window->GetKeyState(static_cast<SDL_Scancode>(i));

    /* Update key state with elder state */
    key_states_[i].trigger = !key_states_[i].pressed && key_pressed;

    /* After trigger set, set press state */
    key_states_[i].pressed = key_pressed;

    /* Based on press state update the repeat state */
    key_states_[i].repeat = false;
    if (key_states_[i].pressed) {
      ++key_states_[i].repeat_count;

      bool repeated = false;
      // TODO: RGSS 1/2/3 specific process
      repeated = key_states_[i].repeat_count == 1 ||
                 (key_states_[i].repeat_count >= 23 &&
                  (key_states_[i].repeat_count + 1) % 6 == 0);

      key_states_[i].repeat = repeated;
    } else {
      key_states_[i].repeat_count = 0;
    }

    /* Update recent key infos */
    recent_key_states_[i].pressed = key_states_[i].pressed;
    recent_key_states_[i].trigger = key_states_[i].trigger;
    recent_key_states_[i].repeat = key_states_[i].repeat;
  }

  /* Merge script-defined gamepad bindings into keyboard state. Each gamepad
     button maps to a single SDL scancode and drives the full press/trigger/
     repeat state, just like a physical keyboard key. */
  /* LT/RT (slots 15/16, matching GP_LT/GP_RT) are reported as axes, so copy
     their trigger-axis pressed state into the button-state array used below. */
  gamepad_button_state_[15] = gamepad_trigger_state_[0];
  gamepad_button_state_[16] = gamepad_trigger_state_[1];
  for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
    SDL_Scancode sc = gamepad_scancode_map_[i];
    if (sc == SDL_SCANCODE_UNKNOWN || sc >= SDL_SCANCODE_COUNT)
      continue;

      bool down = gamepad_button_state_[i];
      KeyState& gp = gamepad_button_prev_[i];

    if (down) {
      key_states_[sc].trigger = !gp.pressed;

      key_states_[sc].pressed = true;

      ++key_states_[sc].repeat_count;
      key_states_[sc].repeat =
          key_states_[sc].repeat_count == 1 ||
          (key_states_[sc].repeat_count >= 23 &&
           (key_states_[sc].repeat_count + 1) % 6 == 0);

      recent_key_states_[sc].trigger = key_states_[sc].trigger;
      recent_key_states_[sc].pressed = key_states_[sc].pressed;
      recent_key_states_[sc].repeat = key_states_[sc].repeat;
    } else if (gp.pressed) {
      /* Button just released. */
      key_states_[sc].pressed = false;
      key_states_[sc].trigger = false;
      key_states_[sc].repeat = false;
      key_states_[sc].repeat_count = 0;
      recent_key_states_[sc].pressed = false;
      recent_key_states_[sc].trigger = false;
      recent_key_states_[sc].repeat = false;
    }

    gp.pressed = down;
  }

  /* D-pad and left stick drive the direction symbols (DOWN/UP/LEFT/RIGHT) as
     configured in key_bindings_, so they follow the same scancodes the game
     queries (e.g. a config that maps DOWN to Q makes the D-pad press Q). They
     drive the full press/trigger/repeat state so directional movement (which
     depends on `pressed`) works from the gamepad. */
  {
    struct DirSym {
      SDL_GamepadButton button;
      SDL_GamepadAxis axis;
      int16_t sign;  // +1: positive axis triggers, -1: negative axis triggers
      const char* sym;
    };
    const DirSym kDirSyms[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_AXIS_LEFTY, +1, "DOWN"},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_AXIS_LEFTY, -1, "UP"},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_AXIS_LEFTX, -1, "LEFT"},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_AXIS_LEFTX, +1, "RIGHT"},
    };
    for (const auto& d : kDirSyms) {
      SDL_Scancode target = SDL_SCANCODE_UNKNOWN;
      for (const auto& b : key_bindings_) {
        if (b.sym == d.sym) {
          target = b.scancode;
          break;
        }
      }
      if (target == SDL_SCANCODE_UNKNOWN || target >= SDL_SCANCODE_COUNT)
        continue;

      bool active = gamepad_button_state_[d.button];
      if (!active && d.sign > 0)
        active = gamepad_axis_[d.axis] > kGamepadDeadzone;
      else if (!active && d.sign < 0)
        active = gamepad_axis_[d.axis] < -kGamepadDeadzone;

      size_t idx = &d - kDirSyms;
      if (active) {
        key_states_[target].trigger = gamepad_dir_repeat_count_[idx] == 0;
        key_states_[target].pressed = true;
        ++gamepad_dir_repeat_count_[idx];
        key_states_[target].repeat =
            gamepad_dir_repeat_count_[idx] == 1 ||
            (gamepad_dir_repeat_count_[idx] >= 23 &&
             (gamepad_dir_repeat_count_[idx] + 1) % 6 == 0);
        recent_key_states_[target].trigger = key_states_[target].trigger;
        recent_key_states_[target].pressed = true;
        recent_key_states_[target].repeat = key_states_[target].repeat;
      } else if (gamepad_dir_repeat_count_[idx] != 0) {
        key_states_[target].pressed = false;
        key_states_[target].trigger = false;
        key_states_[target].repeat = false;
        gamepad_dir_repeat_count_[idx] = 0;
        recent_key_states_[target].pressed = false;
        recent_key_states_[target].trigger = false;
        recent_key_states_[target].repeat = false;
      }
    }
  }

  /* Now that all input sources (keyboard, gamepad buttons, D-pad/stick) have
     updated key_states_, compute the directional states for this frame. */
  UpdateDir4Internal();
  UpdateDir8Internal();
}

bool KeyboardControllerImpl::IsPressed(const std::string& sym,
                                       ExceptionState& exception_state) {
  if (sym.empty())
    return false;

  for (auto& it : key_bindings_) {
    if (it.sym == sym)
      if (key_states_[it.scancode].pressed)
        return true;
  }

  return false;
}

bool KeyboardControllerImpl::IsTriggered(const std::string& sym,
                                         ExceptionState& exception_state) {
  if (sym.empty())
    return false;

  for (auto& it : key_bindings_) {
    if (it.sym == sym)
      if (key_states_[it.scancode].trigger)
        return true;
  }

  return false;
}

bool KeyboardControllerImpl::IsRepeated(const std::string& sym,
                                        ExceptionState& exception_state) {
  if (sym.empty())
    return false;

  for (auto& it : key_bindings_) {
    if (it.sym == sym)
      if (key_states_[it.scancode].repeat)
        return true;
  }

  return false;
}

int32_t KeyboardControllerImpl::Dir4(ExceptionState& exception_state) {
  return dir4_state_.active;
}

int32_t KeyboardControllerImpl::Dir8(ExceptionState& exception_state) {
  return dir8_state_.active;
}

bool KeyboardControllerImpl::KeyPressed(int32_t scancode,
                                        ExceptionState& exception_state) {
  return key_states_[scancode].pressed;
}

bool KeyboardControllerImpl::KeyTriggered(int32_t scancode,
                                          ExceptionState& exception_state) {
  return key_states_[scancode].trigger;
}

bool KeyboardControllerImpl::KeyRepeated(int32_t scancode,
                                         ExceptionState& exception_state) {
  return key_states_[scancode].repeat;
}

std::string KeyboardControllerImpl::GetKeyName(
    int32_t scancode,
    ExceptionState& exception_state) {
  SDL_Keycode key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode),
                                           SDL_KMOD_NONE, false);
  return std::string(SDL_GetKeyName(key));
}

std::vector<int32_t> KeyboardControllerImpl::GetKeysFromFlag(
    const std::string& flag,
    ExceptionState& exception_state) {
  std::vector<int32_t> out;
  if (flag.empty())
    return out;

  for (auto& it : key_bindings_)
    if (it.sym == flag)
      out.push_back(it.scancode);

  return out;
}

void KeyboardControllerImpl::SetKeysFromFlag(const std::string& flag,
                                             const std::vector<int32_t>& keys,
                                             ExceptionState& exception_state) {
  if (flag.empty())
    return;

  auto iter = std::remove_if(
      key_bindings_.begin(), key_bindings_.end(),
      [&](const KeyBinding& binding) { return binding.sym == flag; });
  key_bindings_.erase(iter, key_bindings_.end());

  for (const auto& i : keys) {
    KeyBinding binding;
    binding.sym = flag;
    binding.scancode = static_cast<SDL_Scancode>(i);

    key_bindings_.push_back(std::move(binding));
  }
}

std::vector<int32_t> KeyboardControllerImpl::GetRecentPressed(
    ExceptionState& exception_state) {
  std::vector<int32_t> out;

  for (size_t i = 0; i < recent_key_states_.size(); ++i)
    if (recent_key_states_[i].pressed)
      out.push_back(i);

  return out;
}

std::vector<int32_t> KeyboardControllerImpl::GetRecentTriggered(
    ExceptionState& exception_state) {
  std::vector<int32_t> out;

  for (size_t i = 0; i < recent_key_states_.size(); ++i)
    if (recent_key_states_[i].trigger)
      out.push_back(i);

  return out;
}

std::vector<int32_t> KeyboardControllerImpl::GetRecentRepeated(
    ExceptionState& exception_state) {
  std::vector<int32_t> out;

  for (size_t i = 0; i < recent_key_states_.size(); ++i)
    if (recent_key_states_[i].repeat)
      out.push_back(i);

  return out;
}

bool KeyboardControllerImpl::Emulate(int32_t scancode,
                                     bool down,
                                     int32_t modifier,
                                     bool repeat,
                                     ExceptionState& exception_state) {
  SDL_Event emulate_event;
  emulate_event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  emulate_event.key.timestamp = SDL_GetTicksNS();
  emulate_event.key.windowID = context()->window->GetWindowID();
  emulate_event.key.which = 0;
  emulate_event.key.scancode = static_cast<SDL_Scancode>(scancode);
  emulate_event.key.mod = modifier;
  emulate_event.key.raw = 0;
  emulate_event.key.down = down;
  emulate_event.key.repeat = repeat;
  emulate_event.key.key = SDL_GetKeyFromScancode(emulate_event.key.scancode,
                                                 emulate_event.key.mod, true);

  return SDL_PushEvent(&emulate_event);
}

void KeyboardControllerImpl::OpenGamepad() {
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    LOG(ERROR) << "[Keyboard] Failed to init gamepad subsystem: "
               << SDL_GetError();
    return;
  }

  int count = 0;
  SDL_JoystickID* games = SDL_GetGamepads(&count);
  if (!games)
    return;

  if (count > 0)
    gamepad_ = SDL_OpenGamepad(games[0]);
  SDL_free(games);
}

void KeyboardControllerImpl::CloseGamepad() {
  if (gamepad_) {
    SDL_CloseGamepad(gamepad_);
    gamepad_ = nullptr;
  }
  std::memset(gamepad_button_state_.data(), 0,
              gamepad_button_state_.size() * sizeof(bool));
  std::memset(gamepad_axis_, 0, sizeof(gamepad_axis_));
}

void KeyboardControllerImpl::FeedGamepadButton(SDL_GamepadButton button,
                                               bool down) {
  if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT)
    return;
  gamepad_button_state_[button] = down;
}

void KeyboardControllerImpl::FeedGamepadAxis(SDL_GamepadAxis axis,
                                             int16_t value) {
  if (axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT)
    return;
  gamepad_axis_[axis] = value;
  /* Some gamepads report the triggers (LT/RT) as axes only and never emit
     button events. SDL3 exposes the triggers purely as axes (LEFT_TRIGGER /
     RIGHT_TRIGGER), so record their pressed state in gamepad_trigger_state_.
     The 15/16 indices match the button slots used by BindGamepad for LT/RT. */
  if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
    gamepad_trigger_state_[0] = value > kGamepadTriggerThreshold;
  else if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
    gamepad_trigger_state_[1] = value > kGamepadTriggerThreshold;
}

void KeyboardControllerImpl::BindGamepad(int32_t button,
                                          int32_t scancode,
                                          ExceptionState& exception_state) {
  if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT)
    return;
  if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT)
    return;

  gamepad_scancode_map_[button] = static_cast<SDL_Scancode>(scancode);
  StorageBindingsInternal();
}

void KeyboardControllerImpl::UnbindGamepad(int32_t button,
                                            ExceptionState& exception_state) {
  if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT)
    return;
  gamepad_scancode_map_[button] = SDL_SCANCODE_UNKNOWN;
  StorageBindingsInternal();
}

void KeyboardControllerImpl::UpdateDir4Internal() {
  bool key_states[kArrowDirsSymbolSize] = {0};
  for (auto& it : key_bindings_)
    for (int32_t i = 0; i < kArrowDirsSymbolSize; ++i)
      if (it.sym == kArrowDirsSymbol[i])
        key_states[i] |= key_states_[it.scancode].pressed;

  int32_t dir_flag = 0;
  const int32_t dir_flags_fix[] = {
      1 << 1,
      1 << 2,
      1 << 3,
      1 << 4,
  };

  const int32_t block_dir_flags[] = {dir_flags_fix[0] | dir_flags_fix[3],
                                     dir_flags_fix[1] | dir_flags_fix[2]};

  const int32_t other_dirs[][3] = {
      {1, 2, 3},
      {0, 3, 2},
      {0, 3, 1},
      {1, 2, 0},
  };

  for (size_t i = 0; i < 4; ++i)
    dir_flag |= (key_states[i] ? dir_flags_fix[i] : 0);

  if (dir_flag == block_dir_flags[0] || dir_flag == block_dir_flags[1]) {
    dir4_state_.active = 0;
    return;
  }

  if (dir4_state_.previous) {
    if (key_states[dir4_state_.previous / 2 - 1]) {
      for (size_t i = 0; i < 3; ++i) {
        int32_t other_key = other_dirs[dir4_state_.previous / 2 - 1][i];
        if (!key_states[other_key])
          continue;

        dir4_state_.active = (other_key + 1) * 2;
        return;
      }
    }
  }

  for (size_t i = 0; i < 4; ++i) {
    if (!key_states[i])
      continue;

    dir4_state_.active = (i + 1) * 2;
    dir4_state_.previous = (i + 1) * 2;
    return;
  }

  dir4_state_.active = 0;
  dir4_state_.previous = 0;
}

void KeyboardControllerImpl::UpdateDir8Internal() {
  bool key_states[kArrowDirsSymbolSize] = {0};
  for (auto& it : key_bindings_)
    for (int32_t i = 0; i < kArrowDirsSymbolSize; ++i)
      if (it.sym == kArrowDirsSymbol[i])
        key_states[i] |= key_states_[it.scancode].pressed;

  static const int32_t combos[4][4] = {
      {2, 1, 3, 0}, {1, 4, 0, 7}, {3, 0, 6, 9}, {0, 7, 9, 8}};

  const int32_t other_dirs[][3] = {
      {1, 2, 3},
      {0, 3, 2},
      {0, 3, 1},
      {1, 2, 0},
  };

  dir8_state_.active = 0;

  for (size_t i = 0; i < 4; ++i) {
    if (!key_states[i])
      continue;

    for (int32_t j = 0; j < 3; ++j) {
      int32_t other_key = other_dirs[i][j];
      if (!key_states[other_key])
        continue;

      dir8_state_.active = combos[i][other_key];
      return;
    }

    dir8_state_.active = (i + 1) * 2;
    return;
  }
}

void KeyboardControllerImpl::TryReadBindingsInternal() {
  std::string filepath = context()->engine_profile->program_name;
  filepath += INPUT_CONFIG_SUBFIX;
  filepath += std::to_string(
      static_cast<int32_t>(context()->engine_profile->api_version));

  filesystem::IOState io_state;
  auto* fstream = context()->io_service->OpenReadRaw(filepath, &io_state);
  if (fstream) {
    /* Read cfg into a temporary list, then merge with the default bindings so
       that symbols present in kDefaultKeyboardBindings but absent from the
       saved file (e.g. a user-customized file that omits some default symbol)
       are preserved instead of being dropped. */
    std::vector<KeyBinding> loaded;

    const Uint32 kBindingMagic = 0x4B424E44;  // "KBND"
    Uint32 magic = 0;
    SDL_ReadIO(fstream, &magic, sizeof(magic));

    if (magic == kBindingMagic) {
      Uint32 item_size;
      SDL_ReadIO(fstream, &item_size, sizeof(item_size));
      for (uint32_t i = 0; i < item_size; ++i) {
        Uint32 token_size;
        SDL_ReadIO(fstream, &token_size, sizeof(token_size));

        std::string token(token_size, 0);
        SDL_ReadIO(fstream, token.data(), token_size);

        SDL_Scancode scancode;
        SDL_ReadIO(fstream, &scancode, sizeof(scancode));

        /* Gamepad bindings live in the separate GPND block (read below),
           so the KBND entry only stores <sym, scancode>. */
        loaded.push_back({token, scancode});
      }
    } else {
      /* Legacy format: only <sym, scancode>, gamepad binding is INVALID. */
      Uint32 item_size = magic;
      for (uint32_t i = 0; i < item_size; ++i) {
        Uint32 token_size;
        SDL_ReadIO(fstream, &token_size, sizeof(token_size));

        std::string token(token_size, 0);
        SDL_ReadIO(fstream, token.data(), token_size);

        SDL_Scancode scancode;
        SDL_ReadIO(fstream, &scancode, sizeof(scancode));

        loaded.push_back({token, scancode});
      }
    }

    /* Read optional gamepad binding block (GPND) appended after the KBND
       data. Each entry is <button int32, scancode int32>. */
    {
      Uint32 gp_magic = 0;
      Sint64 cur = SDL_SeekIO(fstream, 0, SDL_IO_SEEK_CUR);
      SDL_ReadIO(fstream, &gp_magic, sizeof(gp_magic));
      if (gp_magic == 0x47504E44) {  // "GPND"
        Uint32 gp_size;
        SDL_ReadIO(fstream, &gp_size, sizeof(gp_size));
        for (uint32_t i = 0; i < gp_size; ++i) {
          int32_t btn, sc;
          SDL_ReadIO(fstream, &btn, sizeof(btn));
          SDL_ReadIO(fstream, &sc, sizeof(sc));
          if (btn >= 0 && btn < SDL_GAMEPAD_BUTTON_COUNT && sc >= 0 &&
              sc < SDL_SCANCODE_COUNT)
            gamepad_scancode_map_[btn] = static_cast<SDL_Scancode>(sc);
        }
      } else {
        SDL_SeekIO(fstream, cur, SDL_IO_SEEK_SET);
      }
    }

    SDL_CloseIO(fstream);

    /* Merge: keep every default binding, overriding its scancode when the
       saved file contains the same symbol; append saved symbols that are not
       part of the defaults. This prevents a saved file that omits a default
       symbol from silently dropping it. */
    for (const auto& def : key_bindings_) {
      bool found = false;
      for (const auto& l : loaded) {
        if (l.sym == def.sym) {
          found = true;
          break;
        }
      }
      if (!found)
        loaded.push_back(def);
    }
    key_bindings_ = std::move(loaded);
  }
}

void KeyboardControllerImpl::StorageBindingsInternal() {
  std::string filepath = context()->engine_profile->program_name;
  filepath += INPUT_CONFIG_SUBFIX;
  filepath += std::to_string(
      static_cast<int32_t>(context()->engine_profile->api_version));

  filesystem::IOState io_state;
  auto* fstream = context()->io_service->OpenWrite(filepath, &io_state);
  if (fstream) {
    const Uint32 kBindingMagic = 0x4B424E44;  // "KBND"
    SDL_WriteIO(fstream, &kBindingMagic, sizeof(kBindingMagic));

    Uint32 item_size = key_bindings_.size();
    SDL_WriteIO(fstream, &item_size, sizeof(item_size));
    for (const auto& it : key_bindings_) {
      uint32_t token_size = it.sym.size();
      SDL_WriteIO(fstream, &token_size, sizeof(token_size));

      SDL_WriteIO(fstream, it.sym.data(), token_size);

      SDL_Scancode scancode = it.scancode;
      SDL_WriteIO(fstream, &scancode, sizeof(scancode));

      /* Gamepad bindings are written to the separate GPND block below. */
    }

    /* Append gamepad binding block (GPND): <button int32, scancode int32>
       for every button that has a valid scancode mapping. */
    {
      const Uint32 gp_magic = 0x47504E44;  // "GPND"
      SDL_WriteIO(fstream, &gp_magic, sizeof(gp_magic));

      Uint32 gp_size = 0;
      for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
        if (gamepad_scancode_map_[i] != SDL_SCANCODE_UNKNOWN)
          ++gp_size;
      SDL_WriteIO(fstream, &gp_size, sizeof(gp_size));

      for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
        SDL_Scancode sc = gamepad_scancode_map_[i];
        if (sc == SDL_SCANCODE_UNKNOWN)
          continue;
        int32_t btn = i;
        int32_t scv = static_cast<int32_t>(sc);
        SDL_WriteIO(fstream, &btn, sizeof(btn));
        SDL_WriteIO(fstream, &scv, sizeof(scv));
      }
    }

    SDL_CloseIO(fstream);
  }
}

}  // namespace content