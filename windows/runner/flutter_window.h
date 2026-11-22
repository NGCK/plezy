#ifndef RUNNER_FLUTTER_WINDOW_H_
#define RUNNER_FLUTTER_WINDOW_H_

#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <memory>

#include "win32_window.h"

class FlutterWindow : public Win32Window {
 public:
  explicit FlutterWindow(const flutter::DartProject& project);
  virtual ~FlutterWindow();

 protected:
  bool OnCreate() override;
  void OnDestroy() override;
  LRESULT MessageHandler(HWND window, UINT const message, WPARAM const wparam, LPARAM const lparam) noexcept override;

 private:
  flutter::DartProject project_;

  std::unique_ptr<flutter::FlutterViewController> flutter_controller_;

  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> window_channel_;

  // Fullscreen state tracking for monitor-aware native fullscreen.
  // Maximize state lives inside `placement_before_fullscreen_.showCmd`.
  bool is_fullscreen_ = false;
  WINDOWPLACEMENT placement_before_fullscreen_{};
  LONG_PTR style_before_fullscreen_ = 0;
  LONG_PTR ex_style_before_fullscreen_ = 0;

  // Desktop picture-in-picture: borderless, always-on-top, aspect-locked and
  // parked in a corner. Mirrors the fullscreen save/restore pattern above —
  // same placement/style bookkeeping, a small target rect instead of the
  // monitor. The aspect comes from the video's own dimensions, passed by the
  // Dart side over the com.plezy/pip channel, and is held through user
  // resizes by ApplyPipAspect. Mutually exclusive with fullscreen.
  bool is_pip_ = false;
  double pip_aspect_ = 16.0 / 9.0;
  WINDOWPLACEMENT placement_before_pip_{};
  LONG_PTR style_before_pip_ = 0;
  LONG_PTR ex_style_before_pip_ = 0;

  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> pip_channel_;

  void RegisterWindowChannel();
  void SetNativeFullScreen(bool fullscreen);
  void NotifyFullScreenChanged();

  void RegisterPipChannel();
  void SetNativePip(bool pip, double aspect);
  void NotifyPipChanged();
  bool ApplyPipAspect(WPARAM edge, RECT* rect);
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
