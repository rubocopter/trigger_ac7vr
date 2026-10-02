#pragma once

#include <cstdint>

namespace ac7vr {

struct BridgeQuat {
    float x;
    float y;
    float z;
    float w;
};

struct BridgeVector3 {
    float x;
    float y;
    float z;
};

struct BridgeHeadPose {
    BridgeQuat orientation;
    BridgeVector3 position_meters;
    bool orientation_valid;
    bool position_valid;
};

// Signed angles in radians, in OpenXR's left/right/up/down order.
struct BridgeEyeFov {
    float left;
    float right;
    float up;
    float down;
};

// OpenXR uses +X right, +Y up and -Z forward. Unreal uses +X forward,
// +Y right and +Z up. Keep these conversions independent of the runtime so
// they can be regression-tested without a headset.
BridgeVector3 ConvertOpenXrPositionToUnreal(float x, float y, float z);
BridgeQuat ConvertOpenXrOrientationToUnreal(float x, float y, float z, float w);

// Patches the main executable's DXGI factory imports. This is intentionally
// loader-light so it can run during process attach before UE creates DXGI.
bool InstallGraphicsBridgeHooks();

// Returns the most recent center-head pose located by the OpenXR render path.
// Position remains in meters; the UE-facing caller applies WorldToMeters.
bool TryGetLatestOpenXrHeadPose(BridgeHeadPose& pose);
// Eye 0 is left and eye 1 is right. Cleared when the runtime is reset.
bool TryGetLatestOpenXrEyeFov(std::uint32_t eye, BridgeEyeFov& fov);

}  // namespace ac7vr
