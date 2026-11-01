/* launcher_picker.cpp - the launcher's drawn disc picker (contract in
 * launcher_picker.h): the folder model over std::filesystem, the command
 * map, and the panel drawn with the launcher's shape pass and imgui. */
#include "launcher_picker.h"

#include "launcher_internal.h"
#include "launcher_shapes.h"
#include "launcher_sky.h"

#include "imgui.h"

#include "openpete_log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

/* The panel in the 640x448 layout space, below the title. */
constexpr float kPanelX0 = 80.0f, kPanelX1 = 560.0f, kPanelY0 = 132.0f, kPanelY1 = 416.0f;
constexpr float kPadX = 18.0f, kPadY = 12.0f;
constexpr float kPathSize = 9.0f;    /* the folder line's text size, layout px as ProggyClean */
constexpr float kRowSize = 12.0f;    /* a row's text size */
constexpr float kRowH = 20.0f;       /* a row's pitch */
constexpr float kListTop = kPanelY0 + kPadY + 22.0f;
constexpr int kRowsShown = (int)((kPanelY1 - kPadY - kListTop) / kRowH);
/* The most entries one folder lists; the rest are logged as left out. */
constexpr size_t kMaxRows = 4096;

/* One row: the parent folder, a folder, or an image. */
struct Row {
    enum Kind { Up, Dir, Image } kind;
    std::string label;
    fs::path path;
};

struct Picker {
    bool open = false;
    std::vector<fs::path> roots;
    std::vector<std::string> root_labels;
    fs::path dir;            /* the folder listed; empty for the root list */
    std::vector<Row> rows;
    int sel = 0;
    int top = 0;             /* the first row shown */
    std::string chosen;
};
Picker pk;

/** @brief Compare two names case-insensitively, byte by byte.
 *
 * @param a A name.
 * @param b Another.
 * @return true when @p a sorts before @p b. */
bool name_less(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower((unsigned char)x) < std::tolower((unsigned char)y);
    });
}

/** @brief Report whether a file name ends in .cue or .bin, in any case.
 *
 * @param name The name.
 * @return true for a disc image. */
bool image_name(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".cue" || ext == ".bin";
}

/** @brief Fill the roots: the harness roots when OPENPETE_LAUNCHER_PICKER_ROOTS is set, else the home directory (USERPROFILE on Windows) and /run/media when each is a folder. */
void read_roots() {
    pk.roots.clear();
    pk.root_labels.clear();
    std::error_code ec;
    if (const char* e = getenv("OPENPETE_LAUNCHER_PICKER_ROOTS"); e && *e) {
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::string all(e);
        for (size_t at = 0; at <= all.size();) {
            const size_t end = std::min(all.find(sep, at), all.size());
            if (end > at) {
                const fs::path p(all.substr(at, end - at));
                pk.roots.push_back(p);
                pk.root_labels.push_back(p.string());
            }
            at = end + 1;
        }
        return;
    }
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    if (home && *home && fs::is_directory(home, ec)) {
        pk.roots.emplace_back(home);
        pk.root_labels.push_back(std::string("Home (") + home + ")");
    }
    if (fs::is_directory("/run/media", ec)) {
        pk.roots.emplace_back("/run/media");
        pk.root_labels.emplace_back("Mounted media (/run/media)");
    }
}

/** @brief List a folder into the rows: the parent row, then its folders, then its .cue and .bin images, each group by name; hidden entries (a leading dot) are left out.
 *
 * An empty @p dir lists the roots. An unreadable folder lists the parent
 * row alone and logs the error.
 * @param dir The folder, or empty for the root list. */
void list(const fs::path& dir) {
    pk.dir = dir;
    pk.rows.clear();
    pk.sel = 0;
    pk.top = 0;
    if (dir.empty()) {
        for (size_t i = 0; i < pk.roots.size(); i++) pk.rows.push_back({Row::Dir, pk.root_labels[i], pk.roots[i]});
        OPL_INFO_F("launcher", "picker: the root list, %zu roots", pk.roots.size());
        return;
    }
    pk.rows.push_back({Row::Up, "..", {}});
    std::vector<Row> dirs, images;
    std::error_code ec;
    size_t left_out = 0;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) OPL_WARN_F("launcher", "picker: %s cannot be read: %s", dir.string().c_str(), ec.message().c_str());
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code e2;
        const bool is_dir = it->is_directory(e2);
        const bool is_image = !is_dir && it->is_regular_file(e2) && image_name(name);
        if (!is_dir && !is_image) continue;
        if (dirs.size() + images.size() >= kMaxRows) { left_out++; continue; }
        (is_dir ? dirs : images).push_back({is_dir ? Row::Dir : Row::Image, is_dir ? name + "/" : name, it->path()});
    }
    auto by_name = [](const Row& a, const Row& b) { return name_less(a.label, b.label); };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(images.begin(), images.end(), by_name);
    pk.rows.insert(pk.rows.end(), dirs.begin(), dirs.end());
    pk.rows.insert(pk.rows.end(), images.begin(), images.end());
    OPL_INFO_F("launcher", "picker: in %s: %zu folders, %zu images%s", dir.string().c_str(), dirs.size(),
               images.size(), left_out ? " (more left out)" : "");
}

/** @brief Report whether a folder is one of the roots.
 *
 * @param dir The folder.
 * @return true for a root. */
bool is_root(const fs::path& dir) {
    return std::find(pk.roots.begin(), pk.roots.end(), dir) != pk.roots.end();
}

/** @brief Go up: from a root to the root list, from another folder to its parent; from the root list, close.
 *
 * @return LAUNCHER_PICKER_CLOSED from the root list, else LAUNCHER_PICKER_STAYS. */
launcher_picker_result go_up() {
    if (pk.dir.empty()) {
        pk.open = false;
        OPL_INFO_F("launcher", "picker: closed from the root list");
        return LAUNCHER_PICKER_CLOSED;
    }
    const fs::path from = pk.dir;
    list(is_root(from) || !from.has_parent_path() || from.parent_path() == from ? fs::path() : from.parent_path());
    /* Select the folder just left, so going back down is one press. */
    for (size_t i = 0; i < pk.rows.size(); i++)
        if (pk.rows[i].path == from) pk.sel = (int)i;
    return LAUNCHER_PICKER_STAYS;
}

/** @brief Activate the selected row: go up, open its folder, or choose its image and close.
 *
 * @return What it did. */
launcher_picker_result activate() {
    if (pk.sel < 0 || (size_t)pk.sel >= pk.rows.size()) return LAUNCHER_PICKER_STAYS;
    const Row r = pk.rows[(size_t)pk.sel];
    if (r.kind == Row::Up) return go_up();
    if (r.kind == Row::Dir) {
        list(r.path);
        return LAUNCHER_PICKER_STAYS;
    }
    pk.chosen = r.path.string();
    pk.open = false;
    OPL_INFO_F("launcher", "picker: chosen %s", pk.chosen.c_str());
    return LAUNCHER_PICKER_CHOSEN;
}

/** @brief Move the selection by rows, clamped to the list, and scroll so it shows.
 *
 * @param d The rows to move; negative moves up. */
void move(int d) {
    if (pk.rows.empty()) return;
    pk.sel = std::clamp(pk.sel + d, 0, (int)pk.rows.size() - 1);
    if (pk.sel < pk.top) pk.top = pk.sel;
    if (pk.sel >= pk.top + kRowsShown) pk.top = pk.sel - kRowsShown + 1;
}

/** @brief Shorten a line with an ellipsis to fit a width at the current font.
 *
 * With @p keep_end the result is "..." and the line's end, else the line's
 * start and "...".
 * Bytes are dropped one UTF-8 character at a time.
 * @param text The line.
 * @param width The width in window pixels.
 * @param keep_end true to keep the end (a folder path), false the start (a name).
 * @return The line, shortened when it does not fit. */
std::string fit(const std::string& text, float width, bool keep_end) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string t = text;
    auto lead = [](char c) { return ((unsigned char)c & 0xC0) != 0x80; };
    while (!t.empty()) {
        if (keep_end) {
            size_t n = 1;
            while (n < t.size() && !lead(t[n])) n++;
            t.erase(0, n);
        } else {
            size_t n = t.size() - 1;
            while (n > 0 && !lead(t[n])) n--;
            t.erase(n);
        }
        const std::string out = keep_end ? "..." + t : t + "...";
        if (ImGui::CalcTextSize(out.c_str()).x <= width) return out;
    }
    return "...";
}

}  // namespace

bool launcher_picker_only(void) {
    const char* gs = getenv("GAMESCOPE_WAYLAND_DISPLAY");
    return gs && *gs;
}

void launcher_picker_open(const char* why) {
    if (pk.open) return;
    pk.open = true;
    read_roots();
    OPL_INFO_F("launcher", "picker: open (%s)", why ? why : "");
    list(fs::path());
}

bool launcher_picker_shown(void) { return pk.open; }

launcher_picker_result launcher_picker_command(launcher_picker_cmd c) {
    if (!pk.open) return LAUNCHER_PICKER_CLOSED;
    switch (c) {
        case LAUNCHER_PICKER_UP: move(-1); return LAUNCHER_PICKER_STAYS;
        case LAUNCHER_PICKER_DOWN: move(1); return LAUNCHER_PICKER_STAYS;
        case LAUNCHER_PICKER_ACTIVATE: return activate();
        case LAUNCHER_PICKER_BACK: return go_up();
        case LAUNCHER_PICKER_CLOSE:
            pk.open = false;
            OPL_INFO_F("launcher", "picker: closed");
            return LAUNCHER_PICKER_CLOSED;
    }
    return LAUNCHER_PICKER_STAYS;
}

const char* launcher_picker_chosen(void) { return pk.chosen.c_str(); }

launcher_picker_result launcher_picker_draw(float scale, float ox, float oy) {
    if (!pk.open) return LAUNCHER_PICKER_STAYS;
    const ImVec2 a(ox + kPanelX0 * scale, oy + kPanelY0 * scale), b(ox + kPanelX1 * scale, oy + kPanelY1 * scale);
    launcher_shape_panel("picker.panel", a, b, scale, kChipAlpha * launcher_sky_cover());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x = a.x + kPadX * scale, x_end = b.x - kPadX * scale;   /* the text's left and right edges */
    ImGui::PushClipRect(ImVec2(x, a.y), ImVec2(x_end, b.y), true);
    launcher_push_body_font(kPathSize * scale);
    const std::string where =
        fit(pk.dir.empty() ? std::string("Choose a disc image (.cue or .bin)") : pk.dir.string(), x_end - x, true);
    const ImVec2 at(x, a.y + kPadY * scale);
    const ImVec2 psz = ImGui::CalcTextSize(where.c_str());
    launcher_shape_scrim("picker.path", nullptr, ImGui::GetFontSize(), at, where.c_str(), scale);
    dl->AddText(at, ImGui::GetColorU32(IM_COL32(190, 175, 225, 255)), where.c_str());
    launcher_manifest_item("picker.path", "text", at.x, at.y, at.x + psz.x, at.y + psz.y);
    ImGui::PopFont();
    launcher_push_body_font(kRowSize * scale);
    launcher_picker_result res = LAUNCHER_PICKER_STAYS;
    const float row_w = x_end - x;
    for (int i = 0; i < kRowsShown && pk.top + i < (int)pk.rows.size(); i++) {
        const int r = pk.top + i;
        const ImVec2 ra(x, oy + (kListTop + (float)i * kRowH) * scale);
        ImGui::SetCursorScreenPos(ra);
        const std::string id = "##picker.row." + std::to_string(i);
        const bool clicked = ImGui::InvisibleButton(id.c_str(), ImVec2(std::max(1.0f, row_w), kRowH * scale));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (clicked) {
            pk.sel = r;
            OPL_INFO_F("launcher", "picker: row %d clicked", r);
            res = activate();
            break;   /* the list may have changed; the next present draws it */
        }
        const ImU32 col = r == pk.sel ? IM_COL32(240, 138, 30, 255) : IM_COL32(235, 230, 245, 255);
        const float ty = ra.y + (kRowH * scale - ImGui::GetFontSize()) * 0.5f;
        const std::string shown = fit(pk.rows[(size_t)r].label, x_end - x, false);
        const char* label = shown.c_str();
        const std::string rid = "picker.row." + std::to_string(i);
        launcher_shape_scrim(rid.c_str(), nullptr, ImGui::GetFontSize(), ImVec2(x, ty), label, scale);
        dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(col), label);
        const ImVec2 tsz = ImGui::CalcTextSize(label);
        launcher_manifest_item(rid.c_str(), "text", x, ty, x + tsz.x, ty + tsz.y);
    }
    ImGui::PopFont();
    ImGui::PopClipRect();
    const ImGuiIO& io = ImGui::GetIO();
    if (res == LAUNCHER_PICKER_STAYS && io.MouseWheel != 0.0f && ImGui::IsMouseHoveringRect(a, b))
        move(io.MouseWheel > 0.0f ? -1 : 1);
    return res;
}
