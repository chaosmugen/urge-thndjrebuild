// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/worker/event_controller.h"

namespace content {

EventController::EventController(base::WeakPtr<ui::Widget> window)
    : window_(window) {}

EventController::~EventController() = default;

void EventController::DispatchEvent(SDL_Event* event) {
  // Dispatch event with filter
  auto window_size = window_->GetSize();
  auto window_scale = window_->GetDisplayState().scale;
  auto window_viewport = window_->GetDisplayState().viewport;
  switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      if (event->key.windowID == window_->GetWindowID()) {
        auto& raw_event = event->key;
        KeyEventData out_event = {};
        out_event.timestamp = raw_event.timestamp;
        out_event.keyboard_id = raw_event.which;
        out_event.type = static_cast<decltype(out_event.type)>(
            event->type - SDL_EVENT_KEY_DOWN);
        out_event.scancode = raw_event.scancode;
        out_event.keycode = raw_event.key;
        out_event.modifier = raw_event.mod;
        out_event.down = raw_event.down;
        out_event.repeat = raw_event.repeat;
        key_events_.push_back(out_event);
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (event->motion.windowID == window_->GetWindowID()) {
        auto& raw_event = event->motion;
        MouseEventData out_event = {};
        out_event.timestamp = raw_event.timestamp;
        out_event.mouse_id = raw_event.which;
        out_event.type = static_cast<decltype(out_event.type)>(
            event->type - SDL_EVENT_MOUSE_MOTION);
        out_event.x = (raw_event.x - window_viewport.x) / window_scale.x;
        out_event.y = (raw_event.y - window_viewport.y) / window_scale.y;

        out_event.motion_state = raw_event.state;
        out_event.motion_relx = raw_event.xrel / window_scale.x;
        out_event.motion_rely = raw_event.yrel / window_scale.y;
        mouse_events_.push_back(out_event);
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event->button.windowID == window_->GetWindowID()) {
        auto& raw_event = event->button;
        MouseEventData out_event = {};
        out_event.timestamp = raw_event.timestamp;
        out_event.mouse_id = raw_event.which;
        out_event.type = static_cast<decltype(out_event.type)>(
            event->type - SDL_EVENT_MOUSE_MOTION);
        out_event.x = (raw_event.x - window_viewport.x) / window_scale.x;
        out_event.y = (raw_event.y - window_viewport.y) / window_scale.y;

        out_event.button_id = raw_event.button;
        out_event.button_down = raw_event.down;
        out_event.button_clicks = raw_event.clicks;
        mouse_events_.push_back(out_event);
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      if (event->wheel.windowID == window_->GetWindowID()) {
        auto& raw_event = event->wheel;
        MouseEventData out_event = {};
        out_event.timestamp = raw_event.timestamp;
        out_event.mouse_id = raw_event.which;
        out_event.type = static_cast<decltype(out_event.type)>(
            event->type - SDL_EVENT_MOUSE_MOTION);
        out_event.x = (raw_event.x - window_viewport.x) / window_scale.x;
        out_event.y = (raw_event.y - window_viewport.y) / window_scale.y;

        out_event.wheel_dir =
            static_cast<MouseEvent::WheelState>(raw_event.direction);
        out_event.wheel_x = raw_event.x;
        out_event.wheel_y = raw_event.y;
        mouse_events_.push_back(out_event);
      }
      break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
      if (event->tfinger.windowID == window_->GetWindowID()) {
        auto& raw_event = event->tfinger;
        TouchEventData out_event = {};
        out_event.timestamp = raw_event.timestamp;
        out_event.touch_id = raw_event.touchID;
        out_event.type = static_cast<decltype(out_event.type)>(
            event->type - SDL_EVENT_FINGER_DOWN);
        out_event.finger = raw_event.fingerID;
        out_event.x =
            (raw_event.x * window_size.x - window_viewport.x) / window_scale.x;
        out_event.y =
            (raw_event.y * window_size.y - window_viewport.y) / window_scale.y;
        out_event.dx = (raw_event.dx * window_size.x) / window_scale.x;
        out_event.dy = (raw_event.dy * window_size.y) / window_scale.y;
        out_event.pressure = raw_event.pressure;
        touch_events_.push_back(out_event);
      }
      break;
    case SDL_EVENT_TEXT_EDITING:
      if (event->edit.windowID == window_->GetWindowID()) {
        auto& raw_event = event->edit;
        TextInputEventData out_event = {};
        out_event.timestamp = raw_event.timestamp;
        out_event.type = static_cast<decltype(out_event.type)>(
            event->type - SDL_EVENT_TEXT_EDITING);
        out_event.text = raw_event.text;
        out_event.select_start = raw_event.start;
        out_event.select_length = raw_event.length;
        text_input_events_.push_back(out_event);
      }
      break;
    case SDL_EVENT_TEXT_INPUT:
      if (event->text.windowID == window_->GetWindowID()) {
        auto& raw_event = event->text;
        TextInputEventData out_event = {};
        out_event.timestamp = raw_event.timestamp;
        out_event.type = static_cast<decltype(out_event.type)>(
            event->type - SDL_EVENT_TEXT_EDITING);
        out_event.text = raw_event.text;
        out_event.select_start = -1;
        out_event.select_length = -1;
        text_input_events_.push_back(out_event);
      }
      break;
    default:
      break;
  }
}

}  // namespace content
