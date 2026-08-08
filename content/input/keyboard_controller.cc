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

    /* F1/F2/F12 are engine-reserved shortcut keys (settings menu / FPS monitor
       / reset), so they are intentionally excluded from the game-queryable
       default bindings. */
    {"F3", SDL_SCANCODE_F3},        {"F4", SDL_SCANCODE_F4},
    {"F5", SDL_SCANCODE_F5},        {"F6", SDL_SCANCODE_F6},
    {"F7", SDL_SCANCODE_F7},        {"F8", SDL_SCANCODE_F8},
    {"F9", SDL_SCANCODE_F9},        {"F10", SDL_SCANCODE_F10},
    {"F11", SDL_SCANCODE_F11},

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

/* Input config file format:
   - KBND magic (Uint32) + item count (Uint32), then per item
     <token_size Uint32><symbol bytes><scancode int32>, optionally followed by
     a GPND magic (Uint32) + count (Uint32) + <slot int32, scancode int32>.
   - Legacy files lack the KBND magic; their first u32 is the item count. */
constexpr Uint32 kBindingMagic = 0x4B424E44;  // "KBND"
constexpr Uint32 kGpMagic = 0x47504E44;       // "GPND"
/* Reasonable upper bound per item: 256-byte symbol + scancode + token_size
   header. 512 items × 272 bytes ≈ 136 KB, far above any real config. */
constexpr uint32_t kMaxItemCount = 512;
/* Maximum length of a single symbol string stored in the config. */
constexpr uint32_t kMaxTokenSize = 256;
/* Maximum number of gamepad binding entries. */
constexpr uint32_t kMaxGpCount = 64;

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

    /* Trigger detection uses the physical keyboard's previous state rather
       than the merged key_states_ (which also carries gamepad input), so a held
       gamepad button bound to the same scancode cannot suppress a keyboard
       keypress. */
    key_states_[i].trigger = !keyboard_prev_state_[i] && key_pressed;
    keyboard_prev_state_[i] = key_pressed;

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
     input (physical button or virtual LT/RT trigger slot) maps to a single SDL
     scancode and ORs its press/trigger/repeat contribution into the keyboard
     state, so keyboard and gamepad never clobber each other's input. The
     keyboard pass above has already established the physical-keyboard baseline,
     so releasing a gamepad input only stops its own repeat stream — the merged
     state keeps whatever the keyboard (or another still-held gamepad source)
     contributes. */
  for (int i = 0; i < kGamepadSlotCount; ++i) {
    SDL_Scancode sc = gamepad_scancode_map_[i];
    if (sc == SDL_SCANCODE_UNKNOWN || sc >= SDL_SCANCODE_COUNT)
      continue;

    /* Physical buttons report their state directly; the two virtual LT/RT
       slots (26/27) are axis-driven and read from gamepad_trigger_state_. */
    bool down;
    if (i < SDL_GAMEPAD_BUTTON_COUNT)
      down = gamepad_button_state_[i];
    else
      down = gamepad_trigger_state_[i - SDL_GAMEPAD_BUTTON_COUNT];
    KeyState& gp = gamepad_button_prev_[i];

    if (down) {
      key_states_[sc].trigger |= !gp.pressed;
      key_states_[sc].pressed = true;

      /* Repeat on the same schedule as a held keyboard key, but counted per
         gamepad slot so the keyboard pass's per-scancode counter is untouched
         (sharing it made the counter reset every frame, repeating every frame). */
      int32_t& rc = gamepad_button_repeat_count_[i];
      ++rc;
      key_states_[sc].repeat |=
          rc == 1 || (rc >= 23 && (rc + 1) % 6 == 0);

      recent_key_states_[sc].trigger = key_states_[sc].trigger;
      recent_key_states_[sc].pressed = key_states_[sc].pressed;
      recent_key_states_[sc].repeat = key_states_[sc].repeat;
    } else if (gp.pressed) {
      /* Just released: stop this input's repeat stream. The keyboard baseline
         in key_states_ already reflects whether the key is still held. */
      gamepad_button_repeat_count_[i] = 0;
    }

    gp.pressed = down;
  }

  /* D-pad and left stick drive the direction symbols (DOWN/UP/LEFT/RIGHT) as
     configured in key_bindings_, so they follow the same scancodes the game
     queries (e.g. a config that maps DOWN to Q makes the D-pad press Q). They
     drive the full press/trigger/repeat state so directional movement (which
     depends on `pressed`) works from the gamepad. When a symbol has several
     bindings (DOWN→↓ and DOWN→Q), only the first one in key_bindings_ order is
     driven — that order is the config load order, matching the keyboard path. */
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
        /* OR into the keyboard baseline like the button merge above, so the
           D-pad/stick never overwrites a keyboard press on the same scancode. */
        key_states_[target].trigger |= gamepad_dir_repeat_count_[idx] == 0;
        key_states_[target].pressed = true;
        ++gamepad_dir_repeat_count_[idx];
        key_states_[target].repeat |=
            gamepad_dir_repeat_count_[idx] == 1 ||
            (gamepad_dir_repeat_count_[idx] >= 23 &&
             (gamepad_dir_repeat_count_[idx] + 1) % 6 == 0);
        recent_key_states_[target].trigger = key_states_[target].trigger;
        recent_key_states_[target].pressed = key_states_[target].pressed;
        recent_key_states_[target].repeat = key_states_[target].repeat;
      } else if (gamepad_dir_repeat_count_[idx] != 0) {
        /* Just released: clear only this input's repeat stream; the keyboard
           baseline in key_states_ already reflects whether the key is held. */
        gamepad_dir_repeat_count_[idx] = 0;
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
  /* Single-gamepad support: keep the controller already in use. A second
     GAMEPAD_ADDED (an extra controller, or a duplicate startup event for the
     same one) must not steal the handle. */
  if (gamepad_)
    return;

  /* Init once for the process lifetime. SDL_InitSubSystem ref-counts, so
     calling it on each (re)open is harmless, and other engine parts may use
     the gamepad subsystem too — we never pair it with SDL_QuitSubSystem; SDL
     shuts the subsystem down at SDL_Quit() anyway. */
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

  /* Also clear the merge-side transient state, otherwise a freshly connected
     gamepad reuses the previous controller's prev/repeat state and its first
     press is not reported as a trigger. Bindings (gamepad_scancode_map_) are
     persistent config and are intentionally kept. */
  std::memset(gamepad_button_prev_.data(), 0,
              gamepad_button_prev_.size() * sizeof(KeyState));
  std::memset(gamepad_button_repeat_count_.data(), 0,
              gamepad_button_repeat_count_.size() * sizeof(int32_t));
  gamepad_trigger_state_.fill(false);
  gamepad_dir_repeat_count_.fill(0);
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
  /* SDL3 exposes the triggers (LT/RT) purely as axes and never emits button
     events for them, so record their pressed state here. The merge loop reads
     it for the virtual trigger slots kGamepadVtLT / kGamepadVtRT. */
  if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
    gamepad_trigger_state_[0] = value > kGamepadTriggerThreshold;
  else if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
    gamepad_trigger_state_[1] = value > kGamepadTriggerThreshold;
}

void KeyboardControllerImpl::BindGamepad(int32_t button,
                                          int32_t scancode,
                                          ExceptionState& exception_state) {
  if (button < 0 || button >= kGamepadSlotCount)
    return;
  if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT)
    return;

  gamepad_scancode_map_[button] = static_cast<SDL_Scancode>(scancode);
  StorageBindingsInternal();
}

void KeyboardControllerImpl::UnbindGamepad(int32_t button,
                                            ExceptionState& exception_state) {
  if (button < 0 || button >= kGamepadSlotCount)
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
  if (!fstream)
    return;

  /* Obtain file size so we can sanity-check offsets before seeking. */
  Sint64 file_size = SDL_GetIOSize(fstream);
  if (file_size < 0)
    file_size = 0;

  /* Read cfg into a temporary list. KBND files are used authoritatively;
     legacy files are later merged with the default bindings so symbols present
     in kDefaultKeyboardBindings but absent from the saved file are preserved. */
  std::vector<KeyBinding> loaded;

  bool is_kbnd = false;
  Uint32 item_size = 0;
  Uint32 magic = 0;
  if (SDL_ReadIO(fstream, &magic, sizeof(magic)) != sizeof(magic)) {
    /* Truncated header: treat as "no config" and keep the current bindings. */
    SDL_CloseIO(fstream);
    return;
  }

  if (magic == kBindingMagic) {
    is_kbnd = true;
    if (SDL_ReadIO(fstream, &item_size, sizeof(item_size)) !=
        sizeof(item_size)) {
      SDL_CloseIO(fstream);
      return;
    }
  } else {
    /* Legacy format: first u32 is the item count directly. */
    item_size = magic;
  }

  /* Clamp to a reasonable maximum to reject obviously corrupt files. */
  if (item_size > kMaxItemCount)
    item_size = kMaxItemCount;

  for (uint32_t i = 0; i < item_size; ++i) {
    Uint32 token_size;
    if (SDL_ReadIO(fstream, &token_size, sizeof(token_size)) !=
        sizeof(token_size))
      break;

    /* Guard against huge or obviously invalid string sizes. */
    if (token_size > kMaxTokenSize)
      token_size = kMaxTokenSize;

    std::string token(token_size, 0);
    if (SDL_ReadIO(fstream, token.data(), token_size) !=
        static_cast<size_t>(token_size))
      break;

    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    if (SDL_ReadIO(fstream, &scancode, sizeof(scancode)) != sizeof(scancode))
      break;

    /* Reject out-of-range scancodes and empty symbols. Without this, a corrupt
       or legacy cfg could later index key_states_[scancode] out of bounds in
       Update/UpdateDir4/8, which corrupts the heap and hangs the game. */
    if (token.empty() || scancode < SDL_SCANCODE_UNKNOWN ||
        scancode >= SDL_SCANCODE_COUNT)
      continue;

    loaded.push_back({std::move(token), scancode});
  }

  /* Read optional gamepad binding block (GPND) appended after the KBND
     data. Each entry is <button int32, scancode int32>. Only KBND files carry
     it; legacy files end right after the keyboard entries. The map is cleared
     first so bindings removed from the file do not survive a reload. */
  if (is_kbnd) {
    Sint64 pos_after_kbnd = SDL_SeekIO(fstream, 0, SDL_IO_SEEK_CUR);
    if (pos_after_kbnd >= 0 &&
        file_size - pos_after_kbnd >=
            static_cast<Sint64>(sizeof(Uint32) * 2)) {
      Uint32 gp_magic = 0;
      if (SDL_ReadIO(fstream, &gp_magic, sizeof(gp_magic)) ==
          sizeof(gp_magic)) {
        if (gp_magic == kGpMagic) {
          Uint32 gp_size;
          if (SDL_ReadIO(fstream, &gp_size, sizeof(gp_size)) ==
              sizeof(gp_size)) {
            if (gp_size > kMaxGpCount)
              gp_size = kMaxGpCount;

            gamepad_scancode_map_.fill(SDL_SCANCODE_UNKNOWN);
            for (uint32_t i = 0; i < gp_size; ++i) {
              int32_t btn, sc;
              if (SDL_ReadIO(fstream, &btn, sizeof(btn)) != sizeof(btn))
                break;
              if (SDL_ReadIO(fstream, &sc, sizeof(sc)) != sizeof(sc))
                break;
              if (btn >= 0 && btn < kGamepadSlotCount && sc >= 0 &&
                  sc < SDL_SCANCODE_COUNT)
                gamepad_scancode_map_[btn] = static_cast<SDL_Scancode>(sc);
            }
          }
        }
      }
    }
  }

  SDL_CloseIO(fstream);

  /* If loading produced no usable entries, keep the existing defaults. */
  if (loaded.empty())
    return;

  /* KBND files written by this version are authoritative: use their contents
     as-is so a user's unbind/reorder survives a round trip. Legacy files
     predate the format and are merged with the defaults so no default symbol
     is silently dropped. Note this merge is symbol-level: a default symbol
     removed from a legacy file is re-added on the next load, so a legacy file
     cannot permanently unbind a default symbol. Only a KBND round trip (save
     through the settings screen) makes an unbind permanent. */
  if (!is_kbnd) {
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
  }
  key_bindings_ = std::move(loaded);
}

void KeyboardControllerImpl::StorageBindingsInternal() {
  std::string filepath = context()->engine_profile->program_name;
  filepath += INPUT_CONFIG_SUBFIX;
  filepath += std::to_string(
      static_cast<int32_t>(context()->engine_profile->api_version));

  filesystem::IOState io_state;
  auto* fstream = context()->io_service->OpenWrite(filepath, &io_state);
  if (fstream) {
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

    /* Append gamepad binding block (GPND): <slot int32, scancode int32> for
       every input slot (button or virtual trigger) with a valid mapping. */
    {
      SDL_WriteIO(fstream, &kGpMagic, sizeof(kGpMagic));

      Uint32 gp_size = 0;
      for (int i = 0; i < kGamepadSlotCount; ++i)
        if (gamepad_scancode_map_[i] != SDL_SCANCODE_UNKNOWN)
          ++gp_size;
      SDL_WriteIO(fstream, &gp_size, sizeof(gp_size));

      for (int i = 0; i < kGamepadSlotCount; ++i) {
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
