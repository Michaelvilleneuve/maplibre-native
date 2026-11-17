#include <mln/style/layers/custom_layer_render_parameters.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/renderer/render_terrain.hpp>
#include <mln/util/mat4.hpp>
#include <mln/renderer/layer_group.hpp>

#if MLN_RENDER_BACKEND_OPENGL
#include <mln/gl/texture2d.hpp>
#include <mln/gfx/renderer_backend.hpp>
#include <mln/gfx/renderable.hpp>
#endif

namespace mln {
namespace style {

CustomLayerRenderParameters::CustomLayerRenderParameters(const mln::PaintParameters& paintParameters) {
    const TransformState& state = paintParameters.state;
    width = state.getSize().width;
    height = state.getSize().height;
    latitude = state.getLatLng().latitude();
    longitude = state.getLatLng().longitude();
    zoom = state.getZoom();
    bearing = util::rad2deg(-state.getBearing());
    pitch = state.getPitch();
    fieldOfView = state.getFieldOfView();
    mat4 projMatrix;
    state.getProjMatrix(projMatrix);
    projectionMatrix = projMatrix;
    nearClippedProjectionMatrix = paintParameters.transformParams.nearClippedProjMatrix;
    if (paintParameters.terrain) {
        terrainExaggeration = paintParameters.terrain->getExaggeration();
#if MLN_RENDER_BACKEND_OPENGL
        // RenderTerrain::renderDepth draws before the main pass, where the custom layers draw.
        // Before it has drawn at this size there is only the 1x1 far-plane placeholder.
        const auto& depthLayerGroup = paintParameters.terrain->getDepthLayerGroup();
        if (depthLayerGroup && !depthLayerGroup->empty()) {
            const auto& depthTexture = paintParameters.terrain->getDepthTexture(paintParameters.context);
            if (depthTexture && depthTexture->getSize() == paintParameters.backend.getDefaultRenderable().getSize()) {
                terrainDepthTexture = static_cast<gl::Texture2D&>(*depthTexture).getTextureID();
            }
        }
#endif
    }
}

} // namespace style
} // namespace mln
