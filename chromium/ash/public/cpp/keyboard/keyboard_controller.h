// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_PUBLIC_CPP_KEYBOARD_KEYBOARD_CONTROLLER_H_
#define ASH_PUBLIC_CPP_KEYBOARD_KEYBOARD_CONTROLLER_H_

#include <set>
#include <vector>

#include "ash/public/cpp/ash_public_export.h"
#include "ash/public/cpp/keyboard/keyboard_config.h"
#include "ash/public/cpp/keyboard/keyboard_types.h"
#include "base/optional.h"
#include "ui/gfx/geometry/rect.h"

namespace ash {

class KeyboardControllerObserver;

enum class HideReason {
  // Hide requested by an explicit user action.
  kUser,

  // Hide requested due to a system event (e.g. because it would interfere with
  // a menu or other on screen UI).
  kSystem,
};

class ASH_PUBLIC_EXPORT KeyboardController {
 public:
  static KeyboardController* Get();

  // Sets the global KeyboardController instance to |this|.
  KeyboardController();

  virtual ~KeyboardController();

  // Retrieves the current keyboard configuration.
  virtual keyboard::KeyboardConfig GetKeyboardConfig() = 0;

  // Sets the current keyboard configuration.
  virtual void SetKeyboardConfig(const keyboard::KeyboardConfig& config) = 0;

  // Returns whether the virtual keyboard has been enabled.
  virtual bool IsKeyboardEnabled() = 0;

  // Sets the provided keyboard enable flag. If the computed enabled state
  // changes, enables or disables the keyboard to match the new state.
  virtual void SetEnableFlag(keyboard::KeyboardEnableFlag flag) = 0;

  // Clears the provided keyboard enable flag. If the computed enabled state
  // changes, enables or disables the keyboard to match the new state.
  virtual void ClearEnableFlag(keyboard::KeyboardEnableFlag flag) = 0;

  // Gets the current set of keyboard enable flags.
  virtual const std::set<keyboard::KeyboardEnableFlag>& GetEnableFlags() = 0;

  // Reloads the virtual keyboard if it is enabled and the URL has changed, e.g.
  // the focus has switched from one type of field to another.
  virtual void ReloadKeyboardIfNeeded() = 0;

  // Rebuilds (disables and re-enables) the virtual keyboard if it is enabled.
  // This is used to force a reload of the virtual keyboard when preferences or
  // other configuration that affects loading the keyboard may have changed.
  virtual void RebuildKeyboardIfEnabled() = 0;

  // Returns whether the virtual keyboard is visible.
  virtual bool IsKeyboardVisible() = 0;

  // Shows the virtual keyboard on the current display if it is enabled.
  virtual void ShowKeyboard() = 0;

  // Hides the virtual keyboard if it is visible.
  virtual void HideKeyboard(HideReason reason) = 0;

  // Sets the keyboard container type. If non empty, |target_bounds| provides
  // the container size. Returns whether the transition succeeded once the
  // container type changes (or fails to change).
  using SetContainerTypeCallback = base::OnceCallback<void(bool)>;
  virtual void SetContainerType(keyboard::ContainerType container_type,
                                const base::Optional<gfx::Rect>& target_bounds,
                                SetContainerTypeCallback callback) = 0;

  // If |locked| is true, the keyboard remains visible even when no window has
  // input focus.
  virtual void SetKeyboardLocked(bool locked) = 0;

  // Sets the regions of the keyboard window that occlude whatever is behind it.
  virtual void SetOccludedBounds(const std::vector<gfx::Rect>& bounds) = 0;

  // Sets the regions of the keyboard window where events should be handled.
  virtual void SetHitTestBounds(const std::vector<gfx::Rect>& bounds) = 0;

  // Sets the region of the keyboard window that can be used as a drag handle.
  virtual void SetDraggableArea(const gfx::Rect& bounds) = 0;

  // Adds a KeyboardControllerObserver.
  virtual void AddObserver(KeyboardControllerObserver* observer) = 0;

 protected:
  static KeyboardController* g_instance_;
};

}  // namespace ash

#endif  // ASH_PUBLIC_CPP_KEYBOARD_KEYBOARD_CONTROLLER_H_
