#include "BarPassElement.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include "barDeco.hpp"

using namespace Render::GL;

CBarPassElement::CBarPassElement(const CBarPassElement::SBarData& data_) : data(data_) {
    ;
}

std::vector<UP<IPassElement>> CBarPassElement::draw() {
    data.deco->renderPass(g_pHyprRenderer->m_renderData.pMonitor.lock(), data.a);
    return {};
}

bool CBarPassElement::needsLiveBlur() {
    static auto PENABLEBLURGLOBAL = CConfigValue<Config::BOOL>("decoration:blur:enabled");

    CHyprColor  color = data.deco->m_bForcedBarColor.value_or(CHyprColor{static_cast<uint64_t>(g_pGlobalState->config.barColor->value())});
    color.a *= data.a;
    const bool SHOULDBLUR = g_pGlobalState->config.barBlur->value() && *PENABLEBLURGLOBAL && color.a < 1.F && !g_pGlobalState->config.barGradualBlur->value();

    return SHOULDBLUR;
}

std::optional<CBox> CBarPassElement::boundingBox() {
    // Temporary fix: expand the bar bb a bit, otherwise occlusion gets too
    // aggressive. The gradual blur reach below the strip is included too.
    int expand = 10;
    if (g_pGlobalState->config.barGradualBlur->value())
        expand += g_pGlobalState->config.barBlurReach->value();
    return data.deco->assignedBoxGlobal().translate(-g_pHyprRenderer->m_renderData.pMonitor->m_position).expand(expand);
}

bool CBarPassElement::needsPrecomputeBlur() {
    return false;
}
