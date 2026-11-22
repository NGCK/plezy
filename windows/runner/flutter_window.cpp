#include "flutter_window.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "flutter/generated_plugin_registrant.h"
#include "mpv/display_mode_manager.h"
#include "mpv/mpv_plugin.h"

static constexpr wchar_t kWindowPlacementKey[] = L"Software\\Plezy";
static constexpr wchar_t kWindowPlacementValue[] = L"WindowPlacement";

static UINT_PTR g_saveTimerId = 0;
static HWND g_mainHwnd = nullptr;
// When true, WM_WINDOWPOSCHANGED should not persist placement. Used while a
// native fullscreen toggle is in flux so we don't record the fullscreen rect
// as the user's last "normal" placement.
static bool g_suppressPlacementSave = false;

static void SaveWindowPlacement(HWND hwnd);

static void CALLBACK SaveTimerProc(HWND, UINT, UINT_PTR, DWORD) {
  if (g_mainHwnd) SaveWindowPlacement(g_mainHwnd);
  KillTimer(nullptr, g_saveTimerId);
  g_saveTimerId = 0;
}

static void WriteWindowPlacement(const WINDOWPLACEMENT& wp) {
  HKEY hKey;
  if (RegCreateKeyExW(
          HKEY_CURRENT_USER, kWindowPlacementKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey,
          nullptr) == ERROR_SUCCESS) {
    RegSetValueExW(hKey, kWindowPlacementValue, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&wp), sizeof(wp));
    RegCloseKey(hKey);
  }
}

// GetWindowPlacement with Aero Snap folded in. A snapped window is "arranged":
// Windows keeps the pre-snap rect in rcNormalPosition and still reports
// SW_SHOWNORMAL, so persisting the raw placement restores the window where it
// was *before* the snap. Substitute the on-screen rect instead; there is no
// API to re-enter the snapped state, so relaunch lands a normal window on the
// same rect. IsWindowArranged is exported by user32 since Windows 10 1903 but
// has no header/import-lib declaration; older builds fall through unchanged.
static bool QueryWindowPlacement(HWND hwnd, WINDOWPLACEMENT* wp) {
  wp->length = sizeof(*wp);
  if (!GetWindowPlacement(hwnd, wp)) return false;

  using IsWindowArrangedFn = BOOL(WINAPI*)(HWND);
  static const IsWindowArrangedFn is_window_arranged =
      reinterpret_cast<IsWindowArrangedFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "IsWindowArranged"));
  if (!is_window_arranged || IsIconic(hwnd) || IsZoomed(hwnd) || !is_window_arranged(hwnd)) return true;

  // rcNormalPosition uses the window monitor's workspace inset, not the
  // primary work area's screen origin. LoadWindowPlacement reverses this.
  RECT rect{};
  if (!GetWindowRect(hwnd, &rect)) return true;
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return true;
  OffsetRect(&rect, mi.rcMonitor.left - mi.rcWork.left, mi.rcMonitor.top - mi.rcWork.top);
  wp->rcNormalPosition = rect;
  return true;
}

static void SaveWindowPlacement(HWND hwnd) {
  // Never persist a hidden window: the exit path hides the window before its
  // multi-second teardown, and a save landing in that gap would record
  // SW_HIDE over the user's real show state.
  if (!IsWindowVisible(hwnd)) return;
  WINDOWPLACEMENT wp{};
  if (!QueryWindowPlacement(hwnd, &wp)) return;
  WriteWindowPlacement(wp);
}

// Loads the saved WINDOWPLACEMENT and applies it while keeping the window
// hidden; the first-frame callback in OnCreate performs the single show.
// Returns whether the window should be shown maximized.
static bool LoadWindowPlacement(HWND hwnd) {
  HKEY hKey;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kWindowPlacementKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;

  WINDOWPLACEMENT wp{};
  wp.length = sizeof(wp);
  DWORD size = sizeof(wp);
  bool wasMaximized = false;

  if (RegQueryValueExW(hKey, kWindowPlacementValue, nullptr, nullptr, reinterpret_cast<BYTE*>(&wp), &size) ==
          ERROR_SUCCESS &&
      size == sizeof(wp)) {
    // A window minimized away from the maximized state stores SW_SHOWMINIMIZED
    // plus WPF_RESTORETOMAXIMIZED; both spellings mean "maximized" on relaunch.
    wasMaximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED) != 0;

    // The saved monitor may be gone (undocked laptop, powered-off TV).
    // Resolve the saved rect's monitor before reversing its workspace inset.
    // On a miss, keep the size but fall back to the default creation position
    // so the window never restores invisible.
    RECT screenRect = wp.rcNormalPosition;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    bool on_screen = false;
    if (GetMonitorInfoW(MonitorFromRect(&screenRect, MONITOR_DEFAULTTONEAREST), &mi)) {
      OffsetRect(&screenRect, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
      on_screen = MonitorFromRect(&screenRect, MONITOR_DEFAULTTONULL) != nullptr;
    }
    if (!on_screen) {
      // GetWindowPlacement already expresses the creation rect in its own
      // monitor's workspace, including when the default monitor has an inset.
      WINDOWPLACEMENT current{};
      current.length = sizeof(current);
      if (!GetWindowPlacement(hwnd, &current)) {
        RegCloseKey(hKey);
        return false;
      }
      const LONG width = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
      const LONG height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
      wp.rcNormalPosition.left = current.rcNormalPosition.left;
      wp.rcNormalPosition.top = current.rcNormalPosition.top;
      wp.rcNormalPosition.right = wp.rcNormalPosition.left + width;
      wp.rcNormalPosition.bottom = wp.rcNormalPosition.top + height;
    }

    // Apply hidden. A visible showCmd here would display a blank window at
    // the restored spot before Flutter has rendered anything.
    wp.showCmd = SW_HIDE;
    SetWindowPlacement(hwnd, &wp);
  }

  RegCloseKey(hKey);
  return wasMaximized;
}

static void DebounceSaveWindowPlacement(HWND hwnd) {
  g_mainHwnd = hwnd;
  if (g_saveTimerId) KillTimer(nullptr, g_saveTimerId);
  g_saveTimerId = SetTimer(nullptr, 0, 500, SaveTimerProc);
}

FlutterWindow::FlutterWindow(const flutter::DartProject& project) : project_(project) {}

FlutterWindow::~FlutterWindow() {
  // Clear the controller before destroying its child HWND can re-enter the
  // window procedure. Implicit member destruction leaves it visible there.
  Destroy();
}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ =
      std::make_unique<flutter::FlutterViewController>(frame.right - frame.left, frame.bottom - frame.top, project_);
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());

  MpvPlayerPluginRegisterWithRegistrar(flutter_controller_->engine()->GetRegistrarForPlugin("MpvPlayerPlugin"));
  MpvAudioPlayerPluginRegisterWithRegistrar(
      flutter_controller_->engine()->GetRegistrarForPlugin("MpvAudioPlayerPlugin"));

  RegisterWindowChannel();
  RegisterPipChannel();

  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  HWND hwnd = GetHandle();
  bool maximized = LoadWindowPlacement(hwnd);

  flutter_controller_->engine()->SetNextFrameCallback(
      [this, maximized]() { ::ShowWindow(this->GetHandle(), maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL); });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  // Cancel any pending save timer and save immediately
  if (g_saveTimerId) {
    KillTimer(nullptr, g_saveTimerId);
    g_saveTimerId = 0;
  }
  g_mainHwnd = nullptr;
  // If still fullscreen at shutdown, persist the pre-fullscreen placement
  // rather than the fullscreen rect so the next launch restores correctly.
  if (is_fullscreen_ && placement_before_fullscreen_.length != 0) {
    WriteWindowPlacement(placement_before_fullscreen_);
  } else if (is_pip_ && placement_before_pip_.length != 0) {
    // Same reasoning for PiP: quitting from the small corner tile must not
    // make the next launch open as a tiny borderless window.
    WriteWindowPlacement(placement_before_pip_);
  } else {
    SaveWindowPlacement(GetHandle());
  }

  window_channel_ = nullptr;
  pip_channel_ = nullptr;

  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result = flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam, lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_DISPLAYCHANGE:
      // One bounded, serialized retry for a display that may have reconnected.
      mpv::DisplayModeManager::RecoverIfNeeded();
      break;
    case WM_FONTCHANGE:
      if (flutter_controller_) {
        flutter_controller_->engine()->ReloadSystemFonts();
      }
      break;
    case WM_SIZING:
      // While in PiP, hold the window to the video's aspect ratio as the user
      // drags an edge. Returning TRUE means the adjusted rect is authoritative.
      if (ApplyPipAspect(wparam, reinterpret_cast<RECT*>(lparam))) return TRUE;
      break;
    case WM_NCCALCSIZE:
      // The PiP tile keeps WS_THICKFRAME so Windows still runs its sizing loop
      // (and WM_SIZING still holds the aspect ratio), but with no caption that
      // frame paints as a grey strip across the top of the video. Claiming the
      // whole window rect as client area removes it.
      //
      // The edges stop being grabbable as a result — the Flutter view is a
      // child window covering the entire client area, so the cursor never
      // reaches this window's own frame and WM_NCHITTEST here would never see
      // it. PipWindowOverlay draws the grab zones instead and calls
      // window_manager's startResizing, which re-enters the same sizing loop.
      if (is_pip_ && wparam == TRUE) return 0;
      break;
    case WM_WINDOWPOSCHANGED:
      // Don't persist placement while fullscreen, mid-toggle, or hidden — the
      // rect would overwrite the user's real window position or show state.
      if (flutter_controller_ && !is_fullscreen_ && !g_suppressPlacementSave && IsWindowVisible(hwnd)) {
        DebounceSaveWindowPlacement(hwnd);
      }
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Native fullscreen (plezy/window method channel)
//
// Works around window_manager 0.5.1's multi-monitor fullscreen bug where the
// window ends up on the wrong monitor and renders only a fragment of the
// Flutter surface. We:
//   1) Resolve the target monitor by the current window's center point
//      (robust across DPI and restore transitions).
//   2) Save WINDOWPLACEMENT + styles once.
//   3) Strip WS_OVERLAPPEDWINDOW and move+size in a single SetWindowPos so
//      Flutter only relayouts once, at the final DPI/size.
//   4) On exit, restore styles and WINDOWPLACEMENT, re-maximizing if needed.
//
// The channel also carries setDarkMode, the app's frame theme: window_manager's
// setBrightness cannot make the caption darker than the OS app theme.
// ---------------------------------------------------------------------------
void FlutterWindow::RegisterWindowChannel() {
  auto messenger = flutter_controller_->engine()->messenger();
  window_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      messenger, "plezy/window", &flutter::StandardMethodCodec::GetInstance());

  window_channel_->SetMethodCallHandler([this](
                                            const flutter::MethodCall<flutter::EncodableValue>& call,
                                            std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    const std::string& name = call.method_name();
    if (name == "setFullScreen") {
      bool value = false;
      if (const auto* args = std::get_if<flutter::EncodableMap>(call.arguments())) {
        auto it = args->find(flutter::EncodableValue("isFullScreen"));
        if (it != args->end()) {
          if (const bool* b = std::get_if<bool>(&it->second)) value = *b;
        }
      }
      SetNativeFullScreen(value);
      result->Success();
    } else if (name == "isFullScreen") {
      result->Success(flutter::EncodableValue(is_fullscreen_));
    } else if (name == "setDarkMode") {
      const bool* dark = std::get_if<bool>(call.arguments());
      if (dark == nullptr) {
        result->Error("invalid_arguments", "setDarkMode expects a bool");
        return;
      }
      SetDarkMode(*dark);
      result->Success();
    } else {
      result->NotImplemented();
    }
  });
}

void FlutterWindow::NotifyFullScreenChanged() {
  if (!window_channel_) return;
  window_channel_->InvokeMethod("onFullScreenChanged", std::make_unique<flutter::EncodableValue>(is_fullscreen_));
}

void FlutterWindow::SetNativeFullScreen(bool fullscreen) {
  HWND hwnd = GetHandle();
  if (!hwnd) return;
  if (fullscreen == is_fullscreen_) return;

  g_suppressPlacementSave = true;
  // Also cancel any pending save so a WM_WINDOWPOSCHANGED just before the
  // toggle doesn't fire SaveTimerProc mid-transition.
  if (g_saveTimerId) {
    KillTimer(nullptr, g_saveTimerId);
    g_saveTimerId = 0;
  }

  if (fullscreen) {
    // Resolve target monitor first — if this fails we bail before mutating
    // any saved state, so a subsequent exit call has nothing stale to act on.
    RECT wr{};
    ::GetWindowRect(hwnd, &wr);
    // Center point is stable across maximize/straddle cases where
    // MonitorFromWindow would resolve to the neighbor monitor.
    POINT center{(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(::MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &mi)) {
      g_suppressPlacementSave = false;
      return;
    }

    // Save pre-fullscreen state (showCmd inside the placement carries the
    // maximize bit, so no separate flag is needed).
    QueryWindowPlacement(hwnd, &placement_before_fullscreen_);
    style_before_fullscreen_ = ::GetWindowLongPtr(hwnd, GWL_STYLE);
    ex_style_before_fullscreen_ = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    // Strip frame/caption. Stripping WS_OVERLAPPEDWINDOW alone is enough to
    // make the following SetWindowPos use the given rect exactly — no need
    // to ShowWindow(SW_SHOWNORMAL) first (would cause a second relayout).
    ::SetWindowLongPtr(hwnd, GWL_STYLE, style_before_fullscreen_ & ~WS_OVERLAPPEDWINDOW);
    ::SetWindowLongPtr(
        hwnd, GWL_EXSTYLE,
        ex_style_before_fullscreen_ & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));

    const RECT& r = mi.rcMonitor;
    ::SetWindowPos(
        hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
        SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);

    is_fullscreen_ = true;
  } else {
    if (style_before_fullscreen_ != 0) {
      ::SetWindowLongPtr(hwnd, GWL_STYLE, style_before_fullscreen_);
      ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex_style_before_fullscreen_);
    }

    if (placement_before_fullscreen_.length == sizeof(WINDOWPLACEMENT)) {
      WINDOWPLACEMENT wp = placement_before_fullscreen_;
      if (wp.showCmd == SW_SHOWMINIMIZED) wp.showCmd = SW_SHOWNORMAL;
      ::SetWindowPlacement(hwnd, &wp);
    }

    // Force a frame refresh so restored chrome paints.
    ::SetWindowPos(
        hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    is_fullscreen_ = false;
    placement_before_fullscreen_ = {};
    style_before_fullscreen_ = 0;
    ex_style_before_fullscreen_ = 0;
  }

  g_suppressPlacementSave = false;
  NotifyFullScreenChanged();
}

// ---------------------------------------------------------------------------
// Desktop picture-in-picture (com.plezy/pip method channel)
//
// Windows has no OS-level PiP for a Win32 window (CompactOverlay is UWP only),
// so this builds it from the same pieces as native fullscreen: save the
// placement and styles, strip the caption, move to a small rect, restore on
// exit. Differences from fullscreen: WS_THICKFRAME stays so the tile can be
// resized, the window goes topmost, the rect is a corner of the *work* area
// (clear of the taskbar), and WM_SIZING holds the video's aspect ratio.
//
// The channel name matches what lib/services/pip_service.dart already uses for
// Android, iOS and macOS, so the existing player button, PiP video filters and
// isPipActive listeners all drive this unchanged.
// ---------------------------------------------------------------------------
void FlutterWindow::RegisterPipChannel() {
  auto messenger = flutter_controller_->engine()->messenger();
  pip_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      messenger, "com.plezy/pip", &flutter::StandardMethodCodec::GetInstance());

  pip_channel_->SetMethodCallHandler([this](
                                         const flutter::MethodCall<flutter::EncodableValue>& call,
                                         std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    const std::string& name = call.method_name();
    if (name == "isSupported") {
      result->Success(flutter::EncodableValue(true));
    } else if (name == "enter") {
      // Dart sends the video's pixel dimensions; a missing or degenerate pair
      // leaves the previous (or default 16:9) ratio in place.
      double aspect = 0.0;
      if (const auto* args = std::get_if<flutter::EncodableMap>(call.arguments())) {
        auto number = [args](const char* key) -> double {
          auto it = args->find(flutter::EncodableValue(key));
          if (it == args->end()) return 0.0;
          if (const int32_t* i = std::get_if<int32_t>(&it->second)) return static_cast<double>(*i);
          if (const int64_t* l = std::get_if<int64_t>(&it->second)) return static_cast<double>(*l);
          if (const double* d = std::get_if<double>(&it->second)) return *d;
          return 0.0;
        };
        const double width = number("width");
        const double height = number("height");
        if (width > 0.0 && height > 0.0) aspect = width / height;
      }
      SetNativePip(true, aspect);
      flutter::EncodableMap response{{flutter::EncodableValue("success"), flutter::EncodableValue(is_pip_)}};
      if (!is_pip_) {
        response[flutter::EncodableValue("errorCode")] = flutter::EncodableValue("failed");
      }
      result->Success(flutter::EncodableValue(response));
    } else if (name == "exit") {
      SetNativePip(false, 0.0);
      result->Success();
    } else if (name == "setAutoPipReady") {
      // No OS-driven auto-PiP on Windows (nothing enters PiP on minimize), so
      // there is nothing to arm — but answer, or the Dart call throws.
      result->Success();
    } else {
      result->NotImplemented();
    }
  });
}

void FlutterWindow::NotifyPipChanged() {
  if (!pip_channel_) return;
  pip_channel_->InvokeMethod("onPipChanged", std::make_unique<flutter::EncodableValue>(is_pip_));
}

bool FlutterWindow::ApplyPipAspect(WPARAM edge, RECT* rect) {
  if (!is_pip_ || pip_aspect_ <= 0.0 || rect == nullptr) return false;
  HWND hwnd = GetHandle();
  if (!hwnd) return false;

  // Measure the frame from the live window rather than AdjustWindowRectEx:
  // exact at any DPI, and the padding is constant through a resize drag.
  RECT window_rect{};
  RECT client_rect{};
  if (!::GetWindowRect(hwnd, &window_rect) || !::GetClientRect(hwnd, &client_rect)) return false;
  const LONG pad_w = (window_rect.right - window_rect.left) - (client_rect.right - client_rect.left);
  const LONG pad_h = (window_rect.bottom - window_rect.top) - (client_rect.bottom - client_rect.top);

  const LONG client_w = (rect->right - rect->left) - pad_w;
  const LONG client_h = (rect->bottom - rect->top) - pad_h;
  if (client_w <= 0 || client_h <= 0) return false;

  switch (edge) {
    case WMSZ_LEFT:
    case WMSZ_RIGHT:
      // Horizontal drag drives the height.
      rect->bottom = rect->top + static_cast<LONG>(std::llround(client_w / pip_aspect_)) + pad_h;
      return true;
    case WMSZ_TOP:
    case WMSZ_BOTTOM:
      // Vertical drag drives the width.
      rect->right = rect->left + static_cast<LONG>(std::llround(client_h * pip_aspect_)) + pad_w;
      return true;
    case WMSZ_TOPLEFT:
    case WMSZ_TOPRIGHT:
      // Corner drags follow the width and grow away from the anchored edge.
      rect->top = rect->bottom - static_cast<LONG>(std::llround(client_w / pip_aspect_)) - pad_h;
      return true;
    case WMSZ_BOTTOMLEFT:
    case WMSZ_BOTTOMRIGHT:
      rect->bottom = rect->top + static_cast<LONG>(std::llround(client_w / pip_aspect_)) + pad_h;
      return true;
    default:
      return false;
  }
}

void FlutterWindow::SetNativePip(bool pip, double aspect) {
  HWND hwnd = GetHandle();
  if (!hwnd) return;

  if (aspect > 0.0) pip_aspect_ = aspect;

  if (pip == is_pip_) {
    // Already in PiP and the video changed shape (next episode, a different
    // file): reshape around the current top-left instead of re-parking.
    if (pip && aspect > 0.0) {
      RECT window_rect{};
      RECT client_rect{};
      if (::GetWindowRect(hwnd, &window_rect) && ::GetClientRect(hwnd, &client_rect)) {
        const LONG pad_h = (window_rect.bottom - window_rect.top) - (client_rect.bottom - client_rect.top);
        const LONG width = window_rect.right - window_rect.left;
        const LONG client_w = client_rect.right - client_rect.left;
        const LONG height = static_cast<LONG>(std::llround(client_w / pip_aspect_)) + pad_h;
        ::SetWindowPos(hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
    return;
  }

  // Fullscreen and PiP are mutually exclusive. Leaving fullscreen first also
  // puts the window back on its pre-fullscreen placement, which is the one
  // PiP should save and return to.
  if (pip && is_fullscreen_) SetNativeFullScreen(false);

  g_suppressPlacementSave = true;
  if (g_saveTimerId) {
    KillTimer(nullptr, g_saveTimerId);
    g_saveTimerId = 0;
  }

  if (pip) {
    // Resolve the target monitor before mutating saved state, so a failure
    // leaves nothing stale for a later exit call to act on.
    RECT wr{};
    ::GetWindowRect(hwnd, &wr);
    POINT center{(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(::MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &mi)) {
      g_suppressPlacementSave = false;
      return;
    }

    // placement_before_pip_ is what OnDestroy persists; rect/maximize are what
    // the exit path below restores.
    QueryWindowPlacement(hwnd, &placement_before_pip_);
    ::GetWindowRect(hwnd, &rect_before_pip_);
    was_maximized_before_pip_ = ::IsZoomed(hwnd) != FALSE;
    style_before_pip_ = ::GetWindowLongPtr(hwnd, GWL_STYLE);
    ex_style_before_pip_ = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    // Drop out of the maximized state before sizing. A maximized window that
    // is merely moved and sized stays *logically* maximized, and then nothing
    // on the way out can restore it: SetWindowPlacement(SW_SHOWMAXIMIZED) and
    // ShowWindow(SW_SHOWMAXIMIZED) are both no-ops for a window Windows
    // already considers maximized, so the PiP rect would simply stay.
    if (was_maximized_before_pip_) ::ShowWindow(hwnd, SW_RESTORE);

    // Keep WS_THICKFRAME so the tile stays resizable from its edges; drop the
    // caption and its buttons, which is what makes it read as PiP. WS_CAPTION
    // carries WS_BORDER, so this removes the title bar entirely.
    const LONG_PTR style =
        (style_before_pip_ & ~(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_THICKFRAME;
    const LONG_PTR ex_style =
        ex_style_before_pip_ & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    ::SetWindowLongPtr(hwnd, GWL_STYLE, style);
    ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex_style);

    // A quarter of the work area's width, clamped to something watchable, and
    // never taller than half the screen for very tall aspect ratios.
    const LONG work_w = mi.rcWork.right - mi.rcWork.left;
    const LONG work_h = mi.rcWork.bottom - mi.rcWork.top;
    const UINT dpi = ::GetDpiForWindow(hwnd);
    const double scale = dpi > 0 ? dpi / 96.0 : 1.0;
    const auto scaled = [scale](double logical) { return static_cast<LONG>(std::llround(logical * scale)); };

    LONG client_w = std::clamp<LONG>(work_w / 4, scaled(320), scaled(854));
    LONG client_h = static_cast<LONG>(std::llround(client_w / pip_aspect_));
    if (client_h > work_h / 2) {
      client_h = work_h / 2;
      client_w = static_cast<LONG>(std::llround(client_h * pip_aspect_));
    }

    // Size the client area, not the window: the thick frame would otherwise
    // eat into the video and break the ratio before the first resize.
    RECT target{0, 0, client_w, client_h};
    ::AdjustWindowRectEx(&target, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(ex_style));
    const LONG width = target.right - target.left;
    const LONG height = target.bottom - target.top;

    // Set before the move: SWP_FRAMECHANGED is what makes Windows ask for the
    // frame again (WM_NCCALCSIZE), and that handler keys off is_pip_.
    is_pip_ = true;

    const LONG margin = scaled(24);
    ::SetWindowPos(
        hwnd, HWND_TOPMOST, mi.rcWork.right - width - margin, mi.rcWork.bottom - height - margin, width, height,
        SWP_FRAMECHANGED | SWP_NOACTIVATE);
  } else {
    // Clear first, so the frame recalculation below rebuilds a real frame.
    is_pip_ = false;

    if (style_before_pip_ != 0) {
      // Mask out the maximize/minimize *state* bits. They were captured while
      // the window was maximized, and putting WS_MAXIMIZE back on a window
      // that is currently small makes Windows believe it is already maximized
      // — ShowWindow(SW_SHOWMAXIMIZED) then does nothing and the tile-sized
      // window stays. The state is restored below instead.
      ::SetWindowLongPtr(hwnd, GWL_STYLE, style_before_pip_ & ~(WS_MAXIMIZE | WS_MINIMIZE));
      ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex_style_before_pip_);
    }

    // Drop topmost before resizing, or the window flashes above everything
    // else at its PiP size for a frame.
    ::SetWindowPos(
        hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Restore the rect directly instead of through SetWindowPlacement: the
    // saved placement is in registry shape (see rect_before_pip_), and a plain
    // SetWindowPos is unambiguous for a window that is on screen right now.
    if (was_maximized_before_pip_) {
      ::ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    } else if (rect_before_pip_.right > rect_before_pip_.left &&
               rect_before_pip_.bottom > rect_before_pip_.top) {
      ::SetWindowPos(
          hwnd, nullptr, rect_before_pip_.left, rect_before_pip_.top,
          rect_before_pip_.right - rect_before_pip_.left, rect_before_pip_.bottom - rect_before_pip_.top,
          SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    placement_before_pip_ = {};
    rect_before_pip_ = {};
    was_maximized_before_pip_ = false;
    style_before_pip_ = 0;
    ex_style_before_pip_ = 0;
  }

  g_suppressPlacementSave = false;
  NotifyPipChanged();
}
