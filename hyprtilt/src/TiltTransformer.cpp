#include "TiltTransformer.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Hyprtilt {

    std::unordered_map<PHLWINDOWREF, SWindowSpringState> CTiltTransformer::s_springStates;

    std::unordered_map<uint64_t, CTiltTransformer::SOutputFBSlot> CTiltTransformer::s_outFBs;
    uint64_t                                                      CTiltTransformer::s_fboUseCounter = 0;

    std::atomic<uint64_t> CTiltTransformer::s_transformCalls{0};
    std::atomic<uint64_t> CTiltTransformer::s_transformWarps{0};
    std::atomic<float>    CTiltTransformer::s_lastRotX{0.0f};
    std::atomic<float>    CTiltTransformer::s_lastRotY{0.0f};
    std::atomic<float>    CTiltTransformer::s_lastScale{1.0f};

    GLuint CTiltTransformer::s_program           = 0;
    GLint  CTiltTransformer::s_uTexLoc           = -1;
    GLint  CTiltTransformer::s_uOutSizeLoc       = -1;
    GLint  CTiltTransformer::s_uWinCenterLoc     = -1;
    GLint  CTiltTransformer::s_uHalfSizeLoc      = -1;
    GLint  CTiltTransformer::s_uInOffsetLoc      = -1;
    GLint  CTiltTransformer::s_uInSizeLoc        = -1;
    GLint  CTiltTransformer::s_uKLoc             = -1;
    GLint  CTiltTransformer::s_uNormCenterLoc    = -1;
    GLint  CTiltTransformer::s_uNormHalfLoc      = -1;
    GLint  CTiltTransformer::s_uScaleLoc         = -1;
    GLint  CTiltTransformer::s_uBrightnessLoc    = -1;
    GLint  CTiltTransformer::s_uSpotlightLoc     = -1;
    GLint  CTiltTransformer::s_uOpacityLoc       = -1;
    GLint  CTiltTransformer::s_uEdgeMinLoc       = -1;
    GLuint CTiltTransformer::s_vao               = 0;
    GLuint CTiltTransformer::s_vbo               = 0;
    bool   CTiltTransformer::s_glInitialized     = false;

    // Two size slots (each double-buffered) are enough for a window pass plus
    // its blur matte and one in-flight resize, while bounding GPU memory.
    static constexpr size_t MAX_FBO_SLOTS = 2;

    static const char* VERTEX_SHADER_SRC = R"(#version 320 es
precision highp float;

layout(location = 0) in vec2 a_pos;
out vec2 v_screenUV;

void main() {
    // Hyprland renders top-down: NDC y = -1 is the first (top) row of the
    // framebuffer, so this also maps UV (0, 0) to the top-left texel.
    v_screenUV = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

    static const char* FRAGMENT_SHADER_SRC = R"(#version 320 es
precision highp float;

in vec2 v_screenUV;
uniform sampler2D u_tex;

uniform vec2  u_outSize;     // output canvas size in pixels
uniform vec2  u_winCenter;   // window centre in output-canvas-local pixels
uniform vec2  u_halfSize;    // window half-size in pixels
uniform vec2  u_inOffset;    // input canvas origin relative to output canvas, pixels
uniform vec2  u_inSize;      // input canvas size in pixels
uniform vec2  u_k;           // perspective tilt coefficients
uniform vec2  u_normCenter;  // affine centre that aligns the tilt with the window box
uniform vec2  u_normHalf;    // affine half-size that aligns the tilt with the window box
uniform float u_scale;       // optional uniform card shrink (1.0 = unchanged)
uniform float u_brightness;  // luminance multiplier (1.0 = unchanged)
uniform float u_edge_min;    // minimum edge feather factor (0 = feathered, 1 = hard edge)
uniform float u_opacity;     // output alpha multiplier (1.0 = unchanged)
uniform float u_spotlight;   // border spotlight intensity (0 = off, 1 = strong)

out vec4 fragColor;

void main() {
    // Output pixel position and window-relative offset in [-1, 1].
    vec2  p = v_screenUV * u_outSize;
    vec2  d = (p - u_winCenter) / u_halfSize;

    // Map the aligned output position back into raw perspective space, then
    // invert the perspective projection. u_normCenter/u_normHalf make the
    // projected quadrilateral's bounding box exactly match the original
    // window box, so the card stays centred and does not drift or change
    // apparent size.
    vec2  o = u_normCenter + d * u_normHalf / u_scale;
    float denom = 1.0 - dot(u_k, o);
    if (denom <= 0.001) {
        fragColor = vec4(0.0);
        return;
    }

    // Inverse perspective mapping back to the source window.
    vec2  us = o / denom;

    // Sub-pixel antialiased feather along the card edges.
    vec2  edgeDist   = (vec2(1.0) - abs(us)) * u_halfSize;
    float edgeFactor = clamp(min(edgeDist.x, edgeDist.y), 0.0, 1.0);
    edgeFactor = clamp(max(edgeFactor, u_edge_min), 0.0, 1.0);

    // Border spotlight: an additive rim glow confined to a thin band along the
    // card edge. It never touches the interior or the alpha channel, so the
    // window keeps Hyprland's own opacity and the wallpaper cannot bleed in.
    vec3 glow = vec3(0.0);
    if (u_spotlight > 0.001) {
        float edgePx    = min(edgeDist.x, edgeDist.y);
        float glowWidth = clamp(min(u_halfSize.x, u_halfSize.y) * 0.06, 4.0, 48.0);
        float rim       = 1.0 - smoothstep(0.0, glowWidth, edgePx);
        rim             = pow(clamp(rim, 0.0, 1.0), 2.0) * u_spotlight;
        glow            = vec3(rim * 0.65);
    }

    vec2 srcLocal = u_winCenter + us * u_halfSize;
    vec2 srcUV    = (srcLocal + u_inOffset) / u_inSize;
    if (srcUV.x < 0.0 || srcUV.x > 1.0 || srcUV.y < 0.0 || srcUV.y > 1.0) {
        fragColor = vec4(0.0);
        return;
    }

    // Hyprland's offscreen canvases are premultiplied. Recover straight colour,
    // apply the optional brightness compensation and the border spotlight, then
    // rebuild a premultiplied result so translucency still survives.
    vec4  col      = texture(u_tex, srcUV);
    vec3  straight = col.a > 0.001 ? clamp(col.rgb / col.a, 0.0, 1.0) : vec3(0.0);
    vec3  rgb      = clamp(straight * u_brightness, 0.0, 1.0);
    rgb            = 1.0 - (1.0 - rgb) * (1.0 - glow);
    float alpha    = clamp(col.a * edgeFactor * u_opacity, 0.0, 1.0);

    fragColor = vec4(rgb * alpha, alpha);
}
)";

    bool CTiltTransformer::initGL() {
        if (s_glInitialized)
            return true;

        // initGL() can run lazily during a render pass, so preserve the
        // bindings it touches instead of clobbering Hyprland's state.
        GLint prevVAO = 0, prevVBO = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevVBO);

        const auto fail = [&](const char* what, const char* log) {
            LOG(Log::ERR, "[hyprtilt] {} failed: {}", what, log);
            glBindVertexArray(prevVAO);
            glBindBuffer(GL_ARRAY_BUFFER, prevVBO);
        };

        GLuint vert = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vert, 1, &VERTEX_SHADER_SRC, nullptr);
        glCompileShader(vert);

        GLint ok = 0;
        glGetShaderiv(vert, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = {};
            glGetShaderInfoLog(vert, sizeof(log), nullptr, log);
            glDeleteShader(vert);
            fail("Vertex shader compilation", log);
            return false;
        }

        GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(frag, 1, &FRAGMENT_SHADER_SRC, nullptr);
        glCompileShader(frag);

        glGetShaderiv(frag, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = {};
            glGetShaderInfoLog(frag, sizeof(log), nullptr, log);
            glDeleteShader(vert);
            glDeleteShader(frag);
            fail("Fragment shader compilation", log);
            return false;
        }

        s_program = glCreateProgram();
        glAttachShader(s_program, vert);
        glAttachShader(s_program, frag);
        glLinkProgram(s_program);

        glGetProgramiv(s_program, GL_LINK_STATUS, &ok);
        glDeleteShader(vert);
        glDeleteShader(frag);

        if (!ok) {
            char log[512] = {};
            glGetProgramInfoLog(s_program, sizeof(log), nullptr, log);
            glDeleteProgram(s_program);
            s_program = 0;
            fail("Shader program linking", log);
            return false;
        }

        s_uTexLoc        = glGetUniformLocation(s_program, "u_tex");
        s_uOutSizeLoc    = glGetUniformLocation(s_program, "u_outSize");
        s_uWinCenterLoc  = glGetUniformLocation(s_program, "u_winCenter");
        s_uHalfSizeLoc   = glGetUniformLocation(s_program, "u_halfSize");
        s_uInOffsetLoc   = glGetUniformLocation(s_program, "u_inOffset");
        s_uInSizeLoc     = glGetUniformLocation(s_program, "u_inSize");
        s_uKLoc          = glGetUniformLocation(s_program, "u_k");
        s_uNormCenterLoc = glGetUniformLocation(s_program, "u_normCenter");
        s_uNormHalfLoc   = glGetUniformLocation(s_program, "u_normHalf");
        s_uScaleLoc      = glGetUniformLocation(s_program, "u_scale");
        s_uBrightnessLoc = glGetUniformLocation(s_program, "u_brightness");
        s_uSpotlightLoc = glGetUniformLocation(s_program, "u_spotlight");
        s_uOpacityLoc = glGetUniformLocation(s_program, "u_opacity");
        s_uEdgeMinLoc = glGetUniformLocation(s_program, "u_edge_min");

        // Quad covering Hyprland NDC [-1, 1]. y = -1 is the top row.
        const float quadVertices[] = {
            -1.0f, -1.0f, // Top-Left
            -1.0f,  1.0f, // Bottom-Left
             1.0f, -1.0f, // Top-Right
             1.0f,  1.0f  // Bottom-Right
        };

        glGenVertexArrays(1, &s_vao);
        glGenBuffers(1, &s_vbo);

        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

        glBindVertexArray(prevVAO);
        glBindBuffer(GL_ARRAY_BUFFER, prevVBO);

        s_glInitialized = true;
        Log::logger->log(Log::INFO, "[hyprtilt] OpenGL ES perspective shader pipeline initialized");
        return true;
    }

    void CTiltTransformer::releaseGL() {
        if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
            for (auto& [key, slot] : s_outFBs) {
                for (auto& fb : slot.fbs) {
                    if (fb && fb->isAllocated())
                        fb->release();
                }
            }

            if (s_program) {
                glDeleteProgram(s_program);
                s_program = 0;
            }
            if (s_vbo) {
                glDeleteBuffers(1, &s_vbo);
                s_vbo = 0;
            }
            if (s_vao) {
                glDeleteVertexArrays(1, &s_vao);
                s_vao = 0;
            }
        }

        s_outFBs.clear();
        s_uTexLoc = s_uOutSizeLoc = s_uWinCenterLoc = s_uHalfSizeLoc = -1;
        s_uInOffsetLoc = s_uInSizeLoc = s_uKLoc = s_uNormCenterLoc = s_uNormHalfLoc = s_uScaleLoc = s_uBrightnessLoc = -1;
        s_uSpotlightLoc = s_uOpacityLoc = s_uEdgeMinLoc = -1;
        s_glInitialized = false;
        Log::logger->log(Log::INFO, "[hyprtilt] OpenGL resources released");
    }

    CTiltTransformer::CTiltTransformer(PHLWINDOWREF pWindow) : m_pWindow(std::move(pWindow)) {
    }

    bool CTiltTransformer::currentState(SWindowSpringState& out) const {
        std::scoped_lock lock(s_stateMutex);
        const auto                  it = s_springStates.find(m_pWindow);
        if (it == s_springStates.end())
            return false;

        out = it->second;
        return true;
    }

    CTiltTransformer::STiltCoefficients CTiltTransformer::coefficientsFor(const SWindowSpringState& state, double halfW, double halfH) const {
        const float perspDist = confFloat(conf_perspective, 1000.0f, 50.0f, 100000.0f);
        const float radX      = (float)(state.rotX.val * (M_PI / 180.0));
        const float radY      = (float)(state.rotY.val * (M_PI / 180.0));

        // Half-sizes are logical here so the perspective strength does not
        // change with monitor scale (HiDPI outputs).
        const float hw = (float)std::max(halfW, 1.0);
        const float hh = (float)std::max(halfH, 1.0);
        const float Dx = std::max(perspDist / hw, 0.5f);
        const float Dy = std::max(perspDist / hh, 0.5f);

        STiltCoefficients c;
        // Reverse tilt: hovering a corner pushes it away from the camera.
        c.kx    = -std::sin(radY) / Dx;
        c.ky    = std::sin(radX) / Dy;
        c.scale = std::max(state.scale.val, 0.1f);
        return c;
    }

    CTiltTransformer::SNormalization CTiltTransformer::normalizationFor(const STiltCoefficients& coefficients) {
        // Raw forward mapping of the four source corners:
        //   o = d / (1 + k . d)
        // The result is a perspective quadrilateral whose bounding box is
        // generally off-centre and slightly larger or smaller than the source
        // square. Normalise that bounding box back onto [-1, 1]^2 so the
        // rendered card fills exactly the window's original box.
        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();

        for (int i = 0; i < 4; ++i) {
            const float dx    = (i & 1) ? 1.0f : -1.0f;
            const float dy    = (i & 2) ? 1.0f : -1.0f;
            const float denom = 1.0f + coefficients.kx * dx + coefficients.ky * dy;
            if (denom <= 0.001f)
                return {};

            const float ox = dx / denom;
            const float oy = dy / denom;
            minX = std::min(minX, ox);
            minY = std::min(minY, oy);
            maxX = std::max(maxX, ox);
            maxY = std::max(maxY, oy);
        }

        if (minX > maxX || minY > maxY)
            return {};

        SNormalization n;
        n.cx = (minX + maxX) * 0.5f;
        n.cy = (minY + maxY) * 0.5f;
        n.hx = std::max((maxX - minX) * 0.5f, 0.001f);
        n.hy = std::max((maxY - minY) * 0.5f, 0.001f);
        return n;
    }

    CBox CTiltTransformer::transformedExtents(const CBox& currentBox) const {
        if (!m_active.load(std::memory_order_relaxed))
            return currentBox;

        SWindowSpringState state;
        if (!currentState(state) || state.isFlat())
            return currentBox;

        // The normalised projection fills the original box. A user-requested
        // hover shrink only makes it smaller, so the box is the upper bound;
        // a hover scale above 1.0 needs a little extra room.
        const double scale = std::max<double>(state.scale.val, 0.1);
        if (scale <= 1.0)
            return currentBox.copy().expand(1.0);

        const double growX = currentBox.w * (scale - 1.0) * 0.5;
        const double growY = currentBox.h * (scale - 1.0) * 0.5;
        return CBox{currentBox.x - growX, currentBox.y - growY, currentBox.w + growX * 2.0, currentBox.h + growY * 2.0}.expand(1.0);
    }

    CBox CTiltTransformer::sourceBoxForOutput(const CBox& outputBox, const CBox& inputBox) const {
        if (!m_active.load(std::memory_order_relaxed) || inputBox.empty())
            return outputBox.intersection(inputBox);

        SWindowSpringState state;
        if (!currentState(state))
            return outputBox.intersection(inputBox);

        const auto     c      = coefficientsFor(state, inputBox.w * 0.5, inputBox.h * 0.5);
        const auto     n      = normalizationFor(c);
        const Vector2D center = inputBox.pos() + inputBox.size() * 0.5;
        const Vector2D half   = inputBox.size() * 0.5;

        double minX = std::numeric_limits<double>::max();
        double minY = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double maxY = std::numeric_limits<double>::lowest();

        for (int i = 0; i < 4; ++i) {
            const Vector2D corner{(i & 1) ? outputBox.x + outputBox.w : outputBox.x, (i & 2) ? outputBox.y + outputBox.h : outputBox.y};
            const Vector2D q{(corner.x - center.x) / half.x, (corner.y - center.y) / half.y};

            // Invert the aligned mapping used by the shader.
            const Vector2D o{n.cx + q.x * n.hx / c.scale, n.cy + q.y * n.hy / c.scale};
            const double   denom = 1.0 - c.kx * o.x - c.ky * o.y;
            if (denom <= 0.001)
                continue;

            const Vector2D d{o.x / denom, o.y / denom};
            const Vector2D p{center.x + d.x * half.x, center.y + d.y * half.y};
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
        }

        if (minX > maxX || minY > maxY)
            return outputBox.intersection(inputBox);

        return CBox{minX, minY, maxX - minX, maxY - minY}.expand(1.0 / c.scale).intersection(inputBox);
    }

    int CTiltTransformer::priority() const {
        return 5;
    }

    bool CTiltTransformer::active() const {
        return m_active.load(std::memory_order_relaxed);
    }

    bool CTiltTransformer::blocksDirectScanout() const {
        return active();
    }

    Render::SWindowTransformBuffer CTiltTransformer::transform([[maybe_unused]] Render::CRenderContext& ctx, const Render::SWindowTransformBuffer& in,
                                                               const Render::SWindowTransformContext& context) {
        s_transformCalls.fetch_add(1, std::memory_order_relaxed);

        if (!m_active.load(std::memory_order_relaxed))
            return in;

        // Snapshots and standalone captures must never be warped.
        if (context.standalone || context.renderingSnapshot)
            return in;

        auto monitor = context.monitor.lock();
        if (!monitor)
            return in;

        auto window = m_pWindow.lock();
        if (!window || !in.framebuffer || !in.framebuffer->getTexture())
            return in;

        SWindowSpringState state;
        if (!currentState(state) || state.isFlat())
            return in;

        const CBox& winBox = context.currentBox;
        if (winBox.w < 1.0 || winBox.h < 1.0)
            return in;

        const double scale = std::max<double>(monitor->m_scale, 0.01);

        // Reproduce Hyprland's EXPECTEDOUTPUT exactly so the renderer keeps
        // motion blur / matte alignment intact.
        const CBox outBox = Render::pixelBoxForLogical(context.outputBox.copy().expand(1.0 / scale), scale);
        if (outBox.empty())
            return in;

        const int outW = (int)std::lround(outBox.w);
        const int outH = (int)std::lround(outBox.h);
        if (outW <= 0 || outH <= 0)
            return in;

        auto inTex = in.framebuffer->getTexture();
        const Vector2D inSize = inTex->m_size;
        if (inSize.x < 1.0 || inSize.y < 1.0)
            return in;

        const auto coeffs     = coefficientsFor(state, winBox.w * 0.5, winBox.h * 0.5);
        const auto norm       = normalizationFor(coeffs);
        const float brightness = confFloat(conf_brightness_boost, 1.0f, 0.25f, 4.0f);

        // Window centre/half-size in output-canvas-local pixels.
        const Vector2D winCenter{(winBox.x + winBox.w * 0.5) * scale - outBox.x, (winBox.y + winBox.h * 0.5) * scale - outBox.y};
        const Vector2D halfPx{std::max(winBox.w * 0.5 * scale, 1.0), std::max(winBox.h * 0.5 * scale, 1.0)};
        const Vector2D inOffset = outBox.pos() - in.box.pos();

        // Pick (or allocate) the output canvas. Cache by pixel size so a sweep
        // across differently sized windows does not reallocate every frame.
        const uint64_t key = (uint64_t(uint32_t(outW)) << 32) | uint32_t(outH);

        if (s_outFBs.size() >= MAX_FBO_SLOTS && !s_outFBs.contains(key)) {
            auto lru = s_outFBs.end();
            for (auto it = s_outFBs.begin(); it != s_outFBs.end(); ++it) {
                if (lru == s_outFBs.end() || it->second.lastUse < lru->second.lastUse)
                    lru = it;
            }
            if (lru != s_outFBs.end())
                s_outFBs.erase(lru);
        }

        auto& slot      = s_outFBs[key];
        slot.lastUse    = ++s_fboUseCounter;
        auto& outFB     = slot.fbs[slot.toggle ? 1 : 0];
        slot.toggle     = !slot.toggle;

        if (!outFB)
            outFB = makeShared<Render::GL::CGLFramebuffer>("hyprtilt-out-fb");

        if (!s_glInitialized && !initGL())
            return in;

        // CGLFramebuffer::alloc() and initGL() can rebind textures/FBOs, so
        // save every piece of state we touch before calling them.
        Render::GL::CFramebufferBindingGuard bindingGuard{g_pHyprRenderer->glBackend()};

        GLint prevProg = 0, prevVAO = 0, prevTex0 = 0, prevActiveTex = GL_TEXTURE0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
        Render::GL::g_pHyprOpenGL->setActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);

        GLfloat prevClear[4] = {};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);

        GLboolean prevColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
        GLboolean prevDepthMask    = GL_TRUE;
        glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);

        const bool prevBlend   = Render::GL::g_pHyprOpenGL->blendEnabled();
        const bool prevScissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
        const bool prevStencil = glIsEnabled(GL_STENCIL_TEST) == GL_TRUE;
        const bool prevDepth   = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
        const bool prevCull    = glIsEnabled(GL_CULL_FACE) == GL_TRUE;

        const auto restoreState = [&]() {
            glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
            glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
            glDepthMask(prevDepthMask);
            glUseProgram(prevProg);
            glBindVertexArray(prevVAO);

            Render::GL::g_pHyprOpenGL->setActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, prevTex0);
            Render::GL::g_pHyprOpenGL->setActiveTexture(prevActiveTex);

            Render::GL::g_pHyprOpenGL->blend(prevBlend);
            Render::GL::g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, prevScissor);
            Render::GL::g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, prevStencil);
            Render::GL::g_pHyprOpenGL->setCapStatus(GL_DEPTH_TEST, prevDepth);
            Render::GL::g_pHyprOpenGL->setCapStatus(GL_CULL_FACE, prevCull);
        };

        if (!outFB->isAllocated() || outFB->m_size.x != outW || outFB->m_size.y != outH) {
            if (outFB->isAllocated())
                outFB->release();
            if (!outFB->alloc(outW, outH, DRM_FORMAT_ARGB8888)) {
                restoreState();
                return in;
            }
        }
        outFB->setImageDescription(in.framebuffer->imageDescription());

        outFB->bind();
        Render::GL::g_pHyprOpenGL->blend(false); // direct write into a cleared buffer
        Render::GL::g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
        Render::GL::g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
        Render::GL::g_pHyprOpenGL->setCapStatus(GL_DEPTH_TEST, false);
        Render::GL::g_pHyprOpenGL->setCapStatus(GL_CULL_FACE, false);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);

        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(s_program);
        glUniform2f(s_uOutSizeLoc, (float)outW, (float)outH);
        glUniform2f(s_uWinCenterLoc, (float)winCenter.x, (float)winCenter.y);
        glUniform2f(s_uHalfSizeLoc, (float)halfPx.x, (float)halfPx.y);
        glUniform2f(s_uInOffsetLoc, (float)inOffset.x, (float)inOffset.y);
        glUniform2f(s_uInSizeLoc, (float)inSize.x, (float)inSize.y);
        glUniform2f(s_uKLoc, coeffs.kx, coeffs.ky);
        glUniform2f(s_uNormCenterLoc, norm.cx, norm.cy);
        glUniform2f(s_uNormHalfLoc, norm.hx, norm.hy);
        glUniform1f(s_uScaleLoc, coeffs.scale);
        glUniform1f(s_uSpotlightLoc, confFloat(conf_spotlight, 0.5f, 0.0f, 1.0f));
        glUniform1f(s_uBrightnessLoc, brightness);
        glUniform1f(s_uOpacityLoc, confFloat(conf_opacity, 1.0f, 0.0f, 1.0f));
        glUniform1f(s_uEdgeMinLoc, confFloat(conf_edge_min, 0.0f, 0.0f, 1.0f));

        // Bind the source canvas and remember its sampler state so other users
        // of the same texture are unaffected.
        glBindTexture(GL_TEXTURE_2D, inTex->m_texID);

        GLint prevMin = GL_LINEAR, prevMag = GL_LINEAR, prevWrapS = GL_CLAMP_TO_EDGE, prevWrapT = GL_CLAMP_TO_EDGE;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &prevMin);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &prevMag);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &prevWrapS);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &prevWrapT);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUniform1i(s_uTexLoc, 0);

        glBindVertexArray(s_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);

        s_lastRotX.store(state.rotX.val, std::memory_order_relaxed);
        s_lastRotY.store(state.rotY.val, std::memory_order_relaxed);
        s_lastScale.store(state.scale.val, std::memory_order_relaxed);
        s_transformWarps.fetch_add(1, std::memory_order_relaxed);

        glBindTexture(GL_TEXTURE_2D, inTex->m_texID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, prevMin);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, prevMag);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, prevWrapS);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, prevWrapT);

        restoreState();

        return {
            .framebuffer = outFB,
            .box         = outBox,
        };
    }

} // namespace Hyprtilt
