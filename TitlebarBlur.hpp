#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

class CHyprBar;

// Live gradual blur under the title bar: the aura-blur matte system
// (https://github.com/Sh3nron's omarchy aura-blur, and the ReactBits
// GradualBlur falloff it is modeled on) applied to a per-window top band.
// The matte is strongest at the bar and melts smoothly into the app
// content below, so titles and buttons read clearly while the window
// never shows a hard blur edge.
class CTitlebarGradualBlurElement : public IPassElement {
  public:
    struct SBlurData {
        PHLMONITOR monitor;
        CHyprBar*  deco = nullptr; // owns the reuse matte framebuffer

        // all in monitor render coordinates (scaled pixels)
        CBox       box;      // the blur region: strip + reach below
        double     cardH;    // current bar band height (moves with the reveal)
        double     round;    // inner rounded-corner radius of the window edge
        double     reach;    // falloff length below the bar
        double     strength; // reveal progress based alpha (0..1)
    };

    CTitlebarGradualBlurElement(const SBlurData& data_) : m_data(data_) {}
    virtual ~CTitlebarGradualBlurElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override;

    virtual bool                needsLiveBlur() override {
        return true;
    }
    virtual bool                needsPrecomputeBlur() override {
        return false;
    }
    virtual const char*         passName() override {
        return "CTitlebarGradualBlur";
    }
    virtual ePassElementType    type() override {
        return EK_CUSTOM;
    }
    virtual std::optional<CBox> boundingBox() override {
        return m_data.box;
    }
    virtual bool                disableSimplification() override {
        return true;
    }

  private:
    SBlurData m_data;
};
