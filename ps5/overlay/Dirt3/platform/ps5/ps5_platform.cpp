// PS5 host services that are not SDL's: dialogs, save sync and upscaling.
// None of them exist yet on the console; each takes the default the game can run with.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

#include "../platform.h"
#include "ps5_console.h"
#include "recomp/host.h"

namespace platform {

std::atomic<bool> g_prompt_open{false};

// Name entry: the on-screen keyboard (ps5_console.cpp). One at a time: the game asks again for
// every press of its button while the keyboard is up. The console's own text dialog is not
// reachable from a title (its modules cannot be loaded), so the game draws one itself.
bool prompt_text(const std::string&, const std::string&, const std::string& initial, std::string& result) {
    if (g_prompt_open.exchange(true)) return false;
    result = initial;
    const bool confirmed = ps5::keyboard_run(initial, 14, result);
    rt::log("platform: name entry %s: \"%s\"", confirmed ? "confirmed" : "cancelled", confirmed ? result.c_str() : initial.c_str());
    g_prompt_open = false;
    return confirmed;
}

// First start: the game's hardware detection (no hardware_settings_info.xml yet) crashed on the console,
// so the files it writes are seeded, as a finished detection of the console's GPU would leave them
// (native 2560x1440, statistics off). Written only where missing, so the player's own settings stay.
namespace {
const char* const kSeedConfig = R"XML(<?xml version="1.0" encoding="UTF-8" ?>
<hardware_settings_config version="56" deviceId="0x0000">
	<cpu>
		<threadStrategy parallelUpdateRender="true" workerMapFile="system/workerMap8Core.xml" forceFeedbackProcessor="6" dvdStorageProcessor="7" dataSetMonitorProcessor="4" renderProcessor="0" updateProcessor="2" fileStreamProcessor="5" />
	</cpu>
	<audio_card>
		<audio mixing="software" />
	</audio_card>
	<graphics_card>
		<directx forcedx9="false" />
		<stereo enabled="false" separation="0.015" convergence="1.5" />
		<resolution width="2560" height="1440" aspect="normal" fullscreen="true" vsync="0" multisampling="2xmsaa">
			<refreshRate rate="60" />
		</resolution>
		<gamma level="1.0" />
	</graphics_card>
	<graphics_detail level="medium" />
	<shadows enabled="true" size="1024" maskQuality="1" />
	<particles enabled="true" wind="true" dynamicRes="true" />
	<crowd enabled="true" detail="0" />
	<cloth enabled="true" tessellation="true" />
	<postprocess quality="1" />
	<groundcover mode="blended" clutter="true" />
	<objects lod="1.0" maxlod="0" />
	<trees lod="1.0" maxlod="0" />
	<vehicles characterQuality="1" lodQuality="1" />
	<envmap faces="6" size="256" forceBilinear="true" />
	<water update="true" detail="1" tessellation="true" />
	<mirrors enabled="true" forceBilinear="true" width="384" height="128" car_maxlod="3" car_culldist="200.0" />
	<skidmarks enabled="true" />
	<dynamic_ambient_occ enabled="true" quality="1" />
	<night_lighting volumes="true" lights="0" shadows="false" />
	<physics environmentalDamage="true" vehicleDamage="true" />
	<input device_type="auto" />
	<motion enabled="true" ip="dbox" port="20777" delay="1" extradata="0" />
</hardware_settings_config>)XML";
const char* const kSeedInfo = R"XML(<?xml version="1.0" encoding="UTF-8" ?>
<hardware_settings_info>
	<systemInfo OS="Professional Service Pack 1 (Build 7601)" totalMemory="2097088" winPlatformID="2" winMajorVersion="6" winMinorVersion="1" isWinXP="false" isWinVista="false" isWin7="true" rating="3" />
	<cpu name="UnknownCPU" speed="-1" cores="8" hyperThreading="false" sse2Supported="true" rating="3" />
	<graphics_card name="PlayStation 5 GPU (RADV NAVI21)" vendorId="0x1002" deviceId="0x0000" vs_shader="3_0" ps_shader="3_0" native_width="2560" native_height="1440" rating="2">
		<resolution width="1280" height="720" aspect="auto">
			<refreshRate rate="60" />
		</resolution>
		<resolution width="1920" height="1080" aspect="auto">
			<refreshRate rate="60" />
		</resolution>
		<resolution width="2560" height="1440" aspect="auto">
			<refreshRate rate="60" />
		</resolution>
		<resolution width="3840" height="2160" aspect="auto">
			<refreshRate rate="60" />
		</resolution>
		<capabilities a2r10g10b10="true" fetch4="true" hardwarepcf="true">
			<alpha_to_coverage supported="true" method="ATi" />
			<multisampling>
				<mode name="off" />
				<mode name="2xmsaa" />
				<mode name="4xmsaa" />
				<mode name="8xmsaa" />
			</multisampling>
		</capabilities>
	</graphics_card>
	<audio_card audioEnabled="true">
		<unsupported type="hardware" index="0" reverbs="1" voices="0" />
		<unsupported type="software" index="1" reverbs="1" voices="0" />
		<unsupported type="rapture3D" index="5" reverbs="4" voices="0" />
	</audio_card>
</hardware_settings_info>)XML";

void seed_file(const std::string& path, const char* text) {
    if (access(path.c_str(), F_OK) == 0) return;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fputs(text, f);
        std::fclose(f);
        rt::log("platform: first start, seeded %s", path.c_str());
    }
}
}  // namespace

// config_path: the game's hardware_settings_config.xml in the data folder; the info file is its neighbour.
void ps5_seed_settings(const std::string& config_path) {
    const size_t slash = config_path.rfind('/');
    if (slash == std::string::npos) return;
    seed_file(config_path, kSeedConfig);
    seed_file(config_path.substr(0, slash + 1) + "hardware_settings_info.xml", kSeedInfo);
}

// No two-button dialog yet: the first option.
int choose(const std::string& title, const std::string& message, const std::string&, const std::string&) {
    rt::log("platform: %s: %s (first option taken)", title.c_str(), message.c_str());
    return 0;
}

bool choose_folder(const std::string&, std::string&) { return false; }
void set_app_icon(const std::string&) {}  // the title folder carries its icon

void cloud_saves_start(const std::string&) {}
void cloud_saves_flush() {}

// No MetalFX here, but the graphics menu's extra Off/On row (shims/frontend/menu_patch.cpp) is wanted:
// on the console it switches DXVK's statistics display (frame rate, frame times, draw calls).
namespace {
std::atomic<bool> g_statistics{false};
}
void metalfx_init() {}
bool metalfx_supported() { return true; }
bool metalfx_enabled() { return g_statistics.load(std::memory_order_relaxed); }
void set_metalfx_enabled(bool on) { g_statistics.store(on, std::memory_order_relaxed); }

}  // namespace platform

// DXVK (the console build's swapchain blitter) asks this every frame.
extern "C" bool dirt3_ps5_hud_enabled() { return platform::metalfx_enabled(); }

// DXVK's overlay (the console build) draws the keyboard from these.
extern "C" bool dirt3_ps5_keyboard_active() { return ps5::keyboard_active(); }
extern "C" int dirt3_ps5_keyboard_state(char* out, int capacity, int* row, int* col) {
    return ps5::keyboard_state(out, capacity, row, col);
}
