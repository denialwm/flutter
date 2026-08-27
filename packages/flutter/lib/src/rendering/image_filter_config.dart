// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// @docImport 'package:flutter/widgets.dart';
library;

import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';

/// Context information provided when resolving an [ImageFilterConfig].
///
///  See also:
///  * [ImageFilterConfig.resolve], which takes an instance of this class to
///    create a [ui.ImageFilter].
@immutable
class ImageFilterContext {
  /// Creates an [ImageFilterContext].
  const ImageFilterContext({required this.bounds});

  /// The bounds to apply the filter in the local coordinate space.
  ///
  /// Specified in the canvas's current coordinate space and affected by the
  /// current transform. The bounds may not be axis-aligned in final canvas
  /// coordinates.
  ///
  /// Typically the bounds of the widget or render object applying the filter.
  final ui.Rect bounds;
}

/// A description of an image filter that can adapt to the layout of its target.
///
/// Use [ImageFilterConfig] to define visual effects that depend on the size,
/// position, or other layout attributes of a widget or render object.
///
/// Unlike [ui.ImageFilter], which is an engine-level object with static
/// parameters, [ImageFilterConfig] acts as a framework-level blueprint. It
/// delays the creation of the actual filter until the painting phase, where
/// it is resolved into a [ui.ImageFilter] using an [ImageFilterContext].
///
/// This resolution process allows filters to use layout information as
/// parameters. For example, a filter can use the [Rect] bounds provided by
/// the context to restrict its sampling area to the object's boundaries.
///
/// Most layout-independent filters can be wrapped using the default
/// constructor. For effects that require layout information, such as a
/// "bounded" blur, use the specialized constructors.
///
/// See also:
///
///  * [ImageFilterContext], which provides the layout information used to
///    resolve this configuration.
///  * [ImageFilterConfig.blur], which can be used to create a "bounded blur"
///    that limits sampling to the object's boundaries.
///  * [ui.ImageFilter], the engine-level class that this config resolves to.
///  * [BackdropFilter.filterConfig], which uses this class to configure its effect.
@immutable
abstract class ImageFilterConfig {
  /// Creates a configuration that directly wraps an existing [ui.ImageFilter].
  ///
  /// This constructor adapts standard engine-level filters to APIs that
  /// require an [ImageFilterConfig].
  ///
  /// Because the provided [ui.ImageFilter] is already instantiated, it cannot
  /// incorporate layout information (such as bounds) from the
  /// [ImageFilterContext] during resolution. For example, wrapping a
  /// [ui.ImageFilter.blur] results in a static blur with fixed parameters.
  ///
  /// For effects that must adapt to the layout, such as a "bounded" blur, use
  /// the [ImageFilterConfig.blur] constructor instead.
  ///
  /// The [filter] property of an instance created with this constructor
  /// returns the original filter.
  const factory ImageFilterConfig(ui.ImageFilter filter) = _DirectImageFilterConfig;

  /// Abstract const constructor. This constructor enables subclasses to provide
  /// const constructors so that they can be used in const expressions.
  const ImageFilterConfig._();

  /// Creates a configuration for a Gaussian blur.
  ///
  /// The `sigmaX` and `sigmaY` arguments are the standard deviation of the
  /// Gaussian kernel in the x and y directions, respectively.
  ///
  /// The `tileMode` argument determines how the blur should handle edges.
  ///
  /// The `downsampleScale` argument controls the linear resolution of the
  /// intermediate blur textures on Impeller. Values below 1.0 trade detail for
  /// lower GPU cost while preserving the requested blur radius. It defaults to
  /// 1.0 and must be between 0.0625 and 1.0, inclusive.
  ///
  /// When `backdropAlphaThreshold` is non-null, Denial's native compositor
  /// applies a backdrop blur only where the final sampled child alpha is
  /// strictly greater than this value. It must be between 0.0 and 1.0.
  /// `backdropAlphaThresholdIsSingleSurface` is an optimization promise for a
  /// child containing exactly one external surface.
  ///
  /// The `bounded` argument (defaults to false) controls the sampling strategy:
  ///
  ///  * If false, the filter is applied to the entire canvas using standard
  ///    sampling.
  ///  * If true, the filter performs a "bounded blur", typically used to
  ///    replicate the high-fidelity frosted-glass effect seen on iOS.
  ///
  /// In "bounded blur" mode, the kernel samples exclusively from within the
  /// bounding rectangle of the object. Pixels outside the bounds are treated
  /// as transparent, and the result is normalized to maintain full opacity
  /// at the edges. This mode prevents color bleeding from adjacent content.
  ///
  /// Unlike the low-level API [ui.ImageFilter.blur], this constructor does not
  /// require an explicit [Rect]. Because [ImageFilterConfig] is resolved during
  /// the painting phase, it automatically uses the [Rect] bounds provided by
  /// the [ImageFilterContext]. This ensures the effect stays perfectly
  /// synchronized with the layout without manual coordinate management.
  ///
  /// This mode only restricts the blur's sampling source; it does not clip the
  /// output. This should almost always be paired with a clipping widget (e.g.,
  /// [ClipRect]) to avoid seeing blur artifacts beyond the object's boundaries.
  const factory ImageFilterConfig.blur({
    double sigmaX,
    double sigmaY,
    ui.TileMode tileMode,
    bool bounded,
    double downsampleScale,
    double? backdropAlphaThreshold,
    bool backdropAlphaThresholdIsSingleSurface,
  }) = _BlurImageFilterConfig;

  /// Composes the `inner` filter configuration with `outer`, to combine their
  /// effects.
  ///
  /// Creates a single [ImageFilterConfig] that when applied, has the same
  /// effect as subsequently applying `inner` and `outer`, i.e., result =
  /// outer(inner(source)).
  const factory ImageFilterConfig.compose({
    required ImageFilterConfig outer,
    required ImageFilterConfig inner,
  }) = _ComposeImageFilterConfig;

  /// Resolves this configuration into a [ui.ImageFilter], given the context of
  /// the widget applying the filter.
  ui.ImageFilter resolve(ImageFilterContext context);

  /// The underlying [ui.ImageFilter] if this configuration was created by
  /// wrapping an existing filter.
  ///
  /// This getter returns non-null only if this object was created using the
  /// default [ImageFilterConfig.new] constructor.
  ///
  /// For all other constructors (such as [ImageFilterConfig.blur]), this getter
  /// returns null, even if the filter's parameters do not currently depend on
  /// layout information. For these configurations, you must use [resolve] to
  /// obtain the actual [ui.ImageFilter].
  ui.ImageFilter? get filter => null;

  /// The description text to show when the filter is part of a composite
  /// [ImageFilterConfig] created using [ImageFilterConfig.compose].
  String get debugShortDescription;

  @override
  String toString() => 'ImageFilterConfig.$debugShortDescription';
}

class _DirectImageFilterConfig extends ImageFilterConfig {
  const _DirectImageFilterConfig(this.filter) : super._();

  @override
  final ui.ImageFilter filter;

  @override
  ui.ImageFilter resolve(ImageFilterContext context) {
    return filter;
  }

  @override
  bool operator ==(Object other) {
    if (identical(this, other)) {
      return true;
    }
    if (other.runtimeType != runtimeType) {
      return false;
    }
    return other is _DirectImageFilterConfig && other.filter == filter;
  }

  @override
  int get hashCode => filter.hashCode;

  @override
  String get debugShortDescription => filter.debugShortDescription;

  @override
  String toString() => 'ImageFilterConfig(${filter.debugShortDescription})';
}

class _BlurImageFilterConfig extends ImageFilterConfig {
  const _BlurImageFilterConfig({
    this.sigmaX = 0.0,
    this.sigmaY = 0.0,
    this.tileMode = ui.TileMode.clamp,
    this.bounded = false,
    this.downsampleScale = 1.0,
    this.backdropAlphaThreshold,
    this.backdropAlphaThresholdIsSingleSurface = false,
  }) : assert(downsampleScale >= 0.0625 && downsampleScale <= 1.0),
       assert(
         backdropAlphaThreshold == null ||
             (backdropAlphaThreshold >= 0.0 && backdropAlphaThreshold <= 1.0),
       ),
       super._();

  final double sigmaX;
  final double sigmaY;
  final ui.TileMode tileMode;
  final bool bounded;
  final double downsampleScale;
  final double? backdropAlphaThreshold;
  final bool backdropAlphaThresholdIsSingleSurface;

  @override
  ui.ImageFilter resolve(ImageFilterContext context) {
    return ui.ImageFilter.blur(
      sigmaX: sigmaX,
      sigmaY: sigmaY,
      tileMode: tileMode,
      bounds: bounded ? context.bounds : null,
      downsampleScale: downsampleScale,
      backdropAlphaThreshold: backdropAlphaThreshold,
      backdropAlphaThresholdIsSingleSurface: backdropAlphaThresholdIsSingleSurface,
    );
  }

  @override
  bool operator ==(Object other) {
    if (identical(this, other)) {
      return true;
    }
    if (other.runtimeType != runtimeType) {
      return false;
    }
    return other is _BlurImageFilterConfig &&
        other.sigmaX == sigmaX &&
        other.sigmaY == sigmaY &&
        other.tileMode == tileMode &&
        other.bounded == bounded &&
        other.downsampleScale == downsampleScale &&
        other.backdropAlphaThreshold == backdropAlphaThreshold &&
        other.backdropAlphaThresholdIsSingleSurface == backdropAlphaThresholdIsSingleSurface;
  }

  @override
  int get hashCode => Object.hash(
    sigmaX,
    sigmaY,
    tileMode,
    bounded,
    downsampleScale,
    backdropAlphaThreshold,
    backdropAlphaThresholdIsSingleSurface,
  );

  String get _modeString {
    switch (tileMode) {
      case ui.TileMode.clamp:
        return 'clamp';
      case ui.TileMode.mirror:
        return 'mirror';
      case ui.TileMode.repeated:
        return 'repeated';
      case ui.TileMode.decal:
        return 'decal';
    }
  }

  String get _boundedString => bounded ? 'bounded' : 'unbounded';

  String get _downsampleString => downsampleScale == 1.0 ? '' : ', downsample: $downsampleScale';

  String get _backdropAlphaThresholdString =>
      backdropAlphaThreshold == null ? '' : ', backdrop alpha threshold: $backdropAlphaThreshold';

  @override
  String get debugShortDescription =>
      'blur($sigmaX, $sigmaY, $_modeString, $_boundedString$_downsampleString$_backdropAlphaThresholdString)';
}

class _ComposeImageFilterConfig extends ImageFilterConfig {
  const _ComposeImageFilterConfig({required this.outer, required this.inner}) : super._();

  final ImageFilterConfig outer;
  final ImageFilterConfig inner;

  @override
  ui.ImageFilter resolve(ImageFilterContext context) {
    return ui.ImageFilter.compose(outer: outer.resolve(context), inner: inner.resolve(context));
  }

  @override
  bool operator ==(Object other) {
    if (identical(this, other)) {
      return true;
    }
    if (other.runtimeType != runtimeType) {
      return false;
    }
    return other is _ComposeImageFilterConfig && other.outer == outer && other.inner == inner;
  }

  @override
  int get hashCode => Object.hash(outer, inner);

  @override
  String get debugShortDescription =>
      '${inner.debugShortDescription} -> ${outer.debugShortDescription}';

  @override
  String toString() => 'ImageFilterConfig.compose(source -> $debugShortDescription -> result)';
}
