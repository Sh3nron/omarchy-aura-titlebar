#include "TitlebarBlur.hpp"
#include "BlurShaders.hpp"
#include "BarPassElement.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <algorithm>
#include <cmath>

namespace {
// Raw GL changes are local, including failure paths. Restore the actual state
// so Hyprland's cached program/capability state still agrees with the driver.
struct GLState {
    GLint program, vao, arrayBuffer, drawFB, readFB, activeTexture, texture0;
    GLint viewport[4], scissor[4], srcRGB, dstRGB, srcA, dstA, eqRGB, eqA;
    GLboolean blend, stencil, scissorEnabled, colorMask[4];
    GLState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFB);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqA);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        blend = glIsEnabled(GL_BLEND);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    }
    ~GLState() {
        glUseProgram(program);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFB);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        glBlendFuncSeparate(srcRGB, dstRGB, srcA, dstA);
        glBlendEquationSeparate(eqRGB, eqA);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        (blend ? glEnable : glDisable)(GL_BLEND);
        (stencil ? glEnable : glDisable)(GL_STENCIL_TEST);
        (scissorEnabled ? glEnable : glDisable)(GL_SCISSOR_TEST);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture0);
        glActiveTexture(activeTexture);
    }
};
void use(CShader& shader, int width, int height) {
    glUseProgram(shader.program());
    glBindVertexArray(shader.getUniformLocation(SHADER_SHADER_VAO));
    glUniform1i(glGetUniformLocation(shader.program(), "tex"), 0);
    glUniform2f(glGetUniformLocation(shader.program(), "framebufferSize"), width, height);
}
void scissorBox(const CBox& box) {
    glScissor(std::floor(box.x), std::floor(box.y),
              std::max(0, int(std::ceil(box.x + box.w) - std::floor(box.x))),
              std::max(0, int(std::ceil(box.y + box.h) - std::floor(box.y))));
}
}

std::vector<UP<IPassElement>> CTitlebarGradualBlurElement::draw() {
    auto& rd = g_pHyprRenderer->m_renderData;
    auto& resources = *m_data.resources;
    if (!rd.currentFB || m_data.opacity <= .004 || resources.failed)
        return {};

    GLState state;
    if (!resources.blur) {
        resources.blur = makeUnique<CShader>();
        resources.composite = makeUnique<CShader>();
        // Dynamic compilation returns an error instead of aborting Hyprland.
        if (!resources.blur->createProgram(AuraBlurShaders::vertex, AuraBlurShaders::blur, true, true) ||
            !resources.composite->createProgram(AuraBlurShaders::vertex, AuraBlurShaders::composite, true, true)) {
            resources.failed = true;
            return {};
        }
    }
    const int width = rd.currentFB->m_size.x, height = rd.currentFB->m_size.y;
    for (auto* fb : {&resources.horizontal, &resources.vertical}) {
        if (!*fb || (*fb)->m_size != rd.currentFB->m_size || (*fb)->m_drmFormat != rd.currentFB->m_drmFormat) {
            *fb = g_pHyprRenderer->createFB("aura-titlebar-blur");
            if (!*fb || !(*fb)->alloc(width, height, rd.currentFB->m_drmFormat)) {
                resources.failed = true;
                return {};
            }
            (*fb)->setImageDescription(rd.currentFB->imageDescription());
            (*fb)->getTexture()->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            (*fb)->getTexture()->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            (*fb)->getTexture()->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            (*fb)->getTexture()->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }

    const auto transform = Math::wlTransformToHyprutils(Math::invertTransform(m_data.monitor->m_transform));
    auto toRaw = [&](CBox box) {
        return box.transform(transform, m_data.monitor->m_transformedSize.x, m_data.monitor->m_transformedSize.y);
    };
    // Orthogonal output transforms have an inverse equal to their transpose.
    const auto origin = toRaw({0, 0, 0, 0}).pos();
    const auto x = toRaw({1, 0, 0, 0}).pos() - origin;
    const auto y = toRaw({0, 1, 0, 0}).pos() - origin;
    const GLfloat inverse[9] = {float(x.x), float(y.x), 0, float(x.y), float(y.y), 0,
        float(-x.x * origin.x - x.y * origin.y), float(-y.x * origin.x - y.y * origin.y), 1};
    CBox effect = m_data.window;
    effect.h = std::min(effect.h, m_data.height + m_data.reach);
    const CBox rawWindow = toRaw(m_data.window);
    const CBox rawEffect = toRaw(effect);
    CRegion outputDamage = rd.damage.copy();
    outputDamage.intersect(effect);
    if (rd.clipBox.w > 0 && rd.clipBox.h > 0)
        outputDamage.intersect(rd.clipBox);
    outputDamage.transform(transform, m_data.monitor->m_transformedSize.x, m_data.monitor->m_transformedSize.y);
    if (outputDamage.empty())
        return {};

    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    auto composite = [&](int layer) {
        rd.currentFB->bind();
        glViewport(0, 0, width, height);
        glEnable(GL_BLEND);
        use(*resources.composite, width, height);
        const auto program = resources.composite->program();
        auto loc = [&](const char* name) { return glGetUniformLocation(program, name); };
        glUniformMatrix3fv(loc("rawToLogical"), 1, GL_FALSE, inverse);
        glUniform4f(loc("windowBox"), m_data.window.x, m_data.window.y, m_data.window.w, m_data.window.h);
        glUniform1f(loc("radius"), m_data.round);
        glUniform1f(loc("roundingPower"), m_data.roundingPower);
        glUniform1f(loc("bandHeight"), m_data.height);
        glUniform1f(loc("reach"), m_data.reach);
        glUniform1f(loc("opacity"), m_data.opacity);
        glUniform1i(loc("layer"), layer);
        glUniform4f(loc("tint"), m_data.tint.r, m_data.tint.g, m_data.tint.b, m_data.tint.a);
        glBindTexture(GL_TEXTURE_2D, resources.vertical->getTexture()->m_texID);
        outputDamage.forEachRect([](const auto& rect) {
            glScissor(rect.x1, rect.y1, rect.x2 - rect.x1, rect.y2 - rect.y1);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        });
    };

    for (int layer = 1; layer <= 5 && m_data.strength > 0; ++layer) {
        const float sigma = (layer + 1) * m_data.strength * m_data.monitor->m_scale;
        // Each layer samples the preceding backdrop, never its own destination.
        // Render both intermediates over the effect plus kernel support.
        CBox work = rawEffect.copy().expand(std::ceil(3 * sigma) + 1);
        work = work.intersection({0, 0, double(width), double(height)});
        glDisable(GL_BLEND);
        for (int axis = 0; axis < 2; ++axis) {
            auto destination = axis == 0 ? resources.horizontal : resources.vertical;
            auto source = axis == 0 ? rd.currentFB : resources.horizontal;
            destination->bind();
            glViewport(0, 0, width, height);
            use(*resources.blur, width, height);
            const auto program = resources.blur->program();
            glUniform2f(glGetUniformLocation(program, "direction"), axis == 0 ? 1 : 0, axis == 1 ? 1 : 0);
            glUniform1f(glGetUniformLocation(program, "sigma"), sigma);
            glUniform4f(glGetUniformLocation(program, "sampleBounds"), rawWindow.x, rawWindow.y,
                        rawWindow.x + rawWindow.w, rawWindow.y + rawWindow.h);
            glBindTexture(GL_TEXTURE_2D, source->getTexture()->m_texID);
            scissorBox(work);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        composite(layer);
    }
    composite(0);
    // The blur pass engine may defer needsLiveBlur elements behind the
    // window's own pass elements; the bar must paint after this composite,
    // so hand it back as our child element.
    std::vector<UP<IPassElement>> result;
    result.emplace_back(makeUnique<CBarPassElement>(CBarPassElement::SBarData{m_data.resources->owner, sc<float>(m_data.passAlpha)}));
    return result;
}
