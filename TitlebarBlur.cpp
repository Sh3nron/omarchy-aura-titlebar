#include "TitlebarBlur.hpp"
#include "barDeco.hpp"

#include <hyprland/src/render/pass/TextureMatteElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <algorithm>
#include <cmath>

// The same quarter-resolution scalar matte as aura-blur: linear GPU sampling
// over a quarter-resolution alpha mask is indistinguishable from full res for
// a smooth falloff, at a quarter of the per-frame cost.
constexpr int   MASK_SCALE = 4;
// extend the matte beneath the band so bilinear mask sampling and the bar
// edge never expose an unblurred seam (same trick as aura-blur)
constexpr float UNDERLAP   = 12.F;

// same rounded-rect signed distance as aura-blur's matte
static float roundedDistance(float px, float py, const CBox& card, float radius) {
    const float r  = std::clamp(radius, 0.F, static_cast<float>(std::min(card.w, card.h) * .5));
    const float qx = std::abs(px - (card.x + card.w * .5F)) - (card.w * .5F - r);
    const float qy = std::abs(py - (card.y + card.h * .5F)) - (card.h * .5F - r);
    return std::hypot(std::max(qx, 0.F), std::max(qy, 0.F)) + std::min(std::max(qx, qy), 0.F) - r;
}

std::vector<UP<IPassElement>> CTitlebarGradualBlurElement::draw() {
    std::vector<UP<IPassElement>> result;
    if (!m_data.monitor || !m_data.deco || m_data.strength <= 0.004F)
        return result;

    const int matteW = std::max(1, (sc<int>(m_data.fullBox.w) + MASK_SCALE - 1) / MASK_SCALE);
    const int matteH = std::max(1, (sc<int>(m_data.fullBox.h) + MASK_SCALE - 1) / MASK_SCALE);

    // matte framebuffer owned by the bar, allocated for this monitor size
    // and re-rasterized only when the descriptor's regen key changes —
    // ordinary redraws (typing, cursors) reuse the last matte
    auto fb = m_data.deco->matteFB();
    if (!fb || sc<int>(fb->m_size.x) < matteW || sc<int>(fb->m_size.y) < matteH) {
        fb = g_pHyprRenderer->createFB("aura-titlebar-matte");
        if (!fb || !fb->alloc(matteW, matteH, DRM_FORMAT_ARGB8888))
            return result;
        m_data.deco->setMatteFB(fb);
        m_data.deco->setMatteKey(0); // force regen after (re)allocation
    }

    if (m_data.deco->matteKey() != m_data.gen) {
        std::vector<uint8_t> pixels(sc<size_t>(matteW) * sc<size_t>(matteH) * 4, 0);

        // restrict the scan to the band's bounding quadrants: everything
        // outside is alpha 0 and the loop skips whole rows cheaply
        const CBox  band = m_data.card;
        const float rad  = sc<float>(m_data.round);
        const float rch  = sc<float>(m_data.reach);
        const float str  = sc<float>(m_data.strength);

        const int yStart = std::max(0, sc<int>(band.y - UNDERLAP) / MASK_SCALE - 1);
        const int yEnd   = std::min(matteH, sc<int>(band.y + band.h + rch + UNDERLAP) / MASK_SCALE + 2);
        const int xStart = std::max(0, sc<int>(band.x - UNDERLAP) / MASK_SCALE - 1);
        const int xEnd   = std::min(matteW, sc<int>(band.x + band.w + rch) / MASK_SCALE + 2);

        for (int my = yStart; my < yEnd; ++my) {
            const float py = (my + .5F) * MASK_SCALE;
            for (int mx = xStart; mx < xEnd; ++mx) {
                const float px  = (mx + .5F) * MASK_SCALE;

                float alpha    = 0.F;
                const float d  = roundedDistance(px, py, band, rad);
                if (d <= -UNDERLAP) {
                    alpha = 1.F;
                } else if (d < rch) {
                    const float t           = std::max(d, 0.F) / rch;
                    const float smooth      = t * t * (3.F - 2.F * t);
                    const float innerT      = std::clamp((d + UNDERLAP) / (UNDERLAP * .6F), 0.F, 1.F);
                    const float innerSmooth = innerT * innerT * (3.F - 2.F * innerT);
                    alpha                   = std::pow(1.F - smooth, 1.35F) * innerSmooth;
                }

                alpha *= str;
                if (alpha <= 0.F)
                    continue;

                const auto  value = static_cast<uint8_t>(std::clamp(alpha, 0.F, 1.F) * 255.F + .5F);
                const auto  p     = (sc<size_t>(my) * sc<size_t>(matteW) + sc<size_t>(mx)) * 4;
                pixels[p]         = pixels[p + 1] = pixels[p + 2] = pixels[p + 3] = value;
            }
        }

        const CRegion all{0., 0., sc<double>(matteW), sc<double>(matteH)};
        fb->getTexture()->update(DRM_FORMAT_ARGB8888, pixels.data(), matteW * 4, all);
        m_data.deco->setMatteKey(m_data.gen);
    }

    // Hyprland's render pass expands our damage because needsLiveBlur() is
    // true; blur only that fully repainted region.
    CRegion blurDamage = g_pHyprRenderer->m_renderData.damage.copy();
    auto    blurred    = g_pHyprRenderer->blurMainFramebuffer(1.F, &blurDamage);
    if (!blurred)
        return result;

    result.emplace_back(makeUnique<CTextureMatteElement>(CTextureMatteElement::STextureMatteData{
        .box = m_data.fullBox, .tex = blurred, .fb = fb, .disableTransformAndModify = true}));
    return result;
}
