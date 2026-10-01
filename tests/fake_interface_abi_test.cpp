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

    puts("PASS: reconstructed shared-pointer and float-return ABIs are initialized safely");
    return 0;
}
