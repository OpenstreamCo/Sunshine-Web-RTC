/**
 * @file src/platform/windows/session_input.cpp
 * @brief Session-local keyboard and mouse injection for non-console sessions.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <sstream>
#include <vector>

// platform includes
#include <Windows.h>

// lib includes
#include <platform/windows/keylayout.hpp>

// local includes
#include "misc.h"
#include "session_input.h"
#include "src/config.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf::session_input {
  namespace {
    /**
     * @brief Input desktop last bound to this thread, used to retry SendInput after a desktop switch.
     */
    thread_local HDESK last_input_desktop = nullptr;

    /**
     * @brief Call SendInput() and retry once on the new input desktop (UAC, lock screen).
     *
     * @param inputs Events to inject.
     */
    void send_local(std::span<INPUT> inputs) {
      const auto count = static_cast<UINT>(inputs.size());
      if (SendInput(count, inputs.data(), sizeof(INPUT)) == count) {
        return;
      }

      if (const auto desktop = syncThreadDesktop(); desktop != last_input_desktop) {
        last_input_desktop = desktop;
        if (SendInput(count, inputs.data(), sizeof(INPUT)) == count) {
          return;
        }
      }

      BOOST_LOG(error) << "Couldn't send session-local input: "sv << GetLastError();
    }

    DWORD own_session() {
      static const DWORD session = [] {
        DWORD s = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &s);
        return s;
      }();
      return session;
    }

    // ── Mosaic: mouse and keyboard to another session (osms-input.txt) ─────────

    /**
     * @brief Header of each batch sent to the OpenStreamMS input relay: "OSIN" and the event count,
     * followed by that many INPUT structures.
     */
    constexpr std::uint32_t relay_magic = 0x4E49534F;

    std::mutex redirect_mutex;
    std::optional<input_target_t> target;  ///< nullopt = this session.
    std::chrono::steady_clock::time_point next_target_check;
    HANDLE relay_pipe = INVALID_HANDLE_VALUE;
    DWORD relay_session = 0;
    std::chrono::steady_clock::time_point next_relay_connect;
    std::vector<INPUT> held;  ///< Keys and mouse buttons down, released when the target changes.

    std::filesystem::path target_path() {
      return std::filesystem::path(config::sunshine.config_file).parent_path() / "osms-input.txt";
    }

    void close_relay() {
      if (relay_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(relay_pipe);
        relay_pipe = INVALID_HANDLE_VALUE;
      }
    }

    /**
     * @brief Whether the pipe server is a SYSTEM process of that session (the OpenStreamMS relay):
     * any process can create a pipe with that name first and would get the keystrokes.
     */
    bool trusted_relay(HANDLE pipe, DWORD session) {
      ULONG pid = 0;
      DWORD pid_session = 0;
      if (!GetNamedPipeServerProcessId(pipe, &pid) || !ProcessIdToSessionId(pid, &pid_session) || pid_session != session) {
        return false;
      }
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!process) {
        return false;
      }
      bool system = false;
      HANDLE token = nullptr;
      if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        alignas(TOKEN_USER) std::array<std::uint8_t, 256> buffer {};
        DWORD length = 0;
        if (GetTokenInformation(token, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()), &length)) {
          system = IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, WinLocalSystemSid);
        }
        CloseHandle(token);
      }
      CloseHandle(process);
      return system;
    }

    bool connect_relay(DWORD session) {
      if (relay_pipe != INVALID_HANDLE_VALUE && relay_session == session) {
        return true;
      }
      close_relay();
      const auto now = std::chrono::steady_clock::now();
      if (now < next_relay_connect) {
        return false;
      }
      next_relay_connect = now + 1s;

      const auto name = L"\\\\.\\pipe\\osms-input-" + std::to_wstring(session);
      // Identification only: the relay must not be able to impersonate Sunshine.
      HANDLE pipe = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
      if (pipe == INVALID_HANDLE_VALUE) {
        BOOST_LOG(warning) << "No input relay in session "sv << session << " ("sv << GetLastError() << "); mouse and keyboard stay in this session"sv;
        return false;
      }
      if (!trusted_relay(pipe, session)) {
        BOOST_LOG(error) << "Input relay of session "sv << session << " is not a SYSTEM process; ignored"sv;
        CloseHandle(pipe);
        return false;
      }
      relay_pipe = pipe;
      relay_session = session;
      BOOST_LOG(info) << "Mouse and keyboard go to session "sv << session;
      return true;
    }

    /**
     * @brief Inject in the target session, or here when there is none (or its relay is not
     * running: better here than lost).
     */
    void deliver(const std::optional<input_target_t> &to, std::span<INPUT> inputs) {
      if (inputs.empty()) {
        return;
      }
      if (!to || !connect_relay(to->session)) {
        send_local(inputs);
        return;
      }
      std::vector<std::uint8_t> buffer(8 + inputs.size_bytes());
      const std::uint32_t header[] {relay_magic, static_cast<std::uint32_t>(inputs.size())};
      std::memcpy(buffer.data(), header, sizeof(header));
      std::memcpy(buffer.data() + 8, inputs.data(), inputs.size_bytes());
      DWORD written = 0;
      if (!WriteFile(relay_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr) || written != buffer.size()) {
        BOOST_LOG(warning) << "Input relay of session "sv << relay_session << " went away ("sv << GetLastError() << ')';
        close_relay();
        send_local(inputs);
      }
    }

    void track_held(const INPUT &input) {
      if (input.type == INPUT_KEYBOARD) {
        if (input.ki.dwFlags & KEYEVENTF_UNICODE) {
          return;
        }
        std::erase_if(held, [&](const INPUT &h) {
          return h.type == INPUT_KEYBOARD && h.ki.wVk == input.ki.wVk && h.ki.wScan == input.ki.wScan;
        });
        if (!(input.ki.dwFlags & KEYEVENTF_KEYUP)) {
          held.push_back(input);
        }
        return;
      }
      if (input.type != INPUT_MOUSE) {
        return;
      }
      constexpr DWORD downs = MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_MIDDLEDOWN | MOUSEEVENTF_XDOWN;
      constexpr DWORD ups = downs << 1;  // every *UP flag is its *DOWN flag shifted once
      const DWORD flags = input.mi.dwFlags;
      if (flags & (downs | ups)) {
        const DWORD down = (flags & downs) | ((flags & ups) >> 1);
        std::erase_if(held, [&](const INPUT &h) {
          return h.type == INPUT_MOUSE && h.mi.dwFlags == down && h.mi.mouseData == input.mi.mouseData;
        });
        if (flags & downs) {
          INPUT h {};
          h.type = INPUT_MOUSE;
          h.mi.dwFlags = flags & downs;
          h.mi.mouseData = input.mi.mouseData;
          held.push_back(h);
        }
      }
    }

    /**
     * @brief Release what is held in the session that stops receiving input: a Ctrl or a mouse
     * button would stay down there.
     */
    void release_held(const std::optional<input_target_t> &from) {
      std::vector<INPUT> releases;
      for (auto h : held) {
        if (h.type == INPUT_KEYBOARD) {
          h.ki.dwFlags |= KEYEVENTF_KEYUP;
        } else {
          h.mi.dwFlags <<= 1;
        }
        releases.push_back(h);
      }
      held.clear();
      deliver(from, releases);
    }

    /**
     * @brief Target from osms-input.txt, re-read at most every 250 ms. Call with redirect_mutex held.
     */
    const std::optional<input_target_t> &current_target() {
      const auto now = std::chrono::steady_clock::now();
      if (now < next_target_check) {
        return target;
      }
      next_target_check = now + 250ms;

      std::optional<input_target_t> next;
      if (std::ifstream file(target_path(), std::ios::binary); file) {
        std::ostringstream content;
        content << file.rdbuf();
        next = parse_input_target(content.str());
      }
      if (next && next->session == own_session()) {
        next.reset();
      }

      const DWORD before = target ? target->session : 0;
      const DWORD after = next ? next->session : 0;
      if (before != after) {
        release_held(target);
        if (!next) {
          close_relay();
          BOOST_LOG(info) << "Mouse and keyboard back to this session"sv;
        }
      }
      target = next;
      return target;
    }

    void send(std::span<INPUT> inputs) {
      std::scoped_lock lock {redirect_mutex};
      const auto &to = current_target();
      for (const auto &input : inputs) {
        track_held(input);
      }
      deliver(to, inputs);
    }

    void send(INPUT input) {
      std::array inputs {input};
      send(std::span<INPUT> {inputs});
    }

    INPUT mouse_input(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
      INPUT input {};
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = flags;
      input.mi.dx = dx;
      input.mi.dy = dy;
      input.mi.mouseData = data;
      return input;
    }

    /**
     * @brief Scale a coordinate inside [0, dimension] to the 0..65535 range used by MOUSEEVENTF_ABSOLUTE.
     */
    LONG scale_absolute_axis(float value, int dimension) {
      if (dimension <= 1) {
        return 0;
      }

      const auto clamped = std::clamp(value, 0.0F, static_cast<float>(dimension));
      return static_cast<LONG>(std::lround(clamped * static_cast<float>(std::numeric_limits<std::uint16_t>::max()) / static_cast<float>(dimension)));
    }

    /**
     * @brief Map a viewport-relative coordinate into the virtual desktop (same math as libvirtualhid's Win32 mouse).
     */
    LONG scale_to_desktop(float value, int viewport_offset, int viewport_dimension, int desktop_offset, int desktop_dimension) {
      if (viewport_dimension <= 0 || desktop_dimension <= 0) {
        return 0;
      }

      const auto clamped = std::clamp(value, 0.0F, static_cast<float>(viewport_dimension));
      const auto span = static_cast<float>(std::max(viewport_dimension - 1, 0));
      const auto target = static_cast<float>(viewport_offset - desktop_offset) + clamped * span / static_cast<float>(viewport_dimension);
      return scale_absolute_axis(target, desktop_dimension);
    }

    bool extended_key(std::uint16_t key_code) {
      switch (key_code) {
        case VK_LWIN:
        case VK_RWIN:
        case VK_RMENU:
        case VK_RCONTROL:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_DIVIDE:
        case VK_APPS:
          return true;
        default:
          return false;
      }
    }
  }  // namespace

  std::optional<input_target_t> parse_input_target(const std::string &text) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t\r") == std::string::npos) {
        continue;
      }
      std::istringstream fields(line);
      long long session = 0;
      input_target_t t;
      if (!(fields >> session >> t.x >> t.y >> t.width >> t.height >> t.canvas_width >> t.canvas_height) ||
          session <= 0 || session > std::numeric_limits<std::uint32_t>::max() ||
          t.x < 0 || t.y < 0 || t.width <= 0 || t.height <= 0 ||
          t.x + t.width > t.canvas_width || t.y + t.height > t.canvas_height) {
        return std::nullopt;
      }
      t.session = static_cast<std::uint32_t>(session);
      return t;
    }
    return std::nullopt;
  }

  std::pair<long, long> map_to_target(const input_target_t &target, float canvas_fx, float canvas_fy) {
    const auto axis = [](float fraction, int canvas, int start, int length) {
      const float pixel = std::clamp(fraction, 0.0F, 1.0F) * static_cast<float>(canvas);
      const float inside = std::clamp((pixel - static_cast<float>(start)) / static_cast<float>(length), 0.0F, 1.0F);
      return static_cast<long>(std::lround(inside * static_cast<float>(std::numeric_limits<std::uint16_t>::max())));
    };
    return {
      axis(canvas_fx, target.canvas_width, target.x, target.width),
      axis(canvas_fy, target.canvas_height, target.y, target.height),
    };
  }

  bool should_use() {
    // The console can move between sessions while streaming (e.g. someone logs
    // in locally), so compare on every event. 0xFFFFFFFF means "no console
    // session right now": SendInput is still the right choice then.
    const bool bypass = WTSGetActiveConsoleSessionId() != own_session();

    static std::atomic<int> logged {-1};
    if (const int state = bypass ? 1 : 0; logged.exchange(state) != state) {
      if (bypass) {
        BOOST_LOG(info) << "Sunshine runs outside the console session ("sv << own_session()
                        << "): mouse and keyboard use SendInput instead of Virtual HID devices"sv;
      } else {
        BOOST_LOG(info) << "Sunshine runs in the console session: mouse and keyboard use libvirtualhid"sv;
      }
    }
    return bypass;
  }

  void move_mouse(int delta_x, int delta_y) {
    send(mouse_input(MOUSEEVENTF_MOVE, delta_x, delta_y));
  }

  void abs_mouse(const touch_port_t &touch_port, float x, float y) {
    // Same desktop/viewport split as virtualhid::pointer_viewports(): the
    // desktop is the whole virtual screen, the viewport the streamed display.
    const bool has_logical = touch_port.logical_width > 0 && touch_port.logical_height > 0;
    const int viewport_x = has_logical ? touch_port.offset_x : touch_port.env_offset_x;
    const int viewport_y = has_logical ? touch_port.offset_y : touch_port.env_offset_y;
    const int viewport_w = has_logical ? touch_port.logical_width : touch_port.width;
    const int viewport_h = has_logical ? touch_port.logical_height : touch_port.height;

    // Mouse and keyboard moved to another session of the mosaic: the client points at the
    // canvas and that session's desktop is its part of it.
    std::optional<input_target_t> to;
    {
      std::scoped_lock lock {redirect_mutex};
      to = current_target();
    }
    if (to && viewport_w > 0 && viewport_h > 0) {
      const auto fx = (x - static_cast<float>(viewport_x)) / static_cast<float>(viewport_w);
      const auto fy = (y - static_cast<float>(viewport_y)) / static_cast<float>(viewport_h);
      const auto [tx, ty] = map_to_target(*to, fx, fy);
      send(mouse_input(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, tx, ty));
      return;
    }

    const auto dx = scale_to_desktop(x - static_cast<float>(viewport_x), viewport_x, viewport_w, touch_port.env_offset_x, touch_port.width);
    const auto dy = scale_to_desktop(y - static_cast<float>(viewport_y), viewport_y, viewport_h, touch_port.env_offset_y, touch_port.height);
    send(mouse_input(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, dx, dy));
  }

  void button_mouse(int button, bool release) {
    switch (button) {
      case BUTTON_LEFT:
        send(mouse_input(release ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN));
        return;
      case BUTTON_MIDDLE:
        send(mouse_input(release ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN));
        return;
      case BUTTON_RIGHT:
        send(mouse_input(release ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN));
        return;
      case BUTTON_X1:
        send(mouse_input(release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN, 0, 0, XBUTTON1));
        return;
      case BUTTON_X2:
        send(mouse_input(release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN, 0, 0, XBUTTON2));
        return;
      default:
        BOOST_LOG(warning) << "Unknown mouse button: "sv << button;
        return;
    }
  }

  void scroll(int high_res_distance) {
    send(mouse_input(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(high_res_distance)));
  }

  void hscroll(int high_res_distance) {
    send(mouse_input(MOUSEEVENTF_HWHEEL, 0, 0, static_cast<DWORD>(high_res_distance)));
  }

  void keyboard_update(std::uint16_t modcode, bool release, std::uint8_t flags, bool extended) {
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    auto &ki = input.ki;
    ki.wVk = modcode;

    // Same rules as libvirtualhid's Win32 keyboard: normalized VK codes map
    // through the US English layout; otherwise only map when asked to.
    if (!(flags & SS_KBE_FLAG_NON_NORMALIZED)) {
      ki.wScan = lvh::detail::windows_us_english_scan_code(modcode);
    } else if (config::input.always_send_scancodes && modcode != VK_LWIN && modcode != VK_RWIN && modcode != VK_PAUSE) {
      ki.wScan = static_cast<WORD>(MapVirtualKeyW(modcode, MAPVK_VK_TO_VSC));
    }

    if (ki.wScan != 0) {
      ki.wVk = 0;
      ki.dwFlags |= KEYEVENTF_SCANCODE;
    }
    if (extended || extended_key(modcode)) {
      ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (release) {
      ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    send(input);
  }

  void unicode(const char *utf8, int size) {
    if (!utf8 || size <= 0) {
      return;
    }

    const auto chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, size, nullptr, 0);
    if (chars <= 0) {
      return;
    }

    std::vector<WCHAR> wide(static_cast<std::size_t>(chars));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, size, wide.data(), chars) <= 0) {
      return;
    }

    std::vector<INPUT> inputs;
    inputs.reserve(wide.size() * 2);
    for (const auto release : {false, true}) {
      for (const auto ch : wide) {
        INPUT input {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = ch;
        input.ki.dwFlags = KEYEVENTF_UNICODE | (release ? KEYEVENTF_KEYUP : 0);
        inputs.push_back(input);
      }
    }
    send(std::span<INPUT> {inputs});
  }
}  // namespace platf::session_input
