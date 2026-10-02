#include <cmath>
#include <cstdio>

#include "../src/graphics_bridge.h"

namespace {

bool Near(float actual, float expected) {
    return std::fabs(actual - expected) <= 0.00001f;
}

int CheckVector(const ac7vr::BridgeVector3& actual, float x, float y, float z,
                const char* label) {
    if (Near(actual.x, x) && Near(actual.y, y) && Near(actual.z, z)) return 0;
    std::fprintf(stderr, "FAIL: %s got=(%.6f, %.6f, %.6f) expected=(%.6f, %.6f, %.6f)\n",
                 label, actual.x, actual.y, actual.z, x, y, z);
    return 1;
}

int CheckQuat(const ac7vr::BridgeQuat& actual, float x, float y, float z, float w,
              const char* label) {
    if (Near(actual.x, x) && Near(actual.y, y) && Near(actual.z, z) && Near(actual.w, w)) return 0;
    std::fprintf(stderr,
                 "FAIL: %s got=(%.6f, %.6f, %.6f, %.6f) expected=(%.6f, %.6f, %.6f, %.6f)\n",
                 label, actual.x, actual.y, actual.z, actual.w, x, y, z, w);
    return 1;
}

}  // namespace

int main() {
    int failures = 0;

    // A wrong axis order or handedness here would make physical head motion
    // drive the wrong Unreal axis even though OpenXR tracking itself is valid.
    failures += CheckVector(ac7vr::ConvertOpenXrPositionToUnreal(1.0f, 2.0f, 3.0f),
                            -3.0f, 1.0f, 2.0f, "position basis conversion");

    constexpr float s = 0.7071067811865475f;
    failures += CheckQuat(ac7vr::ConvertOpenXrOrientationToUnreal(0.0f, 0.0f, 0.0f, 1.0f),
                          0.0f, 0.0f, 0.0f, 1.0f, "identity orientation");
    failures += CheckQuat(ac7vr::ConvertOpenXrOrientationToUnreal(s, 0.0f, 0.0f, s),
                          0.0f, -s, 0.0f, s, "OpenXR +X rotation");
    failures += CheckQuat(ac7vr::ConvertOpenXrOrientationToUnreal(0.0f, s, 0.0f, s),
                          0.0f, 0.0f, -s, s, "OpenXR +Y rotation");
    failures += CheckQuat(ac7vr::ConvertOpenXrOrientationToUnreal(0.0f, 0.0f, s, s),
                          s, 0.0f, 0.0f, s, "OpenXR +Z rotation");

    if (failures) return 1;
    std::puts("PASS: OpenXR-to-Unreal position and quaternion basis conversion");
    return 0;
}
