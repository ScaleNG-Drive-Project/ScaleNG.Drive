#include "camera_cb.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace {
constexpr size_t kCameraCbFloats = 1616 / sizeof(float);
constexpr size_t kWorldToCamera = 224 / sizeof(float);
constexpr size_t kCameraToScreen = 432 / sizeof(float);
constexpr size_t kProjectionParams = 688 / sizeof(float);

std::array<float, kCameraCbFloats> MakeGameplayCamera(float farPlane)
{
    std::array<float, kCameraCbFloats> cb{};
    cb[kWorldToCamera + 12] = 168.78f;
    cb[kWorldToCamera + 13] = 277.82f;
    cb[kWorldToCamera + 14] = 545.91f;
    cb[kWorldToCamera + 15] = 1.0f;
    cb[kCameraToScreen + 7] = 1.0f;
    cb[kProjectionParams + 0] = 0.10f;
    cb[kProjectionParams + 1] = farPlane;
    cb[kProjectionParams + 2] = 0.0f;
    cb[kProjectionParams + 3] = 0.10f;
    return cb;
}
}

int main()
{
    auto gameplay = MakeGameplayCamera(12500.0f);
    if (!ValidateCameraCb(gameplay.data(), sizeof(gameplay))) {
        std::fputs("FAIL: logged gameplay camera (far plane 12500) rejected\n", stderr);
        return 1;
    }

    auto tooNear = MakeGameplayCamera(500.0f);
    if (ValidateCameraCb(tooNear.data(), sizeof(tooNear))) {
        std::fputs("FAIL: implausibly small far plane accepted\n", stderr);
        return 1;
    }

    auto implausiblyFar = MakeGameplayCamera(100001.0f);
    if (ValidateCameraCb(implausiblyFar.data(), sizeof(implausiblyFar))) {
        std::fputs("FAIL: far plane above sanity ceiling accepted\n", stderr);
        return 1;
    }

    if (ValidateCameraCb(gameplay.data(), 1615)) {
        std::fputs("FAIL: undersized camera buffer accepted\n", stderr);
        return 1;
    }

    std::puts("PASS: gameplay far plane 12500 accepted; invalid bounds rejected");
    return 0;
}
