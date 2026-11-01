#pragma once

#include <cmath>
#include <algorithm>

namespace Hyprtilt {

    // Motion below these thresholds is visually indistinguishable from rest.
    // Shared by the simulation, the damage scheduler and the shader fast-path
    // so all three agree on when a window has stopped moving.
    inline constexpr float FLAT_ROTATION_DEG = 0.02f;
    inline constexpr float FLAT_SCALE_DELTA  = 0.001f;
    inline constexpr float FLAT_ROT_VEL      = 0.05f;  // degrees / second
    inline constexpr float FLAT_SCALE_VEL    = 0.005f; // scale units / second

    struct Spring1D {
        float val = 0.0f;
        float vel = 0.0f;

        void reset(float initialVal = 0.0f) {
            val = initialVal;
            vel = 0.0f;
        }

        void step(float target, float stiffness, float damping, float dt) {
            if (!std::isfinite(target) || !std::isfinite(stiffness) || !std::isfinite(damping) || !std::isfinite(dt))
                return;

            // Semi-implicit Euler with sub-stepping for unconditional numerical stability.
            constexpr float SUB_STEP_DT = 0.002f; // 2 ms sub-steps
            float           remainingDt = std::clamp(dt, 0.0001f, 0.05f);

            while (remainingDt > 0.0f) {
                const float h     = std::min(remainingDt, SUB_STEP_DT);
                const float force = -stiffness * (val - target) - damping * vel;
                vel += force * h;
                val += vel * h;
                remainingDt -= h;
            }
        }
    };

    struct SWindowSpringState {
        Spring1D rotX;
        Spring1D rotY;
        Spring1D scale;

        bool wasAttached = false;

        SWindowSpringState() {
            scale.reset(1.0f);
        }

        void step(float targetRotX, float targetRotY, float targetScale, float stiffness, float damping, float dt) {
            rotX.step(targetRotX, stiffness, damping, dt);
            rotY.step(targetRotY, stiffness, damping, dt);
            scale.step(targetScale, stiffness, damping, dt);
        }

        bool isSettled(float targetRotX, float targetRotY, float targetScale) const {
            return std::abs(rotX.val - targetRotX) < FLAT_ROTATION_DEG &&
                   std::abs(rotY.val - targetRotY) < FLAT_ROTATION_DEG &&
                   std::abs(scale.val - targetScale) < FLAT_SCALE_DELTA &&
                   std::abs(rotX.vel) < FLAT_ROT_VEL &&
                   std::abs(rotY.vel) < FLAT_ROT_VEL &&
                   std::abs(scale.vel) < FLAT_SCALE_VEL;
        }

        bool isFlat() const {
            return std::abs(rotX.val) < FLAT_ROTATION_DEG &&
                   std::abs(rotY.val) < FLAT_ROTATION_DEG &&
                   std::abs(scale.val - 1.0f) < FLAT_SCALE_DELTA &&
                   std::abs(rotX.vel) < FLAT_ROT_VEL &&
                   std::abs(rotY.vel) < FLAT_ROT_VEL &&
                   std::abs(scale.vel) < FLAT_SCALE_VEL;
        }

        void snapFlat() {
            rotX.reset(0.0f);
            rotY.reset(0.0f);
            scale.reset(1.0f);
        }
    };

} // namespace Hyprtilt
