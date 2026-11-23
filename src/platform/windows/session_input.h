/**
 * @file src/platform/windows/session_input.h
 * @brief Session-local keyboard and mouse injection for non-console sessions.
 *
 * libvirtualhid prefers a driver-backed HID mouse and keyboard when the
 * Virtual HID Driver is installed and licensed. Windows routes physical (HID)
 * input to the session attached to the console, so when Sunshine runs inside
 * an RDP session the host console receives the client's mouse and keyboard.
 * SendInput() injects into the caller's own session, so these helpers bypass
 * libvirtualhid for mouse and keyboard whenever Sunshine is not running in
 * the active console session.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>

// local includes
#include "src/platform/common.h"

namespace platf::session_input {
  /**
   * @brief Session that receives the mouse and keyboard of a mosaic stream.
   *
   * OpenStreamMS writes osms-input.txt next to sunshine.conf when the user moves the mouse
   * and keyboard of a composite stream to another of its sessions (from the HUD). Events
   * then go to an OpenStreamMS relay in that session (named pipe osms-input-<session>, owned
   * by SYSTEM) instead of this one; absolute mouse positions are mapped from the session's
   * part of the canvas to its desktop.
   */
  struct input_target_t {
    std::uint32_t session = 0;  ///< Terminal Services session id.
    int x = 0;  ///< Left edge of its part of the canvas.
    int y = 0;  ///< Top edge of its part of the canvas.
    int width = 0;  ///< Width of its part.
    int height = 0;  ///< Height of its part.
    int canvas_width = 0;  ///< Canvas width.
    int canvas_height = 0;  ///< Canvas height.
  };

  /**
   * @brief Parse osms-input.txt: "<session> <x> <y> <width> <height> <canvas width> <canvas height>".
   *
   * @param text File content; lines starting with # are comments.
   * @return The target, or nullopt when the content is not valid.
   */
  std::optional<input_target_t> parse_input_target(const std::string &text);

  /**
   * @brief Map a position on the canvas (0..1) to the 0..65535 range of the target's desktop.
   *
   * @param target Target session and its part of the canvas.
   * @param canvas_fx Horizontal position on the whole canvas, 0..1.
   * @param canvas_fy Vertical position on the whole canvas, 0..1.
   * @return Absolute coordinates for MOUSEEVENTF_ABSOLUTE, clamped to the target's part.
   */
  std::pair<long, long> map_to_target(const input_target_t &target, float canvas_fx, float canvas_fy);

  /**
   * @brief Whether mouse and keyboard must bypass driver-backed HID devices.
   *
   * @return true when the current process is not in the active console session.
   */
  bool should_use();

  void move_mouse(int delta_x, int delta_y);
  void abs_mouse(const touch_port_t &touch_port, float x, float y);
  void button_mouse(int button, bool release);
  void scroll(int high_res_distance);
  void hscroll(int high_res_distance);
  void keyboard_update(std::uint16_t modcode, bool release, std::uint8_t flags, bool extended);
  void unicode(const char *utf8, int size);
}  // namespace platf::session_input
