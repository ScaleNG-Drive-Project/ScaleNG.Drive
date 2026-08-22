#include "camera_cb.h"
#include <cstring>
#include <cfloat>

namespace {

constexpr size_t kCameraCbSize = 1616;
constexpr size_t kVelocityCbSize = 176;

constexpr size_t kWorldToCamera = 224;
constexpr size_t kWorldToScreenPos0 = 352;
constexpr size_t kCameraToScreen = 432;
constexpr size_t kViewProj = 496;
constexpr size_t kProjectionParams = 688;
constexpr size_t kViewProjPrevFrame = 1184;
constexpr size_t kWorldToScreenPos0PrevFrame = 1248;

bool IsFinite(const float* f, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(f[i])) return false;
    }
    return true;
}

bool NearOne(float v)
{
    return v > 0.9f && v < 1.1f;
}

} // namespace

Jitter2D ComputeJitter(unsigned int frameIndex)
{
    float h2 = 0.0f, h3 = 0.0f;
    unsigned int i = frameIndex + 1;
    float f2 = 1.0f / 2.0f;
    while (i > 0) {
        h2 += f2 * (float)(i & 1);
        i >>= 1;
        f2 *= 0.5f;
    }
    i = frameIndex + 1;
    float f3 = 1.0f / 3.0f;
    while (i > 0) {
        h3 += f3 * (float)(i % 3);
        i /= 3;
        f3 /= 3.0f;
    }
    Jitter2D j = { h2 - 0.5f, h3 - 0.5f };
    if (j.x > 0.5f) j.x -= 1.0f;
    if (j.x < -0.5f) j.x += 1.0f;
    if (j.y > 0.5f) j.y -= 1.0f;
    if (j.y < -0.5f) j.y += 1.0f;
    return j;
}

bool ValidateCameraCb(const void* data, size_t size)
{
    if (!data || size < kCameraCbSize) return false;
    const float* f = static_cast<const float*>(data);
    if (!IsFinite(f, kCameraCbSize / 4)) return false;

    const float* worldToCamera = f + kWorldToCamera / 4;
    const float* worldToScreenPos0 = f + kWorldToScreenPos0 / 4;
    const float* cameraToScreen = f + kCameraToScreen / 4;
    const float* viewProj = f + kViewProj / 4;
    const float* projParams = f + kProjectionParams / 4;
    const float* viewProjPrev = f + kViewProjPrevFrame / 4;
    const float* worldToScreenPos0Prev = f + kWorldToScreenPos0PrevFrame / 4;

    if (!NearOne(worldToCamera[15])) return false;
    if (!NearOne(cameraToScreen[7])) return false;

    if (std::fabs(worldToCamera[12]) + std::fabs(worldToCamera[13]) +
        std::fabs(worldToCamera[14]) < 0.1f) return false;

    if (projParams[0] < 0.01f || projParams[0] > 1.0f) return false;
    if (projParams[1] < 1000.0f || projParams[1] > 10000.0f) return false;
    if (!std::isfinite(projParams[2]) || !std::isfinite(projParams[3])) return false;

    return true;
}

void ApplyCameraCbJitter(void* data, size_t size,
                         unsigned int renderWidth, unsigned int renderHeight,
                         const Jitter2D& curr, const Jitter2D& prev)
{
    if (!data || size < kCameraCbSize || renderWidth == 0 || renderHeight == 0) return;
    float* f = static_cast<float*>(data);

    float jx = curr.x * 2.0f / (float)renderWidth;
    float jy = curr.y * 2.0f / (float)renderHeight;
    float px = prev.x * 2.0f / (float)renderWidth;
    float py = prev.y * 2.0f / (float)renderHeight;

    float* worldToScreenPos0 = f + kWorldToScreenPos0 / 4;
    float* cameraToScreen = f + kCameraToScreen / 4;
    float* viewProj = f + kViewProj / 4;
    float* viewProjPrev = f + kViewProjPrevFrame / 4;
    float* worldToScreenPos0Prev = f + kWorldToScreenPos0PrevFrame / 4;

    worldToScreenPos0[8] += jx * worldToScreenPos0[11];
    worldToScreenPos0[9] += jy * worldToScreenPos0[11];

    cameraToScreen[4] += jx * cameraToScreen[7];
    cameraToScreen[5] += jy * cameraToScreen[7];

    viewProj[8] += jx * viewProj[11];
    viewProj[9] += jy * viewProj[11];

    viewProjPrev[8] += px * viewProjPrev[11];
    viewProjPrev[9] += py * viewProjPrev[11];

    worldToScreenPos0Prev[8] += px * worldToScreenPos0Prev[11];
    worldToScreenPos0Prev[9] += py * worldToScreenPos0Prev[11];
}

bool ValidateVelocityCb(const void* data, size_t size,
                        unsigned int mvWidth, unsigned int mvHeight)
{
    if (!data || size < kVelocityCbSize || mvWidth == 0 || mvHeight == 0) return false;
    const float* f = static_cast<const float*>(data);
    if (!IsFinite(f, kVelocityCbSize / 4)) return false;

    float ux = f[0];
    float uy = f[1];
    bool texSizeOk =
        (std::fabs(ux * (float)mvWidth - 1.0f) < 0.02f) ||
        (std::fabs(ux - (float)mvWidth) < 0.5f);
    if (!texSizeOk) return false;

    const float* stw = f + 4;
    if (!NearOne(stw[15])) return false;
    if (std::fabs(stw[12]) > 0.01f || std::fabs(stw[13]) > 0.01f || std::fabs(stw[14]) > 0.01f) return false;

    return true;
}

void PatchVelocityCb(void* data, size_t size, const void* cameraCb)
{
    if (!data || size < kVelocityCbSize || !cameraCb) return;
    const float* camF = static_cast<const float*>(cameraCb);
    float* f = static_cast<float*>(data);

    float inv[16];
    const float* src = camF + kWorldToScreenPos0 / 4;
    for (int i = 0; i < 16; ++i) inv[i] = src[i];

    for (int col = 0; col < 4; ++col) {
        float best = 0.0f;
        int bestRow = -1;
        for (int row = col; row < 4; ++row) {
            float v = std::fabs(inv[row * 4 + col]);
            if (v > best) { best = v; bestRow = row; }
        }
        if (bestRow < 0 || best < 1e-9f) return;
        if (bestRow != col) {
            for (int k = 0; k < 4; ++k) {
                float t = inv[col * 4 + k];
                inv[col * 4 + k] = inv[bestRow * 4 + k];
                inv[bestRow * 4 + k] = t;
            }
        }
        float d = inv[col * 4 + col];
        for (int k = 0; k < 4; ++k) inv[col * 4 + k] /= d;
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            float m = inv[row * 4 + col];
            if (m == 0.0f) continue;
            for (int k = 0; k < 4; ++k) inv[row * 4 + k] -= m * inv[col * 4 + k];
        }
    }

    std::memcpy(f + 4, inv, sizeof(inv));
    std::memcpy(f + 20, camF + kWorldToScreenPos0PrevFrame / 4, 64);
}
