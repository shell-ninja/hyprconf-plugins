#pragma once

#include "compat.hpp"
#include "Spring.hpp"

#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace Hyprtilt {

    class CTiltTransformer : public Render::IWindowTransformer {
      public:
        explicit CTiltTransformer(PHLWINDOWREF pWindow);
        virtual ~CTiltTransformer() override = default;

        // Warps the window's source canvas into a tilted card.
        virtual Render::SWindowTransformBuffer transform(Render::CRenderContext& ctx, const Render::SWindowTransformBuffer& in,
                                                         const Render::SWindowTransformContext& context) override;

        virtual int  priority() const override;
        virtual bool active() const override;
        virtual bool blocksDirectScanout() const override;
        virtual CBox transformedExtents(const CBox& currentBox) const override;
        virtual CBox sourceBoxForOutput(const CBox& outputBox, const CBox& inputBox) const override;

        // Hyprland skips inactive transformers and drops them via
        // CWindowTransformerList::removeInactive().
        void setActive(bool active) {
            m_active.store(active, std::memory_order_relaxed);
        }

        static bool initGL();
        static void releaseGL();

        // Global spring states shared by the update loop and the transformer.
        static std::unordered_map<PHLWINDOWREF, SWindowSpringState> s_springStates;

        // Diagnostics surfaced through `hyprctl hyprtilt debug`.
        static std::atomic<uint64_t> s_transformCalls;
        static std::atomic<uint64_t> s_transformWarps;
        static std::atomic<float>    s_lastRotX;
        static std::atomic<float>    s_lastRotY;
        static std::atomic<float>    s_lastScale;

      private:
        struct SOutputFBSlot {
            SP<Render::GL::CGLFramebuffer> fbs[2];
            bool                           toggle  = false;
            uint64_t                       lastUse = 0;
        };

        struct STiltCoefficients {
            float kx    = 0.0f;
            float ky    = 0.0f;
            float scale = 1.0f;
        };

        // Affine normalisation that maps the raw perspective quadrilateral's
        // bounding box onto the original window box. This keeps the tilted
        // window centred and aligned with its layout slot instead of drifting
        // and changing apparent size.
        struct SNormalization {
            float cx = 0.0f;
            float cy = 0.0f;
            float hx = 1.0f;
            float hy = 1.0f;
        };

        // Copies the current spring state, or returns false when the window no
        // longer has animation state (destroyed/unmapped).
        bool               currentState(SWindowSpringState& out) const;
        STiltCoefficients  coefficientsFor(const SWindowSpringState& state, double halfW, double halfH) const;
        static SNormalization normalizationFor(const STiltCoefficients& coefficients);

        PHLWINDOWREF      m_pWindow;
        std::atomic<bool> m_active{false};

        // Double-buffered ARGB output canvases keyed by pixel size. Our own
        // framebuffer keeps a real alpha channel (Hyprland's pooled work buffer
        // is XRGB), so rounded corners and translucent windows survive the
        // warp. Two buffers let the window pass and the blur matte pass coexist.
        static std::unordered_map<uint64_t, SOutputFBSlot> s_outFBs;
        static uint64_t                                    s_fboUseCounter;

        static GLuint s_program;
        static GLint  s_uTexLoc;
        static GLint  s_uOutSizeLoc;
        static GLint  s_uWinCenterLoc;
        static GLint  s_uHalfSizeLoc;
        static GLint  s_uInOffsetLoc;
        static GLint  s_uInSizeLoc;
        static GLint  s_uKLoc;
        static GLint  s_uNormCenterLoc;
        static GLint  s_uNormHalfLoc;
        static GLint  s_uScaleLoc;
        static GLint  s_uBrightnessLoc;
        static GLint  s_uSpotlightLoc;
        static GLint  s_uOpacityLoc;
        static GLint  s_uEdgeMinLoc;
        static GLuint s_vao;
        static GLuint s_vbo;
        static bool   s_glInitialized;
    };

} // namespace Hyprtilt
