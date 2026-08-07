// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD - style license that can be
// found in the LICENSE file.

#include "binding/mri/binding_patch.h"

#include "content/input/mouse_controller.h"
#include "content/public/engine_input.h"

namespace binding {

namespace {

struct BindingSet {
  std::string name;
  int key_id;
};

const BindingSet kKeyboardBindings[] = {
    {"DOWN", 2},   {"LEFT", 4},  {"RIGHT", 6}, {"UP", 8},

    {"B", 12},     {"C", 13},    {"Z", 16},    {"L", 17},    {"R", 18},

    {"SHIFT", 21}, {"CTRL", 22}, {"ALT", 23},

    {"F5", 25},    {"F6", 26},   {"F7", 27},   {"F8", 28}, {"F9", 29},
};

std::string GetButtonSymbol(int argc, VALUE* argv) {
  MriCheckArgc(argc, 1);

  std::string sym;
  if (FIXNUM_P(*argv)) {
    int key_id = FIX2INT(*argv);
    for (size_t i = 0; i < std::size(kKeyboardBindings); ++i)
      if (kKeyboardBindings[i].key_id == key_id)
        return kKeyboardBindings[i].name;
  } else if (SYMBOL_P(*argv)) {
    MriParseArgsTo(argc, argv, "n", &sym);
  }

  return sym;
}

}  // namespace

MRI_METHOD(input_is_pressed) {
  scoped_refptr<content::Input> input = MriGetGlobalModules()->Input;
  std::string key = GetButtonSymbol(argc, argv);
  content::ExceptionState exception_state;
  bool v = input->IsPressed(key, exception_state);
  MriProcessException(exception_state);
  return v ? Qtrue : Qfalse;
}

MRI_METHOD(input_is_triggered) {
  scoped_refptr<content::Input> input = MriGetGlobalModules()->Input;
  std::string key = GetButtonSymbol(argc, argv);
  content::ExceptionState exception_state;
  bool v = input->IsTriggered(key, exception_state);
  MriProcessException(exception_state);
  return v ? Qtrue : Qfalse;
}

MRI_METHOD(input_is_repeated) {
  scoped_refptr<content::Input> input = MriGetGlobalModules()->Input;
  std::string key = GetButtonSymbol(argc, argv);
  content::ExceptionState exception_state;
  bool v = input->IsRepeated(key, exception_state);
  MriProcessException(exception_state);
  return v ? Qtrue : Qfalse;
}

void ApplyInputPatch() {
  VALUE klass = rb_const_get(rb_cObject, rb_intern("Input"));

  MriDefineModuleFunction(klass, "press?", input_is_pressed);
  MriDefineModuleFunction(klass, "trigger?", input_is_triggered);
  MriDefineModuleFunction(klass, "repeat?", input_is_repeated);

  for (size_t i = 0; i < std::size(kKeyboardBindings); ++i) {
    auto& binding_set = kKeyboardBindings[i];
    ID key = rb_intern(binding_set.name.c_str());
    rb_const_set(klass, key, INT2FIX(binding_set.key_id));
  }

  // Gamepad physical button constants (values match SDL_GamepadButton), usable
  // as the first argument of Input.bind_gamepad / unbind_gamepad. These use a
  // distinct "GP_" prefix so they never collide with the keyboard symbol
  // constants (e.g. Input::A), which would otherwise be shadowed and break
  // symbol-based queries such as Input.press?(Input::A).
  struct GamepadButtonSet {
    const char* name;
    int button_id;
  };
  const GamepadButtonSet kGamepadButtonSets[] = {
      {"GP_A", 0},         {"GP_B", 1},        {"GP_X", 2},        {"GP_Y", 3},
      {"GP_SELECT", 4},    {"GP_START", 6},    {"GP_L3", 7},       {"GP_R3", 8},
      {"GP_LB", 9},        {"GP_RB", 10},      {"GP_DPAD_UP", 11}, {"GP_DPAD_DOWN", 12},
      {"GP_DPAD_LEFT", 13}, {"GP_DPAD_RIGHT", 14}, {"GP_LT", 15},  {"GP_RT", 16},
  };
  for (size_t i = 0; i < std::size(kGamepadButtonSets); ++i) {
    rb_const_set(klass, rb_intern(kGamepadButtonSets[i].name),
                 INT2FIX(kGamepadButtonSets[i].button_id));
  }
}

struct MouseButtonSet {
  std::string name;
  int button_id;
};

const MouseButtonSet kMouseButtonSets[] = {
    {"LEFT", content::MouseImpl::Button::Left},
    {"MIDDLE", content::MouseImpl::Button::Middle},
    {"RIGHT", content::MouseImpl::Button::Right},
    {"X1", content::MouseImpl::Button::X1},
    {"X2", content::MouseImpl::Button::X2},
};

void ApplyMousePatch() {
  VALUE klass = rb_const_get(rb_cObject, rb_intern("Mouse"));

  for (size_t i = 0; i < std::size(kMouseButtonSets); ++i)
    rb_const_set(klass, rb_intern(kMouseButtonSets[i].name.c_str()),
                 INT2FIX(kMouseButtonSets[i].button_id));
}

void MriApplyBindingPatch() {
  ApplyInputPatch();
  ApplyMousePatch();
}

}  // namespace binding
