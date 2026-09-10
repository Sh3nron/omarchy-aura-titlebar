#include "barDeco.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/parserUtils/ParserUtils.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include "globals.hpp"
#include "BarPassElement.hpp"
#include "TitlebarBlur.hpp"

#include <climits>
#include <cstring>

using namespace Render::GL;

static CHyprColor configColor(Config::INTEGER color) {
    return CHyprColor{sc<uint64_t>(color)};
}

CHyprBar::CHyprBar(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
    m_pWindow = pWindow;

    const auto PMONITOR         = pWindow->m_monitor.lock();
    PMONITOR->m_scheduledRecalc = true;

    // button events
    m_pMouseButtonCallback = Event::bus()->m_events.input.mouse.button.listen([&](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(info, e); });
    m_pTouchDownCallback   = Event::bus()->m_events.input.touch.down.listen([&](ITouch::SDownEvent e, Event::SCallbackInfo& info) { onTouchDown(info, e); });
    m_pTouchUpCallback     = Event::bus()->m_events.input.touch.up.listen([&](ITouch::SUpEvent e, Event::SCallbackInfo& info) { onTouchUp(info, e); });

    // move events
    m_pTouchMoveCallback = Event::bus()->m_events.input.touch.motion.listen([&](ITouch::SMotionEvent e, Event::SCallbackInfo& info) { onTouchMove(info, e); });
    m_pMouseMoveCallback = Event::bus()->m_events.input.mouse.move.listen([&](Vector2D c, Event::SCallbackInfo& info) { onMouseMove(c); });

    Animation::mgr()->createAnimation(configColor(g_pGlobalState->config.barColor->value()), m_cRealBarColor, Config::animationTree()->getAnimationPropertyConfig("border"),
                                      pWindow, AVARDAMAGE_NONE);
    m_cRealBarColor->setUpdateCallback([&](auto) { damageEntire(); });

    // the reveal spring: shares the window open animation curve so the
    // desktop feels coherent. Overshoot of the spring lands the bar a few
    // pixels past the strip before settling — the iOS sheet feel.
    Animation::mgr()->createAnimation(0.F, m_fRevealProgress, Config::animationTree()->getAnimationPropertyConfig("windowsIn"), pWindow, AVARDAMAGE_NONE);
    m_fRevealProgress->setUpdateCallback([&](auto) { damageEntire(); });
}

CHyprBar::~CHyprBar() {
    std::erase(g_pGlobalState->bars, m_self);
}

SDecorationPositioningInfo CHyprBar::getPositioningInfo() {
    SDecorationPositioningInfo info;
    // never reserves space: the bar overlays the window content on hover,
    // and draws nothing when the cursor is away so only the user's own
    // compositor border remains
    info.policy         = DECORATION_POSITION_ABSOLUTE;
    info.edges          = DECORATION_EDGE_TOP;
    info.priority       = 5000;
    info.reserved       = false;
    info.desiredExtents = {{0, 0}, {0, 0}};
    return info;
}

void CHyprBar::onPositioningReply(const SDecorationPositioningReply& reply) {
    if (reply.assignedGeometry.size() != m_bAssignedBox.size())
        m_bWindowSizeChanged = true;

    m_bAssignedBox = reply.assignedGeometry;
}

std::string CHyprBar::getDisplayName() {
    return "AuraTitlebar";
}

bool CHyprBar::barAcceptsInput() {
    if (m_bRevealed)
        return true;

    // during the slide the strip becomes interactive past ~60% of the way in
    if (m_fRevealProgress->goal() == 0.F)
        return false;

    return m_fRevealProgress->value() > 0.6F;
}

bool CHyprBar::inputIsValid() {
    if (!g_pGlobalState->config.enabled->value() || m_hidden)
        return false;

    if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_pWindow->wlSurface()->resource()))
        return false;

    if (!barAcceptsInput())
        return false;

    const auto MOUSE    = g_pInputManager->getMouseCoordsInternal();
    auto       PMONITOR = Desktop::focusState()->monitor();

    if (!PMONITOR)
        return false;

    Desktop::CViewHitTester hitTester{*Desktop::viewState()};

    const auto              WINDOWATCURSOR = hitTester.windowAt(MOUSE, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

    auto                    focusState = Desktop::focusState();
    auto                    window     = focusState->window();

    if (WINDOWATCURSOR != m_pWindow && m_pWindow != window)
        return false;

    PHLLS    foundSurface = nullptr;
    Vector2D surfaceCoords;

    // Check Top Layer
    hitTester.layerSurfaceAt(MOUSE, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &foundSurface);
    if (foundSurface)
        return false;

    // Check Overlay Layer
    hitTester.layerSurfaceAt(MOUSE, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &foundSurface);
    if (foundSurface)
        return false;

    return true;
}

void CHyprBar::onMouseButton(Event::SCallbackInfo& info, IPointer::SButtonEvent e) {
    if (!inputIsValid())
        return;

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        handleUpEvent(info);
        return;
    }

    handleDownEvent(info, std::nullopt);
}

void CHyprBar::onTouchDown(Event::SCallbackInfo& info, ITouch::SDownEvent e) {
    // Don't do anything if you're already grabbed a window with another finger
    if (!inputIsValid() || e.touchID != 0)
        return;

    handleDownEvent(info, e);
}

void CHyprBar::onTouchUp(Event::SCallbackInfo& info, ITouch::SUpEvent e) {
    if (!m_bDragPending || !m_bTouchEv || e.touchID != m_touchId)
        return;

    handleUpEvent(info);
}

void CHyprBar::onMouseMove(Vector2D coords) {
    // ensure proper redraws of button icons on hover when using hardware cursors
    if (g_pGlobalState->config.iconOnHover->value())
        damageOnButtonHover();

    if (g_pGlobalState->config.revealOnHover->value() && !m_bTouchEv) {
        const bool want = shouldReveal(g_pInputManager->getMouseCoordsInternal());
        if (want != m_bRevealed)
            setReveal(want);
    }

    if (!m_bDragPending || m_bTouchEv || !validMapped(m_pWindow) || m_touchId != 0)
        return;

    m_bDragPending = false;
    handleMovement();
}

void CHyprBar::onTouchMove(Event::SCallbackInfo& info, ITouch::SMotionEvent e) {
    if (!m_bDragPending || !m_bTouchEv || !validMapped(m_pWindow) || e.touchID != m_touchId)
        return;

    auto PMONITOR     = m_pWindow->m_monitor.lock();
    PMONITOR          = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
    const auto COORDS = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y);

    if (!m_bDraggingThis) {
        // Initial setup for dragging a window.
        g_pKeybindManager->m_dispatchers["setfloating"]("activewindow");
        g_pKeybindManager->m_dispatchers["resizewindowpixel"]("exact 50% 50%,activewindow");
        // pin it so you can change workspaces while dragging a window
        g_pKeybindManager->m_dispatchers["pin"]("activewindow");
    }
    g_pKeybindManager->m_dispatchers["movewindowpixel"](std::format("exact {} {},activewindow", (int)(COORDS.x - (stripBoxGlobal().w / 2)), (int)COORDS.y));
    m_bDraggingThis = true;
}

void CHyprBar::handleDownEvent(Event::SCallbackInfo& info, std::optional<ITouch::SDownEvent> touchEvent) {
    m_bTouchEv = touchEvent.has_value();
    if (m_bTouchEv)
        m_touchId = touchEvent.value().touchID;

    const auto PWINDOW = m_pWindow.lock();

    auto       COORDS = cursorRelativeToBar();
    if (m_bTouchEv) {
        ITouch::SDownEvent e        = touchEvent.value();
        PHLMONITOR         PMONITOR = nullptr;
        for (auto& m : State::monitorState()->monitors()) {
            if (m->m_name == (!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "")) {
                PMONITOR = m;
                break;
            }
        }
        PMONITOR = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
        COORDS   = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y) - stripBoxGlobal().pos();
    }

    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const auto ON_DOUBLE_CLICK  = g_pGlobalState->config.onDoubleClick->value();

    const auto  STRIPBOX     = stripBoxGlobal();
    const bool  BUTTONSRIGHT = ALIGNBUTTONS != "left";

    if (!VECINRECT(COORDS, 0, 0, STRIPBOX.w, HEIGHT - 1)) {

        if (m_bDraggingThis) {
            if (m_bTouchEv)
                g_pKeybindManager->m_dispatchers["settiled"]("activewindow");
            g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
            Log::logger->log(Log::DEBUG, "[aura-titlebar] Dragging ended on {:x}", (uintptr_t)PWINDOW.get());
        }

        m_bDraggingThis = false;
        m_bDragPending  = false;
        m_bTouchEv      = false;
        return;
    }

    if (PWINDOW->hasPopupAt(COORDS + STRIPBOX.pos()))
        return;

    if (Desktop::focusState()->window() != PWINDOW)
        Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

    if (PWINDOW->m_isFloating)
        Desktop::windowState()->raise(PWINDOW);

    info.cancelled   = true;
    m_bCancelledDown = true;
    m_bPointerHeld   = true;

    if (doButtonPress(BARPADDING, BARBUTTONPADDING, HEIGHT, COORDS, BUTTONSRIGHT))
        return;

    if (!ON_DOUBLE_CLICK.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastMouseDown).count() < 400 /* Arbitrary delay I found suitable */) {
        Config::Supplementary::executor()->spawn(ON_DOUBLE_CLICK);
        m_bDragPending = false;
    } else {
        m_lastMouseDown = Time::steadyNow();
        m_bDragPending  = true;
    }
}

void CHyprBar::handleUpEvent(Event::SCallbackInfo& info) {
    if (m_pWindow.lock() != Desktop::focusState()->window())
        return;

    if (m_bCancelledDown)
        info.cancelled = true;

    m_bCancelledDown = false;

    if (m_bDraggingThis) {
        g_pKeybindManager->changeMouseBindMode(MBIND_INVALID);
        m_bDraggingThis = false;
        if (m_bTouchEv)
            (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE);

        Log::logger->log(Log::DEBUG, "[aura-titlebar] Dragging ended on {:x}", (uintptr_t)m_pWindow.lock().get());
    }

    m_bDragPending  = false;
    m_bTouchEv      = false;
    m_bPointerHeld  = false;
    m_touchId       = 0;
}

void CHyprBar::handleMovement() {
    g_pKeybindManager->changeMouseBindMode(MBIND_MOVE);
    m_bDraggingThis = true;
    Log::logger->log(Log::DEBUG, "[aura-titlebar] Dragging initiated on {:x}", (uintptr_t)m_pWindow.lock().get());
    return;
}

bool CHyprBar::doButtonPress(Config::INTEGER barPadding, Config::INTEGER barButtonPadding, Config::INTEGER barHeight, Vector2D COORDS, const bool BUTTONSRIGHT) {
    //check if on a button
    float offset = barPadding;

    for (auto& b : g_pGlobalState->buttons) {
        const auto BARBUF     = Vector2D{(int)stripBoxGlobal().w, barHeight};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - barButtonPadding - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

        if (VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + b.size + barButtonPadding, currentPos.y + b.size)) {
            // hit on button
            g_pKeybindManager->m_dispatchers["exec"](b.cmd);
            return true;
        }

        offset += barButtonPadding + b.size;
    }
    return false;
}

void CHyprBar::renderBarTitle(const Vector2D& bufferSize, const float scale) {
    const auto COLORVAL         = g_pGlobalState->config.textColor->value();
    const auto SIZE             = g_pGlobalState->config.barTextSize->value();
    const auto WEIGHT           = g_pGlobalState->config.barTextWeight->value();
    const auto FONT             = g_pGlobalState->config.barTextFont->value();
    const auto ALIGN            = g_pGlobalState->config.barTextAlign->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();

    float      buttonSizes = BARBUTTONPADDING;
    for (auto& b : g_pGlobalState->buttons) {
        buttonSizes += b.size + BARBUTTONPADDING;
    }

    const int  scaledSize        = std::round(SIZE * scale);
    const auto scaledButtonsSize = buttonSizes * scale;
    const auto scaledBarPadding  = BARPADDING * scale;
    const int  paddingTotal      = scaledBarPadding * 2 + scaledButtonsSize + (ALIGN != "left" ? scaledButtonsSize : 0);
    const int  maxWidth          = std::clamp(static_cast<int>(bufferSize.x - paddingTotal), 0, INT_MAX);

    if (m_szLastTitle.empty() || maxWidth < 1) {
        m_pTextTex = nullptr;
        return;
    }

    const CHyprColor COLOR = m_bForcedTitleColor.value_or(configColor(COLORVAL));
    m_pTextTex             = g_pHyprRenderer->renderText(m_szLastTitle, COLOR, scaledSize, false, FONT, maxWidth, WEIGHT.m_value);
}

size_t CHyprBar::getVisibleButtonCount(Config::INTEGER barButtonPadding, Config::INTEGER barPadding, const Vector2D& bufferSize, const float scale) {
    float  availableSpace = bufferSize.x - barPadding * scale * 2;
    size_t count          = 0;

    for (const auto& button : g_pGlobalState->buttons) {
        const float buttonSpace = (button.size + barButtonPadding) * scale;
        if (availableSpace >= buttonSpace) {
            count++;
            availableSpace -= buttonSpace;
        } else
            break;
    }

    return count;
}

void CHyprBar::renderBarButtons(CBox* barBox, const float scale, const float a) {
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const auto INACTIVECOLOR    = g_pGlobalState->config.inactiveButtonColor->value();

    const bool BUTTONSRIGHT    = ALIGNBUTTONS != "left";
    const auto visibleCount    = getVisibleButtonCount(BARBUTTONPADDING, BARPADDING, Vector2D{barBox->w, barBox->h}, scale);
    const bool INVALIDATEICONS = m_bButtonsDirty || m_bWindowSizeChanged;

    int        offset = BARPADDING * scale;
    for (size_t i = 0; i < visibleCount; ++i) {
        auto&      button           = g_pGlobalState->buttons[i];
        const auto scaledButtonSize = button.size * scale;
        const auto scaledButtonsPad = BARBUTTONPADDING * scale;

        auto       color = button.bgcol;

        if (INACTIVECOLOR > 0) {
            color = m_bWindowHasFocus ? color : configColor(INACTIVECOLOR);
            if (INVALIDATEICONS && button.userfg && button.iconTex)
                button.iconTex = nullptr;
        }

        color.a *= a;

        CBox buttonBox = {barBox->x + (BUTTONSRIGHT ? barBox->w - offset - scaledButtonSize : offset), barBox->y + (barBox->h - scaledButtonSize) / 2.0, scaledButtonSize,
                          scaledButtonSize};
        buttonBox.round();

        g_pHyprOpenGL->renderRect(buttonBox, color, {.round = static_cast<int>(std::round(scaledButtonSize / 2.0)), .roundingPower = 2.F});

        offset += scaledButtonsPad + scaledButtonSize;
    }
}

void CHyprBar::renderBarButtonsText(CBox* barBox, const float scale, const float a) {
    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const auto ICONONHOVER      = g_pGlobalState->config.iconOnHover->value();

    const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
    const auto visibleCount = getVisibleButtonCount(BARBUTTONPADDING, BARPADDING, Vector2D{barBox->w, barBox->h}, scale);
    const auto COORDS       = cursorRelativeToBar();

    int        offset        = BARPADDING * scale;
    float      noScaleOffset = BARPADDING;

    for (size_t i = 0; i < visibleCount; ++i) {
        auto&      button           = g_pGlobalState->buttons[i];
        const auto scaledButtonSize = button.size * scale;
        const auto scaledButtonsPad = BARBUTTONPADDING * scale;

        // check if hovering here
        const auto BARBUF     = Vector2D{(int)stripBoxGlobal().w, HEIGHT};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - BARBUTTONPADDING - button.size - noScaleOffset : noScaleOffset), (BARBUF.y - button.size) / 2.0}.floor();
        bool       hovering   = VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + button.size + BARBUTTONPADDING, currentPos.y + button.size);
        noScaleOffset += BARBUTTONPADDING + button.size;

        if ((!button.iconTex || button.iconTex->m_texID == 0) && !button.icon.empty()) {
            // render icon
            auto fgcol = button.userfg ? button.fgcol : (button.bgcol.r + button.bgcol.g + button.bgcol.b < 1) ? CHyprColor(0xFFFFFFFF) : CHyprColor(0xFF000000);

            button.iconTex = g_pHyprRenderer->renderText(button.icon, fgcol, std::round(button.size * 0.62 * scale), false, "sans", scaledButtonSize);
        }

        if (!button.iconTex || button.iconTex->m_texID == 0)
            continue;

        const auto iconX = barBox->x + (BUTTONSRIGHT ? barBox->width - offset - scaledButtonSize / 2.0 : offset + scaledButtonSize / 2.0) - button.iconTex->m_size.x / 2.0;
        const auto iconY = barBox->y + barBox->height / 2.0 - button.iconTex->m_size.y / 2.0;
        CBox       pos   = {iconX, iconY, button.iconTex->m_size.x, button.iconTex->m_size.y};

        if (!ICONONHOVER || (ICONONHOVER && m_iButtonHoverState > 0))
            g_pHyprOpenGL->renderTexture(button.iconTex, pos, {.a = a});
        offset += scaledButtonsPad + scaledButtonSize;

        bool currentBit = (m_iButtonHoverState & (1 << i)) != 0;
        if (hovering != currentBit) {
            m_iButtonHoverState ^= (1 << i);
            // damage to get rid of some artifacts when icons are "hidden"
            damageEntire();
        }
    }
}

void CHyprBar::draw(PHLMONITOR pMonitor, const float& a) {
    const auto ENABLED = g_pGlobalState->config.enabled->value();

    if (m_bLastEnabledState != ENABLED) {
        m_bLastEnabledState = ENABLED;
        g_pDecorationPositioner->repositionDeco(this);
    }

    if (m_hidden || !validMapped(m_pWindow) || !ENABLED)
        return;

    if (m_fRevealProgress->value() <= 0.004F)
        return;

    const auto PWINDOW = m_pWindow.lock();

    if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault())
        return;

    // gradual blur backdrop first, then the bar itself: the frosted band
    // samples the framebuffer before any bar pixel is committed
    if (g_pGlobalState->config.barGradualBlur->value()) {
        auto windowBox = monitorRelativeWindowBox(pMonitor);
        if (auto blurElement = makeGradualBlurElement(pMonitor, windowBox))
            g_pHyprRenderer->m_renderPass.add(std::move(blurElement));
    }

    auto data = CBarPassElement::SBarData{this, a};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(data));
}

UP<IPassElement> CHyprBar::makeGradualBlurElement(PHLMONITOR pMonitor, const CBox& windowBoxScaled) {
    static auto PENABLEBLURGLOBAL = CConfigValue<Config::BOOL>("decoration:blur:enabled");
    if (!*PENABLEBLURGLOBAL)
        return nullptr;

    const auto PWINDOW     = m_pWindow.lock();
    if (!PWINDOW)
        return nullptr;

    const float PROGRESS    = m_fRevealProgress->value();
    const float strength    = std::clamp(PROGRESS * 1.8F, 0.F, 1.F);
    if (strength <= 0.004F)
        return nullptr;

    const auto  HEIGHT = g_pGlobalState->config.barHeight->value();
    const auto  REACH  = g_pGlobalState->config.barBlurReach->value();

    const double scaledBarHeight = sc<double>(HEIGHT) * pMonitor->m_scale;
    const double scaledReach     = sc<double>(REACH) * pMonitor->m_scale;
    const double scaledRound     = PWINDOW->rounding() > 1 ? (PWINDOW->rounding() - 1) * pMonitor->m_scale : 0.0;

    // the band edge follows the reveal (overshoot sinks it a touch further)
    double bandH = scaledBarHeight * std::clamp(PROGRESS, 0.F, 1.5F);
    bandH     = std::clamp(bandH, 1.0, static_cast<double>(sc<double>(windowBoxScaled.h)));
    double reach = std::clamp(scaledReach, 0.0, std::max(0.0, windowBoxScaled.h - bandH));

    if (bandH + reach < 2)
        return nullptr;

    // CTextureMatteElement maps the blurred texture onto the box, so the box
    // must be the full monitor (aura-blur does the same); the band lives in
    // the matte alpha only
    CTitlebarGradualBlurElement::SBlurData data;
    data.monitor  = pMonitor;
    data.deco     = this;
    data.fullBox  = {0.0, 0.0, pMonitor->m_transformedSize.x, pMonitor->m_transformedSize.y};
    data.card     = {windowBoxScaled.x, windowBoxScaled.y, windowBoxScaled.w, bandH};
    data.round    = scaledRound;
    data.reach    = reach;
    data.strength = strength;

    // regen key: descriptor mixing so untouched redraws re-use the last matte
    auto mix = [&data](uint64_t h, double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        return h * 0x100000001b3ULL ^ bits;
    };
    uint64_t key = 0xcbf29ce484222325ULL;
    key = mix(key, windowBoxScaled.x);
    key = mix(key, windowBoxScaled.y);
    key = mix(key, windowBoxScaled.w);
    key = mix(key, bandH);
    key = mix(key, reach);
    key = mix(key, scaledRound);
    key = mix(key, std::round(strength * 64.F)); // quantized during animation
    key = mix(key, sc<double>(pMonitor->m_scale));
    data.gen = key;

    return makeUnique<CTitlebarGradualBlurElement>(data);
}

void CHyprBar::renderPass(PHLMONITOR pMonitor, const float& a) {
    const auto  PWINDOW = m_pWindow.lock();

    static auto PENABLEBLURGLOBAL = CConfigValue<Config::BOOL>("decoration:blur:enabled");
    const auto  BARCOLOR          = g_pGlobalState->config.barColor->value();
    const auto  HEIGHT            = g_pGlobalState->config.barHeight->value();
    const auto  ALIGNBUTTONS      = g_pGlobalState->config.barButtonsAlignment->value();
    const auto  ENABLETITLE       = g_pGlobalState->config.barTitleEnabled->value();
    const auto  ENABLEBLUR        = g_pGlobalState->config.barBlur->value();
    const auto  INACTIVECOLOR     = g_pGlobalState->config.inactiveButtonColor->value();

    if (INACTIVECOLOR > 0) {
        bool currentWindowFocus = PWINDOW == Desktop::focusState()->window();
        if (currentWindowFocus != m_bWindowHasFocus) {
            m_bWindowHasFocus = currentWindowFocus;
            m_bButtonsDirty   = true;
        }
    }

    const CHyprColor DEST_COLOR = m_bForcedBarColor.value_or(configColor(BARCOLOR));
    if (DEST_COLOR != m_cRealBarColor->goal())
        *m_cRealBarColor = DEST_COLOR;

    CHyprColor color = m_cRealBarColor->value();

    // reveal progress: 0 = hidden (fully above the window top edge, clipped
    // out), 1 = resting on the strip; spring overshoot > 1 pushes the bar
    // slightly past its resting spot before it settles back
    const float PROGRESS    = m_fRevealProgress->value();
    const float RENDERALPHA = std::clamp(PROGRESS * 1.8F, 0.F, 1.F);

    if (HEIGHT < 1 || PROGRESS <= 0.004F)
        return;

    color.a *= a * RENDERALPHA;

    const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
    const bool GRADUALBLUR  = g_pGlobalState->config.barGradualBlur->value() && *PENABLEBLURGLOBAL;
    const bool SHOULDBLUR   = ENABLEBLUR && *PENABLEBLURGLOBAL && color.a < 1.F && !GRADUALBLUR;

    const auto PWORKSPACE      = PWINDOW->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !PWINDOW->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    // the bar lies inside the window box, so its curve must be the inner
    // curve of the user's rounding — never the outer (rounding + border)
    // curve stock hyprbars used when it sat on top of the border
    const auto ROUNDR = PWINDOW->rounding();

    // the stencil box below is inset by 1 px, so its radius shrinks by 1 too
    const auto scaledRounding = ROUNDR > 1 ? (ROUNDR - 1) * pMonitor->m_scale : 0;

    m_seExtents = {{0, 0}, {0, 0}};

    // window box in monitor-local space; the bar covers only its top strip
    CBox windowBox = monitorRelativeWindowBox(pMonitor);

    if (windowBox.w < 1 || windowBox.h < 1)
        return;

    const auto scaledBarHeight = sc<double>(HEIGHT) * pMonitor->m_scale;

    // slide geometry: at PROGRESS=p the bar spans
    //   [windowTop - (1 - p) * H, windowTop + p * H]
    // so it emerges from the top edge and, with overshoot, sinks slightly
    // past the strip before settling — sliding down over the app window
    CBox barBox = {windowBox.x, windowBox.y - (1.F - PROGRESS) * scaledBarHeight, windowBox.w, scaledBarHeight};
    barBox.round();

    if (barBox.w < 1 || barBox.h < 1)
        return;

    // clip to the window box: never paints into gaps, over the border, or
    // outside the window's rounded corners
    g_pHyprOpenGL->scissor(windowBox);

    if (ROUNDR > 1) {
        CBox stencilBox = {windowBox.x + 1, windowBox.y + 1, windowBox.w - 2, windowBox.h - 2};

        if (stencilBox.w < 1 || stencilBox.h < 1)
            return;

        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);

        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, true);

        glStencilFunc(GL_ALWAYS, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        g_pHyprOpenGL->renderRect(stencilBox, CHyprColor(0, 0, 0, 0), {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glStencilFunc(GL_NOTEQUAL, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }

    if (SHOULDBLUR)
        g_pHyprOpenGL->renderRect(barBox, color, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower(), .blur = true, .blurA = a * RENDERALPHA});
    else
        g_pHyprOpenGL->renderRect(barBox, color, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});

    // render title
    if (ENABLETITLE && (m_szLastTitle != PWINDOW->m_title || m_bWindowSizeChanged || !m_pTextTex || m_pTextTex->m_texID == 0 || m_bTitleColorChanged)) {
        m_szLastTitle = PWINDOW->m_title;
        renderBarTitle(barBox.size(), pMonitor->m_scale);
    }

    if (ROUNDR > 1) {
        // cleanup stencil
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
        glStencilMask(-1);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
    }

    const auto BARBUF = barBox.size();

    CBox textBox = {barBox.x, barBox.y, (int)BARBUF.x, (int)BARBUF.y};
    if (ENABLETITLE && m_pTextTex) {
        const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
        const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
        const auto ALIGN            = g_pGlobalState->config.barTextAlign->value();

        float      buttonSizes = BARBUTTONPADDING;
        for (auto& b : g_pGlobalState->buttons) {
            buttonSizes += b.size + BARBUTTONPADDING;
        }

        const auto scaledButtonsSize = buttonSizes * pMonitor->m_scale;
        const auto scaledBarPadding  = BARPADDING * pMonitor->m_scale;
        const auto xOffset           = ALIGN == "left" ? std::round(scaledBarPadding + (BUTTONSRIGHT ? 0 : scaledButtonsSize)) :
                                                         std::round(BARBUF.x / 2.0 - m_pTextTex->m_size.x / 2.0);
        const auto yOffset           = std::round((scaledBarHeight - m_pTextTex->m_size.y) / 2.0);
        CBox       titleBox          = {barBox.x + xOffset, barBox.y + yOffset, m_pTextTex->m_size.x, m_pTextTex->m_size.y};

        g_pHyprOpenGL->renderTexture(m_pTextTex, titleBox, {.a = a * RENDERALPHA});
    }

    renderBarButtons(&textBox, pMonitor->m_scale, a * RENDERALPHA);
    m_bButtonsDirty = false;

    g_pHyprOpenGL->scissor(nullptr);

    renderBarButtonsText(&textBox, pMonitor->m_scale, a * RENDERALPHA);

    m_bWindowSizeChanged = false;
    m_bTitleColorChanged = false;

    (void)m_iLastHeight;
}

eDecorationType CHyprBar::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CHyprBar::updateWindow(PHLWINDOW pWindow) {
    damageEntire();
}

void CHyprBar::onConfigReloaded() {
    m_bButtonsDirty      = true;
    m_bTitleColorChanged = true;
    m_pTextTex           = nullptr;

    g_pDecorationPositioner->repositionDeco(this);
    damageEntire();
}

void CHyprBar::damageEntire() {
    // the bar overlays the window's top strip and can overshoot past it;
    // damaging the whole window box covers both states
    if (!validMapped(m_pWindow))
        return;

    CBox window = windowBoxGlobal();
    if (window.w < 1 || window.h < 1)
        return;

    window.expand(2);
    if (g_pGlobalState->config.barGradualBlur->value())
        window.h += g_pGlobalState->config.barBlurReach->value();
    g_pHyprRenderer->damageBox(window);
}

Vector2D CHyprBar::cursorRelativeToBar() {
    return g_pInputManager->getMouseCoordsInternal() - stripBoxGlobal().pos();
}

eDecorationLayer CHyprBar::getDecorationLayer() {
    // above the window surface (below popups) so the bar slides over content
    return DECORATION_LAYER_OVER;
}

uint64_t CHyprBar::getDecorationFlags() {
    return DECORATION_ALLOWS_MOUSE_INPUT;
}

CBox CHyprBar::windowBoxGlobal() {
    if (!validMapped(m_pWindow))
        return {};

    const auto PWINDOW = m_pWindow.lock();

    Vector2D pos  = PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + PWINDOW->m_floatingOffset;
    Vector2D size = PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    return {pos, size};
}
CBox CHyprBar::monitorRelativeWindowBox(PHLMONITOR pMonitor) {
    if (!validMapped(m_pWindow) || !pMonitor)
        return {};

    const auto PWINDOW         = m_pWindow.lock();
    const auto PWORKSPACE      = PWINDOW->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !PWINDOW->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    CBox box = {PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + PWINDOW->m_floatingOffset.x - pMonitor->m_position.x,
                PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y + PWINDOW->m_floatingOffset.y - pMonitor->m_position.y,
                PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x, PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y};

    box.translate(WORKSPACEOFFSET).scale(pMonitor->m_scale).round();
    return box;
}


CBox CHyprBar::stripBoxGlobal() {
    const auto BOX = windowBoxGlobal();
    if (BOX.w < 1 || BOX.h < 1)
        return {};

    const auto HEIGHT = std::min<double>(g_pGlobalState->config.barHeight->value(), BOX.h);

    CBox strip = {BOX.x, BOX.y, BOX.w, (int)HEIGHT};

    const auto PWORKSPACE      = m_pWindow->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !m_pWindow->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    return strip.translate(WORKSPACEOFFSET);
}

bool CHyprBar::stripContainsPoint(const Vector2D& coords) {
    const auto STRIP = stripBoxGlobal();
    if (STRIP.w < 1 || STRIP.h < 1)
        return false;

    // small padding above the top edge so moving up through the bar's top
    // edge doesn't instantly hide it
    return VECINRECT(coords, STRIP.x - 2, STRIP.y - 4, STRIP.x + STRIP.w + 2, STRIP.y + STRIP.h);
}

CBox CHyprBar::assignedBoxGlobal() {
    if (!validMapped(m_pWindow))
        return {};

    CBox box = m_bAssignedBox;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_pWindow.lock()));

    const auto PWORKSPACE      = m_pWindow->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !m_pWindow->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    return box.translate(WORKSPACEOFFSET);
}

PHLWINDOW CHyprBar::getOwner() {
    return m_pWindow.lock();
}

void CHyprBar::setReveal(bool want) {
    if (m_bRevealed == want)
        return;

    m_bRevealed = want;

    if (!validMapped(m_pWindow)) {
        m_fRevealProgress->setValueAndWarp(0.F);
        return;
    }

    *m_fRevealProgress = want ? 1.F : 0.F;
}

bool CHyprBar::shouldReveal(const Vector2D& coords) {
    if (!validMapped(m_pWindow) || m_hidden || !g_pGlobalState->config.enabled->value())
        return false;

    if (m_bPointerHeld || m_bDraggingThis)
        return true;

    const auto PWINDOW = m_pWindow.lock();

    // the bar also lives over maximized and fullscreen windows: it is the
    // only way out (close / minimize / restore) once a window covers them
    return stripContainsPoint(coords);
}

void CHyprBar::updateRules() {
    const auto PWINDOW              = m_pWindow.lock();
    auto       prevHidden           = m_hidden;
    auto       prevForcedTitleColor = m_bForcedTitleColor;

    m_bForcedBarColor   = std::nullopt;
    m_bForcedTitleColor = std::nullopt;
    m_hidden            = false;

    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->nobarRuleIdx))
        m_hidden = truthy(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->nobarRuleIdx)->effect);
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->barColorRuleIdx))
        m_bForcedBarColor = CHyprColor(Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->barColorRuleIdx)->effect).value_or(0));
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->titleColorRuleIdx))
        m_bForcedTitleColor = CHyprColor(Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->titleColorRuleIdx)->effect).value_or(0));

    if (prevHidden != m_hidden) {
        g_pDecorationPositioner->repositionDeco(this);
        m_bRevealed = false;
        m_fRevealProgress->setValueAndWarp(0.F);
    }
    if (prevForcedTitleColor != m_bForcedTitleColor)
        m_bTitleColorChanged = true;
}

void CHyprBar::damageOnButtonHover() {
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const bool BUTTONSRIGHT     = ALIGNBUTTONS != "left";

    float      offset = BARPADDING;

    const auto COORDS = cursorRelativeToBar();

    for (auto& b : g_pGlobalState->buttons) {
        const auto BARBUF     = Vector2D{(int)stripBoxGlobal().w, HEIGHT};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - BARBUTTONPADDING - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

        bool       hover = VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + b.size + BARBUTTONPADDING, currentPos.y + b.size);

        if (hover != m_bButtonHovered) {
            m_bButtonHovered = hover;
            damageEntire();
        }

        offset += BARBUTTONPADDING + b.size;
    }
}
