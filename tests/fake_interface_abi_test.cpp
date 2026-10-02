#include <cmath>
#include <thread>
#include <type_traits>
#include "../src/xinput_proxy.cpp"
#include "../src/graphics_bridge.cpp"

// Exercise the actual vtable through the x64 hidden-result-buffer ABI. Poison
// both shared-pointer words, as the game's uninitialized stack did in the dump.
// Reject an invalid result before attempting to destroy a garbage controller.
struct SharedPointerOutput {
    void* object;
    void* controller;
};

int main() {
    InitializeFakeInterfaces();

    if (ShouldLogPoseDiagnostic(1, false, false) ||
        !ShouldLogPoseDiagnostic(37, true, false) ||
        ShouldLogPoseDiagnostic(1999, true, true) ||
        !ShouldLogPoseDiagnostic(2000, true, true) ||
        ShouldLogPoseDiagnostic(4000, false, true)) {
        fprintf(stderr, "FAIL: pose diagnostics must log the first valid pose and then every 2000 calls\n");
        return 1;
    }

    // Exception attribution must be local to the crashing thread. AC7 can
    // exercise the synthetic interfaces concurrently, so a process-global
    // "last slot" can identify an unrelated call when another thread faults.
    g_last_interface = 0xCAFE;
    g_last_slot = 0xBEEF;
    bool worker_tls_ok = false;
    std::thread worker([&] {
        RecordSlot(FakeInterfaceKind::Device, 40, false);
        worker_tls_ok =
            g_last_interface == static_cast<std::uint32_t>(FakeInterfaceKind::Device) &&
            g_last_slot == 40;
    });
    worker.join();
    if (!worker_tls_ok || g_last_interface != 0xCAFE || g_last_slot != 0xBEEF) {
        fprintf(stderr, "FAIL: last-interface/slot exception attribution must be thread-local\n");
        return 1;
    }
    InitializeFakeInterfaces();

    using GetCurrentPoseFn = bool (*)(FakeInterface*, std::int32_t, FakeQuat&, FakeVector3&);
    auto get_current_pose = reinterpret_cast<GetCurrentPoseFn>(g_fake_hmd.vtable[9]);
    FakeQuat orientation{};
    FakeVector3 position{};
    memset(&orientation, 0xA5, sizeof(orientation));
    memset(&position, 0xA5, sizeof(position));
    if (get_current_pose(&g_fake_hmd, 0, orientation, position) ||
        orientation.x != 0.0f || orientation.y != 0.0f || orientation.z != 0.0f ||
        orientation.w != 1.0f || position.x != 0.0f || position.y != 0.0f ||
        position.z != 0.0f) {
        fprintf(stderr, "FAIL: GetCurrentPose must return false with identity orientation and zero position\n");
        return 1;
    }

    ac7vr::BridgeHeadPose tracked_pose{
        {0.1f, -0.2f, 0.3f, 0.9f},
        {1.25f, -0.5f, 0.125f},
        true,
        true,
    };
    if (!ApplyBridgeHeadPose(tracked_pose, 100.0f, orientation, position) ||
        orientation.x != 0.1f || orientation.y != -0.2f || orientation.z != 0.3f ||
        orientation.w != 0.9f || position.x != 125.0f || position.y != -50.0f ||
        position.z != 12.5f) {
        fprintf(stderr, "FAIL: tracked OpenXR pose must preserve orientation and use observed WorldToMeters\n");
        return 1;
    }
    if (!ApplyBridgeHeadPose(tracked_pose, 0.0f, orientation, position) ||
        orientation.x != 0.1f || orientation.y != -0.2f || orientation.z != 0.3f ||
        orientation.w != 0.9f || position.x != 0.0f || position.y != 0.0f || position.z != 0.0f) {
        fprintf(stderr, "FAIL: pose must expose rotation but suppress translation until WorldToMeters is observed\n");
        return 1;
    }

    struct GuardedAudioOffset {
        std::uint64_t before;
        FakeVector3 value;
        std::uint32_t padding;
        std::uint64_t after;
    } audio_offset;
    using GetAudioListenerOffsetFn = FakeVector3* (*)(FakeInterface*, FakeVector3*, std::int32_t);
    auto get_audio_listener_offset =
        reinterpret_cast<GetAudioListenerOffsetFn>(g_fake_hmd.vtable[14]);
    memset(&audio_offset, 0xA5, sizeof(audio_offset));
    auto* audio_offset_returned =
        get_audio_listener_offset(&g_fake_hmd, &audio_offset.value, 0);
    if (audio_offset_returned != &audio_offset.value ||
        audio_offset.value.x != 0.0f || audio_offset.value.y != 0.0f ||
        audio_offset.value.z != 0.0f ||
        audio_offset.before != 0xA5A5A5A5A5A5A5A5ULL ||
        audio_offset.after != 0xA5A5A5A5A5A5A5A5ULL) {
        fprintf(stderr, "FAIL: XR slot 14 must return a zero audio-listener FVector through its hidden result buffer\n");
        return 1;
    }

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

    struct DeviceSlot40Object {
        std::uint64_t vtable;
        std::uint64_t payload0;
        std::uint64_t payload1;
    } slot40_object = {
        0x1122334455667788ULL,
        0x99AABBCCDDEEFF00ULL,
        0x0123456789ABCDEFULL,
    };
    using DeviceSlot40Fn = void (*)(FakeInterface*, void*);
    auto device_slot40 = reinterpret_cast<DeviceSlot40Fn>(g_fake_device.vtable[40]);
    device_slot40(&g_fake_device, &slot40_object);
    if (slot40_object.vtable != 0x1122334455667788ULL ||
        slot40_object.payload0 != 0x99AABBCCDDEEFF00ULL ||
        slot40_object.payload1 != 0x0123456789ABCDEFULL) {
        fprintf(stderr, "FAIL: device slot 40 must not overwrite its explicit object argument\n");
        return 1;
    }

    struct DeviceSlot37Context {
        std::uint64_t words[4];
    } slot37_context = {{
        0x1122334455667788ULL,
        0x99AABBCCDDEEFF00ULL,
        0x0123456789ABCDEFULL,
        0x0F1E2D3C4B5A6978ULL,
    }};
    if (g_fake_device.vtable[37] ==
        reinterpret_cast<void*>(&FakeUnknownSlot<FakeInterfaceKind::Device, 37>)) {
        fprintf(stderr, "FAIL: device slot 37 must have an explicit observed-ABI hook\n");
        return 1;
    }
    using DeviceSlot37Fn = void (*)(FakeInterface*, void*, std::int32_t);
    auto device_slot37 = reinterpret_cast<DeviceSlot37Fn>(g_fake_device.vtable[37]);
    device_slot37(&g_fake_device, &slot37_context, 1);
    const DeviceSlot37Context expected_slot37_context = {{
        0x1122334455667788ULL,
        0x99AABBCCDDEEFF00ULL,
        0x0123456789ABCDEFULL,
        0x0F1E2D3C4B5A6978ULL,
    }};
    if (memcmp(&slot37_context, &expected_slot37_context, sizeof(slot37_context)) != 0) {
        fprintf(stderr, "FAIL: device slot 37 must not modify its caller-owned context\n");
        return 1;
    }

    struct GuardedSlot41Buffer {
        std::uint64_t before;
        struct { float x, y; } value;
        std::uint64_t after;
    } slot41_buffer = {
        0x5A5A5A5A5A5A5A5AULL,
        {0.5f, 1.0f},
        0x5A5A5A5A5A5A5A5AULL,
    };
    using DeviceSlot41Fn = void (*)(FakeInterface*, void*);
    auto device_slot41 = reinterpret_cast<DeviceSlot41Fn>(g_fake_device.vtable[41]);
    device_slot41(&g_fake_device, &slot41_buffer.value);
    if (slot41_buffer.value.x != 0.5f || slot41_buffer.value.y != 1.0f ||
        slot41_buffer.before != 0x5A5A5A5A5A5A5A5AULL ||
        slot41_buffer.after != 0x5A5A5A5A5A5A5A5AULL) {
        fprintf(stderr, "FAIL: device slot 41 must preserve its caller-owned buffer until value semantics are proven\n");
        return 1;
    }

    struct Vector { float x, y, z; };
    struct Rotation { float pitch, yaw, roll; };
    struct MatrixOutput { float m[4][4]; };

    constexpr float kSqrtHalf = 0.7071067811865475f;
    {
        const FakeRotator identity = QuaternionToUnrealRotator({0, 0, 0, 1});
        const FakeRotator yaw90 = QuaternionToUnrealRotator({0, 0, kSqrtHalf, kSqrtHalf});
        if (fabsf(identity.pitch) > 0.0001f || fabsf(identity.yaw) > 0.0001f ||
            fabsf(identity.roll) > 0.0001f || fabsf(yaw90.pitch) > 0.001f ||
            fabsf(yaw90.yaw - 90.0f) > 0.001f || fabsf(yaw90.roll) > 0.001f) {
            fprintf(stderr, "FAIL: bridge quaternion must use AC7/Unreal rotator conventions\n");
            return 1;
        }

        ac7vr::BridgeHeadPose head_pose{
            {0, 0, kSqrtHalf, kSqrtHalf},
            {0, 0, 0},
            true,
            false,
        };
        FakeRotator base{10.0f, 20.0f, -5.0f};
        if (!ApplyBridgeHeadRotationToStereoView(head_pose, base) ||
            fabsf(base.pitch - 10.0f) > 0.001f ||
            fabsf(base.yaw - 110.0f) > 0.001f ||
            fabsf(base.roll + 5.0f) > 0.001f) {
            fprintf(stderr, "FAIL: stereo view must add live HMD rotation to the game camera\n");
            return 1;
        }
    }

    // AC7 inserted a slot between CalculateStereoViewOffset and UE4's stock
    // GetStereoProjectionMatrix. The callsite at RVA 0x0183C514 has the same
    // five-argument shape as view offset: RCX=this, EDX=pass, R8=&rotation,
    // XMM3=world-to-meters, and &location as the first stack argument.
    struct Slot6RotationStorage {
        std::uint64_t before;
        unsigned char bytes[64];
        std::uint64_t after;
    } slot6_rotation;
    using ViewOffsetFn = void (*)(FakeInterface*, std::int32_t, Rotation&, float, Vector&);
    auto extra_view_offset = reinterpret_cast<ViewOffsetFn>(g_fake_stereo.vtable[6]);
    for (std::int32_t eye : {1, 2}) {
        memset(&slot6_rotation, 0xA5, sizeof(slot6_rotation));
        auto& rotation = *reinterpret_cast<Rotation*>(slot6_rotation.bytes);
        rotation = {0, 0, 0};
        Vector location{10, 20, 30};
        extra_view_offset(&g_fake_stereo, eye, rotation, 100.0f, location);
        if (slot6_rotation.before != 0xA5A5A5A5A5A5A5A5ULL ||
            slot6_rotation.after != 0xA5A5A5A5A5A5A5A5ULL) {
            fprintf(stderr, "FAIL: stereo slot 6 wrote outside its rotation argument\n");
            return 1;
        }
        for (std::size_t i = sizeof(Rotation); i < sizeof(slot6_rotation.bytes); ++i) {
            if (slot6_rotation.bytes[i] != 0xA5) {
                fprintf(stderr, "FAIL: stereo slot 6 overwrote memory beyond the 12-byte rotation\n");
                return 1;
            }
        }
        const float expected_y = eye == 1 ? 16.8f : 23.2f;
        if (rotation.pitch != 0 || rotation.yaw != 0 || rotation.roll != 0 ||
            location.x != 10 || fabsf(location.y - expected_y) > 0.0001f || location.z != 30) {
            fprintf(stderr, "FAIL: stereo slot 6 must behave like the observed view-offset ABI\n");
            return 1;
        }
    }

    // AC7 slot 7 matches UE4's return-by-value GetStereoProjectionMatrix ABI:
    // RCX=this, RDX=hidden 64-byte FMatrix result buffer, R8D=stereo pass.
    // The caller immediately copies four xmmwords from the returned RAX.
    struct GuardedStandardProjection {
        std::uint64_t before;
        MatrixOutput matrix;
        std::uint64_t after;
    } standard_projection;
    using StandardProjectionFn = MatrixOutput* (*)(FakeInterface*, MatrixOutput*, std::int32_t);
    static_assert(std::is_same_v<decltype(&FakeStereoGetStandardProjection),
        FakeMatrix* (*)(FakeInterface*, FakeMatrix*, std::int32_t)>);
    auto standard_projection_fn = reinterpret_cast<StandardProjectionFn>(g_fake_stereo.vtable[7]);
    for (std::int32_t eye : {1, 2}) {
        memset(&standard_projection, 0x5A, sizeof(standard_projection));
        auto* result = standard_projection_fn(&g_fake_stereo, &standard_projection.matrix, eye);
        if (result != &standard_projection.matrix ||
            standard_projection.before != 0x5A5A5A5A5A5A5A5AULL ||
            standard_projection.after != 0x5A5A5A5A5A5A5A5AULL) {
            fprintf(stderr, "FAIL: stereo slot 7 must return its hidden 64-byte matrix buffer without overwriting guards\n");
            return 1;
        }
        const auto& m = standard_projection.matrix.m;
        if (!(m[0][0] > 0 && m[1][1] > 0) || m[2][3] != 1 || m[3][3] != 0 ||
            m[2][2] != 0 || m[3][2] != 10.0f || m[0][1] != 0 || m[1][0] != 0) {
            fprintf(stderr, "FAIL: stereo slot 7 must initialize a valid projection matrix\n");
            return 1;
        }
    }

    // Catch symmetric projection, a wrong offset sign, or swapped eyes at the
    // actual boundary: xrLocateViews publication -> AC7's slot-7 FMatrix.
    // These are the measured PSVR2 FOVs; rays on each frustum edge must land
    // exactly on the corresponding clip-space edge, independently of depth.
    std::array<XrView, 2> runtime_views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    runtime_views[0].pose.orientation.w = runtime_views[1].pose.orientation.w = 1;
    runtime_views[0].fov = {-1.073377f, 0.758284f, 0.925725f, -0.925725f};
    runtime_views[1].fov = {-0.758284f, 1.073377f, 0.925725f, -0.925725f};
    ac7vr::PublishPose(runtime_views.data(), 2, 0);
    for (std::int32_t eye : {1, 2}) {
        standard_projection_fn(&g_fake_stereo, &standard_projection.matrix, eye);
        const auto& m = standard_projection.matrix.m;
        const auto& fov = runtime_views[eye - 1].fov;
        for (float depth : {10.0f, 1000.0f}) {
            const float w = depth * m[2][3] + m[3][3];
            const float left = (tanf(fov.angleLeft) * depth * m[0][0] + depth * m[2][0]) / w;
            const float right = (tanf(fov.angleRight) * depth * m[0][0] + depth * m[2][0]) / w;
            const float up = (tanf(fov.angleUp) * depth * m[1][1] + depth * m[2][1]) / w;
            const float down = (tanf(fov.angleDown) * depth * m[1][1] + depth * m[2][1]) / w;
            const float z = (depth * m[2][2] + m[3][2]) / w;
            if (fabsf(left + 1) > 0.00001f || fabsf(right - 1) > 0.00001f ||
                fabsf(up - 1) > 0.00001f || fabsf(down + 1) > 0.00001f ||
                fabsf(z - (depth == 10.0f ? 1.0f : 0.01f)) > 0.00001f) {
                fprintf(stderr, "FAIL: eye %d runtime FOV edges must project to clip edges; got L/R/U/D=(%f,%f,%f,%f) z=%f\n",
                        eye, left, right, up, down, z);
                return 1;
            }
        }
    }
    // A vertically asymmetric frustum catches the same sign/layout error on
    // the second axis, which the measured symmetric vertical PSVR2 FOV cannot.
    runtime_views[0].fov = {-1.107148718f, 0.785398163f, 1.107148718f, -0.463647609f};
    ac7vr::PublishPose(runtime_views.data(), 2, 0);
    standard_projection_fn(&g_fake_stereo, &standard_projection.matrix, 1);
    const auto& asymmetric = standard_projection.matrix.m;
    if (fabsf(-2.0f * asymmetric[0][0] + asymmetric[2][0] + 1.0f) > 0.00001f ||
        fabsf(asymmetric[0][0] + asymmetric[2][0] - 1.0f) > 0.00001f ||
        fabsf(2.0f * asymmetric[1][1] + asymmetric[2][1] - 1.0f) > 0.00001f ||
        fabsf(-0.5f * asymmetric[1][1] + asymmetric[2][1] + 1.0f) > 0.00001f) {
        fprintf(stderr, "FAIL: asymmetric horizontal and vertical rays must reach their clip edges\n");
        return 1;
    }
    for (std::int32_t non_eye : {0, 3, -1}) {
        standard_projection_fn(&g_fake_stereo, &standard_projection.matrix, non_eye);
        if (standard_projection.matrix.m[0][0] != 1.0f ||
            standard_projection.matrix.m[2][0] != 0.0f ||
            standard_projection.matrix.m[2][1] != 0.0f) {
            fprintf(stderr, "FAIL: non-eye projection must not consume a runtime eye FOV\n");
            return 1;
        }
    }
    for (XrFovf invalid : {
             XrFovf{}, XrFovf{0.5f, -0.5f, 0.5f, -0.5f},
             XrFovf{-0.5f, 0.5f, -0.5f, 0.5f},
             XrFovf{-2.0f, 0.5f, 0.5f, -0.5f},
             XrFovf{NAN, 0.5f, 0.5f, -0.5f},
             XrFovf{-0.5f, INFINITY, 0.5f, -0.5f}}) {
        runtime_views[0].fov = invalid;
        ac7vr::PublishPose(runtime_views.data(), 2, 0);
        standard_projection_fn(&g_fake_stereo, &standard_projection.matrix, 1);
        if (standard_projection.matrix.m[0][0] != 1.0f ||
            standard_projection.matrix.m[2][0] != 0.0f ||
            standard_projection.matrix.m[2][1] != 0.0f) {
            fprintf(stderr, "FAIL: invalid runtime FOV must retain the initialized fallback projection\n");
            return 1;
        }
    }
    ac7vr::ClearPublishedPose();
    standard_projection_fn(&g_fake_stereo, &standard_projection.matrix, 1);
    if (standard_projection.matrix.m[0][0] != 1.0f ||
        standard_projection.matrix.m[2][0] != 0.0f) {
        fprintf(stderr, "FAIL: runtime reset must clear stale eye FOV and restore fallback projection\n");
        return 1;
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
    auto ortho_fn = reinterpret_cast<OrthoFn>(g_fake_stereo.vtable[11]);
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
