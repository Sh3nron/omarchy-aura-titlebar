#pragma once

#include <cstdint>

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

class CHyprBar;

// Live gradual blur under the title bar: the aura-blur matte system
// (the ReactBits GradualBlur falloff, as a compositor render pass)
// applied to a per-window top band. The matte is strongest at the bar
// and melts smoothly into the app content below, so titles and buttons
// read clearly while the window never shows a hard blur edge.
//
// The matte element must be handed a FULL-MONITOR box and full-monitor
// matte framebuffer, exactly like aura-blur: CTextureMatteElement maps
// its blurred texture onto the given box, so a sub-region box would
// squeeze the whole blurred desktop into the strip. The band lives
// purely in the matte alpha.
class CTitlebarGradualBlurElement : public IPassElement {
  public:
    struct SBlurData {
        PHLMONITOR monitor;
        CHyprBar*  deco = nullptr; // owns the matte framebuffer + regen key

        // full monitor render box (disableTransformAndModify path)
        CBox       fullBox;   // {0, 0, transformedSize} of the monitor
        CBox       card;      // the bar band in monitor render coordinates
        double     reach;     // falloff length below the band
        double     round;     // inner rounded-corner radius of the window edge
        double     strength;  // reveal-progress alpha (0..1)
        uint64_t   gen;       // regen key: geometry + quantized strength
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
        return m_data.fullBox;
    }
    virtual bool                disableSimplification() override {
        return true;
    }

  private:
    SBlurData m_data;
};
