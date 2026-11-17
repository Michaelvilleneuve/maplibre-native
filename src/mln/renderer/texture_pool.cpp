#include <mln/renderer/texture_pool.hpp>

#include <mln/gfx/texture2d.hpp>
#include <mln/gfx/context.hpp>

namespace mln {

namespace {
// The width of the target's texture, 0 if it has none. The texture object exists before its memory
// does (that is allocated when the target first bakes), so a target's size is known at once.
uint32_t textureWidth(const std::shared_ptr<RenderTarget>& target) {
    if (!target) return 0;
    const auto& texture = target->getTexture();
    return texture ? texture->getSize().width : 0;
}

// A drape target of a stack above the first: cleared transparent, so where its layers draw nothing
// the terrain drawn again with it leaves what is under it (TexturePool::createRenderTargets)
std::shared_ptr<RenderTarget> makeStackTarget(gfx::Context& context,
                                              uint32_t size,
                                              const UnwrappedTileID& id,
                                              uint8_t stack) {
    auto target = context.createRenderTarget({size, size}, gfx::TextureChannelDataType::UnsignedByte, /*stencil=*/true);
    if (target) {
        target->setClearColor(Color{0.0f, 0.0f, 0.0f, 0.0f});
        target->setDrapeTileID(id);
        target->setDrapeStack(stack);
    }
    return target;
}

// The stacks above the first of a tile's target, as many as `stacks` asks, the size of its own
void matchStackTargets(gfx::Context& context,
                       const std::shared_ptr<RenderTarget>& target,
                       const UnwrappedTileID& id,
                       uint8_t stacks) {
    auto& uppers = target->getStackTargets();
    const std::size_t wanted = stacks > 0 ? stacks - 1u : 0u;
    const uint32_t size = textureWidth(target);
    if (uppers.size() > wanted) uppers.resize(wanted);
    while (uppers.size() < wanted) {
        auto upper = makeStackTarget(context, size, id, static_cast<uint8_t>(uppers.size() + 1));
        if (!upper) break;
        uppers.push_back(std::move(upper));
    }
}
} // namespace

TexturePool::TexturePool(uint32_t fullSize_, uint32_t halfSize_)
    : fullSize(fullSize_),
      halfSize(halfSize_) {}

TexturePool::~TexturePool() = default;

TexturePool::Resizes TexturePool::createRenderTargets(gfx::Context& context,
                                                      const std::set<UnwrappedTileID>& cover,
                                                      const std::set<UnwrappedTileID>& fullSizeTiles,
                                                      const Color& backgroundColor,
                                                      size_t maxResizes,
                                                      uint8_t stacks) {
    const bool held = fullSizeHold > 0;
    if (held) --fullSizeHold;
    Resizes resizes;
    // Targets shrinking go first (so the full-size count only falls before it rises), then the
    // ones growing; tiles without a target get one in the first pass.
    for (const bool growing : {false, true}) {
        for (const auto& id : cover) {
            const auto found = renderTargets.find(id);
            const uint32_t have = found != renderTargets.end() ? textureWidth(found->second) : 0;
            uint32_t wanted = fullSizeTiles.contains(id) ? fullSize : halfSize;
            if (held && wanted == fullSize && have != fullSize) wanted = halfSize;
            // Keep an existing render target; recreating it every frame churns GL
            // texture memory and invalidates the texture bound to the terrain drawable.
            if (have == wanted) {
                if (!growing) {
                    found->second->setClearColor(backgroundColor);
                    matchStackTargets(context, found->second, id, stacks);
                }
                continue;
            }
            if (have != 0) {
                if ((wanted > have) != growing) continue;
                if (maxResizes > 0 && resizes.replaced.size() == maxResizes) {
                    resizes.deferred = true;
                    found->second->setClearColor(backgroundColor);
                    matchStackTargets(context, found->second, id, stacks);
                    continue;
                }
                resizes.replaced.insert(id);
            }
            // Drape targets render several overlapping draped tiles (a parent standing in for
            // missing children beside already-loaded children); the stencil clips them against each
            // other, as maplibre-gl-js does for its render-to-texture framebuffer.
            auto renderTarget = context.createRenderTarget(
                {wanted, wanted}, gfx::TextureChannelDataType::UnsignedByte, /*stencil=*/true);
            renderTarget->setClearColor(backgroundColor);
            renderTarget->setDrapeTileID(id);
            matchStackTargets(context, renderTarget, id, stacks);
            renderTargets[id] = std::move(renderTarget);
        }
    }
    return resizes;
}

std::set<UnwrappedTileID> TexturePool::tilesAtFullSize() const {
    std::set<UnwrappedTileID> tiles;
    for (const auto& [id, target] : renderTargets) {
        if (textureWidth(target) == fullSize) tiles.insert(id);
    }
    return tiles;
}

bool TexturePool::renderTargetFailed(const UnwrappedTileID& id) {
    renderTargets.erase(id);
    // Not extended by a failure inside the window, so that one that goes on is reported again
    // once the window is over, and once per window.
    const bool first = fullSizeHold == 0;
    if (first) fullSizeHold = failureWindowFrames;
    return first;
}

void TexturePool::removeStaleRenderTargets(const std::set<UnwrappedTileID>& currentTiles) {
    for (auto it = renderTargets.begin(); it != renderTargets.end();) {
        if (!currentTiles.contains(it->first)) {
            it = renderTargets.erase(it);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<RenderTarget> TexturePool::getRenderTarget(const UnwrappedTileID& id) const {
    return renderTargets.contains(id) ? renderTargets.at(id) : nullptr;
}

std::shared_ptr<RenderTarget> TexturePool::getStackRenderTarget(const UnwrappedTileID& id, uint8_t stack) const {
    const auto target = getRenderTarget(id);
    if (!target || stack == 0) return target;
    const auto& uppers = target->getStackTargets();
    return stack <= uppers.size() ? uppers[stack - 1] : nullptr;
}

std::shared_ptr<RenderTarget> TexturePool::getRenderTargetAncestorOrDescendant(
    const UnwrappedTileID& id, std::optional<UnwrappedTileID>& terrainTileID) const {
    terrainTileID = std::nullopt;
    std::shared_ptr<RenderTarget> bestRenderTarget;
    for (const auto& [tileID, renderTarget] : renderTargets) {
        if (tileID == id || tileID.isChildOf(id) || id.isChildOf(tileID)) {
            if (!terrainTileID || tileID.canonical.z > terrainTileID->canonical.z) {
                terrainTileID = tileID;
                bestRenderTarget = renderTarget;
            }
        }
    }
    return bestRenderTarget;
}
} // namespace mln
