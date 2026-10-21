/* texture-thread-probe - api->texture_register from a mod-started
 * thread (see mod.toml).
 *
 * Model layout, from the game's own headers: a Model has m_NumAnimations
 * at +0 and m_Animations[] at +0x38; an AnimationHeader has its HP
 * faceblock at +0x14 and LP faceblock at +0x1C. A faceblock is a u32
 * byte count, then records whose stride the renderer's walk derives from
 * w0: a fresh record (bit 31) is 0x0C bytes, 0x18 textured (bit 1), 0x14
 * for a sprite (bit 2); a non-fresh record is 0x08, 0x14 textured. A
 * textured record ends with three words: u0|v0<<8|clut<<16,
 * u1|v1<<8|tpage<<16, u2|v2<<8|u3<<16|v3<<24. tpage mode bits
 * (attr >> 7 & 3) = 3 select a mod texture whose id is the CLUT
 * halfword. */
#include <openpete_mod_api.h>
#include "openpete_sdk_symbols.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define PROBE_ID     0x7E01u
#define WOODEN_CHEST 194u
#define TEX_DIM      256

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static atomic_int s_reg_rc = -2;      /* -3 off, -2 not yet, 0 registered, -1 failed */
static int s_reg_logged;
static uint32_t s_last_model;

/** @brief Thread body: register a magenta image as PROBE_ID. @param arg Unused. */
static void register_main(void* arg) {
    unsigned char* px = (unsigned char*)malloc(TEX_DIM * TEX_DIM * 4);
    int rc = -1;
    (void)arg;
    if (px) {
        for (int i = 0; i < TEX_DIM * TEX_DIM; i++) {
            px[4 * i + 0] = 255; px[4 * i + 1] = 0;
            px[4 * i + 2] = 255; px[4 * i + 3] = 255;
        }
        rc = g_api->texture_register(g_self, (uint16_t)PROBE_ID, px,
                                     TEX_DIM, TEX_DIM);
        free(px);       /* the engine copied the pixels */
    }
    atomic_store(&s_reg_rc, rc);
}

#ifdef _WIN32
/** @brief Win32 thread trampoline into register_main. @param a Unused. @return 0. */
static DWORD WINAPI thread_entry(LPVOID a) { register_main(a); return 0; }
#else
/** @brief pthread trampoline into register_main. @param a Unused. @return NULL. */
static void* thread_entry(void* a) { register_main(a); return NULL; }
#endif

/** @brief Start the registration thread detached. @return 0, or -1. */
static int start_thread(void) {
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, thread_entry, NULL, 0, NULL);
    if (!h) return -1;
    CloseHandle(h);
    return 0;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, thread_entry, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
#endif
}

/** @brief Retarget every textured face of one faceblock to PROBE_ID.
 * @param fb Faceblock vaddr. @return Faces retargeted. */
static unsigned retarget(uint32_t fb) {
    unsigned n = 0;
    if (fb < 0x80000000u) fb |= 0x80000000u;
    uint32_t bc = op_read_u32(g_api, fb);
    uint32_t fp = fb + 4u, end = fb + 4u + bc;
    while (fp + 8u <= end) {
        uint32_t w0 = op_read_u32(g_api, fp);
        int fresh = (int32_t)w0 < 0, tex = (w0 & 2u) != 0;
        int sprite = fresh && (w0 & 4u);
        uint32_t adv = sprite ? 0x14u : fresh ? (tex ? 0x18u : 0x0Cu)
                                              : (tex ? 0x14u : 0x08u);
        if (!sprite && !(w0 & 8u) && tex) {
            uint32_t td0 = op_read_u32(g_api, fp + adv - 0xCu);
            uint32_t td1 = op_read_u32(g_api, fp + adv - 0x8u);
            if ((td0 >> 16) != PROBE_ID) {
                op_write_u32(g_api, fp + adv - 0xCu, (td0 & 0xFFFFu) | PROBE_ID << 16);
                op_write_u32(g_api, fp + adv - 0x8u, td1 | (0x180u << 16));
                n++;
            }
        }
        fp += adv;
    }
    return n;
}

/** @brief Post-hook: retarget the wooden chest's faces once per installed model. @param cpu Guest CPU. */
static void on_patch_models(CPUState* cpu) {
    g_api->base(cpu);
    uint32_t m = op_read_u32(g_api, OP_GADDR_g_Models + WOODEN_CHEST * 4u);
    if (!m || m == s_last_model) return;
    s_last_model = m;
    m |= 0x80000000u;
    unsigned faces = 0, anims = op_read_u32(g_api, m);
    for (unsigned a = 0; a < anims && a < 16; a++) {
        uint32_t ah = op_read_u32(g_api, m + 0x38u + 4u * a) | 0x80000000u;
        uint32_t hp = op_read_u32(g_api, ah + 0x14u), lp = op_read_u32(g_api, ah + 0x1Cu);
        if (hp) faces += retarget(hp);
        if (lp && lp != hp) faces += retarget(lp);
    }
    int rc = atomic_load(&s_reg_rc);
    g_api->log(g_self, OP_MOD_LOG_INFO, "texture-thread-probe: %u chest faces "
               "select %#06x (registration %s)", faces, PROBE_ID,
               rc == 0 ? "done" : rc == -2 ? "pending" : rc == -3 ? "off" : "failed");
    if (rc == 0 && !s_reg_logged) {
        s_reg_logged = 1;
        g_api->log(g_self, OP_MOD_LOG_INFO, "texture-thread-probe: registered "
                   "from the mod's thread");
    }
}

/** @brief Start the registration thread (unless `register = false`) and hook the model-install pass.
 * @param api Engine api. @param self This mod.
 * @return 0, or 1 when the thread or the hook cannot start. */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (!api->config_bool(self, "register", 1))
        atomic_store(&s_reg_rc, -3);
    else if (start_thread() != 0) {
        api->log(self, OP_MOD_LOG_ERROR, "texture-thread-probe: thread start failed");
        return 1;
    }
    if (api->override_addr(self, OP_FNADDR_PatchMobyModelPointers, on_patch_models)) {
        api->log(self, OP_MOD_LOG_ERROR, "texture-thread-probe: hook failed");
        return 1;
    }
    return 0;
}
