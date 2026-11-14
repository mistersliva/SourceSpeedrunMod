// Copyright Source Authors. All rights reserved.
//
// htmlhost: a thin, CEF-free C++ facade over Chromium Embedded Framework used
// by the HTML GameUI. It is compiled separately (C++20, CMake) so that the
// engine side (C++17, VPC) never sees CEF headers.
//
// Threading model:
//   * Init/Shutdown/CreateView/View::Destroy must be called on the game main
//     thread.
//   * CEF runs its own UI/IO threads (multi-threaded message loop). Paint data
//     is copied into a frame buffer guarded by a lock; the engine pulls it with
//     View::AcquireFrame()/ReleaseFrame().
//   * Requests to https://gameui.local/* are delivered to IRequestHandler on a
//     CEF thread. The handler may answer immediately or later (from any thread)
//     via CompleteRequest().

#ifndef SE_GAMEUI_CEFBRIDGE_HTMLHOST_H_
#define SE_GAMEUI_CEFBRIDGE_HTMLHOST_H_

#include <cstddef>
#include <cstdint>

namespace htmlhost {

// Origin served by the host. Everything under it is routed to IRequestHandler.
constexpr inline char kOrigin[]{"https://gameui.local"};

using RequestId = uint64_t;

struct Request {
  RequestId id;
  const char *method;  // "GET", "POST", ...
  const char *url;     // full URL, e.g. https://gameui.local/api/cvar.get
  const char *path;    // path part only, e.g. /api/cvar.get (no query)
  const char *query;   // query string without '?', may be empty
  const void *body;    // POST body, may be nullptr
  size_t body_size;
};

// Implemented by the engine side.
class IRequestHandler {
 public:
  // Called on a CEF thread. Must eventually result in exactly one
  // CompleteRequest() call for |req.id| (possibly from another thread).
  virtual void OnRequest(const Request &req) = 0;

 protected:
  ~IRequestHandler() = default;
};

// Implemented by the engine side, all calls happen on CEF threads.
class IViewListener {
 public:
  enum class Cursor { Arrow, Hand, IBeam, Wait, Crosshair, SizeWE, SizeNS, SizeAll, No, None };

  virtual void OnCursorChanged(Cursor cursor) = 0;
  virtual void OnConsoleMessage(int level, const char *message,
                                const char *source, int line) = 0;
  virtual void OnLoadEnd(int http_status) = 0;
  virtual void OnLoadError(int error_code, const char *error_text,
                           const char *url) = 0;
  virtual void OnRendererCrashed() = 0;

 protected:
  ~IViewListener() = default;
};

struct InitParams {
  const char *cef_dir;          // dir with libcef.dll, *.pak, icudtl.dat, locales/
  const char *subprocess_path;  // full path to gameui_cefsub.exe
  const char *cache_dir;        // root cache dir (must be writable)
  const char *log_file;         // may be nullptr
  const char *locale;           // e.g. "en-US", may be nullptr
  int remote_debugging_port;    // 0 disables DevTools remote debugging
  IRequestHandler *request_handler;
};

// Loads libcef.dll from |cef_dir| and initializes CEF. Returns false on
// failure, |error| (if not null) receives a static description.
bool Init(const InitParams &params, const char **error);
void Shutdown();
bool IsInitialized();

// Answers a request issued through IRequestHandler::OnRequest.
// Thread-safe. |data| is copied.
void CompleteRequest(RequestId id, int status, const char *mime_type,
                     const void *data, size_t size,
                     const char *extra_headers = nullptr);

enum Modifiers : uint32_t {
  kModShift = 1u << 1,
  kModCtrl = 1u << 2,
  kModAlt = 1u << 3,
  kModLeftButton = 1u << 4,
  kModMiddleButton = 1u << 5,
  kModRightButton = 1u << 6,
};

enum class MouseButton { Left, Middle, Right };
enum class KeyType { RawKeyDown, KeyUp, Char };

struct Frame {
  const uint8_t *bgra;  // top-down BGRA, premultiplied alpha
  int width;
  int height;
  uint64_t serial;  // increases whenever pixels change
  // Region changed since the last ReleaseFrame(true). Empty (w == 0) when
  // nothing changed; the whole frame after a resize.
  int dirty_x, dirty_y, dirty_w, dirty_h;
};

class View {
 public:
  virtual ~View() = default;

  virtual void Resize(int width, int height) = 0;
  virtual void SetVisible(bool visible) = 0;
  virtual void SetFocus(bool focus) = 0;

  virtual void MouseMove(int x, int y, uint32_t modifiers, bool leave) = 0;
  virtual void MouseButton(int x, int y, htmlhost::MouseButton button,
                           bool up, int click_count, uint32_t modifiers) = 0;
  virtual void MouseWheel(int x, int y, int dx, int dy, uint32_t modifiers) = 0;
  // |windows_vk| is a Win32 virtual key code; |character| a UTF-16 code unit
  // (only for KeyType::Char).
  virtual void Key(KeyType type, int windows_vk, char16_t character,
                   uint32_t modifiers) = 0;

  // Executes |utf8_script| in the main frame. Thread-safe.
  virtual void ExecuteScript(const char *utf8_script) = 0;
  virtual void Navigate(const char *url) = 0;
  virtual void Reload(bool ignore_cache) = 0;
  virtual void ShowDevTools() = 0;

  // Frame access. AcquireFrame returns false when nothing was painted yet.
  // Every successful AcquireFrame must be paired with ReleaseFrame; pass
  // |consumed| = true after uploading the dirty region to reset it.
  virtual bool AcquireFrame(Frame &frame) = 0;
  virtual void ReleaseFrame(bool consumed = false) = 0;

  // Asks Chromium to produce a frame (views are created with external
  // begin-frame scheduling, so the engine drives the frame rate). Thread-safe.
  virtual void BeginFrame() = 0;
  // Whether the page is currently allowed to render (hidden views skip work).
  virtual bool IsHidden() const = 0;
  // Number of frames Chromium painted so far (diagnostics).
  virtual uint64_t PaintCount() const = 0;

  // Closes the browser synchronously (waits up to a few seconds).
  virtual void Destroy() = 0;
};

// Creates an off-screen, transparent browser view.
View *CreateView(const char *url, int width, int height,
                 IViewListener *listener, int frame_rate);

}  // namespace htmlhost

#endif  // SE_GAMEUI_CEFBRIDGE_HTMLHOST_H_
