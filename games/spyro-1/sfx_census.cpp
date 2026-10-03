// games/spyro-1/sfx_census.cpp - the sfx pack's offline census.
//
// Walks every sample the game can key, straight from the disc's WAD (and
// any custom-level Data files handed over), and writes each distinct one
// as a WAV named by its content hash (core/host/sfx_ident.h) plus an
// index.txt that maps identities back to where they were found. No game
// boot, no SPU: the same END-flag walk and decoder the voice path uses
// run over the WAD bytes here, so a file's name and PCM are exactly what
// the runtime computes for the sample at key-on.
//
// Sample sources - the game plays from four kinds, only the first of
// which is a def array:
//   1. A level (or cutscene) entry's SoundDefinition array: the last
//      component of its Data blob is {int size; u_char table[256]; int
//      count; SoundDefinition defs[count]} (SetNewSoundTable); each
//      def's m_Addr is relative to the SRAM bank the entry blasts to SPU
//      0x1010.
//   2. Dragon speech: per level, six WadDragonHeader blobs whose m_SpuData
//      is uploaded mid-level to 0x80000 - len and keyed through the
//      one-entry cutscene def.
//   3. Cutscene entries and the intro: the cutscene def's m_Addr is a
//      literal per site - bank start with g_CutscenePitchTable[idx], and
//      0x262D0 for the post-demo intro (keyed from the titlescreen bank).
//   4. Titlescreen: ten samples at g_TitlescreenSPUOffsets[i] + 0x1010 at
//      pitch 0x800.
// Credits play through the same cutscene def from their own data
// entries and are not walked here (they resolve at runtime like every
// other key-on; only the authoring dump lacks them).
#include "openpete_log.h"
#include "iso_reader.h"
#include "openpete_disc_trace.h"
#include "sfx_ident.h"
#include "spyro1_sfx_names.h"

extern "C" {
#include "../../third_party/PsyCross/src/audio/PsyX_SPUAL.h"
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kWadSector  = 37;
constexpr uint32_t kSectorSize = 2048;
constexpr size_t   kCutsceneOff   = 24;   // g_WadHeader.m_CutsceneData[4]
constexpr size_t   kLevelEntryOff = 72;   // g_WadHeader.m_LevelEntry[36]
constexpr int      kLevelSlots    = 36;
constexpr uint32_t kSpuBankBase   = 0x1010;
constexpr uint32_t kSpuTop        = 0x80000;
constexpr int      kDefStride     = 0x14;

/* The cutscene def's literal addresses and pitches (init.c / update.c /
 * titlescreen.c). SPU addresses; bank-relative = addr - 0x1010. */
constexpr int      kCutscenePitch[4] = { 0x800, 0x659, 0x800, 0x800 };
constexpr uint32_t kIntroAddr        = 0x262D0;
constexpr uint32_t kTitleOffsets[10] = {
    0x0003ABE0, 0x0003BD00, 0x0003C870, 0x0003F3E0, 0x000403C0,
    0x00040A70, 0x00042340, 0x00044280, 0x00045310, 0x00045670 };
constexpr int      kTitlePitch       = 0x800;

struct OffsetLength { int32_t offset, length; };

struct LevelHeader {
    int32_t vram_sram_off, vram_sram_size;
    int32_t data_off, data_size;
    int32_t model_off, model_size;
    int32_t scene_off, scene_size;
    OffsetLength dragons[6];
};

struct Source {
    std::string entry;   /* "level_10", "cutscene_1", "file:grim_garden.wad" */
    std::string label;   /* organisational prefix */
    int         defidx;  /* -1 for synthetic sources */
    uint32_t    off;     /* offset into `bank` */
    int         pitch;   /* base pitch, or -1 when the game sets it elsewhere */
    int32_t     loop_raw;/* the def's m_LoopAddr as stored */
};

struct Bank {
    std::vector<uint8_t> bytes;
    std::vector<Source>  sources;
};

struct Stats {
    int entries = 0, sources = 0, dumped = 0, dup = 0, no_end = 0, failed = 0;
};

/** @brief Reads a byte range of the disc's WAD into a buffer.
 *
 * The WAD starts at kWadSector; the range is read sector by sector, with the
 * offset's within-sector skew applied to the first sector only. The buffer
 * is resized to len and zero-filled before the read.
 * @param r The open disc.
 * @param wad_off Byte offset from the start of the WAD.
 * @param len Byte count.
 * @param out Receives the bytes; its contents are unspecified on failure.
 * @return False when a sector read fails. */
bool read_span(PS1::ISOReader& r, uint32_t wad_off, uint32_t len,
               std::vector<uint8_t>& out)
{
    out.assign(len, 0);
    uint32_t lba = kWadSector + wad_off / kSectorSize;
    uint32_t skew = wad_off % kSectorSize;
    uint8_t sec[kSectorSize];
    uint32_t done = 0;
    /* Offsets are sector-aligned in practice; the skew path keeps a
     * hand-built entry honest. */
    while (done < len) {
        bool ok = r.ReadSector(lba, sec);
        openpete_disc_trace_sectors(OPENPETE_DISC_TAG_SFX, lba, 1, OPENPETE_DISC_DIRECT);
        if (!ok) return false;
        uint32_t from = done == 0 ? skew : 0;
        uint32_t n = kSectorSize - from;
        if (n > len - done) n = len - done;
        std::memcpy(out.data() + done, sec + from, n);
        done += n;
        lba++;
    }
    return true;
}

/* An entry's bytes, either from the disc (offset into the WAD) or from a
 * file that IS the entry (a custom level's .wad). */
struct EntryReader {
    PS1::ISOReader* disc = nullptr;
    uint32_t        base = 0;
    const std::vector<uint8_t>* file = nullptr;

    /** @brief Reads a byte range of the entry.
     *
     * A file-backed entry is bounds-checked against the file size; a disc
     * entry reads through read_span at base + off.
     * @param off Byte offset from the start of the entry.
     * @param len Byte count.
     * @param out Receives the bytes.
     * @return False when the range lies outside the file or a disc read
     *         fails. */
    bool read(uint32_t off, uint32_t len, std::vector<uint8_t>& out) const {
        if (file) {
            if ((uint64_t)off + len > file->size()) return false;
            out.assign(file->begin() + off, file->begin() + off + len);
            return true;
        }
        return read_span(*disc, base + off, len, out);
    }
};

/** @brief Reads a native-endian int32 from unaligned bytes.
 *
 * @param p At least 4 readable bytes.
 * @return The value. */
int32_t rd32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
/** @brief Reads a native-endian uint16 from unaligned bytes.
 *
 * @param p At least 2 readable bytes.
 * @return The value. */
uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

/** @brief Locates the sound table in a level's Data blob by its shape.
 *
 * The earlier components are length-prefixed but the table is not (the
 * game reads it with a bare header), so a component walk cannot land on it.
 * Layout: u8 table[256], int count, SoundDefinition defs[count] (0x14
 * bytes each). A 4-aligned candidate matches when count is 1..255, every
 * table byte is below count (an absent slot holds def 0, the null sample),
 * every def's m_Addr is 16-aligned and inside the bank, every def's pitch
 * (+10) is 0x40..0x3FFF, and the defs end less than one sector before the
 * blob's end, since the table is the final component. The last matching
 * offset wins.
 * @param data The Data blob.
 * @param bank_len The SRAM bank's length in bytes.
 * @param count_out Receives the def count on a match; may be null.
 * @return The offset of the 256-byte table, or -1. */
long find_sound_table(const std::vector<uint8_t>& data, size_t bank_len,
                      int* count_out)
{
    long best = -1;
    for (size_t o = 0; o + 260 <= data.size(); o += 4) {
        int32_t count = rd32(data.data() + o + 256);
        if (count < 1 || count > 255) continue;
        size_t end = o + 260 + (size_t)count * kDefStride;
        if (end > data.size() || data.size() - end >= kSectorSize) continue;
        bool ok = true;
        for (int i = 0; i < 256 && ok; i++)
            if (data[o + i] >= (uint8_t)count) ok = false;
        for (int i = 0; i < count && ok; i++) {
            const uint8_t* d = data.data() + o + 260 + (size_t)i * kDefStride;
            uint32_t addr = (uint32_t)rd32(d);
            uint16_t pitch = rd16(d + 10);
            if (addr >= bank_len || (addr & 15) || pitch < 0x40 || pitch > 0x3FFF)
                ok = false;
        }
        if (ok) {
            best = (long)o;
            if (count_out) *count_out = count;
        }
    }
    return best;
}

/** @brief Names a sound definition by the first sfx slot that maps to it.
 *
 * Scans the 256-byte slot table for defidx and formats the matching
 * spyro1_sfx_slot_names entry; a definition no slot names is labelled
 * "def<defidx>".
 * @param table The level's 256-byte slot-to-definition table.
 * @param defidx The definition index.
 * @param buf Receives the label, truncated to cap.
 * @param cap Size of buf in bytes.
 * @return buf. */
const char* slot_label(const uint8_t* table, int defidx, char* buf, size_t cap)
{
    for (int s = 0; s < 256; s++)
        if (table[s] == defidx) {
            std::snprintf(buf, cap, "%s", spyro1_sfx_slot_names[s]);
            return buf;
        }
    std::snprintf(buf, cap, "def%d", defidx);
    return buf;
}

/** @brief Reads one level or cutscene entry's SRAM bank and appends it, with every sample source keyed from it, to banks.
 *
 * An entry whose SRAM size does not exceed 0x80000 or with no Data is
 * skipped as success. A level entry contributes its sound table's defs,
 * each labelled by the first sfx slot naming it, and one bank per dragon
 * speech blob (untagged pitch, since the rescue code sets it at key time).
 * A cutscene entry has no table and contributes the cutscene def at bank
 * start with its literal pitch; cutscenes 0 and 1 add the post-demo intro
 * at 0x262D0, and cutscene 0 the ten titlescreen samples.
 * @param er The entry's byte source.
 * @param name The entry's name for sources and logs.
 * @param cutscene True for a cutscene entry.
 * @param cutscene_idx The cutscene's index, selecting its literal pitch.
 * @param banks Receives the entry's banks.
 * @param st Counts the walked entries.
 * @return False on a read failure or a level entry with no sound table. */
bool walk_entry(const EntryReader& er, const std::string& name, bool cutscene,
                int cutscene_idx, std::vector<Bank>& banks, Stats& st)
{
    std::vector<uint8_t> hdr;
    if (!er.read(0, kSectorSize, hdr)) {
        OPL_ERROR_F("sfxdump", "%s: cannot read header", name.c_str());
        return false;
    }
    LevelHeader lh;
    std::memcpy(&lh, hdr.data(), sizeof lh);
    if (lh.vram_sram_size <= (int32_t)kSpuTop || lh.data_size <= 0) {
        OPL_WARN_F("sfxdump", "%s: no SRAM bank (vram_sram_size=%d) - skipped",
                   name.c_str(), lh.vram_sram_size);
        return true;
    }
    st.entries++;

    Bank bank;
    if (!er.read((uint32_t)lh.vram_sram_off + kSpuTop,
                 (uint32_t)lh.vram_sram_size - kSpuTop, bank.bytes)) {
        OPL_ERROR_F("sfxdump", "%s: cannot read SRAM bank", name.c_str());
        return false;
    }

    /* 1. the def array */
    std::vector<uint8_t> data;
    if (!er.read((uint32_t)lh.data_off, (uint32_t)lh.data_size, data)) {
        OPL_ERROR_F("sfxdump", "%s: cannot read Data", name.c_str());
        return false;
    }
    int count = 0;
    long tbl = cutscene ? -1 : find_sound_table(data, bank.bytes.size(), &count);
    if (tbl < 0 && !cutscene) {
        /* A cutscene entry's Data stops after the cyclorama: no table,
         * by design. A level without one is a layout surprise. */
        OPL_ERROR_F("sfxdump", "%s: sound table not found", name.c_str());
        return false;
    }
    const uint8_t* table = tbl >= 0 ? data.data() + tbl : nullptr;
    const uint8_t* defs  = table ? table + 260 : nullptr;
    for (int i = 0; i < count; i++) {
        const uint8_t* d = defs + i * kDefStride;
        Source s;
        s.entry  = name;
        char lb[32];
        s.label  = slot_label(table, i, lb, sizeof lb);
        s.defidx = i;
        s.off    = (uint32_t)rd32(d + 0);
        s.loop_raw = rd32(d + 4);
        s.pitch  = rd16(d + 10);
        bank.sources.push_back(s);
    }

    /* 3/4. the cutscene def's literal addresses */
    if (cutscene) {
        Source s;
        s.entry = name; s.defidx = -1; s.loop_raw = -1;
        s.label = "cutscene" + std::to_string(cutscene_idx);
        s.off = 0; s.pitch = kCutscenePitch[cutscene_idx & 3];
        bank.sources.push_back(s);
        if (cutscene_idx == 0 || cutscene_idx == 1) {
            /* The post-demo intro keys 0x262D0 in whichever of these two
             * banks is resident; walk both, the hash decides. */
            Source in = s;
            in.label = "intro";
            in.off = kIntroAddr - kSpuBankBase;
            in.pitch = kCutscenePitch[0];
            bank.sources.push_back(in);
        }
        if (cutscene_idx == 0) {
            for (int i = 0; i < 10; i++) {
                Source t = s;
                t.label = "title" + std::to_string(i);
                t.off = kTitleOffsets[i];
                t.pitch = kTitlePitch;
                bank.sources.push_back(t);
            }
        }
    }
    banks.push_back(std::move(bank));

    /* 2. dragon speech blobs: each is its own bank, keyed at SPU
     * 0x80000 - len. The base pitch is set by the rescue code at key
     * time and is not in the WAD, so speech dumps untagged. */
    for (int i = 0; i < 6 && !cutscene; i++) {
        if (lh.dragons[i].length <= 20) continue;
        std::vector<uint8_t> blob;
        if (!er.read((uint32_t)lh.dragons[i].offset,
                     (uint32_t)lh.dragons[i].length, blob)) {
            OPL_ERROR_F("sfxdump", "%s: cannot read dragon %d", name.c_str(), i);
            return false;
        }
        int32_t spu_off = rd32(blob.data() + 4);
        int32_t spu_len = rd32(blob.data() + 8);
        if (spu_len <= 0 || spu_off < 0 ||
            (size_t)spu_off + (size_t)spu_len > blob.size()) {
            OPL_WARN_F("sfxdump", "%s: dragon %d has no SPU data", name.c_str(), i);
            continue;
        }
        Bank sb;
        sb.bytes.assign(blob.begin() + spu_off, blob.begin() + spu_off + spu_len);
        Source s;
        s.entry = name; s.defidx = -1; s.loop_raw = -1;
        s.label = "speech" + std::to_string(i);
        s.off = 0; s.pitch = -1;
        sb.sources.push_back(s);
        banks.push_back(std::move(sb));
    }
    return true;
}

struct Seen { std::string file; };
using SeenKey = std::pair<uint64_t, int>; /* (hash, base pitch) */

/** @brief Writes one WAV per distinct sample of the banks and one index line per source.
 *
 * A source is skipped and counted (failed or no_end) when its offset lies
 * beyond the bank or no END flag is found. Files are keyed by (content hash,
 * base pitch): the first source seen writes the WAV, later ones reuse its
 * name and count as duplicates. Every kept source adds one index line.
 * @param banks The banks and their sources.
 * @param out Directory the WAVs are written to.
 * @param index The open index.txt stream.
 * @param seen Hash-and-pitch to file name, shared across calls.
 * @param st Counters updated per source. */
void dump_banks(const std::vector<Bank>& banks, const std::filesystem::path& out,
                std::ofstream& index, std::map<SeenKey, Seen>& seen, Stats& st)
{
    std::vector<int16_t> pcm;
    for (const Bank& b : banks) {
        for (const Source& s : b.sources) {
            st.sources++;
            if (s.off >= b.bytes.size()) {
                OPL_WARN_F("sfxdump", "%s %s: offset 0x%x beyond bank (%zu)",
                           s.entry.c_str(), s.label.c_str(), s.off, b.bytes.size());
                st.failed++;
                continue;
            }
            int avail = (int)(b.bytes.size() - s.off);
            const uint8_t* p = b.bytes.data() + s.off;
            int len = PsyX_SPUAL_SampleByteLength(p, avail);
            if (len < 16 || !(p[len - 15] & 0x01)) {
                OPL_WARN_F("sfxdump", "%s %s: no END flag within %d bytes",
                           s.entry.c_str(), s.label.c_str(), avail);
                st.no_end++;
                continue;
            }
            uint64_t h = op_sfx_hash(p, (size_t)len);
            pcm.assign((size_t)(len / 16) * 28, 0);
            int ls = 0, ll = 0;
            int n = PsyX_SPUAL_DecodeSample(p, len, pcm.data(), &ls, &ll);
            uint32_t rate = s.pitch > 0 ? op_sfx_rate_for_pitch(s.pitch) : 44100u;
            double ms = rate ? 1000.0 * (double)n / (double)rate : 0.0;

            /* One file per (hash, base pitch): a sample shared by defs at
             * different base pitches needs a tagged file for each. */
            auto it = seen.find(SeenKey(h, s.pitch));
            std::string file;
            if (it == seen.end()) {
                char fn[128];
                op_sfx_name_format(fn, sizeof fn, s.label.c_str(), h, s.pitch);
                file = fn;
                if (op_wav_write_mono16((out / file).string().c_str(), pcm.data(),
                                        (uint32_t)n, rate, ls, ll) != 0) {
                    OPL_ERROR_F("sfxdump", "cannot write %s", file.c_str());
                    st.failed++;
                    continue;
                }
                seen[SeenKey(h, s.pitch)] = Seen{ file };
                st.dumped++;
            } else {
                file = it->second.file;
                st.dup++;
            }
            char pitch_s[8];
            if (s.pitch >= 0) std::snprintf(pitch_s, sizeof pitch_s, "%04x", s.pitch);
            else std::snprintf(pitch_s, sizeof pitch_s, "-");
            char line[512];
            std::snprintf(line, sizeof line,
                          "%016llx pitch=%s entry=%s def=%d label=%s loop_raw=%d "
                          "bytes=%d samples=%d loop=%d+%d duration_ms=%.1f file=%s\n",
                          (unsigned long long)h, pitch_s,
                          s.entry.c_str(), s.defidx, s.label.c_str(), s.loop_raw,
                          len, n, ls, ll, ms, file.c_str());
            index << line;
        }
    }
}

/** @brief Opens the disc image, falling back to a sibling .bin, .img or .iso.
 *
 * @param reader The reader to open.
 * @param cue_path The disc path as given.
 * @return True when the reader opened either the path or an existing
 *         sibling with one of the fallback extensions. */
bool open_disc(PS1::ISOReader& reader, const char* cue_path)
{
    if (reader.Open(cue_path)) return true;
    std::filesystem::path dir = std::filesystem::path(cue_path).parent_path();
    for (const char* ext : { ".bin", ".img", ".iso" }) {
        std::filesystem::path alt = std::filesystem::path(cue_path).replace_extension(ext);
        if (std::filesystem::exists(alt) && reader.Open(alt.string())) return true;
    }
    (void)dir;
    return false;
}

} // namespace

/** @brief Writes the sfx pack census, a WAV per distinct sample plus index.txt, to a directory.
 *
 * Walks the disc's level and cutscene WAD entries (when cue_path is set)
 * and each comma-separated file of extra_wads as one level entry, then
 * decodes every sample source into out_dir. Logs the counters at the end.
 * @param cue_path Disc path; null or empty skips the disc.
 * @param out_dir Output directory, created when missing.
 * @param extra_wads Comma-separated custom-level Data file paths; null or
 *        empty for none.
 * @return 0 when every source dumped; 2 when any source failed or lacked an
 *         END flag, or no entry was walked; -1 when out_dir is null, no
 *         input was given, or the disc cannot be opened or read. */
extern "C" int spyro1_dump_sfx_pack(const char* cue_path, const char* out_dir,
                                    const char* extra_wads)
{
    if (!out_dir) {
        std::fprintf(stderr, "dump-sfx-pack: needs a directory\n");
        return -1;
    }
    if (!(cue_path && cue_path[0]) && !(extra_wads && extra_wads[0])) {
        std::fprintf(stderr, "dump-sfx-pack: no disc and no --dump-sfx-wads: "
                             "nothing to walk\n");
        return -1;
    }
    std::error_code ec;
    std::filesystem::path out(out_dir);
    std::filesystem::create_directories(out, ec);
    std::ofstream index(out / "index.txt");
    index << "# sfx pack census: one line per sample source; file = the WAV\n"
             "# carrying that hash (first source seen wins the label)\n";

    std::vector<Bank> banks;
    Stats st;
    std::map<SeenKey, Seen> seen;

    if (cue_path && cue_path[0]) {
        PS1::ISOReader reader;
        if (!open_disc(reader, cue_path)) {
            OPL_ERROR_F("sfxdump", "cannot open disc: %s", cue_path);
            return -1;
        }
        uint8_t hdr[kSectorSize];
        bool hdr_ok = reader.ReadSector(kWadSector, hdr);
        openpete_disc_trace_sectors(OPENPETE_DISC_TAG_SFX, kWadSector, 1, OPENPETE_DISC_DIRECT);
        if (!hdr_ok) {
            OPL_ERROR_F("sfxdump", "cannot read WAD header at LBA %u", kWadSector);
            return -1;
        }
        for (int f = 0; f < kLevelSlots; f++) {
            OffsetLength d;
            std::memcpy(&d, hdr + kLevelEntryOff + (size_t)f * 16 + 8, sizeof d);
            if (d.length <= 0) continue;
            EntryReader er; er.disc = &reader; er.base = (uint32_t)d.offset;
            char name[32];
            std::snprintf(name, sizeof name, "level_%d", (f / 6 + 1) * 10 + (f % 6));
            if (!walk_entry(er, name, false, 0, banks, st)) st.failed++;
        }
        for (int c = 0; c < 4; c++) {
            OffsetLength d;
            std::memcpy(&d, hdr + kCutsceneOff + (size_t)c * 8, sizeof d);
            if (d.length <= 0) continue;
            EntryReader er; er.disc = &reader; er.base = (uint32_t)d.offset;
            char name[32];
            std::snprintf(name, sizeof name, "cutscene_%d", c);
            if (!walk_entry(er, name, true, c, banks, st)) st.failed++;
        }
        dump_banks(banks, out, index, seen, st);
        banks.clear();
    }

    /* Custom-level Data files: each file IS a level entry. */
    if (extra_wads && extra_wads[0]) {
        std::string list(extra_wads);
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t comma = list.find(',', pos);
            std::string path = list.substr(pos, comma == std::string::npos
                                                    ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
            if (path.empty()) continue;
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                OPL_ERROR_F("sfxdump", "cannot open %s", path.c_str());
                st.failed++;
                continue;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
            EntryReader er; er.file = &bytes;
            std::string name = "file:" + std::filesystem::path(path).filename().string();
            if (!walk_entry(er, name, false, 0, banks, st)) st.failed++;
            dump_banks(banks, out, index, seen, st);
            banks.clear();
        }
    }

    OPL_INFO_F("sfxdump", "entries=%d sources=%d dumped=%d dup=%d no_end=%d "
               "failed=%d -> %s", st.entries, st.sources, st.dumped, st.dup,
               st.no_end, st.failed, out_dir);
    if (st.entries == 0) {
        /* Every source walked cleanly and none existed: the inputs opened
         * but carried no sample bank. Reporting success here hands a
         * caller an empty pack that looks like a finished census. */
        OPL_ERROR_F("sfxdump", "%s", "no entries walked");
        return 2;
    }
    return (st.no_end == 0 && st.failed == 0) ? 0 : 2;
}
