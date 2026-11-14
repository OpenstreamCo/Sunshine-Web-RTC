/**
 * @file src/platform/windows/osmspad.h
 * @brief Virtual Xbox Series gamepads through OpenStreamMS's OsmsPad driver (UMDF + VHF).
 *
 * OsmsPad creates HID gamepads that Windows puts on the XInput path through its inbox
 * xinputhid filter, without ViGEmBus. Each controller belongs to the handle that created it:
 * if Sunshine exits or crashes, the driver removes its controllers. The driver records the
 * caller's Windows session in each controller's instance ID (OSMS_S<session>_P<n>), which
 * OpenStreamMS uses to show the controller only in that session.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// platform includes
#include <Windows.h>

// local includes
#include "src/platform/common.h"

namespace platf {

  /**
   * @brief Client of the OsmsPad driver for every gamepad of one Sunshine instance.
   */
  class osmspad_t {
  public:
    osmspad_t() = default;
    ~osmspad_t();
    osmspad_t(const osmspad_t &) = delete;
    osmspad_t &operator=(const osmspad_t &) = delete;

    /**
     * @brief Check whether the OsmsPad driver interface is present on this machine.
     * @return True when the driver is installed and started.
     */
    static bool available();

    /**
     * @brief Open the driver and start the rumble thread.
     * @return True on success.
     */
    bool init();

    /**
     * @brief Create an Xbox Series controller for a Sunshine gamepad slot.
     * @param id Global and client-relative gamepad indexes.
     * @param feedback_queue Queue that returns rumble to the client.
     * @return 0 on success, -1 on failure.
     */
    int alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue);

    /**
     * @brief Point an existing controller's feedback at a resumed client.
     * @param id Global and client-relative gamepad indexes.
     * @param feedback_queue Queue that returns rumble to the new client.
     * @return 0 on success, -1 when the slot has no controller.
     */
    int rebind(const gamepad_id_t &id, feedback_queue_t feedback_queue);

    /**
     * @brief Remove the controller of a gamepad slot.
     * @param nr Global gamepad index.
     */
    void free(int nr);

    /**
     * @brief Whether a gamepad slot is served by OsmsPad.
     * @param nr Global gamepad index.
     * @return True when the slot has an OsmsPad controller.
     */
    bool has(int nr) const;

    /**
     * @brief Send the full controller state.
     * @param nr Global gamepad index.
     * @param state Buttons, triggers and sticks from the client.
     */
    void update(int nr, const gamepad_state_t &state);

  private:
    struct slot_t {
      bool used = false;
      std::uint32_t pad = 0;  ///< Driver controller number.
      std::uint8_t client_relative_index = 0;
      feedback_queue_t feedback_queue;
      std::uint16_t last_low = 0, last_high = 0, last_left = 0, last_right = 0;
    };

    void feedback_loop();
    bool ioctl(DWORD code, const void *in, DWORD in_size, void *out, DWORD out_size, DWORD *returned = nullptr);

    HANDLE device_ = INVALID_HANDLE_VALUE;
    HANDLE stop_event_ = nullptr;
    std::thread feedback_thread_;
    mutable std::mutex mutex_;
    std::array<slot_t, MAX_GAMEPADS> slots_ {};
  };

}  // namespace platf
