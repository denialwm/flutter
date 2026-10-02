import 'dart:ui' as ui;

// AOT/native-boundary smoke test: builds/disposes scenes, never presents them.
void main() {
  const bounds = ui.Rect.fromLTRB(0, 0, 100, 80);
  const content = ui.Rect.fromLTRB(2, 2, 98, 78);
  final filters = <ui.ImageFilter?>[
    null,
    ui.ImageFilter.blur(sigmaX: 7, sigmaY: 7),
    ui.ImageFilter.glass(shape: ui.RRect.fromRectXY(content, 8, 8)),
    null,
  ];
  for (final id in <int>[0, -1]) {
    ui.WindowSurfaceEngineLayer? previous;
    for (final (index, filter) in filters.indexed) {
      final builder = ui.SceneBuilder();
      final layer = builder.pushWindowSurface(
        bounds,
        contentBounds: content,
        materialBounds: index.isOdd ? content.deflate(12) : null,
        radius: 10,
        backdrop: filter,
        textureId: id,
        oldLayer: previous,
      );
      builder.pop();
      builder.build().dispose();
      previous?.dispose();
      previous = layer;
    }
    previous?.dispose();
  }
  print('WINDOW_SURFACE_SMOKE_OK');
}
