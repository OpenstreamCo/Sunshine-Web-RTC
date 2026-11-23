/**
 * @file src/platform/windows/display_remoteidd.cpp
 * @brief Definitions for the OpenStreamMS RemoteIdd capture backend.
 * @note snapshot() lives in display_vram.cpp, next to the other VRAM backends that share
 *       its image helpers.
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "display.h"
#include "misc.h"
#include "remoteidd/FrameConsumer.h"
#include "src/config.h"
#include "src/logging.h"

namespace platf::dxgi {
  using namespace std::literals;

  namespace {
    /**
     * @brief Read a process environment variable (Win32 view, not the CRT copy).
     */
    std::wstring env(const wchar_t *name) {
      wchar_t buf[64];
      DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
      return (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : std::wstring();
    }

    /**
     * @brief Namespace prefix of the RemoteIdd named objects.
     *
     * The driver publishes in Global\. OSMS_RIDD_NS lets tests use Local\ without privileges.
     */
    std::wstring ridd_namespace() {
      if (auto ns = env(L"OSMS_RIDD_NS"); !ns.empty()) {
        return ns;
      }
      return REMOTEIDD_NS;
    }

    /**
     * @brief Terminal Services session whose slot this process reads.
     *
     * Sunshine runs inside the seat session; OSMS_RIDD_SESSION overrides it for tests.
     */
    uint32_t ridd_session() {
      if (auto s = env(L"OSMS_RIDD_SESSION"); !s.empty()) {
        return static_cast<uint32_t>(std::wcstoul(s.c_str(), nullptr, 10));
      }
      DWORD session = 0;
      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      return session;
    }

    /**
     * @brief osms-mosaic.txt, written by OpenStreamMS next to this instance's sunshine.conf.
     *
     * See parse_mosaic_layout() for the format.
     */
    std::filesystem::path mosaic_path() {
      return std::filesystem::path(config::sunshine.config_file).parent_path() / "osms-mosaic.txt";
    }

    /**
     * @brief Whole content of osms-mosaic.txt, or empty when there is none.
     */
    std::string read_layout() {
      std::ifstream file(mosaic_path(), std::ios::binary);
      if (!file) {
        return {};
      }
      std::ostringstream content;
      content << file.rdbuf();
      return content.str();
    }

  }  // namespace

  int mosaic_area_cells(std::string_view area) {
    constexpr std::pair<std::string_view, int> areas[] {
      {"Full"sv, 0b1111},
      {"Left"sv, 0b0101},
      {"Right"sv, 0b1010},
      {"Top"sv, 0b0011},
      {"Bottom"sv, 0b1100},
      {"TopLeft"sv, 0b0001},
      {"TopRight"sv, 0b0010},
      {"BottomLeft"sv, 0b0100},
      {"BottomRight"sv, 0b1000},
    };
    for (const auto &[name, cells] : areas) {
      if (name == area) {
        return cells;
      }
    }
    return 0;
  }

  mosaic_layout_t parse_mosaic_layout(const std::string &text) {
    mosaic_layout_t layout;
    int used = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty() || line[0] == '#') {
        continue;
      }
      std::istringstream fields(line);
      std::string first;
      std::string second;
      if (!(fields >> first >> second)) {
        continue;
      }
      if (first == "canvas") {
        int w = 0;
        int h = 0;
        std::istringstream size(second);
        if ((size >> w) && (fields >> h) && w > 0 && h > 0) {
          layout.canvas_width = w;
          layout.canvas_height = h;
        }
        continue;
      }
      // Rest of the line: the waiting image path (it may contain spaces).
      std::string waiting;
      std::getline(fields, waiting);
      waiting.erase(0, waiting.find_first_not_of(" \t"));
      uint32_t session = 0;
      auto [end, ec] = std::from_chars(first.data(), first.data() + first.size(), session);
      int cells = mosaic_area_cells(second);
      if (ec != std::errc {} || end != first.data() + first.size() || !cells || (cells & used)) {
        BOOST_LOG(warning) << "RemoteIdd mosaic: skipping line \""sv << line << '"';
        continue;
      }
      used |= cells;
      layout.entries.push_back({session, cells, std::move(waiting)});
    }
    return layout;
  }

  std::optional<waiting_image_t> parse_waiting_image(const std::string &bytes) {
    if (bytes.size() < 12 || bytes.compare(0, 4, "OSWP") != 0) {
      return std::nullopt;
    }
    auto read_int = [&](size_t at) {
      return static_cast<int32_t>(static_cast<uint8_t>(bytes[at]) | (static_cast<uint8_t>(bytes[at + 1]) << 8) | (static_cast<uint8_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 3])) << 24));
    };
    waiting_image_t image;
    image.width = read_int(4);
    image.height = read_int(8);
    if (image.width <= 0 || image.height <= 0 || image.width > 16384 || image.height > 16384 || bytes.size() != 12 + static_cast<size_t>(image.width) * image.height * 4) {
      return std::nullopt;
    }
    image.bgra.assign(bytes.begin() + 12, bytes.end());
    return image;
  }

  mosaic_rect_t mosaic_rect(int cells, int canvas_width, int canvas_height) {
    const bool left = cells & 0b0101;
    const bool right = cells & 0b1010;
    const bool top = cells & 0b0011;
    const bool bottom = cells & 0b1100;
    const int w1 = canvas_width / 2;
    const int h1 = canvas_height / 2;
    return {
      left ? 0 : w1,
      top ? 0 : h1,
      left && right ? canvas_width : left ? w1 :
                                            canvas_width - w1,
      top && bottom ? canvas_height : top ? h1 :
                                            canvas_height - h1,
    };
  }

  // Out of line: FrameConsumer is incomplete in display.h.
  display_ridd_vram_t::display_ridd_vram_t() = default;
  display_ridd_vram_t::~display_ridd_vram_t() = default;

  int display_ridd_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (config.dynamicRange) {
      BOOST_LOG(error) << "RemoteIdd capture does not support HDR yet"sv;
      return -1;
    }
    if (display_base_t::init(config, display_name)) {
      return -1;
    }

    // IddCx 1.4 swapchains are always 8-bit BGRA.
    capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;

    auto ns = ridd_namespace();
    auto session = ridd_session();

    layout = read_layout();
    next_layout_check = std::chrono::steady_clock::now() + 250ms;
    auto parsed = parse_mosaic_layout(layout);
    bool listed = std::ranges::any_of(parsed.entries, [&](const mosaic_entry_t &e) {
      return e.session == session;
    });

    if (parsed.entries.size() < 2 || !listed) {
      consumer = std::make_unique<osms::ridd::FrameConsumer>(Microsoft::WRL::ComPtr<ID3D11Device>(device.get()), session, ns.c_str());
      if (!consumer->Open()) {
        BOOST_LOG(error) << "RemoteIdd: no frame slot for session "sv << session << "; is the driver running?"sv;
        return -1;
      }
      BOOST_LOG(info) << "RemoteIdd: reading frames for session "sv << session;
      return 0;
    }

    // Mosaic: OpenStreamMS sets the canvas size (the host session's own resolution) and
    // reconnects each session to the size of its part; the encoder scales the canvas to
    // the client like any display. Without a canvas line, the stream size is used.
    const int canvas_w = parsed.canvas_width ? parsed.canvas_width : config.width;
    const int canvas_h = parsed.canvas_height ? parsed.canvas_height : config.height;
    if (canvas_w <= 0 || canvas_h <= 0) {
      BOOST_LOG(error) << "RemoteIdd mosaic: invalid canvas size "sv << canvas_w << 'x' << canvas_h;
      return -1;
    }

    for (const auto &e : parsed.entries) {
      auto rect = mosaic_rect(e.cells, canvas_w, canvas_h);
      mosaic_tile_t tile;
      tile.session = e.session;
      tile.x = rect.x;
      tile.y = rect.y;
      tile.width = rect.width;
      tile.height = rect.height;
      if (!e.waiting.empty()) {
        load_waiting(tile, e.waiting);
      }

      if (e.session == session) {
        // Absolute mouse: the client points at the canvas, the desktop is this tile.
        offset_x -= tile.x;
        offset_y -= tile.y;
      }
      BOOST_LOG(info) << "RemoteIdd mosaic: session "sv << tile.session << " at "sv << tile.x << ',' << tile.y << ' ' << tile.width << 'x' << tile.height;
      tiles.push_back(std::move(tile));
    }

    width = width_before_rotation = canvas_w;
    height = height_before_rotation = canvas_h;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = canvas_w;
    desc.Height = canvas_h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = capture_format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *raw_canvas = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &raw_canvas))) {
      BOOST_LOG(error) << "RemoteIdd mosaic: failed to create the canvas"sv;
      return -1;
    }
    canvas.reset(raw_canvas);
    ID3D11RenderTargetView *raw_rt = nullptr;
    if (FAILED(device->CreateRenderTargetView(canvas.get(), nullptr, &raw_rt))) {
      BOOST_LOG(error) << "RemoteIdd mosaic: failed to create the canvas render target"sv;
      return -1;
    }
    canvas_rt.reset(raw_rt);
    clear_canvas();

    for (auto &tile : tiles) {
      if (tile.session) {
        open_tile(tile);
      }
    }
    BOOST_LOG(info) << "RemoteIdd mosaic: "sv << tiles.size() << " sessions on a "sv << canvas_w << 'x' << canvas_h << " canvas"sv;
    return 0;
  }

  bool display_ridd_vram_t::layout_changed() {
    auto now = std::chrono::steady_clock::now();
    if (now < next_layout_check) {
      return false;
    }
    next_layout_check = now + 250ms;
    if (read_layout() == layout) {
      return false;
    }
    BOOST_LOG(info) << "RemoteIdd: mosaic layout changed, reinitializing capture"sv;
    return true;
  }

  bool display_ridd_vram_t::open_tile(mosaic_tile_t &tile) {
    auto ns = ridd_namespace();
    auto reader = std::make_unique<osms::ridd::FrameConsumer>(Microsoft::WRL::ComPtr<ID3D11Device>(device.get()), tile.session, ns.c_str());
    if (!reader->Open()) {
      // The session may still be connecting: try again shortly.
      tile.next_open = std::chrono::steady_clock::now() + 500ms;
      return false;
    }
    tile.consumer = std::move(reader);
    tile.last_frame = 0;
    compose_now = true;
    BOOST_LOG(info) << "RemoteIdd mosaic: reading frames for session "sv << tile.session;
    return true;
  }

  void display_ridd_vram_t::clear_canvas() {
    constexpr float black[4] {0.0f, 0.0f, 0.0f, 1.0f};
    device_ctx->ClearRenderTargetView(canvas_rt.get(), black);
    for (auto &tile : tiles) {
      tile.last_frame = 0;
      tile.showing_waiting = false;
    }
    compose_now = true;
  }

  void display_ridd_vram_t::load_waiting(mosaic_tile_t &tile, const std::string &path) {
    std::ifstream file(std::filesystem::path(reinterpret_cast<const char8_t *>(path.c_str())), std::ios::binary);
    std::ostringstream content;
    if (file) {
      content << file.rdbuf();
    }
    auto image = parse_waiting_image(content.str());
    if (!image) {
      BOOST_LOG(warning) << "RemoteIdd mosaic: no valid waiting image at "sv << path;
      return;
    }
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = image->width;
    desc.Height = image->height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data {image->bgra.data(), static_cast<UINT>(image->width * 4), 0};
    ID3D11Texture2D *raw = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &data, &raw))) {
      BOOST_LOG(warning) << "RemoteIdd mosaic: failed to create the waiting image texture"sv;
      return;
    }
    tile.waiting.reset(raw);
  }

  void display_ridd_vram_t::draw_waiting(mosaic_tile_t &tile) {
    D3D11_TEXTURE2D_DESC desc;
    tile.waiting->GetDesc(&desc);
    // Same size as the part (OpenStreamMS draws it so); otherwise centred and cropped.
    const auto w = std::min<UINT>(desc.Width, tile.width);
    const auto h = std::min<UINT>(desc.Height, tile.height);
    const auto sx = (desc.Width - w) / 2;
    const auto sy = (desc.Height - h) / 2;
    D3D11_BOX box {sx, sy, 0, sx + w, sy + h, 1};
    device_ctx->CopySubresourceRegion(canvas.get(), 0, tile.x + (tile.width - static_cast<int>(w)) / 2, tile.y + (tile.height - static_cast<int>(h)) / 2, 0, tile.waiting.get(), 0, &box);
    tile.showing_waiting = true;
  }
}  // namespace platf::dxgi
