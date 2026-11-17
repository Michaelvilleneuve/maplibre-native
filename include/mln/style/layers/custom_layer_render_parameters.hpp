#pragma once

#include <memory>
#include <array>

namespace mln {

class PaintParameters;

namespace style {

/**
 * Parameters that define the current camera position for a
 * `CustomLayerHost::render()` function.
 */
struct CustomLayerRenderParameters {
    double width;
    double height;
    double latitude;
    double longitude;
    double zoom;
    double bearing;
    double pitch;
    double fieldOfView;

    /// Standard projection matrix (nearZ = 1 tile unit).
    /// Use this for 2D/flat custom geometry.
    std::array<double, 16> projectionMatrix;

    /// A 4×4 matrix representing the map view’s current near clip projection state.
    std::array<double, 16> nearClippedProjectionMatrix;

    /// The 3D terrain's exaggeration, or 0 when terrain is off.
    double terrainExaggeration = 0;

    /// OpenGL only: the name of the GL texture holding the terrain depth pass's output for this
    /// frame, or 0 when terrain is off or the pass has nothing to draw. It is the size of the
    /// default framebuffer (read it at `gl_FragCoord.xy`), RGBA8, nearest-filtered, each texel the
    /// depth `z / w * 0.5 + 0.5` of the terrain under that pixel under `projectionMatrix`, packed
    /// as four base-256 digits `d0..d3` each stored as `k / 255`: `depth = dot(texel * 255,
    /// vec4(1, 1/256, 1/65536, 1/16777216)) / 256`; 1 (white) where no terrain is drawn. With the
    /// inverse of `projectionMatrix` it gives the place on the ground of every pixel, so a layer
    /// can draw onto the relief in one screen-sized pass (Metal: `mtl::CustomLayerRenderParameters
    /// ::terrainDepthTexture`, the same packing).
    unsigned int terrainDepthTexture = 0;

    CustomLayerRenderParameters(const PaintParameters&);
};

} // namespace style
} // namespace mln
