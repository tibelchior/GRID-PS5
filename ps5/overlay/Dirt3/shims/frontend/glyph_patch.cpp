// PlayStation controller glyphs for the menus, served through a vfs override like the menu patches
// (menu_patch.cpp): the frontend pack frontend/b_persistent.pssg holds each controller button as a
// 32x32 DXT5 texture "controller_icons_xbox_<button>" with its 16x16 mip behind it (1280 bytes). The
// console's pad is an Xbox pad to the game, so those are the glyphs it shows; this copies the pack
// into <data>/overlay with the buttons a PlayStation pad names differently redrawn
// (tools/scripts/make_ps_glyphs.py): A/B/X/Y become Cross/Circle/Square/Triangle, the bumpers and
// triggers L1/R1/L2/R2, Start/Back Options and the touch pad, the stick presses L3/R3.
//
// DiRT 3 keeps the buttons in one 256x256 DXT5 atlas texture ("controller_icons_0", no mips) instead:
// each button is a 32x32 cell, named by FEATLASINFODATA entries (texturename, u0/v0 as floats; the
// cell's pixel position is u*256, v*256), and the cell's DXT5 blocks are copied over the atlas's.
//
// PSSG: big-endian. Header "PSSG", file size, attribute and node counts; a table of node types (id,
// name, its attributes' ids and names); then the node tree: id, size, attribute block size,
// attributes (id, size, value), then children, or raw data for a data node.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../vfs.h"
#include "recomp/host.h"

namespace shim {

namespace {

struct Glyph {
    const char* name;
    uint8_t data[1280];
};
const Glyph kGlyphs[] = {
#include "../../platform/ps5/ps_glyphs.inc"
};

constexpr const char* kPrefix = "controller_icons_xbox_";

struct Pssg {
    std::string d;
    std::map<uint32_t, std::string> nodes, attrs;

    uint32_t u32(size_t o) const {
        if (o + 4 > d.size()) return 0;
        const auto* p = reinterpret_cast<const uint8_t*>(d.data()) + o;
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    }

    // The node table; returns where the node tree starts, or 0.
    size_t parse_header() {
        if (d.size() < 16 || d.compare(0, 4, "PSSG") != 0) return 0;
        const uint32_t node_count = u32(12);
        size_t o = 16;
        for (uint32_t i = 0; i < node_count; ++i) {
            const uint32_t id = u32(o), len = u32(o + 4);
            if (o + 8 + len + 4 > d.size()) return 0;
            nodes[id] = d.substr(o + 8, len);
            o += 8 + len;
            const uint32_t attr_count = u32(o);
            o += 4;
            for (uint32_t k = 0; k < attr_count; ++k) {
                const uint32_t aid = u32(o), alen = u32(o + 4);
                if (o + 8 + alen > d.size()) return 0;
                attrs[aid] = d.substr(o + 8, alen);
                o += 8 + alen;
            }
        }
        return o;
    }

    struct Node {
        std::string name;
        size_t off = 0, attr_off = 0, attr_end = 0, end = 0;  // attr_end: where children or data start
        bool ok = false;
    };

    Node node_at(size_t o, size_t limit) const {
        Node n;
        if (o + 12 > limit) return n;
        const uint32_t id = u32(o), size = u32(o + 4), attr_size = u32(o + 8);
        auto it = nodes.find(id);
        if (it == nodes.end() || o + 8 + size > limit || 4 + attr_size > size) return n;
        n.name = it->second;
        n.off = o;
        n.attr_off = o + 12;
        n.attr_end = o + 12 + attr_size;
        n.end = o + 8 + size;
        n.ok = true;
        return n;
    }

    // An attribute's value: offset and size, or false.
    bool attr(const Node& n, const char* key, size_t& at, size_t& size) const {
        for (size_t p = n.attr_off; p + 8 <= n.attr_end;) {
            const uint32_t aid = u32(p), sz = u32(p + 4);
            auto it = attrs.find(aid);
            if (it != attrs.end() && it->second == key) {
                at = p + 8;
                size = sz;
                return true;
            }
            p += 8 + sz;
        }
        return false;
    }

    std::string str_attr(const Node& n, const char* key) const {
        size_t at, size;
        if (!attr(n, key, at, size) || size < 4) return {};
        const uint32_t len = u32(at);
        return len + 4 <= size && at + 4 + len <= d.size() ? d.substr(at + 4, len) : std::string();
    }

    // The texture's image data: its TEXTUREIMAGEBLOCK child's TEXTUREIMAGEBLOCKDATA child (the
    // block is the first child in the pack's separate textures, behind a USERDATA node in DiRT 3's).
    bool image_data(const Node& tex, size_t& at, size_t& size) const {
        for (size_t o = tex.attr_end; o < tex.end;) {
            const Node block = node_at(o, tex.end);
            if (!block.ok) return false;
            if (block.name == "TEXTUREIMAGEBLOCK") {
                const Node data = node_at(block.attr_end, block.end);
                if (!data.ok || data.name != "TEXTUREIMAGEBLOCKDATA") return false;
                at = data.attr_end;
                size = data.end - data.attr_end;
                return true;
            }
            o = block.end;
        }
        return false;
    }

    float f32(size_t o) const {
        const uint32_t v = u32(o);
        float f;
        std::memcpy(&f, &v, sizeof f);
        return f;
    }

    // DiRT 3's atlas: the cells of the controller glyphs (name -> pixel x, y), and the atlas texture.
    std::map<std::string, std::pair<int, int>> cells;
    size_t atlas_node = 0;  // offset of the TEXTURE node, 0 if not seen

    void scan_atlas(size_t from, size_t to) {
        for (size_t o = from; o < to;) {
            const Node n = node_at(o, to);
            if (!n.ok) break;
            if (n.name == "FEATLASINFODATA") {
                const std::string name = str_attr(n, "texturename");
                size_t u, us, v, vs;
                if (name.rfind(kPrefix, 0) == 0 && attr(n, "u0", u, us) && attr(n, "v0", v, vs) && us == 4 && vs == 4) {
                    // Snap to the 32x32 grid: the entries are inset by half a pixel.
                    const int x = int(f32(u) * 256.0f + 0.5f) & ~31, y = int(f32(v) * 256.0f + 0.5f) & ~31;
                    cells[name.substr(std::strlen(kPrefix))] = {x, y};
                }
            } else if (n.name == "TEXTURE") {
                if (str_attr(n, "id") == "controller_icons_0") atlas_node = n.off;
            } else if (n.name == "PSSGDATABASE" || n.name == "LIBRARY" || n.name == "FEATLASINFO") {
                scan_atlas(n.attr_end, n.end);
            }
            o = n.end;
        }
    }

    // Copies the glyphs into the atlas; returns how many.
    int patch_atlas() {
        if (!atlas_node) return 0;
        const Node tex = node_at(atlas_node, d.size());
        size_t at, size, w, ws, h, hs;
        if (!tex.ok || str_attr(tex, "texelFormat") != "dxt5" || !attr(tex, "width", w, ws) || !attr(tex, "height", h, hs) ||
            u32(w) != 256 || u32(h) != 256 || !image_data(tex, at, size) || size < 256 * 256)
            return 0;
        int patched = 0;
        for (const Glyph& g : kGlyphs) {
            // stick_l2/stick_r2 are the "turn the stick" pictures here, not stick presses: keep them.
            if (!std::strcmp(g.name, "stick_l2") || !std::strcmp(g.name, "stick_r2")) continue;
            auto it = cells.find(g.name);
            if (it == cells.end()) continue;
            const int bx = it->second.first / 4, by = it->second.second / 4;  // the cell's first 4x4 block
            if (bx + 8 > 64 || by + 8 > 64) continue;
            for (int r = 0; r < 8; ++r)  // the glyph's base level: 8 rows of 8 blocks of 16 bytes
                std::memcpy(&d[at + size_t((by + r) * 64 + bx) * 16], g.data + r * 8 * 16, 8 * 16);
            ++patched;
        }
        return patched;
    }

    // Replaces the glyph textures under `parent`'s children; returns how many.
    int patch_textures(size_t from, size_t to) {
        int patched = 0;
        for (size_t o = from; o < to;) {
            const Node n = node_at(o, to);
            if (!n.ok) break;
            if (n.name == "TEXTURE") {
                const std::string id = str_attr(n, "id");
                if (id.rfind(kPrefix, 0) == 0) {
                    const std::string button = id.substr(std::strlen(kPrefix));
                    size_t at, size, w, ws, h, hs;
                    for (const Glyph& g : kGlyphs) {
                        if (button != g.name) continue;
                        // Only the pack's own layout: 32x32 DXT5, one image and its mip
                        if (str_attr(n, "texelFormat") == "dxt5" && attr(n, "width", w, ws) && attr(n, "height", h, hs) &&
                            u32(w) == 32 && u32(h) == 32 && image_data(n, at, size) && (size == sizeof g.data || size == 1024)) {
                            // DiRT 2: the base level and its 16x16 mip (1280 bytes); GRID: the base level only (1024).
                            std::memcpy(&d[at], g.data, size);
                            ++patched;
                        }
                        break;
                    }
                }
            } else if (n.name == "PSSGDATABASE" || n.name == "LIBRARY") {
                patched += patch_textures(n.attr_end, n.end);
            }
            o = n.end;
        }
        return patched;
    }
};

}  // namespace

void install_glyph_patch() {
    const std::string rel = "frontend/b_persistent.pssg";
    std::ifstream in(vfs::host_path(std::string(vfs::kInstallDir) + "\\frontend\\b_persistent.pssg"), std::ios::binary);
    if (!in) {
        rt::log("glyphs: cannot read %s", rel.c_str());
        return;
    }
    Pssg p;
    {
        std::ostringstream s;
        s << in.rdbuf();
        p.d = s.str();
    }
    const size_t root = p.parse_header();
    int n = root ? p.patch_textures(root, p.d.size()) : 0;
    if (root && n == 0) {  // DiRT 3's atlas
        p.scan_atlas(root, p.d.size());
        n = p.patch_atlas();
    }
    if (n == 0) {
        rt::log("glyphs: no controller textures found in %s, left as they are", rel.c_str());
        return;
    }
    const std::string dir = vfs::data_dir() + "/overlay";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string out = dir + "/b_persistent.pssg";
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    o.write(p.d.data(), std::streamsize(p.d.size()));
    if (!o) {
        rt::log("glyphs: cannot write %s", out.c_str());
        return;
    }
    o.close();
    vfs::set_override(rel, out);
    rt::log("glyphs: %d controller glyphs replaced with PlayStation ones", n);
}

}  // namespace shim
