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
/// without this there would be no way to move it, resize it, or get back out.
///
/// The resize grips are here rather than in the runner because the Flutter
/// view is a child window covering the whole client area: the cursor never
/// reaches the top-level window's frame, so a native WM_NCHITTEST never sees
/// the edges. startResizing hands the drag to the OS, which runs its normal
/// sizing loop, so the runner's WM_SIZING still holds the aspect ratio.
class PipWindowOverlay extends StatefulWidget {
  const PipWindowOverlay({super.key});

  @override
  State<PipWindowOverlay> createState() => _PipWindowOverlayState();
}

class _PipWindowOverlayState extends State<PipWindowOverlay> {
  /// Grab band along each edge, and the square at each corner. Deliberately
  /// larger at the corners so a diagonal drag is easy to hit on a small tile.
  static const double _edgeBand = 6;
  static const double _cornerBand = 14;

  bool _hovered = false;

  void _setHovered(bool value) {
    if (_hovered == value || !mounted) return;
    setState(() => _hovered = value);
  }

  /// One resize zone. Pointer-down starts the OS resize immediately rather
  /// than waiting for a pan to be recognised, which is what makes the edge
  /// feel like a real window border.
  Widget _grip({
    required ResizeEdge edge,
    required MouseCursor cursor,
    double? left,
    double? top,
    double? right,
    double? bottom,
    double? width,
    double? height,
  }) {
    return Positioned(
      left: left,
      top: top,
      right: right,
      bottom: bottom,
      width: width,
      height: height,
      child: MouseRegion(
        cursor: cursor,
        child: Listener(
          behavior: HitTestBehavior.opaque,
          onPointerDown: (_) => windowManager.startResizing(edge),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return MouseRegion(
      onEnter: (_) => _setHovered(true),
      onExit: (_) => _setHovered(false),
      child: GestureDetector(
        behavior: HitTestBehavior.translucent,
        // Pan rather than pointer-down, so a plain click still reaches the
        // double-tap below instead of being swallowed by a move.
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
            // Last in the stack, so the corners stay grabbable even where the
            // close button overlaps them.
            _grip(
              edge: ResizeEdge.top,
              cursor: SystemMouseCursors.resizeUpDown,
              left: _cornerBand,
              right: _cornerBand,
              top: 0,
              height: _edgeBand,
            ),
            _grip(
              edge: ResizeEdge.bottom,
              cursor: SystemMouseCursors.resizeUpDown,
              left: _cornerBand,
              right: _cornerBand,
              bottom: 0,
              height: _edgeBand,
            ),
            _grip(
              edge: ResizeEdge.left,
              cursor: SystemMouseCursors.resizeLeftRight,
              top: _cornerBand,
              bottom: _cornerBand,
              left: 0,
              width: _edgeBand,
            ),
            _grip(
              edge: ResizeEdge.right,
              cursor: SystemMouseCursors.resizeLeftRight,
              top: _cornerBand,
              bottom: _cornerBand,
              right: 0,
              width: _edgeBand,
            ),
            _grip(
              edge: ResizeEdge.topLeft,
              cursor: SystemMouseCursors.resizeUpLeftDownRight,
              left: 0,
              top: 0,
              width: _cornerBand,
              height: _cornerBand,
            ),
            _grip(
              edge: ResizeEdge.topRight,
              cursor: SystemMouseCursors.resizeUpRightDownLeft,
              right: 0,
              top: 0,
              width: _cornerBand,
              height: _cornerBand,
            ),
            _grip(
              edge: ResizeEdge.bottomLeft,
              cursor: SystemMouseCursors.resizeUpRightDownLeft,
              left: 0,
              bottom: 0,
              width: _cornerBand,
              height: _cornerBand,
            ),
            _grip(
              edge: ResizeEdge.bottomRight,
              cursor: SystemMouseCursors.resizeUpLeftDownRight,
              right: 0,
              bottom: 0,
              width: _cornerBand,
              height: _cornerBand,
            ),
          ],
        ),
      ),
    );
  }
}
