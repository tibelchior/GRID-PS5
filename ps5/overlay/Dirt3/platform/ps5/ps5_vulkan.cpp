// PS5 Vulkan glue for DXVK: the driver is RADV, linked into the title (no loader), and
// the swapchain is VK_KHR_display on the console's one display (no window system).
//
// DXVK's SDL3 window layer asks SDL for three things SDL cannot give here; the PS5
// build of DXVK takes them from this file instead (ps5-spike/patches/dxvk):
//   SDL_Vulkan_LoadLibrary            -> dirt3_ps5_vk_load_library
//   SDL_Vulkan_GetInstanceExtensions  -> dirt3_ps5_vk_instance_extensions
//   SDL_Vulkan_CreateSurface          -> dirt3_ps5_vk_create_surface
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "../platform.h"
#include "recomp/host.h"

extern "C" {

// RADV's entry point, as a title that links the archive gets it.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);

// What DXVK's loader-less build calls (the Switch port's vulkan_loader.cpp).
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    return vk_icdGetInstanceProcAddr(instance, name);
}

bool dirt3_ps5_vk_load_library(const char*) { return true; }

const char* const* dirt3_ps5_vk_instance_extensions(uint32_t* count) {
    static const char* const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    *count = 2;
    return extensions;
}

namespace {
uint32_t g_want_w = 0, g_want_h = 0;  // the game's back buffer, from dirt3_ps5_set_output
uint32_t g_out_w = 0, g_out_h = 0;    // the display mode the surface was created in
}

// The size of the output surface once it exists (false before: the arguments are left alone).
// DXVK scales the picture over the window's client rect, so the window must have this size.
extern "C" bool dirt3_ps5_output_size(uint32_t* width, uint32_t* height) {
    if (!g_out_w) return false;
    *width = g_out_w;
    *height = g_out_h;
    return true;
}

// The size of the picture the game is about to render (its swap chain), so that the console
// outputs that mode and the display does the one scaling: a 1440p picture stretched to 4K by
// the title and then shrunk or stretched again by the monitor looks soft.
extern "C" void dirt3_ps5_set_output(uint32_t width, uint32_t height) {
    g_want_w = width;
    g_want_h = height;
}

namespace {
// The display mode to output: the file "output" in the data folder can name a height (1080,
// 1440, 2160); otherwise the smallest mode that holds the game's picture, 4K if unknown.
uint32_t pick_mode(const VkDisplayModePropertiesKHR* modes, uint32_t count) {
    uint32_t forced = 0;
    const char* root = std::filesystem::exists("/app0/data") ? "/app0/data" : "/data/dirt3/data";
    if (FILE* f = std::fopen((std::string(root) + "/output").c_str(), "r")) {
        unsigned v = 0;
        if (std::fscanf(f, "%u", &v) == 1) forced = v;
        std::fclose(f);
    }
    uint32_t best = 0, best_area = 0;
    bool have = false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = modes[i].parameters.visibleRegion;
        const uint32_t area = r.width * r.height;
        if (forced) {
            if (r.height == forced && (!have || modes[i].parameters.refreshRate > modes[best].parameters.refreshRate)) {
                best = i;
                have = true;
            }
        } else if (g_want_w && r.width >= g_want_w && r.height >= g_want_h && (!have || area < best_area)) {
            best = i;
            best_area = area;
            have = true;
        }
    }
    if (!have) {  // unknown or too big: the largest mode
        for (uint32_t i = 0; i < count; ++i) {
            const auto& r = modes[i].parameters.visibleRegion;
            if (r.width * r.height > best_area) {
                best = i;
                best_area = r.width * r.height;
            }
        }
    }
    rt::log("ps5 vulkan: game picture %ux%u%s -> display mode %u", g_want_w, g_want_h, forced ? " (output file)" : "", best);
    return best;
}
}  // namespace

// The window is not used: every surface is the display's one plane, in the mode pick_mode chose.
bool dirt3_ps5_vk_create_surface(void* window, VkInstance instance, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
#define LOAD(name) const auto name = reinterpret_cast<PFN_##name>(vk_icdGetInstanceProcAddr(instance, #name))
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceDisplayPropertiesKHR);
    LOAD(vkGetDisplayModePropertiesKHR);
    LOAD(vkCreateDisplayPlaneSurfaceKHR);
#undef LOAD
    if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceDisplayPropertiesKHR || !vkGetDisplayModePropertiesKHR ||
        !vkCreateDisplayPlaneSurfaceKHR) {
        rt::log("ps5 vulkan: the driver has no VK_KHR_display commands");
        return false;
    }
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t count = 1;
    VkResult r = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count < 1) {
        rt::log("ps5 vulkan: no physical device (%d)", int(r));
        return false;
    }
    VkDisplayPropertiesKHR display{};
    count = 1;
    r = vkGetPhysicalDeviceDisplayPropertiesKHR(physical, &count, &display);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count < 1) {
        rt::log("ps5 vulkan: no display (%d)", int(r));
        return false;
    }
    VkDisplayModePropertiesKHR modes[16];
    count = 16;
    r = vkGetDisplayModePropertiesKHR(physical, display.display, &count, modes);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count < 1) {
        rt::log("ps5 vulkan: the display has no modes (%d)", int(r));
        return false;
    }
    for (uint32_t i = 0; i < count; ++i)
        rt::log("ps5 vulkan: display mode %u: %ux%u at %.3f Hz", i, modes[i].parameters.visibleRegion.width,
                modes[i].parameters.visibleRegion.height, modes[i].parameters.refreshRate / 1000.0);
    const uint32_t mode = pick_mode(modes, count);
    VkDisplaySurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
    info.displayMode = modes[mode].displayMode;
    info.planeIndex = 0;
    info.planeStackIndex = 0;
    info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.globalAlpha = 1.0f;
    info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    info.imageExtent = modes[mode].parameters.visibleRegion;
    r = vkCreateDisplayPlaneSurfaceKHR(instance, &info, allocator, surface);
    if (r == VK_SUCCESS) {
        g_out_w = info.imageExtent.width;
        g_out_h = info.imageExtent.height;
        // The window is created before the mode is known (and the dummy display is 1080p).
        platform::set_fullscreen(window, false);
        platform::set_window_size(window, int(g_out_w), int(g_out_h));
    }
    rt::log("ps5 vulkan: %s, surface %ux%u: %s (%d)", display.displayName ? display.displayName : "display",
            info.imageExtent.width, info.imageExtent.height, r == VK_SUCCESS ? "created" : "FAILED", int(r));
    return r == VK_SUCCESS;
}

}  // extern "C"
