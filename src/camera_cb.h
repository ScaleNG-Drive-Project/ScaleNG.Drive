#pragma once
#include <cstdint>
#include <cmath>

struct Jitter2D {
    float x;
    float y;
};

Jitter2D ComputeJitter(unsigned int frameIndex);

bool ValidateCameraCb(const void* data, size_t size);

void ApplyCameraCbJitter(void* data, size_t size,
                         unsigned int renderWidth, unsigned int renderHeight,
                         const Jitter2D& curr, const Jitter2D& prev);

bool ValidateVelocityCb(const void* data, size_t size,
                        unsigned int mvWidth, unsigned int mvHeight);

void PatchVelocityCb(void* data, size_t size, const void* cameraCb);
