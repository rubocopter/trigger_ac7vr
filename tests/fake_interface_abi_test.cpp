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
    puts("PASS: GetXRCamera returns a destructible empty shared pointer without overwriting guards");
    return 0;
}
