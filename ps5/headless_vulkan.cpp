// Headless build only: there is no Vulkan driver in a payload process.
// DXVK asks the process for vkGetInstanceProcAddr (the Switch port's loader-less
// build); answering "no such function" makes Direct3D creation fail cleanly, which
// is where a headless run is expected to stop. The title build links RADV instead.
#include <cstdio>

extern "C" void* vkGetInstanceProcAddr(void* instance, const char* name) {
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "[headless] vkGetInstanceProcAddr(%s): no Vulkan driver in this build\n", name ? name : "?");
    }
    (void)instance;
    return nullptr;
}

// The PS5 build of DXVK takes its three Vulkan window-layer entry points from the title
// (platform/ps5/ps5_vulkan.cpp). Headless, they report that there is nothing to load.
extern "C" bool dirt3_ps5_vk_load_library(const char*) { return false; }
extern "C" const char* const* dirt3_ps5_vk_instance_extensions(unsigned* count) {
    *count = 0;
    return nullptr;
}
extern "C" bool dirt3_ps5_vk_create_surface(void*, void*, const void*, void*) { return false; }

// Nor a splash or a controller (platform/ps5/ps5_console.cpp in the title).
#include "../Dirt3/platform/ps5/ps5_console.h"
namespace ps5 {
void hide_splash() {}
bool pad_read(Pad&) { return false; }
void pad_rumble(uint8_t, uint8_t) {}
bool audio_start(AudioFill, void*) { return false; }
void audio_stop() {}
void benchmark() {}
[[noreturn]] void quit(int code) { _exit(code); }
bool keyboard_run(const std::string&, unsigned, std::string&) { return false; }
bool keyboard_feed(const Pad&) { return false; }
bool keyboard_active() { return false; }
int keyboard_state(char*, int, int*, int*) { return 0; }
}  // namespace ps5

extern "C" void dirt3_ps5_set_output(uint32_t, uint32_t) {}
extern "C" bool dirt3_ps5_output_size(uint32_t*, uint32_t*) { return false; }
