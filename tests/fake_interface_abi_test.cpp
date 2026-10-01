#include <cmath>
#include "../src/xinput_proxy.cpp"

// Exercise the actual vtable through the x64 hidden-result-buffer ABI. Poison
// both shared-pointer words, as the game's uninitialized stack did in the dump.
// Reject an invalid result before attempting to destroy a garbage controller.
struct SharedPointerOutput {
    void* object;
    void* controller;
};

int main() {
    InitializeFakeInterfaces();
    using GetCameraFn = SharedPointerOutput* (*)(FakeInterface*, SharedPointerOutput*, std::int32_t);
    auto get_camera = reinterpret_cast<GetCameraFn>(g_fake_hmd.vtable[22]);
    for (const std::int32_t device_id : {0, 1, -1}) {
        struct GuardedOutput {
            std::uint64_t before;
            SharedPointerOutput pointer;
            std::uint64_t after;
        } output;
        memset(&output, 0xA5, sizeof(output));
        auto* returned = get_camera(&g_fake_hmd, &output.pointer, device_id);
        if (returned != &output.pointer || output.pointer.object != nullptr ||
            output.pointer.controller != nullptr) {
            fprintf(stderr, "FAIL: GetXRCamera must initialize an empty shared pointer and return its buffer (device=%d)\n", device_id);
            return 1;
        }
        if (output.before != 0xA5A5A5A5A5A5A5A5ULL || output.after != 0xA5A5A5A5A5A5A5A5ULL) {
            fprintf(stderr, "FAIL: GetXRCamera wrote outside the 16-byte result\n");
            return 1;
        }
    }

    using GetStereoDeviceFn = SharedPointerOutput* (*)(FakeInterface*, SharedPointerOutput*);
    auto get_stereo_device = reinterpret_cast<GetStereoDeviceFn>(g_fake_hmd.vtable[24]);
    struct GuardedStereoOutput {
        std::uint64_t before;
        SharedPointerOutput pointer;
        std::uint64_t after;
    } stereo_output;
    memset(&stereo_output, 0x5A, sizeof(stereo_output));
    auto* stereo_returned = get_stereo_device(&g_fake_hmd, &stereo_output.pointer);
    if (stereo_returned != &stereo_output.pointer || stereo_output.pointer.object != nullptr ||
        stereo_output.pointer.controller != nullptr) {
        fprintf(stderr, "FAIL: GetStereoRenderingDevice must initialize an empty shared pointer and return its buffer\n");
        return 1;
    }
    if (stereo_output.before != 0x5A5A5A5A5A5A5A5AULL ||
        stereo_output.after != 0x5A5A5A5A5A5A5A5AULL) {
        fprintf(stderr, "FAIL: GetStereoRenderingDevice wrote outside the 16-byte result\n");
        return 1;
    }

    using GetLensCenterOffsetFn = float (*)(FakeInterface*);
    auto get_lens_center_offset = reinterpret_cast<GetLensCenterOffsetFn>(g_fake_device.vtable[25]);
    const float lens_center_offset = get_lens_center_offset(&g_fake_device);
    if (lens_center_offset != 0.0f) {
        fprintf(stderr, "FAIL: GetLensCenterOffset must return a defined neutral float, got %.9g\n",
                lens_center_offset);
        return 1;
    }

    // Projection matrices are returned by value through a hidden 64-byte
    // buffer. A scalar stub leaves the renderer using uninitialized memory.
    struct MatrixOutput { float m[4][4]; };
    struct GuardedMatrix {
        std::uint64_t before;
        MatrixOutput matrix;
        std::uint64_t after;
    } projection;
    using ProjectionFn = MatrixOutput* (*)(FakeInterface*, MatrixOutput*, std::int32_t);
    auto projection_fn = reinterpret_cast<ProjectionFn>(g_fake_stereo.vtable[6]);
    for (std::int32_t eye : {1, 2}) {
        memset(&projection, 0xA5, sizeof(projection));
        auto* result = projection_fn(&g_fake_stereo, &projection.matrix, eye);
        if (result != &projection.matrix || projection.before != 0xA5A5A5A5A5A5A5A5ULL ||
            projection.after != 0xA5A5A5A5A5A5A5A5ULL) {
            fprintf(stderr, "FAIL: stereo projection must return its 64-byte buffer without overwriting guards\n");
            return 1;
        }
        const auto& m = projection.matrix.m;
        // Positive X/Y scales and W=Z, with infinite reversed depth: near
        // plane maps to depth 1 and a point ten times further maps to 0.1.
        if (!(m[0][0] > 0 && m[1][1] > 0) || m[2][3] != 1 || m[3][3] != 0 ||
            m[2][2] != 0 || m[3][2] != 10.0f || m[0][1] != 0 || m[1][0] != 0) {
            fprintf(stderr, "FAIL: stereo projection is not a valid infinite reversed-Z perspective\n");
            return 1;
        }
    }

    using AdjustRectFn = void (*)(FakeInterface*, std::int32_t, std::int32_t&,
                                  std::int32_t&, std::uint32_t&, std::uint32_t&);
    auto adjust_rect = reinterpret_cast<AdjustRectFn>(g_fake_stereo.vtable[3]);
    std::int32_t x = 11, y = 13;
    std::uint32_t width = 1921, height = 1080;
    adjust_rect(&g_fake_stereo, 1, x, y, width, height);
    if (x != 11 || y != 13 || width != 960 || height != 1080) {
        fprintf(stderr, "FAIL: left eye must cover the first half of the viewport\n");
        return 1;
    }
    x = 11; y = 13; width = 1921; height = 1080;
    adjust_rect(&g_fake_stereo, 2, x, y, width, height);
    if (x != 971 || y != 13 || width != 961 || height != 1080) {
        fprintf(stderr, "FAIL: right eye must cover the remainder without a gap\n");
        return 1;
    }
    x = 11; y = 13; width = 1921; height = 1080;
    adjust_rect(&g_fake_stereo, 0, x, y, width, height);
    if (x != 11 || y != 13 || width != 1921 || height != 1080) {
        fprintf(stderr, "FAIL: a non-stereo view must retain the full viewport\n");
        return 1;
    }

    struct Vector { float x, y, z; };
    struct Rotation { float pitch, yaw, roll; };
    using ViewOffsetFn = void (*)(FakeInterface*, std::int32_t, Rotation&, float, Vector&);
    auto view_offset = reinterpret_cast<ViewOffsetFn>(g_fake_stereo.vtable[5]);
    Rotation rotation{0, 0, 0};
    Vector left{10, 20, 30}, right = left;
    view_offset(&g_fake_stereo, 1, rotation, 100.0f, left);
    view_offset(&g_fake_stereo, 2, rotation, 100.0f, right);
    if (left.x != 10 || right.x != 10 || left.z != 30 || right.z != 30 ||
        fabsf(left.y - 16.8f) > 0.0001f || fabsf(right.y - 23.2f) > 0.0001f) {
        fprintf(stderr, "FAIL: eyes must be separated by 64 mm on the camera's right axis\n");
        return 1;
    }
    rotation.yaw = 90;
    left = right = {10, 20, 30};
    view_offset(&g_fake_stereo, 1, rotation, 100.0f, left);
    view_offset(&g_fake_stereo, 2, rotation, 100.0f, right);
    if (fabsf(left.x - 13.2f) > 0.0001f || fabsf(right.x - 6.8f) > 0.0001f ||
        fabsf(left.y - 20) > 0.0001f || fabsf(right.y - 20) > 0.0001f) {
        fprintf(stderr, "FAIL: stereo separation must rotate with the camera\n");
        return 1;
    }

    struct GuardedBounds {
        std::uint64_t before;
        struct { float x, y; } bounds;
        std::uint64_t after;
    } safe_region;
    using SafeRegionFn = void* (*)(FakeInterface*, void*);
    auto safe_region_fn = reinterpret_cast<SafeRegionFn>(g_fake_stereo.vtable[4]);
    memset(&safe_region, 0x5A, sizeof(safe_region));
    if (safe_region_fn(&g_fake_stereo, &safe_region.bounds) != &safe_region.bounds ||
        safe_region.bounds.x != 0.75f || safe_region.bounds.y != 0.75f ||
        safe_region.before != 0x5A5A5A5A5A5A5A5AULL || safe_region.after != 0x5A5A5A5A5A5A5A5AULL) {
        fprintf(stderr, "FAIL: text safe-region return must initialize exactly eight bytes\n");
        return 1;
    }

    struct GuardedOrtho {
        std::uint64_t before;
        MatrixOutput matrices[2];
        std::uint64_t after;
    } ortho;
    using OrthoFn = void (*)(FakeInterface*, std::int32_t, std::int32_t, float, MatrixOutput*);
    auto ortho_fn = reinterpret_cast<OrthoFn>(g_fake_stereo.vtable[10]);
    memset(&ortho, 0x5A, sizeof(ortho));
    ortho_fn(&g_fake_stereo, 1920, 1080, 1.0f, ortho.matrices);
    if (ortho.before != 0x5A5A5A5A5A5A5A5AULL || ortho.after != 0x5A5A5A5A5A5A5A5AULL ||
        ortho.matrices[0].m[0][0] != 1 || ortho.matrices[0].m[3][0] != 0 ||
        ortho.matrices[1].m[3][0] != 960 || ortho.matrices[1].m[3][3] != 1) {
        fprintf(stderr, "FAIL: orthographic output must initialize two matrices without overwriting guards\n");
        return 1;
    }

    using EnableStereoFn = bool (*)(FakeInterface*, bool);
    auto enable_stereo = reinterpret_cast<EnableStereoFn>(g_fake_stereo.vtable[2]);
    if (!enable_stereo(&g_fake_stereo, true) || !g_fake_stereo_enabled.load() ||
        enable_stereo(&g_fake_stereo, false) || g_fake_stereo_enabled.load()) {
        fprintf(stderr, "FAIL: EnableStereo must return the requested stereo state\n");
        return 1;
    }

    puts("PASS: shared-pointer, float, matrix, viewport and stereo-offset ABIs");
    return 0;
}
