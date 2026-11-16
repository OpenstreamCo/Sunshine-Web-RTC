/**
 * @file src/platform/windows/osmspad.cpp
 * @brief Virtual Xbox Series gamepads through OpenStreamMS's OsmsPad driver.
 */
// platform includes
#include <Windows.h>
#include <cfgmgr32.h>
#include <initguid.h>

// standard includes
#include <vector>

// local includes
#include "osmspad.h"
#include "osmspad_ioctl.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf {
  namespace {
    /**
     * @brief Device path of the first present OsmsPad interface.
     * @return Path, or an empty string when the driver is not installed or not started.
     */
    std::wstring interface_path() {
      ULONG length = 0;
      auto guid = GUID_DEVINTERFACE_OSMSPAD;
      if (CM_Get_Device_Interface_List_SizeW(&length, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length <= 1) {
        return {};
      }
      std::vector<wchar_t> list(length);
      if (CM_Get_Device_Interface_ListW(&guid, nullptr, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return {};
      }
      return list.data();
    }

    /**
     * @brief Moonlight button mask to the XInput-style bits OsmsPad takes.
     *
     * The low 16 Moonlight bits already use the XInput layout. The miscellaneous and touchpad
     * buttons become the Xbox Series Share button.
     */
    std::uint16_t to_osmspad_buttons(const std::uint32_t flags) {
      auto buttons = static_cast<std::uint16_t>(flags & 0xF7FF);
      if (flags & (MISC_BUTTON | TOUCHPAD_BUTTON)) {
        buttons |= OSMSPAD_SHARE;
      }
      return buttons;
    }
  }  // namespace

  bool osmspad_t::available() {
    return !interface_path().empty();
  }

  osmspad_t::~osmspad_t() {
    if (stop_event_) {
      SetEvent(stop_event_);
    }
    if (device_ != INVALID_HANDLE_VALUE) {
      CancelIoEx(device_, nullptr);
    }
    if (feedback_thread_.joinable()) {
      feedback_thread_.join();
    }
    // Closing the handle removes every controller this instance created.
    if (device_ != INVALID_HANDLE_VALUE) {
      CloseHandle(device_);
    }
    if (stop_event_) {
      CloseHandle(stop_event_);
    }
  }

  bool osmspad_t::init() {
    const auto path = interface_path();
    if (path.empty()) {
      return false;
    }
    device_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (device_ == INVALID_HANDLE_VALUE) {
      BOOST_LOG(warning) << "Couldn't open the OsmsPad driver ["sv << GetLastError() << ']';
      return false;
    }
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    feedback_thread_ = std::thread {&osmspad_t::feedback_loop, this};
    BOOST_LOG(info) << "OsmsPad virtual gamepad driver is available"sv;
    return true;
  }

  bool osmspad_t::ioctl(DWORD code, const void *in, DWORD in_size, void *out, DWORD out_size, DWORD *returned) {
    OVERLAPPED overlapped {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(device_, code, const_cast<void *>(in), in_size, out, out_size, &bytes, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      ok = GetOverlappedResult(device_, &overlapped, &bytes, TRUE);
    }
    const auto error = GetLastError();
    CloseHandle(overlapped.hEvent);
    if (returned) {
      *returned = bytes;
    }
    SetLastError(error);
    return ok != FALSE;
  }

  int osmspad_t::alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    if (id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS) {
      return -1;
    }
    free(id.globalIndex);

    OsmsPadCreateIn request {OSMSPAD_PROTOCOL_VERSION, OSMSPAD_PROFILE_XBOX_SERIES, OSMSPAD_SESSION_CALLER};
    OsmsPadCreateOut created {};
    if (!ioctl(IOCTL_OSMSPAD_CREATE, &request, sizeof request, &created, sizeof created)) {
      BOOST_LOG(error) << "Couldn't create an OsmsPad controller ["sv << GetLastError() << ']';
      return -1;
    }

    std::lock_guard lock {mutex_};
    auto &slot = slots_[id.globalIndex];
    slot = {};
    slot.used = true;
    slot.pad = created.Pad;
    slot.client_relative_index = id.clientRelativeIndex;
    slot.feedback_queue = std::move(feedback_queue);
    BOOST_LOG(info) << "OsmsPad controller "sv << created.Pad << " created for gamepad "sv << id.globalIndex;
    return 0;
  }

  int osmspad_t::rebind(const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    std::lock_guard lock {mutex_};
    if (id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS || !slots_[id.globalIndex].used) {
      return -1;
    }
    auto &slot = slots_[id.globalIndex];
    slot.client_relative_index = id.clientRelativeIndex;
    slot.feedback_queue = std::move(feedback_queue);
    slot.last_low = slot.last_high = slot.last_left = slot.last_right = 0;
    return 0;
  }

  void osmspad_t::free(int nr) {
    std::uint32_t pad;
    {
      std::lock_guard lock {mutex_};
      if (nr < 0 || nr >= MAX_GAMEPADS || !slots_[nr].used) {
        return;
      }
      pad = slots_[nr].pad;
      slots_[nr] = {};
    }
    OsmsPadDestroyIn request {pad};
    if (!ioctl(IOCTL_OSMSPAD_DESTROY, &request, sizeof request, nullptr, 0)) {
      BOOST_LOG(warning) << "Couldn't remove OsmsPad controller "sv << pad << " ["sv << GetLastError() << ']';
    }
  }

  bool osmspad_t::has(int nr) const {
    std::lock_guard lock {mutex_};
    return nr >= 0 && nr < MAX_GAMEPADS && slots_[nr].used;
  }

  void osmspad_t::update(int nr, const gamepad_state_t &state) {
    OsmsPadInput input {};
    {
      std::lock_guard lock {mutex_};
      if (nr < 0 || nr >= MAX_GAMEPADS || !slots_[nr].used) {
        return;
      }
      input.Pad = slots_[nr].pad;
    }
    input.Buttons = to_osmspad_buttons(state.buttonFlags);
    input.LeftTrigger = state.lt;
    input.RightTrigger = state.rt;
    input.ThumbLX = state.lsX;
    input.ThumbLY = state.lsY;
    input.ThumbRX = state.rsX;
    input.ThumbRY = state.rsY;
    if (!ioctl(IOCTL_OSMSPAD_INPUT, &input, sizeof input, nullptr, 0)) {
      BOOST_LOG(warning) << "Couldn't send gamepad input to OsmsPad ["sv << GetLastError() << ']';
    }
  }

  // One pending feedback request at a time: the driver completes it when a game asks one of
  // our controllers to rumble, and the loop posts it to that controller's client.
  void osmspad_t::feedback_loop() {
    OVERLAPPED overlapped {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE waits[] {stop_event_, overlapped.hEvent};

    while (true) {
      OsmsPadFeedback feedback {};
      DWORD bytes = 0;
      ResetEvent(overlapped.hEvent);
      BOOL ok = DeviceIoControl(device_, IOCTL_OSMSPAD_FEEDBACK, nullptr, 0, &feedback, sizeof feedback, &bytes, &overlapped);
      if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
          CancelIoEx(device_, &overlapped);
          GetOverlappedResult(device_, &overlapped, &bytes, TRUE);
          break;
        }
        ok = GetOverlappedResult(device_, &overlapped, &bytes, FALSE);
      }
      if (!ok) {
        if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
          break;
        }
        BOOST_LOG(warning) << "OsmsPad feedback request failed ["sv << GetLastError() << ']';
        if (WaitForSingleObject(stop_event_, 1000) == WAIT_OBJECT_0) {
          break;
        }
        continue;
      }
      if (bytes < sizeof feedback) {
        continue;
      }

      std::lock_guard lock {mutex_};
      for (auto &slot : slots_) {
        if (!slot.used || slot.pad != feedback.Pad || !slot.feedback_queue) {
          continue;
        }
        if (feedback.LargeMotor != slot.last_low || feedback.SmallMotor != slot.last_high) {
          slot.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(slot.client_relative_index, feedback.LargeMotor, feedback.SmallMotor));
          slot.last_low = feedback.LargeMotor;
          slot.last_high = feedback.SmallMotor;
        }
        if (feedback.LeftTrigger != slot.last_left || feedback.RightTrigger != slot.last_right) {
          slot.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble_triggers(slot.client_relative_index, feedback.LeftTrigger, feedback.RightTrigger));
          slot.last_left = feedback.LeftTrigger;
          slot.last_right = feedback.RightTrigger;
        }
        break;
      }
    }
    CloseHandle(overlapped.hEvent);
  }

}  // namespace platf
