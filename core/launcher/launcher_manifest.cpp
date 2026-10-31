// core/launcher/launcher_manifest.cpp - the launcher's element manifest: each
// element a screen draws, by id, kind and rect in window pixels, written once
// per screen when the harness variable OPENPETE_LAUNCHER_MANIFEST=<base> is set
// (contract in launcher_internal.h). The ids are those of the reference
// layout's manifest (tests/launcher/layout-reference.json), which a script
// compares the file with.
#include "launcher_internal.h"

#include "openpete_atomic_file.h"
#include "openpete_log.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

/* One element of the frame being drawn. */
struct Item {
    std::string id;
    const char* kind;
    float r[4];
};

std::vector<Item> s_items;   /* the frame being drawn, cleared by launcher_manifest_begin */
unsigned s_written = 0;      /* one bit per screen tag already written */

/** @brief Append a JSON string literal of @p s to @p out, escaping quotes, backslashes and control characters.
 *
 * @param out The buffer.
 * @param s The string. */
void json_string(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", (unsigned)(unsigned char)c);
            out += esc;
        } else out += c;
    }
    out += '"';
}

}  // namespace

void launcher_manifest_begin(void) { s_items.clear(); }

void launcher_manifest_item(const char* id, const char* kind, float x0, float y0, float x1, float y1) {
    s_items.push_back({id ? id : "", kind ? kind : "", {x0, y0, x1, y1}});
}

void launcher_manifest_write(unsigned tag, const char* screen, float w, float h) {
    const char* base = getenv("OPENPETE_LAUNCHER_MANIFEST");
    const unsigned bit = 1u << tag;
    if (!base || !*base || (s_written & bit)) return;
    s_written |= bit;
    std::string j = "{\"screen\": ";
    json_string(j, screen ? screen : "");
    char num[160];
    snprintf(num, sizeof num, ", \"window\": [%d, %d], \"items\": [", (int)w, (int)h);
    j += num;
    for (size_t i = 0; i < s_items.size(); i++) {
        const Item& it = s_items[i];
        j += i ? ",\n  {\"id\": " : "\n  {\"id\": ";
        json_string(j, it.id);
        j += ", \"kind\": ";
        json_string(j, it.kind);
        snprintf(num, sizeof num, ", \"rect\": [%.2f, %.2f, %.2f, %.2f]}", it.r[0], it.r[1], it.r[2], it.r[3]);
        j += num;
    }
    j += "]}\n";
    const std::string path = std::string(base) + "." + (screen ? screen : "") + ".json";
    const int rc = op_atomic_write_file(path.c_str(), j.data(), j.size());
    OPL_INFO_F("launcher", "manifest %s: %zu items at %dx%d (%s, rc %d)", screen ? screen : "",
               s_items.size(), (int)w, (int)h, path.c_str(), rc);
}
