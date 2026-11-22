import 'package:flutter/material.dart';
import 'package:material_symbols_icons/symbols.dart';
import 'package:window_manager/window_manager.dart';

import '../../../i18n/strings.g.dart';
import '../../../services/pip_service.dart';

/// The only chrome drawn over the video while the Windows PiP tile is active.
///
/// Every other platform renders nothing in PiP: Android and iOS get window
/// chrome from the OS, and macOS leaves the main window on screen. The Win32
/// PiP window has no caption — that is what makes it read as a tile — so
/// without this there would be no way to move it or to get back out. Keep it
/// to that: drag anywhere to move, and a close affordance on hover.
///
/// Resizing needs nothing here; the window keeps WS_THICKFRAME and the runner
/// holds the video's aspect ratio through the drag.
class PipWindowOverlay extends StatefulWidget {
  const PipWindowOverlay({super.key});

  @override
  State<PipWindowOverlay> createState() => _PipWindowOverlayState();
}

class _PipWindowOverlayState extends State<PipWindowOverlay> {
  bool _hovered = false;

  void _setHovered(bool value) {
    if (_hovered == value || !mounted) return;
    setState(() => _hovered = value);
  }

  @override
  Widget build(BuildContext context) {
    return MouseRegion(
      onEnter: (_) => _setHovered(true),
      onExit: (_) => _setHovered(false),
      child: GestureDetector(
        behavior: HitTestBehavior.translucent,
        // startDragging hands the move to the OS, so the tile follows the
        // cursor at the compositor's pace instead of one Flutter frame behind.
        onPanStart: (_) => windowManager.startDragging(),
        onDoubleTap: PipService.exit,
        child: Stack(
          children: [
            Positioned(
              top: 6,
              right: 6,
              child: AnimatedOpacity(
                opacity: _hovered ? 1 : 0,
                duration: const Duration(milliseconds: 120),
                child: IgnorePointer(
                  ignoring: !_hovered,
                  child: Material(
                    color: Colors.black54,
                    shape: const CircleBorder(),
                    clipBehavior: Clip.antiAlias,
                    child: IconButton(
                      iconSize: 18,
                      visualDensity: VisualDensity.compact,
                      tooltip: t.videoControls.pipButton,
                      icon: const Icon(Symbols.close_fullscreen, color: Colors.white),
                      onPressed: PipService.exit,
                    ),
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
