// core/launcher/launcher_sky.cpp - the launcher's background rotation over the
// library's level domes (contract in launcher_sky.h), read from each game's
// launcher cache (domes.bin, games/common/launcher_cache.h) and drawn by the
// sky pass (openpete_launcher_sky.h).
#include "launcher_sky.h"

#include "launcher_internal.h"

#include "openpete_launcher_sky.h"
#include "openpete_log.h"
#include "openpete_paths.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

/* The dome settings of the Sky Lab prototype the design took. */
constexpr double kPanDegPerS = 2.0;
constexpr float kHfovDeg = 90.0f;
constexpr float kPitchDeg = 8.0f;
constexpr double kHoldS = 8.0;
constexpr double kFadeS = 2.5;
constexpr double kRevealS = 3.5;      /* the first disc's transition */
constexpr float kRevealSoft = 0.16f;  /* the reveal edge's half-width, in window heights */
constexpr float kVignette = 0.25f;
constexpr float kGrain = 0.008f;
/* The longest step the sky clock takes between two presents: a present held
 * off longer (a blocked or hidden window) resumes the motion where it was. */
constexpr double kMaxStepS = 0.25;

/* Bounds a domes file is read within (launcher_cache.h's layout). */
constexpr uint32_t kMaxDomes = 1024;
constexpr uint32_t kMaxVerts = 65536;
constexpr uint32_t kMaxTris = 1u << 20;

/* One level's dome as the cache holds it. */
struct Dome {
    std::string game, level;
    std::vector<uint8_t> verts;
    std::vector<uint16_t> tris;
    openpete_sky_mesh mesh = {};
};
using DomeP = std::shared_ptr<const Dome>;

/* One game's domes and the file they were read from. */
struct GameDomes {
    std::string game;
    long long size = -1;
    long long mtime_ns = -1;
    std::vector<DomeP> domes;
};

struct Sky {
    std::vector<GameDomes> games;
    std::vector<DomeP> bag;   /* the rest of the current no-repeat order, next first */
    DomeP cur, nxt;           /* the dome on screen; the incoming one while fading */
    bool fading = false;
    bool reveal = false;      /* the incoming dome enters through the reveal */
    double clock = 0.0;       /* the sky clock, seconds */
    double fade_t0 = 0.0, level_t0 = 0.0;
    op_time_t last_now = 0;
    bool started = false;
    float title_u = 0.5f, title_v = 0.2f;
    uint64_t serial = 0;
    unsigned presents = 0;
    bool opaque = false;      /* the last view handed is one opaque dome */
    std::mt19937 rng{std::random_device{}()};
};
Sky sk;

/** @brief Read a little-endian u32 at an offset of a buffer, advancing it.
 *
 * @param b The buffer.
 * @param at The offset, advanced by 4.
 * @param v Receives the value.
 * @return false when the buffer ends first. */
bool rd_u32(const std::vector<uint8_t>& b, size_t& at, uint32_t& v) {
    if (at > b.size() || b.size() - at < 4) return false;
    v = (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24);
    at += 4;
    return true;
}

/** @brief Parse one dome record of a domes file.
 *
 * @param b The file's bytes.
 * @param at The record's offset, advanced past it.
 * @param game The game id the dome is recorded under.
 * @param d Receives the dome; its mesh serial is set by the caller.
 * @return false on a truncated record, a count past the bounds or a vertex index out of range. */
bool parse_dome(const std::vector<uint8_t>& b, size_t& at, const char* game, Dome& d) {
    if (at >= b.size()) return false;
    const size_t n = b[at++];
    if (b.size() - at < n) return false;
    d.game = game;
    d.level.assign((const char*)b.data() + at, n);
    at += n;
    uint32_t nv = 0, ntri = 0, sectors = 0;
    if (!rd_u32(b, at, nv) || !rd_u32(b, at, ntri) || !rd_u32(b, at, sectors)) return false;
    if (nv > kMaxVerts || ntri > kMaxTris) return false;
    const size_t need = 3 + (size_t)nv * 9u + (size_t)ntri * 6u;
    if (b.size() - at < need) return false;
    std::memcpy(d.mesh.bg, b.data() + at, 3);
    at += 3;
    d.verts.assign(b.begin() + (long)at, b.begin() + (long)(at + (size_t)nv * 9u));
    at += (size_t)nv * 9u;
    d.tris.resize((size_t)ntri * 3u);
    for (size_t k = 0; k < d.tris.size(); k++, at += 2) {
        d.tris[k] = (uint16_t)(b[at] | (b[at + 1] << 8));
        if (d.tris[k] >= nv) return false;
    }
    d.mesh.nv = nv;
    d.mesh.ntri = ntri;
    return true;
}

/** @brief Read a game's domes file from its launcher cache.
 *
 * @param game The game id.
 * @param out Receives the file's size and modification time and, when
 *        @p read is true, its domes.
 * @param read false to stat the file only.
 * @return true when the file exists (and, when read, parsed whole). */
bool load_domes(const char* game, GameDomes& out, bool read) {
    char dir[1024];
    if (openpete_paths_game_state_dir("cache", game, dir, sizeof dir, 0) != 0) return false;
    const std::string path = std::string(dir) + "/domes.bin";
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    out.game = game;
    out.size = (long long)st.st_size;
#if defined(_WIN32)
    out.mtime_ns = (long long)st.st_mtime * 1000000000LL;
#else
    out.mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    if (!read) return true;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> b((size_t)st.st_size);
    const bool whole = std::fread(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    size_t at = 8;
    uint32_t count = 0;
    if (!whole || b.size() < 12 || std::memcmp(b.data(), "OPDOMES1", 8) != 0 || !rd_u32(b, at, count) ||
        count > kMaxDomes) {
        OPL_WARN_F("launcher", "sky: %s is not a domes file", path.c_str());
        return false;
    }
    out.domes.clear();
    for (uint32_t i = 0; i < count; i++) {
        auto d = std::make_shared<Dome>();
        if (!parse_dome(b, at, game, *d)) {
            OPL_WARN_F("launcher", "sky: %s: dome %u of %u is malformed; the game's domes are not shown",
                       path.c_str(), i, count);
            out.domes.clear();
            return false;
        }
        d->mesh.serial = ++sk.serial;
        d->mesh.verts = d->verts.data();
        d->mesh.tris = d->tris.data();
        if (d->mesh.ntri > 0) out.domes.push_back(d);
    }
    return true;
}

/** @brief Report whether a dome belongs to the current pool: the domes of the library games loaded now.
 *
 * @param d The dome.
 * @return true when it does. */
bool in_pool(const DomeP& d) {
    if (!d) return false;
    for (const GameDomes& g : sk.games)
        if (g.game == d->game) return std::find(g.domes.begin(), g.domes.end(), d) != g.domes.end();
    return false;
}

/** @brief The next dome of the no-repeat order, refilling it in a new random order (without the dome on screen, when another exists) once it runs out.
 *
 * @return The dome, or null with an empty pool. */
DomeP pick_next() {
    sk.bag.erase(std::remove_if(sk.bag.begin(), sk.bag.end(),
                                [](const DomeP& d) { return d == sk.cur || !in_pool(d); }),
                 sk.bag.end());
    if (sk.bag.empty()) {
        std::vector<DomeP> pool;
        for (const GameDomes& g : sk.games) pool.insert(pool.end(), g.domes.begin(), g.domes.end());
        if (pool.empty()) return nullptr;
        for (const DomeP& d : pool)
            if (d != sk.cur || pool.size() == 1) sk.bag.push_back(d);
        std::shuffle(sk.bag.begin(), sk.bag.end(), sk.rng);
    }
    DomeP d = sk.bag.front();
    sk.bag.erase(sk.bag.begin());
    return d;
}

/** @brief End a running crossfade or reveal: the incoming dome becomes the current one and its hold starts. */
void finish_fade() {
    if (sk.fading) {
        sk.cur = sk.nxt;
        sk.level_t0 = sk.clock;
    }
    sk.nxt = nullptr;
    sk.fading = false;
    sk.reveal = false;
}

/** @brief Start a crossfade, or with @p reveal the reveal, to a dome.
 *
 * The dome on screen restarts its hold instead when it is the target.
 * @param d The target.
 * @param reveal true for the reveal. */
void start_fade(const DomeP& d, bool reveal) {
    finish_fade();
    if (!d || (d == sk.cur && !reveal)) {
        sk.level_t0 = sk.clock;
        return;
    }
    sk.nxt = d;
    sk.fading = true;
    sk.reveal = reveal;
    sk.fade_t0 = sk.clock;
}

/** @brief Join one game's domes to the rotation, replacing any of the game held before: inserted at random positions of the current order, or, when no dome shows, faded in from the clear colour or revealed.
 *
 * @param gd The game's domes.
 * @param reveal_ok true to reveal the game's first level when no dome shows.
 * @return true when the reveal started. */
bool join(GameDomes&& gd, bool reveal_ok) {
    const bool shown = sk.cur || (sk.fading && sk.nxt);
    const std::string game = gd.game;
    auto it = std::find_if(sk.games.begin(), sk.games.end(), [&](const GameDomes& g) { return g.game == game; });
    if (it != sk.games.end()) *it = std::move(gd);
    else it = sk.games.insert(sk.games.end(), std::move(gd));
    const std::vector<DomeP>& domes = it->domes;
    if (domes.empty()) return false;
    const bool revealing = !shown && reveal_ok;
    sk.bag.erase(std::remove_if(sk.bag.begin(), sk.bag.end(), [](const DomeP& d) { return !in_pool(d); }),
                 sk.bag.end());
    for (size_t i = revealing ? 1 : 0; i < domes.size(); i++) {
        std::uniform_int_distribution<size_t> at(0, sk.bag.size());
        sk.bag.insert(sk.bag.begin() + (long)at(sk.rng), domes[i]);
    }
    if (revealing) start_fade(domes[0], true);
    else if (!shown) start_fade(pick_next(), false);   /* a crossfade from the clear colour */
    return revealing;
}

/** @brief Advance the sky clock to a present's instant and run the rotation's holds and fades.
 *
 * @param now The present's instant. */
void advance(op_time_t now) {
    double dt = sk.started ? (double)(now - sk.last_now) / (double)OP_NS_PER_SEC : 0.0;
    sk.started = true;
    sk.last_now = now;
    sk.clock += std::clamp(dt, 0.0, kMaxStepS);
    if (sk.fading && sk.clock - sk.fade_t0 >= (sk.reveal ? kRevealS : kFadeS)) finish_fade();
    if (sk.cur && !in_pool(sk.cur) && !sk.fading) {   /* a reloaded game's dome gives way at once */
        sk.cur = pick_next();
        sk.level_t0 = sk.clock;
    }
    if (!sk.fading && sk.cur && sk.clock - sk.level_t0 >= kHoldS) start_fade(pick_next(), false);
}

/** @brief The incoming dome's weight, smoothstepped over its fade or reveal.
 *
 * @return 0..1; 0 while no fade runs. */
float fade_mix() {
    if (!sk.fading) return 0.0f;
    const double x = std::clamp((sk.clock - sk.fade_t0) / (sk.reveal ? kRevealS : kFadeS), 0.0, 1.0);
    return (float)(x * x * (3.0 - 2.0 * x));
}

}  // namespace

void launcher_sky_seed(const char* game) {
    if (!launcher_screens() || !game || !*game) return;
    GameDomes probe;
    if (!load_domes(game, probe, false)) return;
    for (const GameDomes& g : sk.games)
        if (g.game == game && g.size == probe.size && g.mtime_ns == probe.mtime_ns) return;
    GameDomes gd;
    if (!load_domes(game, gd, true)) return;
    const size_t n = gd.domes.size();
    join(std::move(gd), false);
    OPL_INFO_F("launcher", "sky: %zu domes of %s loaded from the cache", n, game);
}

void launcher_sky_assets_ready(const char* game) {
    if (!launcher_screens() || !game || !*game) return;
    GameDomes probe;
    if (!load_domes(game, probe, false)) {
        OPL_INFO_F("launcher", "sky: %s's cache holds no domes", game);
        return;
    }
    for (const GameDomes& g : sk.games)
        if (g.game == game && g.size == probe.size && g.mtime_ns == probe.mtime_ns) {
            OPL_INFO_F("launcher", "sky: %s's %zu domes are in the rotation (the cache is unchanged)", game,
                       g.domes.size());
            return;
        }
    GameDomes gd;
    if (!load_domes(game, gd, true)) return;
    const size_t n = gd.domes.size();
    const bool revealed = join(std::move(gd), true);
    OPL_INFO_F("launcher", "sky: %zu domes of %s joined the rotation%s", n, game,
               revealed ? "; the reveal starts" : "");
}

void launcher_sky_title_at(float u, float v) {
    sk.title_u = u;
    sk.title_v = v;
}

void launcher_sky_present(op_time_t now, bool after_first) {
    sk.opaque = false;
    if (!launcher_screens()) return;
    advance(now);
    if (!after_first || (!sk.cur && !sk.nxt)) return;
    sk.opaque = sk.cur && !sk.fading;
    openpete_sky_view v = {};
    const float mix = fade_mix();
    if (sk.cur) v.layer[0] = {&sk.cur->mesh, 1.0f, 0};
    if (sk.fading && sk.nxt) v.layer[1] = {&sk.nxt->mesh, mix, sk.reveal ? 1 : 0};
    v.yaw_deg = (float)std::fmod(sk.clock * kPanDegPerS, 360.0);
    v.pitch_deg = kPitchDeg;
    v.hfov_deg = kHfovDeg;
    v.clock_s = (float)std::fmod(sk.clock, 3600.0);
    v.reveal = mix;
    v.reveal_u = sk.title_u;
    v.reveal_v = sk.title_v;
    v.reveal_soft = kRevealSoft;
    v.vignette = kVignette;
    v.grain = kGrain;
    v.seed = (float)(sk.presents++ % 64u) * 1.37f;
    openpete_native_launcher_sky_next(&v);
}

bool launcher_sky_opaque(void) { return sk.opaque; }

bool launcher_sky_shown(const char** game, const char** level) {
    const bool incoming = sk.fading && sk.nxt && (fade_mix() > 0.5f || !sk.cur);
    const DomeP& d = incoming ? sk.nxt : sk.cur;
    if (game) *game = d ? d->game.c_str() : nullptr;
    if (level) *level = d ? d->level.c_str() : nullptr;
    return d != nullptr;
}
