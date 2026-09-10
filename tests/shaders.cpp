// Standalone regression test of the production GLSL, without loading a plugin.
// c++ -std=c++23 tests/shaders.cpp -lEGL -lGLESv2 -o /tmp/aura-shader-test
#include "../BlurShaders.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static GLuint program(const char* fragment) {
    GLuint p = glCreateProgram();
    for (auto type : {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER}) {
        GLuint s = glCreateShader(type);
        const char* source = type == GL_VERTEX_SHADER ? AuraBlurShaders::vertex : fragment;
        glShaderSource(s, 1, &source, nullptr);
        glCompileShader(s);
        GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof(log), nullptr, log); std::puts(log); }
        check(ok, "production shader compilation");
        glAttachShader(p, s); glDeleteShader(s);
    }
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    check(ok, "production shader link");
    return p;
}
int main() {
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    check(eglInitialize(display, nullptr, nullptr), "EGL initialization");
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint configAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config; EGLint count;
    check(eglChooseConfig(display, configAttrs, &config, 1, &count) && count, "EGL config");
    const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs);
    check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context), "EGL context");
    std::printf("Renderer: %s\n", glGetString(GL_RENDERER));
    const GLuint blur = program(AuraBlurShaders::blur), composite = program(AuraBlurShaders::composite);
    constexpr int W = 768, H = 192;
    std::vector<unsigned char> original(W * H * 4);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        int i = (y * W + x) * 4;
        original[i] = original[i+1] = original[i+2] = (x / 4) % 2 ? 240 : 20;
        original[i+3] = 255;
    }
    GLuint tex[3], fb[3], vao, vbo;
    glGenTextures(3, tex); glGenFramebuffers(3, fb);
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, original.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fb[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer allocation");
    }
    const float quad[] = {1,0,0,0,1,1,0,1};
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr); glEnableVertexAttribArray(0);
    glViewport(0, 0, W, H);
    auto uniform = [](GLuint p, const char* name) { return glGetUniformLocation(p, name); };
    auto render = [&](float reach, float radius, float opacity, float strength) {
        glBindTexture(GL_TEXTURE_2D, tex[0]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, original.data());
        for (int layer = 1; layer <= 5; ++layer) {
            glDisable(GL_BLEND); glUseProgram(blur);
            glUniform1i(uniform(blur,"tex"),0);
            glUniform2f(uniform(blur,"framebufferSize"),W,H);
            glUniform4f(uniform(blur,"sampleBounds"),0,0,W,H);
            glUniform1f(uniform(blur,"sigma"),(layer+1)*strength);
            for (int axis = 0; axis < 2; ++axis) {
                glBindFramebuffer(GL_FRAMEBUFFER,fb[axis+1]);
                glBindTexture(GL_TEXTURE_2D,tex[axis]);
                glUniform2f(uniform(blur,"direction"),axis==0,axis==1);
                glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            }
            glBindFramebuffer(GL_FRAMEBUFFER,fb[0]);
            glBindTexture(GL_TEXTURE_2D,tex[2]);
            glEnable(GL_BLEND); glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(composite);
            glUniform1i(uniform(composite,"tex"),0);
            glUniform2f(uniform(composite,"framebufferSize"),W,H);
            const float identity[] = {1,0,0,0,1,0,0,0,1};
            glUniformMatrix3fv(uniform(composite,"rawToLogical"),1,GL_FALSE,identity);
            glUniform4f(uniform(composite,"windowBox"),0,0,W,H);
            glUniform1f(uniform(composite,"radius"),radius);
            glUniform1f(uniform(composite,"roundingPower"),4);
            glUniform1f(uniform(composite,"bandHeight"),28);
            glUniform1f(uniform(composite,"reach"),reach);
            glUniform1f(uniform(composite,"opacity"),opacity);
            glUniform1i(uniform(composite,"layer"),layer);
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        }
        std::vector<unsigned char> output(original.size());
        glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,output.data());
        check(glGetError()==GL_NO_ERROR,"render without GL errors");
        return output;
    };
    auto contrast = [](const auto& pixels, int y) {
        int lo=255,hi=0;
        for(int x=W/2-48;x<W/2+48;++x){int v=pixels[(y*W+x)*4];lo=std::min(lo,v);hi=std::max(hi,v);}
        return hi-lo;
    };
    auto result=render(96,18,1,2);
    check(contrast(result,10)<4,"strong frost suppresses fine stripes without aliasing");
    check(contrast(result,65)<contrast(result,112),"detail returns toward the clear edge");
    for(int y=125;y<H;++y)for(int x=0;x<W*4;++x)
        check(result[y*W*4+x]==original[y*W*4+x],"content beyond reach is unchanged");
    result=render(96,70,1,2);
    check(result[0]==original[0],"rounded corner excludes blur");
    result=render(0,0,1,2);
    check(contrast(result,10)<4 && contrast(result,30)==220,"zero reach has a finite strip-only result");
    result=render(96,18,0,2);
    check(result==original,"hidden effect changes no pixels");
    result=render(96,0,1,8); // strength 4 at scale 2
    check(contrast(result,10)<4,"high DPI kernels remain free of periodic striping");
    std::puts("PASS: compile, progressive softness, clear edge, corners, zero reach, hidden state, high DPI");
    glDeleteBuffers(1,&vbo);glDeleteVertexArrays(1,&vao);glDeleteTextures(3,tex);glDeleteFramebuffers(3,fb);
    glDeleteProgram(blur);glDeleteProgram(composite);
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context);eglTerminate(display);
}
