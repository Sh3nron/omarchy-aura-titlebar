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
#include <linux/input-event-codes.h>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using namespace Render::GL;

static CHyprColor configColor(Config::INTEGER color) {
    return CHyprColor{sc<uint64_t>(color)};
}

// 2026-grade button pop: per-button ease-out-back once off the reveal
// timeline (bouncy scale past 100% then settle), plus ease-out-cubic alpha
static double easeOutBack(double t) {
    constexpr double C1 = 1.70158;
    constexpr double C3 = C1 + 1;
    return 1 + C3 * std::pow(t - 1, 3) + C1 * std::pow(t - 1, 2);
}

static double easeOutCubic(double t) {
    return 1 - std::pow(1 - t, 3);
}

// size of each button on screen = user-configured size * global button scale
static float effectiveButtonSize(const SHyprButton& b) {
    return b.size * g_pGlobalState->config.barButtonScale->value();
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

    // Expand existing damage before beginRender collects it. Unlike scheduling
    // damage from draw(), this cannot create a continuous repaint loop. The
    // support belongs to our kernels, independent of the global blur radius.
    m_pRenderPreCallback = Event::bus()->m_events.render.pre.listen([this](PHLMONITOR monitor) {
        m_fullscreenDrawn = false;
        static auto blurEnabled = CConfigValue<Config::BOOL>("decoration:blur:enabled");
        if (!monitor->m_damage.hasChanged() || !validMapped(m_pWindow) || m_hidden ||
            m_fRevealProgress->value() <= .004F || !g_pGlobalState->config.enabled->value() ||
            !g_pGlobalState->config.barGradualBlur->value() || !*blurEnabled ||
            !g_pHyprRenderer->shouldRenderWindow(m_pWindow.lock(), monitor))
            return;
        auto box = monitorRelativeWindowBox(monitor);
        box.h = std::min(box.h, (g_pGlobalState->config.barHeight->value() * 1.5 +
                                std::max<int64_t>(0, g_pGlobalState->config.barBlurReach->value())) * monitor->m_scale);
        const double support = std::ceil(60 * std::clamp(g_pGlobalState->config.barBlurStrength->value(), 0.F, 4.F) * monitor->m_scale) + 2;
        box.expand(support);
        if (!monitor->m_damage.getBufferDamage(1).intersect(box).empty())
            monitor->m_damage.damage(box);
    });
    // Hyprland deliberately omits decoration draw() calls in true fullscreen.
    // Emit our overlay once after that window's surface has been scheduled.
    m_pFullscreenRenderCallback = Event::bus()->m_events.render.stage.listen([this](eRenderStage stage) {
        auto& rd = g_pHyprRenderer->m_renderData;
        if (stage != RENDER_POST_WINDOW || m_fullscreenDrawn || !validMapped(m_pWindow) ||
            rd.currentWindow != m_pWindow.lock() ||
            Fullscreen::controller()->getFullscreenModes(m_pWindow.lock()).internal != Fullscreen::FSMODE_FULLSCREEN)
            return;
        m_fullscreenDrawn = true;
        draw(rd.pMonitor.lock(), m_pWindow->alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * m_pWindow->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN));
    });
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
    if (e.button != BTN_LEFT)
        return;
    // Finish our own grab even if release occurs outside the window.
    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        if (m_bCancelledDown && !m_bTouchEv)
            handleUpEvent(info);
        return;
    }
    if (inputIsValid())
        handleDownEvent(info, std::nullopt);
}

void CHyprBar::onTouchDown(Event::SCallbackInfo& info, ITouch::SDownEvent e) {
    // Don't do anything if you're already grabbed a window with another finger
    if (!inputIsValid() || e.touchID != 0)
        return;

    handleDownEvent(info, e);
}

void CHyprBar::onTouchUp(Event::SCallbackInfo& info, ITouch::SUpEvent e) {
    if (!m_bCancelledDown || !m_bTouchEv || e.touchID != m_touchId)
        return;

    handleUpEvent(info);
}

void CHyprBar::onMouseMove(Vector2D coords) {
    // ensure proper redraws of button icons on hover when using hardware cursors
    damageOnButtonHover();

    if (g_pGlobalState->config.revealOnHover->value() && !m_bTouchEv) {
        const bool want = shouldReveal(g_pInputManager->getMouseCoordsInternal());
        if (want != m_bRevealed)
            setReveal(want);
    }

    if (!m_bDragPending || m_bTouchEv || !validMapped(m_pWindow) || m_touchId != 0)
        return;

    if ((g_pInputManager->getMouseCoordsInternal() - m_pressPointer).distance(Vector2D{}) < 4.0)
        return;
    m_bDragPending = false;
    handleMovement();
}

void CHyprBar::onTouchMove(Event::SCallbackInfo& info, ITouch::SMotionEvent e) {
    if (!m_bCancelledDown || !m_bTouchEv || !validMapped(m_pWindow) || e.touchID != m_touchId)
        return;
    auto monitor = m_pWindow->m_monitor.lock();
    if (!monitor)
        return;
    const Vector2D pointer = monitor->m_position + e.pos * monitor->m_size;
    // Release hit testing uses the latest touch position, not the mouse.
    if (m_pressedButton >= 0) {
        syncButtonAnimations();
        if (m_pressedButton < 0)
            return;
        const bool inside = buttonAt(pointer - stripBoxGlobal().pos()) == m_pressedButton;
        *m_buttonAnimations[m_pressedButton].press = inside ? 1.F : 0.F;
        m_touchPointer = pointer;
        info.cancelled = true;
        return;
    }
    if (!m_bDraggingThis && (pointer - m_pressPointer).distance(Vector2D{}) >= 4.0) {
        if (!detachForDrag(pointer))
            return;
        m_bDraggingThis = true;
        m_bDragPending = false;
    }
    if (m_bDraggingThis) {
        const CBox box{m_pressWindow.pos() + pointer - m_pressPointer, m_pressWindow.size()};
        g_layoutManager->setTargetGeom(box, m_pWindow->layoutTarget());
        m_pWindow->layoutTarget()->warpPositionSize();
        info.cancelled = true;
    }
    m_touchPointer = pointer;
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
    m_pressPointer = COORDS + STRIPBOX.pos();
    m_touchPointer = m_pressPointer;
    m_pressWindow = windowBoxGlobal();
    m_bDragPending = false;

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
    if (!m_bCancelledDown)
        return;
    info.cancelled = true;
    std::string command;
    if (validMapped(m_pWindow) && m_pressedButton >= 0 && !m_hidden &&
        g_pGlobalState->config.enabled->value() && Desktop::focusState()->window() == m_pWindow.lock()) {
        const auto pointer = m_bTouchEv ? m_touchPointer : g_pInputManager->getMouseCoordsInternal();
        if (buttonAt(pointer - stripBoxGlobal().pos()) == m_pressedButton &&
            size_t(m_pressedButton) < g_pGlobalState->buttons.size())
            command = g_pGlobalState->buttons[m_pressedButton].cmd;
    }
    for (auto& animation : m_buttonAnimations)
        *animation.press = 0.F;
    m_pressedButton = -1;
    if (m_bDraggingThis && !m_bTouchEv && validMapped(m_pWindow) &&
        g_layoutManager->dragController()->target() == m_pWindow->layoutTarget())
        g_layoutManager->endDragTarget();
    m_bDraggingThis = false;
    m_bDragPending = false;
    m_bCancelledDown = false;
    m_bTouchEv = false;
    m_bPointerHeld = false;
    m_touchId = 0;
    m_lastMouseDown = command.empty() ? m_lastMouseDown : Time::steady_tp{};
    damageOnButtonHover();
    setReveal(shouldReveal(g_pInputManager->getMouseCoordsInternal()));
    if (!command.empty())
        Config::Supplementary::executor()->spawn(command);
}

bool CHyprBar::detachForDrag(const Vector2D& pointer) {
    if (!validMapped(m_pWindow))
        return false;
    const auto window = m_pWindow.lock();
    const auto target = window->layoutTarget();
    if (!target || !target->space())
        return false;
    // Native tiled dragging shrinks and centers. Enter its floating path with
    // the exact visible box instead, before it records the pointer anchor.
    if (Fullscreen::controller()->isFullscreen(window))
        Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE);
    target->rememberFloatingSize(m_pressWindow.size());
    if (!target->floating())
        g_layoutManager->changeFloatingMode(target);
    if (!target->floating())
        return false;
    const CBox box{m_pressWindow.pos() + pointer - m_pressPointer, m_pressWindow.size()};
    g_layoutManager->setTargetGeom(box, target);
    target->warpPositionSize();
    Desktop::windowState()->raise(window);
    m_lastMouseDown = Time::steady_tp{};
    return true;
}

void CHyprBar::handleMovement() {
    if (!validMapped(m_pWindow))
        return;

    const auto window = m_pWindow.lock();
    const auto target = window->layoutTarget();
    if (!target || !target->space())
        return;

    // fullscreen windows must leave fullscreen to move at all
    if (Fullscreen::controller()->isFullscreen(window))
        Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE);

    // Let the compositor's own drag controller run the show. It is
    // tiling-aware: a tiled window follows the cursor and, on drop,
    // re-tiles — swapping with a neighbor when you drag it over one —
    // instead of staying floating. Floating windows just move. Pre-floating
    // the window from the plugin (as detachForDrag does for touch) defeats
    // that re-tile, which regressed tiled grabbing to permanent float.
    g_layoutManager->beginDragTarget(target, MBIND_MOVE, std::nullopt, true);
    m_bDraggingThis = g_layoutManager->dragController()->target() == target;
}

void CHyprBar::syncButtonAnimations() {
    while (m_buttonAnimations.size() < g_pGlobalState->buttons.size()) {
        SButtonAnimation state;
        for (auto* value : {&state.hover, &state.press}) {
            Animation::mgr()->createAnimation(0.F, *value, Config::animationTree()->getAnimationPropertyConfig("fadeIn"), m_pWindow.lock(), AVARDAMAGE_NONE);
            (*value)->setUpdateCallback([this](auto) { damageEntire(); });
        }
        m_buttonAnimations.emplace_back(std::move(state));
    }
    m_buttonAnimations.resize(g_pGlobalState->buttons.size());
    if (m_pressedButton >= int(m_buttonAnimations.size()))
        m_pressedButton = -1;
}

int CHyprBar::buttonAt(const Vector2D& local) {
    const auto padding = g_pGlobalState->config.barPadding->value();
    const auto gap = g_pGlobalState->config.barButtonPadding->value();
    const auto height = g_pGlobalState->config.barHeight->value();
    const auto strip = stripBoxGlobal();
    const bool right = g_pGlobalState->config.barButtonsAlignment->value() != "left";
    const auto count = getVisibleButtonCount(gap, padding, strip.size(), 1.F);
    float offset = padding;
    for (size_t i = 0; i < count; ++i) {
        const float size = effectiveButtonSize(g_pGlobalState->buttons[i]);
        const float x = right ? strip.w - offset - size : offset;
        const float y = (height - size) / 2.F;
        // Stable hit boxes match the resting circles, with half the gap as slop.
        if (VECINRECT(local, x - gap / 2.F, y - 2, x + size + gap / 2.F, y + size + 2))
            return int(i);
        offset += size + gap;
    }
    return -1;
}

float CHyprBar::buttonInteractionScale(size_t index) {
    syncButtonAnimations();
    return 1.F + .12F * std::clamp(m_buttonAnimations[index].hover->value(), 0.F, 1.F)
               - .22F * std::clamp(m_buttonAnimations[index].press->value(), 0.F, 1.F);
}

bool CHyprBar::doButtonPress(Config::INTEGER, Config::INTEGER, Config::INTEGER, Vector2D local, bool) {
    syncButtonAnimations();
    m_pressedButton = buttonAt(local);
    if (m_pressedButton < 0)
        return false;
    *m_buttonAnimations[m_pressedButton].press = 1.F;
    *m_buttonAnimations[m_pressedButton].hover = 1.F;
    m_lastMouseDown = Time::steady_tp{};
    return true;
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
        buttonSizes += effectiveButtonSize(b) + BARBUTTONPADDING;
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
        const float buttonSpace = (effectiveButtonSize(button) + barButtonPadding) * scale;
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

    const float PROGRESS        = std::clamp(m_fRevealProgress->value(), 0.F, 1.F);
    const bool  POP             = g_pGlobalState->config.barButtonsPop->value();
    const double STAGGER        = 0.12; // of the reveal timeline, per button
    const double SPAN           = 0.45; // each button animates over this slice

    int        offset = BARPADDING * scale;
    for (size_t i = 0; i < visibleCount; ++i) {
        auto&      button           = g_pGlobalState->buttons[i];
        const auto scaledButtonSize = effectiveButtonSize(button) * scale;
        const auto scaledButtonsPad = BARBUTTONPADDING * scale;

        // staggered pop-in: close leads, then maximize, then minimize;
        // with pop disabled every button simply rides the bar's own alpha
        double pop = POP ? std::clamp((PROGRESS - STAGGER * sc<double>(i)) / SPAN, 0.0, 1.0) : std::clamp(PROGRESS, 0.F, 1.F);
        const double scaleFrac = (POP ? std::clamp(0.6 + 0.4 * easeOutBack(pop), 0.02, 1.35) : 1.0) * buttonInteractionScale(i);
        const float  popAlpha  = POP ? sc<float>(easeOutCubic(pop)) : 1.F;

        auto       color = button.bgcol;

        if (INACTIVECOLOR > 0) {
            color = m_bWindowHasFocus ? color : configColor(INACTIVECOLOR);
            if (INVALIDATEICONS && button.userfg && button.iconTex)
                button.iconTex = nullptr;
        }

        const float hover = std::clamp(m_buttonAnimations[i].hover->value(), 0.F, 1.F);
        color.r += (1.F - color.r) * .10F * hover;
        color.g += (1.F - color.g) * .10F * hover;
        color.b += (1.F - color.b) * .10F * hover;
        color.a *= popAlpha * a;

        const float renderSize    = sc<float>(scaledButtonSize * scaleFrac);
        const float centerOffsetX = BUTTONSRIGHT ? barBox->w - offset - scaledButtonSize + scaledButtonSize / 2.0 : offset + scaledButtonSize / 2.0;
        CBox buttonBox = {barBox->x + centerOffsetX - renderSize / 2.0, barBox->y + barBox->h / 2.0 - renderSize / 2.0, renderSize, renderSize};
        buttonBox.round();

        g_pHyprOpenGL->renderRect(buttonBox, color, {.round = static_cast<int>(std::round(renderSize / 2.0)), .roundingPower = 2.F});

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

    const float PROGRESS        = std::clamp(m_fRevealProgress->value(), 0.F, 1.F);
    const bool  POP             = g_pGlobalState->config.barButtonsPop->value();
    const double STAGGER        = 0.12;
    const double SPAN           = 0.45;

    int        offset        = BARPADDING * scale;
    float      noScaleOffset = BARPADDING;

    for (size_t i = 0; i < visibleCount; ++i) {
        auto&      button           = g_pGlobalState->buttons[i];
        const auto scaledButtonSize = effectiveButtonSize(button) * scale;
        const auto scaledButtonsPad = BARBUTTONPADDING * scale;

        double pop = POP ? std::clamp((PROGRESS - STAGGER * sc<double>(i)) / SPAN, 0.0, 1.0) : std::clamp(PROGRESS, 0.F, 1.F);
        const double scaleFrac = (POP ? std::clamp(0.6 + 0.4 * easeOutBack(pop), 0.02, 1.35) : 1.0) * buttonInteractionScale(i);
        const float  popAlpha  = POP ? sc<float>(easeOutCubic(pop)) : 1.F;

        if ((!button.iconTex || button.iconTex->m_texID == 0) && !button.icon.empty()) {
            // render icon
            auto fgcol = button.userfg ? button.fgcol : (button.bgcol.r + button.bgcol.g + button.bgcol.b < 1) ? CHyprColor(0xFFFFFFFF) : CHyprColor(0xFF000000);

            button.iconTex = g_pHyprRenderer->renderText(button.icon, fgcol, std::round(effectiveButtonSize(button) * 0.62 * scale), false, "sans", scaledButtonSize);
        }

        if (!button.iconTex || button.iconTex->m_texID == 0)
            continue;

        const float centerX = barBox->x + (BUTTONSRIGHT ? barBox->width - offset - scaledButtonSize / 2.0 : offset + scaledButtonSize / 2.0);
        const float centerY = barBox->y + barBox->height / 2.0;
        const float glyphSize = sc<float>(button.iconTex->m_size.x * scaleFrac);
        const float glyphH    = sc<float>(button.iconTex->m_size.y * scaleFrac);
        CBox        pos       = {centerX - glyphSize / 2.0, centerY - glyphH / 2.0, glyphSize, glyphH};

        if (!ICONONHOVER || buttonAt(COORDS) >= 0 || m_pressedButton >= 0)
            g_pHyprOpenGL->renderTexture(button.iconTex, pos, {.a = popAlpha * a});
        offset += scaledButtonsPad + scaledButtonSize;


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
        if (auto blurElement = makeGradualBlurElement(pMonitor, windowBox, a))
            g_pHyprRenderer->m_renderPass.add(std::move(blurElement));
    }

    auto data = CBarPassElement::SBarData{this, a};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(data));
}

UP<IPassElement> CHyprBar::makeGradualBlurElement(PHLMONITOR pMonitor, const CBox& windowBoxScaled, float opacity) {
    static auto PENABLEBLURGLOBAL = CConfigValue<Config::BOOL>("decoration:blur:enabled");
    if (!*PENABLEBLURGLOBAL || m_blurResources.failed)
        return nullptr;
    const auto window = m_pWindow.lock();
    if (!window || windowBoxScaled.w <= 2 || windowBoxScaled.h <= 2)
        return nullptr;
    const float progress = std::clamp(m_fRevealProgress->value(), 0.F, 1.5F);
    const double height = std::min<double>(windowBoxScaled.h, g_pGlobalState->config.barHeight->value() * pMonitor->m_scale * progress);
    if (height <= 0 || opacity <= .004F)
        return nullptr;
    const auto targetColor = m_bForcedBarColor.value_or(configColor(g_pGlobalState->config.barColor->value()));
    if (targetColor != m_cRealBarColor->goal())
        *m_cRealBarColor = targetColor;
    auto tint = m_cRealBarColor->value();
    tint.a *= std::clamp(g_pGlobalState->config.barTintOpacity->value(), 0.F, 1.F);
    CTitlebarGradualBlurElement::SBlurData data{
        .monitor = pMonitor,
        .resources = &m_blurResources,
        .window = windowBoxScaled,
        .height = height,
        .reach = std::clamp(double(g_pGlobalState->config.barBlurReach->value()) * pMonitor->m_scale, 0.0, windowBoxScaled.h - height),
        .round = Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN ? 0.0 : std::max(0.0, double(window->rounding()) * pMonitor->m_scale),
        .roundingPower = window->roundingPower(),
        .strength = std::clamp(g_pGlobalState->config.barBlurStrength->value(), 0.F, 4.F),
        .opacity = opacity * std::clamp(progress * 1.8F, 0.F, 1.F),
        .tint = tint,
    };
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
    const bool GRADUALBLUR  = g_pGlobalState->config.barGradualBlur->value() && *PENABLEBLURGLOBAL && !m_blurResources.failed;
    const bool SHOULDBLUR   = ENABLEBLUR && *PENABLEBLURGLOBAL && color.a < 1.F && !GRADUALBLUR;

    const auto PWORKSPACE      = PWINDOW->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !PWINDOW->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    // the bar lies inside the window box, so its curve must be the inner
    // curve of the user's rounding — never the outer (rounding + border)
    // curve stock hyprbars used when it sat on top of the border
    const auto ROUNDR = Fullscreen::controller()->getFullscreenModes(PWINDOW).internal == Fullscreen::FSMODE_FULLSCREEN ? 0 : PWINDOW->rounding();

    const int scaledRounding = std::max(0, int(std::round(ROUNDR * pMonitor->m_scale)));

    m_seExtents = {{0, 0}, {0, 0}};

    // window box in monitor-local space; the bar covers only its top strip
    CBox windowBox = monitorRelativeWindowBox(pMonitor);

    if (windowBox.w <= 2 || windowBox.h <= 2)
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
    auto& renderData = g_pHyprRenderer->m_renderData;
    const auto savedClip = renderData.clipBox;
    renderData.clipBox = savedClip.w > 0 && savedClip.h > 0 ? savedClip.intersection(windowBox) : windowBox;
    Hyprutils::Utils::CScopeGuard restoreClip([&] { renderData.clipBox = savedClip; });
    if (renderData.clipBox.w <= 0 || renderData.clipBox.h <= 0)
        return;
    g_pHyprOpenGL->scissor(renderData.clipBox);

    if (ROUNDR > 1) {
        CBox stencilBox = windowBox;

        if (stencilBox.w < 1 || stencilBox.h < 1)
            return;

        glClearStencil(0);
        glStencilMask(0xFF);
        glClear(GL_STENCIL_BUFFER_BIT);

        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, true);

        glStencilFunc(GL_ALWAYS, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        g_pHyprOpenGL->renderRect(stencilBox, CHyprColor(0, 0, 0, 0), {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glStencilFunc(GL_EQUAL, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    }

    if (SHOULDBLUR)
        g_pHyprOpenGL->renderRect(barBox, color, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower(), .blur = true, .blurA = a * RENDERALPHA});
    else if (!GRADUALBLUR)
        g_pHyprOpenGL->renderRect(barBox, color, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});

    // render title
    if (ENABLETITLE && (m_szLastTitle != PWINDOW->m_title || m_bWindowSizeChanged || !m_pTextTex || m_pTextTex->m_texID == 0 || m_bTitleColorChanged)) {
        m_szLastTitle = PWINDOW->m_title;
        renderBarTitle(barBox.size(), pMonitor->m_scale);
    }

    const auto BARBUF = barBox.size();

    CBox textBox = {barBox.x, barBox.y, (int)BARBUF.x, (int)BARBUF.y};
    if (ENABLETITLE && m_pTextTex) {
        const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
        const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
        const auto ALIGN            = g_pGlobalState->config.barTextAlign->value();

        float      buttonSizes = BARBUTTONPADDING;
        for (auto& b : g_pGlobalState->buttons) {
            buttonSizes += effectiveButtonSize(b) + BARBUTTONPADDING;
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

    renderBarButtonsText(&textBox, pMonitor->m_scale, a * RENDERALPHA);

    if (ROUNDR > 1) {
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
        glStencilMask(-1);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
    }
    g_pHyprOpenGL->scissor(nullptr);

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

    // keep-alive zone: the full strip. small padding above the top edge so
    // moving up through the bar's top edge doesn't instantly hide it
    return VECINRECT(coords, STRIP.x - 2, STRIP.y - 4, STRIP.x + STRIP.w + 2, STRIP.y + STRIP.h);
}

bool CHyprBar::triggerZoneContainsPoint(const Vector2D& coords) {
    const auto STRIP = stripBoxGlobal();
    if (STRIP.w < 1 || STRIP.h < 1)
        return false;

    // reveal trigger: `bar_hover_zone` deep from the window's top edge
    // (10px by default). Chrome-style app UI buttons live ~20-40px deep;
    // the bar must demand a deliberate dip.
    const auto ZONE = std::clamp<Config::INTEGER>(g_pGlobalState->config.barHoverZone->value(), 0, STRIP.h);
    return VECINRECT(coords, STRIP.x - 2, STRIP.y - 4, STRIP.x + STRIP.w + 2, STRIP.y + ZONE);
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

    // trigger only from the top half of the strip; once already revealed,
    // the full strip keeps the bar alive
    if (m_bRevealed)
        return stripContainsPoint(coords);

    // the bar also lives over maximized and fullscreen windows: it is the
    // only way out (close / minimize / restore) once a window covers them
    return triggerZoneContainsPoint(coords);
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
    syncButtonAnimations();
    const int hovered = validMapped(m_pWindow) && barAcceptsInput() && inputIsValid()
        ? buttonAt(cursorRelativeToBar()) : -1;
    for (size_t i = 0; i < m_buttonAnimations.size(); ++i) {
        const float hover = int(i) == hovered ? 1.F : 0.F;
        const float press = int(i) == m_pressedButton && int(i) == hovered ? 1.F : 0.F;
        if (m_buttonAnimations[i].hover->goal() != hover)
            *m_buttonAnimations[i].hover = hover;
        if (m_buttonAnimations[i].press->goal() != press)
            *m_buttonAnimations[i].press = press;
    }
}
