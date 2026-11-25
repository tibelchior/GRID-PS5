// GRID: the graphics menu's Aspect Ratio row becomes "Statistics" Off/On (PS5 only).
//
// Same idea as the MetalFX row of the DiRT ports (menu_patch.cpp), but GRID's frontend needs no
// states.bin edit: the row (item "5.library", combo pc_gfx_aspect) keeps its place, and only
//   language/language_eng.lng, language_use.lng   its title text "Aspect Ratio" -> "Statistics"
//                                                  (same length: the table is addressed by offsets)
//   system/hardware_settings_options.xml          the aspect options become off / on:
//                                                  off = <resolution aspect="normal"/>
//                                                  on  = <resolution aspect="metalfx"/>
// The host reads aspect="metalfx" in the saved hardware_settings_config.xml as "statistics on"
// (read_metalfx_setting, watched by sdl_main), and DXVK's overlay shows fps and frame times.
// Copies go to <data>/overlay through vfs overrides; the game folder is never touched.
#include <cstdint>
#include <cstring>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../vfs.h"
#include "recomp/host.h"

namespace shim {

namespace {

constexpr uint32_t kTag = 0x72522200;

uint32_t u32(const std::string& d, size_t o) {
    uint32_t v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}

void put32(std::string& d, uint32_t v) { d.append(reinterpret_cast<const char*>(&v), 4); }

struct States {
    std::vector<std::string> strings;
    std::vector<uint32_t> nodes;  // 6 per node: name, text, attr_count, first_attr, child_count, first_child
    std::vector<uint32_t> attrs;  // 2 per attribute: name, value

    bool parse(const std::string& d) {
        std::string blob, offs, node_bytes, attr_bytes;
        auto walk = [&](auto&& self, size_t o, size_t end) -> void {
            while (o + 8 <= end) {
                const uint32_t tag = u32(d, o) & 0xFF, size = u32(d, o + 4);
                if (o + 8 + size > end) return;
                const std::string body = d.substr(o + 8, size);
                if (tag == 0x1A || tag == 0x17) self(self, o + 8, o + 8 + size);
                else if (tag == 0x1D) blob = body;
                else if (tag == 0x1E) offs = body;
                else if (tag == 0x1B) node_bytes = body;
                else if (tag == 0x1C) attr_bytes = body;
                o += 8 + size;
            }
        };
        walk(walk, 0, d.size());
        if (blob.empty() || offs.empty() || node_bytes.empty()) return false;
        for (size_t i = 0; i + 4 <= offs.size(); i += 4) strings.emplace_back(blob.c_str() + u32(offs, i));
        for (size_t i = 0; i + 4 <= node_bytes.size(); i += 4) nodes.push_back(u32(node_bytes, i));
        for (size_t i = 0; i + 4 <= attr_bytes.size(); i += 4) attrs.push_back(u32(attr_bytes, i));
        return true;
    }

    std::string encode() const {
        std::string blob, offs, node_bytes, attr_bytes;
        for (const std::string& s : strings) {
            put32(offs, uint32_t(blob.size()));
            blob += s;
            blob += '\0';
        }
        while (blob.size() % 4) blob += '\0';
        for (uint32_t v : nodes) put32(node_bytes, v);
        for (uint32_t v : attrs) put32(attr_bytes, v);
        auto chunk = [](uint32_t tag, const std::string& body) {
            std::string c;
            put32(c, kTag | tag);
            put32(c, uint32_t(body.size()));
            return c + body;
        };
        return chunk(0x1A, chunk(0x17, chunk(0x1D, blob) + chunk(0x1E, offs)) + chunk(0x1B, node_bytes) +
                               chunk(0x1C, attr_bytes));
    }

    uint32_t sid(const std::string& s) {
        for (size_t i = 0; i < strings.size(); ++i)
            if (strings[i] == s) return uint32_t(i);
        strings.push_back(s);
        return uint32_t(strings.size() - 1);
    }
    size_t count() const { return nodes.size() / 6; }
    const std::string& name(size_t n) const { return strings[nodes[6 * n]]; }
    // Index into attrs of the value of attribute key on node n, or -1.
    long attr(size_t n, const char* key) const {
        for (uint32_t k = 0; k < nodes[6 * n + 2]; ++k) {
            const size_t a = 2 * (nodes[6 * n + 3] + k);
            if (strings[attrs[a]] == key) return long(a + 1);
        }
        return -1;
    }
    long child(size_t n, const char* node_name) const {
        for (uint32_t k = 0; k < nodes[6 * n + 4]; ++k)
            if (name(nodes[6 * n + 5] + k) == node_name) return long(nodes[6 * n + 5] + k);
        return -1;
    }
};


std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool write_all(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    return bool(out);
}

// GRID relaunches itself (CreateProcessA) after graphics changes that "need a restart", which a
// title cannot do: the confirmation state's decision goes straight to "no restart" instead.
bool patch_states(const std::string& in, const std::string& out) {
    States s;
    if (!s.parse(read_all(in))) return false;
    for (size_t n = 0; n < s.count(); ++n) {
        if (s.name(n) != "StateDecisionBool") continue;
        const long id = s.attr(n, "id"), yes = s.attr(n, "ifTrue");
        if (id < 0 || yes < 0 || s.strings[s.attrs[id]] != "pc_check_restart") continue;
        s.attrs[yes] = s.sid("no");
        return write_all(out, s.encode());
    }
    return false;
}

// <category name="aspect"> becomes off/on; the presets no longer refer to it.
bool patch_options(const std::string& in, const std::string& out) {
    std::string x = read_all(in);
    const size_t a = x.find("<category name=\"aspect\"");
    const size_t b = a == std::string::npos ? a : x.find("</category>", a);
    if (b == std::string::npos) return false;
    x.replace(a, b - a,
              "<category name=\"aspect\" >\r\n"
              "      <option name=\"off\" >\r\n        <graphics_card>\r\n"
              "          <resolution aspect=\"normal\" />\r\n        </graphics_card>\r\n      </option>\r\n"
              "      <option name=\"on\" >\r\n        <graphics_card>\r\n"
              "          <resolution aspect=\"metalfx\" />\r\n        </graphics_card>\r\n      </option>\r\n    ");
    for (size_t r; (r = x.find("<ref category=\"aspect\"")) != std::string::npos;) {
        size_t start = x.rfind('\n', r);
        start = start == std::string::npos ? r : start;
        x.erase(start, x.find("/>", r) + 2 - start);
    }
    return write_all(out, x);
}

// The language table keeps its strings NUL-separated in id order; "Refresh Rate" is followed by
// the aspect row's title. Same length, so every offset stays valid.
bool patch_language(const std::string& in, const std::string& out) {
    std::string x = read_all(in);
    static const char kBefore[] = "Refresh Rate\0Aspect Ratio\0";
    static const char kAfter[] = "Refresh Rate\0Statistics  \0";
    const size_t at = x.find(std::string(kBefore, sizeof kBefore - 1));
    if (at == std::string::npos) return false;
    x.replace(at, sizeof kBefore - 1, std::string(kAfter, sizeof kAfter - 1));
    return write_all(out, x);
}

}  // namespace

void install_grid_stats_row() {
    const std::string dir = vfs::data_dir() + "/overlay";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    struct Patch {
        const char* rel;
        const char* file;
        bool (*fn)(const std::string&, const std::string&);
    };
    const Patch patches[] = {{"system/states.bin", "states.bin", patch_states},
                             {"system/hardware_settings_options.xml", "hardware_settings_options.xml", patch_options},
                             {"language/language_eng.lng", "language_eng.lng", patch_language},
                             {"language/language_use.lng", "language_use.lng", patch_language}};
    int done = 0;
    for (const Patch& p : patches) {
        std::string src = vfs::host_path(std::string(vfs::kInstallDir) + "\\" + p.rel);
        for (char& c : src)
            if (c == '\\') c = '/';
        if (!std::filesystem::exists(src, ec)) continue;
        const std::string out = dir + "/" + p.file;
        if (!p.fn(src, out)) {
            rt::log("menu: cannot patch %s, statistics row not added", p.rel);
            if (std::strcmp(p.file, "hardware_settings_options.xml") == 0) return;  // the options matter most
            if (std::strcmp(p.file, "states.bin") == 0) continue;
            continue;
        }
        vfs::set_override(p.rel, out);
        ++done;
    }
    rt::log("menu: statistics row installed (%d files)", done);
}

}  // namespace shim
