// Copyright Source Authors. All rights reserved.
//
// CEF implementation of the htmlhost facade. See htmlhost.h.

#include "htmlhost.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_scheme.h"
#include "include/cef_task.h"

namespace htmlhost {
namespace {

std::atomic<bool> g_initialized{false};
IRequestHandler *g_request_handler = nullptr;

std::wstring Utf8ToWide(const char *s) {
  if (!s || !*s) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring out(static_cast<size_t>(std::max(n, 1)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
  out.resize(wcslen(out.c_str()));
  return out;
}

// ---------------------------------------------------------------------------
// Resource handling for https://gameui.local/*
// ---------------------------------------------------------------------------

class GameResourceHandler;

std::mutex g_pending_mutex;
std::unordered_map<RequestId, CefRefPtr<GameResourceHandler>> g_pending;
std::atomic<RequestId> g_next_request_id{1};

class GameResourceHandler : public CefResourceHandler {
 public:
  bool Open(CefRefPtr<CefRequest> request, bool &handle_request,
            CefRefPtr<CefCallback> callback) override {
    handle_request = false;

    const std::string url = request->GetURL().ToString();
    const std::string method = request->GetMethod().ToString();

    std::string path, query;
    CefURLParts parts;
    if (CefParseURL(url, parts)) {
      path = CefString(&parts.path).ToString();
      query = CefString(&parts.query).ToString();
    }
    if (path.empty()) path = "/";

    std::vector<uint8_t> body;
    if (CefRefPtr<CefPostData> post = request->GetPostData()) {
      CefPostData::ElementVector elements;
      post->GetElements(elements);
      for (auto &el : elements) {
        if (el->GetType() != PDE_TYPE_BYTES) continue;
        const size_t n = el->GetBytesCount();
        const size_t at = body.size();
        body.resize(at + n);
        el->GetBytes(n, body.data() + at);
      }
    }

    id_ = g_next_request_id.fetch_add(1);
    callback_ = callback;
    {
      std::lock_guard lock(g_pending_mutex);
      g_pending.emplace(id_, this);
    }

    if (!g_request_handler) {
      Complete(404, "text/plain", "no handler", 10, nullptr);
      return true;
    }

    Request req{};
    req.id = id_;
    req.method = method.c_str();
    req.url = url.c_str();
    req.path = path.c_str();
    req.query = query.c_str();
    req.body = body.empty() ? nullptr : body.data();
    req.body_size = body.size();
    g_request_handler->OnRequest(req);
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t &response_length,
                          CefString &redirect_url) override {
    std::lock_guard lock(mutex_);
    response->SetStatus(status_);
    response->SetStatusText(status_ == 200 ? "OK" : "Error");
    response->SetMimeType(mime_);
    if (mime_.rfind("text/", 0) == 0 || mime_ == "application/json" ||
        mime_ == "application/javascript") {
      response->SetCharset("utf-8");
    }
    CefResponse::HeaderMap headers;
    headers.emplace("Access-Control-Allow-Origin", "*");
    headers.emplace("Cache-Control", "no-store");
    for (auto &kv : extra_headers_) headers.emplace(kv.first, kv.second);
    response->SetHeaderMap(headers);
    response_length = static_cast<int64_t>(data_.size());
  }

  bool Read(void *data_out, int bytes_to_read, int &bytes_read,
            CefRefPtr<CefResourceReadCallback>) override {
    std::lock_guard lock(mutex_);
    bytes_read = 0;
    if (offset_ >= data_.size()) return false;
    const size_t n = std::min(static_cast<size_t>(bytes_to_read), data_.size() - offset_);
    std::memcpy(data_out, data_.data() + offset_, n);
    offset_ += n;
    bytes_read = static_cast<int>(n);
    return true;
  }

  void Cancel() override {
    std::lock_guard lock(g_pending_mutex);
    g_pending.erase(id_);
  }

  void Complete(int status, const char *mime, const void *data, size_t size,
                const char *extra_headers) {
    CefRefPtr<CefCallback> cb;
    {
      std::lock_guard lock(mutex_);
      status_ = status;
      mime_ = mime && *mime ? mime : "application/octet-stream";
      data_.assign(static_cast<const uint8_t *>(data),
                   static_cast<const uint8_t *>(data) + (data ? size : 0));
      offset_ = 0;
      ParseHeaders(extra_headers);
      cb = callback_;
      callback_ = nullptr;
    }
    if (cb) cb->Continue();
  }

 private:
  void ParseHeaders(const char *headers) {
    extra_headers_.clear();
    if (!headers) return;
    std::string all(headers);
    size_t pos = 0;
    while (pos < all.size()) {
      size_t eol = all.find('\n', pos);
      if (eol == std::string::npos) eol = all.size();
      std::string line = all.substr(pos, eol - pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const size_t colon = line.find(':');
      if (colon != std::string::npos) {
        std::string value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        extra_headers_.emplace_back(line.substr(0, colon), value);
      }
      pos = eol + 1;
    }
  }

  RequestId id_ = 0;
  std::mutex mutex_;
  CefRefPtr<CefCallback> callback_;
  int status_ = 500;
  std::string mime_ = "text/plain";
  std::vector<uint8_t> data_;
  size_t offset_ = 0;
  std::vector<std::pair<std::string, std::string>> extra_headers_;

  IMPLEMENT_REFCOUNTING(GameResourceHandler);
};

class GameSchemeFactory : public CefSchemeHandlerFactory {
 public:
  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>,
                                       CefRefPtr<CefFrame>, const CefString &,
                                       CefRefPtr<CefRequest>) override {
    return new GameResourceHandler();
  }
  IMPLEMENT_REFCOUNTING(GameSchemeFactory);
};

// ---------------------------------------------------------------------------
// Browser process app
// ---------------------------------------------------------------------------

class HostApp : public CefApp, public CefBrowserProcessHandler {
 public:
  explicit HostApp(bool disable_gpu) : disable_gpu_(disable_gpu) {}

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnBeforeCommandLineProcessing(
      const CefString &process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    if (!process_type.empty()) return;
    command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
    command_line->AppendSwitch("disable-extensions");
    command_line->AppendSwitch("disable-spell-checking");
    command_line->AppendSwitch("disable-pinch");
    command_line->AppendSwitch("no-first-run");
    command_line->AppendSwitch("no-default-browser-check");
    command_line->AppendSwitch("disable-background-networking");
    command_line->AppendSwitch("disable-component-update");
    command_line->AppendSwitch("disable-domain-reliability");
    command_line->AppendSwitchWithValue("password-store", "basic");
    // Frames are paced by the engine (external begin frames); do not wait
    // for the GPU process vsync. (Do NOT pass disable-frame-rate-limit: it
    // makes Chromium ignore external begin frames and render unthrottled.)
    command_line->AppendSwitch("disable-gpu-vsync");
    if (disable_gpu_) {
      command_line->AppendSwitch("disable-gpu");
      command_line->AppendSwitch("disable-gpu-compositing");
    }
  }

 private:
  bool disable_gpu_;
  IMPLEMENT_REFCOUNTING(HostApp);
};

// ---------------------------------------------------------------------------
// Off-screen view
// ---------------------------------------------------------------------------

class ViewImpl;

class ViewClient : public CefClient,
                   public CefRenderHandler,
                   public CefDisplayHandler,
                   public CefLoadHandler,
                   public CefLifeSpanHandler,
                   public CefRequestHandler,
                   public CefContextMenuHandler,
                   public CefJSDialogHandler {
 public:
  explicit ViewClient(ViewImpl *owner) : owner_(owner) {}

  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
  CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }

  // Render
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect) override;
  bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo &info) override;
  void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override;
  void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect &rect) override;
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
               const RectList &dirty, const void *buffer, int width,
               int height) override;

  // Display
  bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                      cef_cursor_type_t type, const CefCursorInfo &) override;
  bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level,
                        const CefString &message, const CefString &source,
                        int line) override;

  // Load
  void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                 int status) override;
  void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                   ErrorCode code, const CefString &text,
                   const CefString &url) override;

  // Life span
  bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
                     const CefString &, const CefString &,
                     CefLifeSpanHandler::WindowOpenDisposition, bool,
                     const CefPopupFeatures &,
                     CefWindowInfo &, CefRefPtr<CefClient> &,
                     CefBrowserSettings &, CefRefPtr<CefDictionaryValue> &,
                     bool *) override {
    return true;  // never open popups
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // Request
  bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request, bool, bool) override {
    // Keep the main frame on our origin; sub-frames may load anything.
    if (!frame->IsMain()) return false;
    const std::string url = request->GetURL().ToString();
    return !(url.rfind(kOrigin, 0) == 0 || url.rfind("devtools://", 0) == 0 ||
             url.rfind("about:", 0) == 0 || url.rfind("data:", 0) == 0);
  }
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus,
                                 int, const CefString &) override;

  // Context menu: suppress completely.
  void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                           CefRefPtr<CefContextMenuParams>,
                           CefRefPtr<CefMenuModel> model) override {
    model->Clear();
  }

  // JS dialogs: alert/confirm are routed to the console, never shown.
  bool OnJSDialog(CefRefPtr<CefBrowser>, const CefString &,
                  JSDialogType, const CefString &, const CefString &,
                  CefRefPtr<CefJSDialogCallback> callback,
                  bool &suppress_message) override {
    suppress_message = true;
    return false;
  }

  void Detach() { owner_ = nullptr; }

 private:
  ViewImpl *owner_;
  IMPLEMENT_REFCOUNTING(ViewClient);
};

class ViewImpl final : public View {
 public:
  ViewImpl(int width, int height, IViewListener *listener)
      : width_(std::max(width, 1)), height_(std::max(height, 1)), listener_(listener) {}

  ~ViewImpl() override = default;

  void Start(const char *url, int frame_rate) {
    client_ = new ViewClient(this);

    CefWindowInfo window_info;
    window_info.SetAsWindowless(nullptr);
    window_info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    // The engine drives rendering through BeginFrame() once per game frame,
    // so the UI runs at the game's frame rate instead of a fixed timer.
    window_info.external_begin_frame_enabled = true;

    CefBrowserSettings settings;
    settings.windowless_frame_rate = std::clamp(frame_rate, 1, 1000);
    settings.background_color = CefColorSetARGB(0, 0, 0, 0);

    CefBrowserHost::CreateBrowser(window_info, client_, url ? url : kOrigin,
                                  settings, nullptr, nullptr);
  }

  // View
  void Resize(int width, int height) override {
    width = std::max(width, 1);
    height = std::max(height, 1);
    {
      std::lock_guard lock(frame_mutex_);
      if (width == width_ && height == height_) return;
      width_ = width;
      height_ = height;
    }
    if (auto b = GetBrowser()) b->GetHost()->WasResized();
  }

  void SetVisible(bool visible) override {
    if (hidden_ == !visible) return;
    hidden_ = !visible;
    if (auto b = GetBrowser()) b->GetHost()->WasHidden(!visible);
  }

  void SetFocus(bool focus) override {
    if (auto b = GetBrowser()) b->GetHost()->SetFocus(focus);
  }

  void MouseMove(int x, int y, uint32_t modifiers, bool leave) override {
    auto b = GetBrowser();
    if (!b) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    ev.modifiers = modifiers;
    b->GetHost()->SendMouseMoveEvent(ev, leave);
  }

  void MouseButton(int x, int y, htmlhost::MouseButton button, bool up,
                   int click_count, uint32_t modifiers) override {
    auto b = GetBrowser();
    if (!b) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    ev.modifiers = modifiers;
    CefBrowserHost::MouseButtonType type = MBT_LEFT;
    if (button == htmlhost::MouseButton::Middle) type = MBT_MIDDLE;
    if (button == htmlhost::MouseButton::Right) type = MBT_RIGHT;
    b->GetHost()->SendMouseClickEvent(ev, type, up, std::clamp(click_count, 1, 3));
  }

  void MouseWheel(int x, int y, int dx, int dy, uint32_t modifiers) override {
    auto b = GetBrowser();
    if (!b) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    ev.modifiers = modifiers;
    b->GetHost()->SendMouseWheelEvent(ev, dx, dy);
  }

  void Key(KeyType type, int vk, char16_t character, uint32_t modifiers) override {
    auto b = GetBrowser();
    if (!b) return;
    CefKeyEvent ev;
    ev.modifiers = modifiers;
    ev.windows_key_code = vk;
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    switch (type) {
      case KeyType::RawKeyDown:
        ev.type = KEYEVENT_RAWKEYDOWN;
        ev.native_key_code = static_cast<int>((scan << 16) | 1);
        break;
      case KeyType::KeyUp:
        ev.type = KEYEVENT_KEYUP;
        ev.native_key_code = static_cast<int>((scan << 16) | 1 | (1u << 30) | (1u << 31));
        break;
      case KeyType::Char:
        ev.type = KEYEVENT_CHAR;
        ev.windows_key_code = character;
        ev.native_key_code = static_cast<int>((scan << 16) | 1);
        break;
    }
    ev.character = character;
    ev.unmodified_character = character;
    ev.is_system_key = false;
    b->GetHost()->SendKeyEvent(ev);
  }

  void ExecuteScript(const char *script) override {
    if (!script) return;
    auto b = GetBrowser();
    if (!b) {
      std::lock_guard lock(browser_mutex_);
      pending_scripts_.emplace_back(script);
      return;
    }
    b->GetMainFrame()->ExecuteJavaScript(CefString(std::string(script)), "", 0);
  }

  void Navigate(const char *url) override {
    if (auto b = GetBrowser()) b->GetMainFrame()->LoadURL(url ? url : kOrigin);
  }

  void Reload(bool ignore_cache) override {
    if (auto b = GetBrowser()) {
      if (ignore_cache) b->ReloadIgnoreCache();
      else b->Reload();
    }
  }

  void ShowDevTools() override {
    auto b = GetBrowser();
    if (!b) return;
    CefWindowInfo wi;
    wi.SetAsPopup(nullptr, "GameUI DevTools");
    wi.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserSettings bs;
    b->GetHost()->ShowDevTools(wi, nullptr, bs, CefPoint());
  }

  bool AcquireFrame(Frame &frame) override {
    frame_mutex_.lock();
    if (out_.empty() || out_w_ <= 0 || out_h_ <= 0) {
      frame_mutex_.unlock();
      return false;
    }
    frame.bgra = out_.data();
    frame.width = out_w_;
    frame.height = out_h_;
    frame.serial = serial_;
    frame.dirty_x = dirty_.x;
    frame.dirty_y = dirty_.y;
    frame.dirty_w = dirty_.width;
    frame.dirty_h = dirty_.height;
    return true;
  }

  void ReleaseFrame(bool consumed) override {
    if (consumed) dirty_ = CefRect();
    frame_mutex_.unlock();
  }

  void BeginFrame() override {
    if (hidden_) return;
    if (auto b = GetBrowser()) b->GetHost()->SendExternalBeginFrame();
  }

  bool IsHidden() const override { return hidden_; }

  uint64_t PaintCount() const override { return paint_count_; }

  void Destroy() override {
    CefRefPtr<CefBrowser> b = GetBrowser();
    if (b) {
      b->GetHost()->CloseBrowser(true);
      std::unique_lock lock(browser_mutex_);
      closed_cv_.wait_for(lock, std::chrono::seconds(5), [this] { return closed_; });
    }
    if (client_) client_->Detach();
    client_ = nullptr;
    delete this;
  }

  // Called from ViewClient (CEF threads)
  void ViewRect(CefRect &rect) {
    std::lock_guard lock(frame_mutex_);
    rect = CefRect(0, 0, width_, height_);
  }

  void PopupShow(bool show) {
    std::lock_guard lock(frame_mutex_);
    popup_visible_ = show;
    if (!show) {
      // Restore the area the popup covered.
      const CefRect old(popup_rect_.x, popup_rect_.y, popup_w_, popup_h_);
      popup_.clear();
      popup_rect_ = CefRect();
      popup_w_ = popup_h_ = 0;
      Composite(old);
    }
  }

  void PopupSize(const CefRect &rect) {
    std::lock_guard lock(frame_mutex_);
    popup_rect_ = rect;
  }

  void Paint(bool popup, const CefRenderHandler::RectList &dirty,
             const void *buffer, int w, int h) {
    std::lock_guard lock(frame_mutex_);
    const uint8_t *src = static_cast<const uint8_t *>(buffer);
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;

    if (!popup) ++paint_count_;
    if (popup) {
      popup_.assign(src, src + bytes);
      popup_w_ = w;
      popup_h_ = h;
      Composite(CefRect(popup_rect_.x, popup_rect_.y, w, h));
      return;
    }

    if (w != view_w_ || h != view_h_ || view_.size() != bytes) {
      // New size: take everything.
      view_.assign(src, src + bytes);
      view_w_ = w;
      view_h_ = h;
      out_.assign(bytes, 0);
      out_w_ = w;
      out_h_ = h;
      dirty_ = CefRect();
      Composite(CefRect(0, 0, w, h));
      return;
    }

    // Copy and composite only the regions Chromium reports as changed.
    for (const CefRect &r : dirty) {
      const CefRect c = Clip(r, w, h);
      if (c.IsEmpty()) continue;
      for (int y = c.y; y < c.y + c.height; ++y) {
        const size_t off = (static_cast<size_t>(y) * w + c.x) * 4;
        std::memcpy(view_.data() + off, src + off, static_cast<size_t>(c.width) * 4);
      }
      Composite(c);
    }
  }

  void AfterCreated(CefRefPtr<CefBrowser> browser) {
    browser->GetHost()->SetWindowlessFrameRate(1000);
    std::vector<std::string> scripts;
    {
      std::lock_guard lock(browser_mutex_);
      browser_ = browser;
      scripts.swap(pending_scripts_);
    }
    for (auto &s : scripts) browser->GetMainFrame()->ExecuteJavaScript(s, "", 0);
  }

  void BeforeClose() {
    std::lock_guard lock(browser_mutex_);
    browser_ = nullptr;
    closed_ = true;
    closed_cv_.notify_all();
  }

  IViewListener *listener() const { return listener_; }

 private:
  CefRefPtr<CefBrowser> GetBrowser() {
    std::lock_guard lock(browser_mutex_);
    return browser_;
  }

  static CefRect Clip(const CefRect &r, int w, int h) {
    const int x0 = std::max(r.x, 0), y0 = std::max(r.y, 0);
    const int x1 = std::min(r.x + r.width, w), y1 = std::min(r.y + r.height, h);
    if (x1 <= x0 || y1 <= y0) return CefRect();
    return CefRect(x0, y0, x1 - x0, y1 - y0);
  }

  static CefRect Union(const CefRect &a, const CefRect &b) {
    if (a.IsEmpty()) return b;
    if (b.IsEmpty()) return a;
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.width, b.x + b.width);
    const int y1 = std::max(a.y + a.height, b.y + b.height);
    return CefRect(x0, y0, x1 - x0, y1 - y0);
  }

  // Rebuilds |rect| of |out_| = view + popup, converting premultiplied alpha
  // to straight alpha (the engine blends with straight alpha), and adds it to
  // the dirty region. frame_mutex_ must be held.
  void Composite(const CefRect &rect) {
    if (view_.empty() || out_.size() != view_.size()) return;
    const CefRect c = Clip(rect, out_w_, out_h_);
    if (c.IsEmpty()) return;

    for (int y = c.y; y < c.y + c.height; ++y) {
      const size_t off = (static_cast<size_t>(y) * out_w_ + c.x) * 4;
      const uint8_t *src = view_.data() + off;
      uint8_t *dst = out_.data() + off;
      for (int x = 0; x < c.width; ++x, src += 4, dst += 4) {
        const uint8_t a = src[3];
        if (a == 255 || a == 0) {
          std::memcpy(dst, src, 4);
        } else {
          dst[0] = static_cast<uint8_t>(std::min(255, src[0] * 255 / a));
          dst[1] = static_cast<uint8_t>(std::min(255, src[1] * 255 / a));
          dst[2] = static_cast<uint8_t>(std::min(255, src[2] * 255 / a));
          dst[3] = a;
        }
      }
    }

    if (popup_visible_ && !popup_.empty()) {
      const CefRect p = Clip(CefRect(popup_rect_.x, popup_rect_.y, popup_w_, popup_h_), out_w_, out_h_);
      const int x0 = std::max(p.x, c.x), y0 = std::max(p.y, c.y);
      const int x1 = std::min(p.x + p.width, c.x + c.width);
      const int y1 = std::min(p.y + p.height, c.y + c.height);
      for (int y = y0; y < y1; ++y) {
        const uint8_t *ps = popup_.data() +
            (static_cast<size_t>(y - popup_rect_.y) * popup_w_ + (x0 - popup_rect_.x)) * 4;
        uint8_t *pd = out_.data() + (static_cast<size_t>(y) * out_w_ + x0) * 4;
        for (int x = x0; x < x1; ++x, ps += 4, pd += 4) {
          pd[0] = ps[0];
          pd[1] = ps[1];
          pd[2] = ps[2];
          pd[3] = 255;
        }
      }
    }

    dirty_ = Union(dirty_, c);
    ++serial_;
  }

  // Frame data (frame_mutex_)
  std::mutex frame_mutex_;
  int width_, height_;
  std::vector<uint8_t> view_, popup_, out_;
  int view_w_ = 0, view_h_ = 0, popup_w_ = 0, popup_h_ = 0, out_w_ = 0, out_h_ = 0;
  bool popup_visible_ = false;
  CefRect popup_rect_;
  CefRect dirty_;
  uint64_t serial_ = 0;
  std::atomic<bool> hidden_{false};
  std::atomic<uint64_t> paint_count_{0};

  // Browser (browser_mutex_)
  std::mutex browser_mutex_;
  std::condition_variable closed_cv_;
  CefRefPtr<CefBrowser> browser_;
  std::vector<std::string> pending_scripts_;
  bool closed_ = false;

  CefRefPtr<ViewClient> client_;
  IViewListener *listener_;
};

void ViewClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect) {
  if (owner_) owner_->ViewRect(rect);
  else rect = CefRect(0, 0, 1, 1);
}

bool ViewClient::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo &info) {
  CefRect rect;
  GetViewRect(browser, rect);
  info.device_scale_factor = 1.0f;
  info.depth = 32;
  info.depth_per_component = 8;
  info.is_monochrome = false;
  info.rect = rect;
  info.available_rect = rect;
  return true;
}

void ViewClient::OnPopupShow(CefRefPtr<CefBrowser>, bool show) {
  if (owner_) owner_->PopupShow(show);
}

void ViewClient::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect &rect) {
  if (owner_) owner_->PopupSize(rect);
}

void ViewClient::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                         const RectList &dirty, const void *buffer, int width,
                         int height) {
  if (owner_) owner_->Paint(type == PET_POPUP, dirty, buffer, width, height);
}

bool ViewClient::OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                                cef_cursor_type_t type, const CefCursorInfo &) {
  if (!owner_ || !owner_->listener()) return true;
  using C = IViewListener::Cursor;
  C c = C::Arrow;
  switch (type) {
    case CT_HAND: c = C::Hand; break;
    case CT_IBEAM:
    case CT_VERTICALTEXT: c = C::IBeam; break;
    case CT_WAIT:
    case CT_PROGRESS: c = C::Wait; break;
    case CT_CROSS: c = C::Crosshair; break;
    case CT_EASTRESIZE:
    case CT_WESTRESIZE:
    case CT_EASTWESTRESIZE:
    case CT_COLUMNRESIZE: c = C::SizeWE; break;
    case CT_NORTHRESIZE:
    case CT_SOUTHRESIZE:
    case CT_NORTHSOUTHRESIZE:
    case CT_ROWRESIZE: c = C::SizeNS; break;
    case CT_MOVE: c = C::SizeAll; break;
    case CT_NOTALLOWED:
    case CT_NODROP: c = C::No; break;
    case CT_NONE: c = C::None; break;
    default: break;
  }
  owner_->listener()->OnCursorChanged(c);
  return true;
}

bool ViewClient::OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level,
                                  const CefString &message,
                                  const CefString &source, int line) {
  if (owner_ && owner_->listener()) {
    owner_->listener()->OnConsoleMessage(static_cast<int>(level),
                                         message.ToString().c_str(),
                                         source.ToString().c_str(), line);
  }
  return true;
}

void ViewClient::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                           int status) {
  if (frame->IsMain() && owner_ && owner_->listener())
    owner_->listener()->OnLoadEnd(status);
}

void ViewClient::OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                             ErrorCode code, const CefString &text,
                             const CefString &url) {
  if (code == ERR_ABORTED) return;
  if (frame->IsMain() && owner_ && owner_->listener())
    owner_->listener()->OnLoadError(static_cast<int>(code),
                                    text.ToString().c_str(),
                                    url.ToString().c_str());
}

void ViewClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  // DevTools popups also land here; only track the first (main) browser.
  if (owner_ && !browser->IsPopup()) owner_->AfterCreated(browser);
}

void ViewClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  if (owner_ && !browser->IsPopup()) owner_->BeforeClose();
}

void ViewClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser>,
                                           TerminationStatus, int,
                                           const CefString &) {
  if (owner_ && owner_->listener()) owner_->listener()->OnRendererCrashed();
}

HMODULE g_libcef = nullptr;

}  // namespace

bool Init(const InitParams &params, const char **error) {
  auto fail = [error](const char *msg) {
    if (error) *error = msg;
    return false;
  };

  if (g_initialized) return true;
  if (!params.cef_dir || !params.subprocess_path)
    return fail("cef_dir / subprocess_path not set");

  // libcef.dll is delay-loaded by the host module. Load it from the CEF dir
  // explicitly so that its own dependencies (chrome_elf.dll, ...) resolve
  // from the same directory. The delay-load helper then binds to it by name.
  const std::wstring cef_dir = Utf8ToWide(params.cef_dir);
  const std::wstring libcef = cef_dir + L"\\libcef.dll";
  g_libcef = LoadLibraryExW(libcef.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!g_libcef) return fail("Unable to load libcef.dll");

  if (GetFileAttributesW(Utf8ToWide(params.subprocess_path).c_str()) ==
      INVALID_FILE_ATTRIBUTES)
    return fail("CEF subprocess executable is missing");

  g_request_handler = params.request_handler;

  CefMainArgs main_args(GetModuleHandleW(nullptr));

  CefSettings settings;
  settings.no_sandbox = true;
  settings.multi_threaded_message_loop = true;
  settings.windowless_rendering_enabled = true;
  settings.background_color = CefColorSetARGB(0, 0, 0, 0);
  settings.persist_session_cookies = false;
  settings.log_severity = LOGSEVERITY_WARNING;
  settings.remote_debugging_port = params.remote_debugging_port;
  CefString(&settings.browser_subprocess_path).FromString(params.subprocess_path);
  CefString(&settings.resources_dir_path).FromWString(cef_dir);
  CefString(&settings.locales_dir_path).FromWString(cef_dir + L"\\locales");
  if (params.cache_dir) {
    // Persistent profile (localStorage etc.) under the root cache dir.
    CefString(&settings.root_cache_path).FromString(params.cache_dir);
    CefString(&settings.cache_path).FromString(std::string(params.cache_dir) + "/profile");
  }
  if (params.log_file) CefString(&settings.log_file).FromString(params.log_file);
  if (params.locale) CefString(&settings.locale).FromString(params.locale);

  const bool disable_gpu = strstr(GetCommandLineA(), "-htmlui_nogpu") != nullptr;
  CefRefPtr<HostApp> app = new HostApp(disable_gpu);
  if (!CefInitialize(main_args, settings, app, nullptr)) {
    return fail("CefInitialize failed");
  }

  CefRegisterSchemeHandlerFactory("https", "gameui.local", new GameSchemeFactory());
  g_initialized = true;
  return true;
}

void Shutdown() {
  if (!g_initialized) return;
  {
    // Fail all outstanding requests so CEF can tear down.
    std::unordered_map<RequestId, CefRefPtr<GameResourceHandler>> pending;
    {
      std::lock_guard lock(g_pending_mutex);
      pending.swap(g_pending);
    }
    for (auto &kv : pending) kv.second->Complete(503, "text/plain", "", 0, nullptr);
  }
  CefClearSchemeHandlerFactories();
  CefShutdown();
  g_request_handler = nullptr;
  g_initialized = false;
}

bool IsInitialized() { return g_initialized; }

void CompleteRequest(RequestId id, int status, const char *mime_type,
                     const void *data, size_t size, const char *extra_headers) {
  CefRefPtr<GameResourceHandler> handler;
  {
    std::lock_guard lock(g_pending_mutex);
    auto it = g_pending.find(id);
    if (it == g_pending.end()) return;
    handler = it->second;
    g_pending.erase(it);
  }
  handler->Complete(status, mime_type, data, size, extra_headers);
}

View *CreateView(const char *url, int width, int height,
                 IViewListener *listener, int frame_rate) {
  if (!g_initialized) return nullptr;
  auto *view = new ViewImpl(width, height, listener);
  view->Start(url, frame_rate);
  return view;
}

}  // namespace htmlhost
