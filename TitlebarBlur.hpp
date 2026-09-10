#pragma once

#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

// Owned by the decoration; reused across frames.
struct STitlebarBlurResources {
    SP<Render::IFramebuffer> horizontal, vertical;
    UP<CShader> blur, composite;
    bool failed = false;
};

class CTitlebarGradualBlurElement : public IPassElement {
  public:
    struct SBlurData {
        PHLMONITOR monitor;
        STitlebarBlurResources* resources;
        CBox window; // monitor-local physical pixels, before output transform
        double height, reach, round, roundingPower, strength, opacity;
        CHyprColor tint;
    };
    explicit CTitlebarGradualBlurElement(const SBlurData& data) : m_data(data) {}
    std::vector<UP<IPassElement>> draw() override;
    bool needsLiveBlur() override { return true; }
    bool needsPrecomputeBlur() override { return false; }
    const char* passName() override { return "CTitlebarGradualBlur"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox() override {
        auto box = m_data.window;
        box.h = std::min(box.h, m_data.height + m_data.reach);
        return box.scale(1.0 / m_data.monitor->m_scale);
    }
    bool disableSimplification() override { return true; }
  private:
    SBlurData m_data;
};
