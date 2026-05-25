//
// ESP loop v23: hero-only scan via GamePlayerCenter.
// Streams binary EspActor[] packets to overlay via TCP 127.0.0.1:47291.
//
// Protocol (host-endian little, all sgame is aarch64):
//   header  : magic[4]="ESP1"  count(u32)
//   actors  : EspActor[count]  (48 bytes each, packed)
//
// EspActor (kept binary-compatible with overlay-app v13's ACTOR_BYTES=48):
//   key(u32)  type(i32)  configId(i32)  camp(i32)
//   battleOrder(i32) objId(u32) x(f32) y(f32) z(f32)
//   fwdX(i32) fwdY(i32) fwdZ(i32)
//
// type=0 (ActorTypeDef.HERO) is the only value we emit.  Overlay filters on
// (type==0 && camp==2) to draw enemy red dots.
//
// Data path (verified against dump.cs ground truth, Assets.Scripts.GameLogic):
//   GamePlayerCenter.s_instance
//     +0x18 m_playerLinkerList = DictionaryView<UInt32, Player>
//     +0x40 hostPlayerID       = u32 (my player id)
//   Player:
//     +0x008 playerCamp         (COM_PLAYERCAMP, i32) -- 1 or 2 in 5v5
//     +0x180 captainConfigID    (u32 hero id)
//     +0x198 Captain            (PoolObjHandle<ActorConfig>, T* at +0x8)
//   ActorConfig:
//     +0x050 inner              (ActorConfigInner*)
//   ActorConfigInner:
//     +0x008 actorLinker        (ActorLinker*)
//   ActorLinker:
//     +0x4AC ObjID              (u32)
//     +0x4B8 forward            (VInt3 = 3*i32, scaled *1000)
//     +0x4C4 position           (Vector3 = 3*f32, world units)
//
// Why not Player.captainLogicPos@0x408 directly? It's never written outside
// recovery snapshots -- not real-time. Live position lives on ActorLinker.
//

#include "esp.h"
#include "log.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"
#include <thread>
#include <atomic>
#include <vector>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define DO_API(r, n, p) extern r (*n) p
#include "il2cpp-api-functions.h"
#undef DO_API

// ===== layout offsets (dump.cs ground truth) =====
#define GPC_PLAYER_LIST     0x18
#define GPC_HOST_PLAYER_ID  0x40

#define PLAYER_CAMP         0x008
#define PLAYER_CFG_ID       0x180
#define PLAYER_CAPTAIN      0x198  // PoolObjHandle<ActorConfig>: 16 bytes
#define POOLHANDLE_T_PTR    0x008  // ActorConfig* at handle+8

#define AC_INNER            0x050
#define INNER_LINKER        0x008
#define AL_OBJID            0x4AC
#define AL_FORWARD          0x4B8
#define AL_POSITION         0x4C4

// IL2CPP Dictionary<TKey,TValue> on this sgame build (mscorlib, no +0x50 prefix):
//   +0x10  _entries (T[])    -- entries array, elements start at array+0x18
//   +0x18  _count            -- live entry count
// Entry layout (stride 24):  hashCode(i32) next(i32) key(u32) pad(i32) value(ptr)
// Free entries have hashCode<0 && next<0.
#define DICT_ENTRIES        0x10
#define DICT_COUNT          0x18
#define ENTRIES_BASE        0x18
#define ENTRY_STRIDE        24
#define ENTRY_KEY           8
#define ENTRY_VALUE         16

// DictionaryView<,>.Context: reflected at +0x8, runtime +0x18 after the IL2CPP
// managed-object header (0x10). We reflect at startup to absorb any future
// layout drift between IL2CPP runtime versions.

#define SOCK_PORT 47291

static inline bool is_plausible_ptr(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000 && (v & 7) == 0;
}

#pragma pack(push, 1)
struct EspActor {
    uint32_t key;
    int32_t  type;
    int32_t  configId;
    int32_t  camp;
    int32_t  battleOrder;
    uint32_t objId;
    float    x, y, z;
    int32_t  fwd_x, fwd_y, fwd_z;
};
struct EspHeader {
    char     magic[4];   // "ESP1"
    uint32_t count;
};
#pragma pack(pop)
static_assert(sizeof(EspActor) == 48, "EspActor must be 48 bytes for overlay v13");

static std::atomic<int> g_client_fd{-1};

static Il2CppClass *find_class_anywhere(const char *ns, const char *name) {
    Il2CppDomain *domain = il2cpp_domain_get();
    if (!domain) return nullptr;
    size_t n = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        const Il2CppImage *img = il2cpp_assembly_get_image(assemblies[i]);
        Il2CppClass *k = il2cpp_class_from_name(img, ns, name);
        if (k) return k;
    }
    return nullptr;
}

// v36: SGW.GetDisplayData() / GetDisplayData_Count() global cache.
// Refreshed once per scan tick; consumed by emit code inside the dict walk.
const void *g_disp_buf   = nullptr;
uint32_t    g_disp_count = 0;

static void refresh_display_data() {
    static Il2CppClass *cached_sgw = nullptr;
    static const MethodInfo *cached_m_data = nullptr;
    static const MethodInfo *cached_m_count = nullptr;
    static bool init_done = false;
    static bool init_logged_ok = false;

    if (!init_done) {
        // SGW lives in default namespace "" (dump.cs line 988228 "Namespace:")
        cached_sgw = find_class_anywhere("", "SGW");
        if (cached_sgw) {
            cached_m_data  = il2cpp_class_get_method_from_name(cached_sgw, "GetDisplayData", 0);
            cached_m_count = il2cpp_class_get_method_from_name(cached_sgw, "GetDisplayData_Count", 0);
            if (cached_m_data && cached_m_count) {
                LOGI("[esp v36] SGW init OK: klass=%p getData=%p getCount=%p",
                     cached_sgw, cached_m_data, cached_m_count);
            } else {
                LOGI("[esp v36] SGW partial init: klass=%p data_method=%p count_method=%p",
                     cached_sgw, cached_m_data, cached_m_count);
            }
        }
        init_done = true;
    }
    if (!cached_m_data || !cached_m_count) {
        g_disp_buf = nullptr;
        g_disp_count = 0;
        return;
    }

    // v38: ABSOLUTELY NO method invoke. v37 proved calling methodPointer
    // directly also triggers ACE -- it monitors the native function entry
    // (likely a caller-LR check inside the native fn that requires LR to
    // land inside libil2cpp's InvokerMethod trampoline range).
    //
    // Instead, only READ memory:
    //   1) lookup MethodInfo (safe -- metadata only, no code execution)
    //   2) read methodPointer (8-byte pointer load -- still no execution)
    //   3) read the bytes that methodPointer points to (.text read, no jump)
    //   4) ARM64 disasm the bytes locally to find the global-var address
    //      the native fn would have returned, and read THAT memory directly.
    //
    // No managed→native call ever happens, so there's no caller LR for ACE
    // to validate.  Reading .text is a normal page read on r-xp memory.
    void *raw_data_fn = *(void **)((char *)cached_m_data + 0x00);
    if (!raw_data_fn) { g_disp_buf = nullptr; g_disp_count = 0; return; }

    // v39: also dump the full fn bytecode and find ALL ADRP+LDR pairs.
    // v38's first-match returned insn[7]'s ADRP which led to a non-buffer ptr.
    // The real "return s_buffer" load is likely AFTER the prologue/setup
    // (maybe also after a tail call or before a RET).  We dump all pairs and
    // both pick the one whose deref looks most like a managed-heap pointer
    // (high byte 0xb4 indicates ART managed obj) -- and log all candidates
    // so we can manually choose later if the heuristic misses.
    static const void *cached_global_addr = nullptr;
    if (!cached_global_addr) {
        const uint32_t *insns = (const uint32_t *)raw_data_fn;
        // Dump 32 instructions (128 bytes) to logcat for offline RE.
        LOGI("[esp v39] fn @ %p first 128 bytes (32 ARM64 insns):", raw_data_fn);
        for (int row = 0; row < 8; ++row) {
            LOGI("[esp v39]   insn[%2d..%2d]: %08x %08x %08x %08x",
                 row * 4, row * 4 + 3,
                 insns[row * 4 + 0], insns[row * 4 + 1],
                 insns[row * 4 + 2], insns[row * 4 + 3]);
        }
        // Try every ADRP+LDR pair in the first 32 instructions; log each
        // candidate's target value and pick the one whose deref looks like a
        // managed-heap ptr (top byte 0xb4 on aarch64 ART).
        int chosen_idx = -1;
        uintptr_t chosen_global = 0;
        for (int i = 0; i < 30; ++i) {
            uint32_t a = insns[i];
            uint32_t b = insns[i + 1];
            bool is_adrp = (a & 0x9F000000) == 0x90000000;
            bool is_ldr  = (b & 0xFFC00000) == 0xF9400000;
            if (!is_adrp || !is_ldr) continue;
            int rd_adrp = a & 0x1F;
            int rn_ldr  = (b >> 5) & 0x1F;
            if (rd_adrp != rn_ldr) continue;
            int64_t immlo = (a >> 29) & 0x3;
            int64_t immhi = (a >> 5) & 0x7FFFF;
            int64_t imm = (immhi << 2) | immlo;
            if (imm & (1LL << 20)) imm |= ~((1LL << 21) - 1);
            imm <<= 12;
            uintptr_t pc = (uintptr_t)(insns + i);
            uintptr_t adrp_target = (pc & ~0xFFFLL) + imm;
            uint32_t imm12 = (b >> 10) & 0xFFF;
            uintptr_t global = adrp_target + ((uintptr_t)imm12 << 3);
            uint64_t deref = *(uint64_t *)global;
            int top_byte = (int)((deref >> 56) & 0xFF);
            LOGI("[esp v39]   cand insn[%d]: global=%p deref=%#018llx top=%02x %s",
                 i, (void *)global, (unsigned long long)deref, top_byte,
                 (top_byte == 0xb4) ? " <-- looks managed" : "");
            if (top_byte == 0xb4 && chosen_idx < 0) {
                chosen_idx = i; chosen_global = global;
            }
        }
        if (chosen_global) {
            cached_global_addr = (const void *)chosen_global;
            LOGI("[esp v39] picked global @ insn[%d] = %p", chosen_idx, (void *)chosen_global);
        } else {
            LOGI("[esp v39] no good ADRP+LDR candidate found, falling back to ActorLinker.position");
            return;
        }
    }

    // v41: chain through the deref ladder that GetDisplayData itself walks.
    // ARM64 disasm of fn body @insn[7-12] revealed:
    //   x19 = *(global)            <- managed state object  (cached above)
    //   x8  = *(x19 + 0xb8)         <- managed inner ptr
    //   x0  = *(x8  + 0x58)         <- final ptr, looks like List<DisplayInfoData>
    //   w8  = *(x0  + 0x18)         <- list._size (standard mscorlib List<>)
    //   items = *(x0 + 0x10)        <- list._items (T[] managed array)
    //   elements start at items + 0x18 (aarch64 sgame Il2CppArray layout)
    // We chase the same chain in pure memory reads.
    static int chain_log_count = 0;
    void *state = *(void **)cached_global_addr;
    if (!is_plausible_ptr(state)) {
        if (chain_log_count < 3) {
            LOGI("[esp v42] chain step 1 FAIL: state=%p (not plausible)", state);
            chain_log_count++;
        }
        g_disp_buf = nullptr; g_disp_count = 0; return;
    }
    void *inner = *(void **)((char *)state + 0xb8);
    if (!is_plausible_ptr(inner)) {
        if (chain_log_count < 3) {
            LOGI("[esp v42] chain step 2 FAIL: state=%p inner@+0xb8=%p", state, inner);
            chain_log_count++;
        }
        g_disp_buf = nullptr; g_disp_count = 0; return;
    }
    void *list = *(void **)((char *)inner + 0x58);
    if (!is_plausible_ptr(list)) {
        if (chain_log_count < 3) {
            LOGI("[esp v42] chain step 3 FAIL: state=%p inner=%p list@+0x58=%p", state, inner, list);
            chain_log_count++;
        }
        g_disp_buf = nullptr; g_disp_count = 0; return;
    }
    void *items = *(void **)((char *)list + 0x10);
    int   size  = *(int   *)((char *)list + 0x18);
    if (!is_plausible_ptr(items) || size <= 0 || size > 256) {
        if (chain_log_count < 3) {
            LOGI("[esp v42] chain step 4 FAIL: state=%p inner=%p list=%p items=%p size=%d",
                 state, inner, list, items, size);
            // hex dump list head 64 bytes
            const uint64_t *q = (const uint64_t *)list;
            LOGI("[esp v42] list hex: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx",
                 (unsigned long long)q[0], (unsigned long long)q[1],
                 (unsigned long long)q[2], (unsigned long long)q[3],
                 (unsigned long long)q[4], (unsigned long long)q[5],
                 (unsigned long long)q[6], (unsigned long long)q[7]);
            chain_log_count++;
        }
        g_disp_buf = nullptr; g_disp_count = 0; return;
    }
    // Skip Il2CppArray header to land on element[0].
    g_disp_buf   = (char *)items + 0x18;
    g_disp_count = (uint32_t)size;
    if (chain_log_count < 3) {
        LOGI("[esp v42] chain OK: state=%p inner=%p list=%p size=%d items=%p entries=%p",
             state, inner, list, size, items, g_disp_buf);
        chain_log_count++;
    }

    // Apply same trick to GetDisplayData_Count -- but optimistically:
    // its native fn likely loads count from a global at the same/neighboring
    // page. Skip for now; emit code can decide count by walking until actorID==0.
    g_disp_count = 256;  // upper-bound walk; emit will stop at actorID==0

    if (!init_logged_ok && g_disp_buf) {
        LOGI("[esp v38] SGW global cache: addr=%p buf=%p", cached_global_addr, g_disp_buf);
        const uint64_t *q = (const uint64_t *)g_disp_buf;
        LOGI("[esp v38] disp[0] hex: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx",
             (unsigned long long)q[0], (unsigned long long)q[1],
             (unsigned long long)q[2], (unsigned long long)q[3],
             (unsigned long long)q[4], (unsigned long long)q[5],
             (unsigned long long)q[6], (unsigned long long)q[7]);
        init_logged_ok = true;
    }
}

// Walk one Player into ActorLinker, emit one EspActor if everything resolves.
static bool emit_player(void *player, uint32_t key, std::vector<EspActor> &out) {
    if (!is_plausible_ptr(player)) return false;

    int32_t camp     = *(int32_t  *)((char *)player + PLAYER_CAMP);
    uint32_t cfgId   = *(uint32_t *)((char *)player + PLAYER_CFG_ID);

    // Captain is a 16-byte PoolObjHandle struct embedded in Player; T* is at +8.
    void *ac = *(void **)((char *)player + PLAYER_CAPTAIN + POOLHANDLE_T_PTR);
    if (!is_plausible_ptr(ac)) return false;

    void *inner = *(void **)((char *)ac + AC_INNER);
    if (!is_plausible_ptr(inner)) return false;

    void *al = *(void **)((char *)inner + INNER_LINKER);
    if (!is_plausible_ptr(al)) return false;

    EspActor a{};
    a.key         = key;
    a.type        = 0;             // ActorTypeDef.HERO
    a.configId    = (int32_t)cfgId;
    a.camp        = camp;
    a.battleOrder = 0;
    a.objId       = *(uint32_t *)((char *)al + AL_OBJID);
    float *p      = (float *)((char *)al + AL_POSITION);
    a.x = p[0]; a.y = p[1]; a.z = p[2];
    int32_t *f    = (int32_t *)((char *)al + AL_FORWARD);
    a.fwd_x = f[0]; a.fwd_y = f[1]; a.fwd_z = f[2];
    out.push_back(a);
    return true;
}

// v28: switched from m_playerLinkerList (DictionaryView, always empty in this
// sgame build) to playersCache (+0x50, ListView<Player>). v27 hexdump proved
// hostPlayer @ gpc+0x48 has correct camp/cfg/captain — so Player layout is fine.
// We just needed a populated container.
//
// Chain:
//   gpc + 0x50      → ListView<Player>
//     +0x18 (Context, ListViewBase) → List<Player>
//       +0x10 _items (Player[])
//       +0x18 _size  (int)
//     T[]  +0x18 elements
//
// ListView fallback order: playersCache (+0x50) -> _playersTempList (+0x30).
// We also always emit hostPlayer (+0x48) as a guaranteed lower-bound so the
// overlay shows something even before BuildPlayers populates the cache.
static int scan_heroes(std::vector<EspActor> &out) {
    out.clear();

    static Il2CppClass *cached_klass = nullptr;
    static FieldInfo *cached_s_inst = nullptr;
    static int last_status = -1;
    auto status_log = [](int s, const char *msg, int extra = 0) {
        if (s == last_status) return;
        last_status = s;
        if (extra) LOGI("[esp v28] %s extra=%d", msg, extra);
        else       LOGI("[esp v28] %s", msg);
    };

    if (!cached_klass) {
        cached_klass = find_class_anywhere("Assets.Scripts.GameLogic", "GamePlayerCenter");
        if (!cached_klass) { status_log(0, "klass NOT_FOUND"); return 0; }
        Il2CppClass *parent = il2cpp_class_get_parent(cached_klass);
        cached_s_inst = il2cpp_class_get_field_from_name(parent, "s_instance");
        if (!cached_s_inst)
            cached_s_inst = il2cpp_class_get_field_from_name(cached_klass, "s_instance");
        if (!cached_s_inst) { status_log(1, "s_instance NOT_FOUND"); return 0; }
        LOGI("[esp v28] init OK klass=%p s_inst=%p", cached_klass, cached_s_inst);
    }

    void *gpc = nullptr;
    il2cpp_field_static_get_value(cached_s_inst, &gpc);
    if (!is_plausible_ptr(gpc)) { status_log(2, "gpc NULL"); return 0; }

    // === v43 path C: CullingGroupMgr Unity Transform probe ===
    // Brainstorm idea: Unity engine needs each GameObject's REAL world
    // position to compute view-frustum culling, so Transform.position must
    // be up-to-date for every actor regardless of game-side FOW filter.
    // FOW only toggles MeshRenderer.enabled, leaving Transform untouched.
    //
    // Walk CullingGroupMgr.indexMap (Dictionary<int, CullingNode>) once,
    // dump first 2 actors' CullingNode + Transform memory so we can RE the
    // managed→native Transform layout in this sgame Unity build.
    // v44: only mark probed *after* inst was non-NULL (CullingGroupMgr is
    // instantiated lazily once a match starts, not in the lobby).
    static bool culling_probed = false;
    if (!culling_probed) {
        Il2CppClass *cg_klass = find_class_anywhere("Assets.Scripts.GameLogic", "CullingGroupMgr");
        if (cg_klass) {
            Il2CppClass *parent_c = il2cpp_class_get_parent(cg_klass);
            FieldInfo *cg_sinst = il2cpp_class_get_field_from_name(parent_c, "s_instance");
            if (!cg_sinst) cg_sinst = il2cpp_class_get_field_from_name(cg_klass, "s_instance");
            void *cg = nullptr;
            if (cg_sinst) il2cpp_field_static_get_value(cg_sinst, &cg);
            if (is_plausible_ptr(cg)) {
                culling_probed = true;  // only set once we got real data
                LOGI("[esp v43] CullingGroupMgr inst=%p", cg);
                void *idx_dict = *(void **)((char *)cg + 0x10);
                LOGI("[esp v43] indexMap=%p isActive=%d", idx_dict, *(int *)((char *)cg + 0x08));
                if (is_plausible_ptr(idx_dict)) {
                    // Dump Dict head + first 4 entries assuming standard mscorlib
                    // layout (count@+0x18, entries@+0x10, stride 24).
                    int dcount = *(int *)((char *)idx_dict + 0x18);
                    void *dents = *(void **)((char *)idx_dict + 0x10);
                    LOGI("[esp v43] indexMap count=%d entries=%p", dcount, dents);
                    if (is_plausible_ptr(dents) && dcount > 0 && dcount < 1024) {
                        for (int i = 0; i < 4 && i < dcount; ++i) {
                            char *e = (char *)dents + 0x18 + i * 24;
                            int hashCode = *(int *)(e + 0);
                            if (hashCode < 0) continue;
                            int key = *(int *)(e + 8);
                            void *node = *(void **)(e + 16);
                            if (!is_plausible_ptr(node)) continue;
                            // CullingNode.transform @ +0x28 (managed Unity Transform)
                            void *xform_managed = *(void **)((char *)node + 0x28);
                            // UnityEngine.Object.m_CachedPtr @ +0x10 in IL2CPP managed
                            void *xform_native = is_plausible_ptr(xform_managed)
                                ? *(void **)((char *)xform_managed + 0x10) : nullptr;
                            const uint64_t *nq = (const uint64_t *)node;
                            LOGI("[esp v43] node[%d] key=%d ptr=%p hex: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx",
                                 i, key, node,
                                 (unsigned long long)nq[0], (unsigned long long)nq[1],
                                 (unsigned long long)nq[2], (unsigned long long)nq[3],
                                 (unsigned long long)nq[4], (unsigned long long)nq[5],
                                 (unsigned long long)nq[6], (unsigned long long)nq[7]);
                            LOGI("[esp v43] node[%d] xform_managed=%p xform_native=%p",
                                 i, xform_managed, xform_native);
                            if (is_plausible_ptr(xform_native)) {
                                const uint64_t *xq = (const uint64_t *)xform_native;
                                LOGI("[esp v43] xform_native[%d] hex: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx",
                                     i,
                                     (unsigned long long)xq[0], (unsigned long long)xq[1],
                                     (unsigned long long)xq[2], (unsigned long long)xq[3],
                                     (unsigned long long)xq[4], (unsigned long long)xq[5],
                                     (unsigned long long)xq[6], (unsigned long long)xq[7],
                                     (unsigned long long)xq[8], (unsigned long long)xq[9],
                                     (unsigned long long)xq[10], (unsigned long long)xq[11]);
                            }
                        }
                    }
                }
            } else {
                LOGI("[esp v43] CullingGroupMgr inst NULL");
            }
        } else {
            LOGI("[esp v43] CullingGroupMgr klass NOT_FOUND");
        }
    }

    // v29: paranoid hostPlayer-only path.  v28 crashed sgame in the lobby --
    // probably emit_player chasing Captain->Inner->ActorLinker on an item
    // that wasn't actually a Player (ListView contents shape unknown), or
    // the listview itself wasn't a managed object.  Skip listviews entirely
    // and only emit the single host player.  Once we prove this stays stable
    // and renders one dot, we'll add a class-validated listview path.
    //
    // Each chain step now logs once on first success so we can verify the
    // Captain handle / inner / linker / position chain works end-to-end
    // before going wider.
    // === v31 listview hex probe ===
    // Dump first 0x40 bytes of cacheList @ gpc+0x50 once, to determine if it's
    // a real IL2CPP object (klass ptr in b400... range + valid List in Context).
    static bool lv_dumped = false;
    if (!lv_dumped) {
        void *lv50 = *(void **)((char *)gpc + 0x50);
        void *lv30 = *(void **)((char *)gpc + 0x30);
        if (is_plausible_ptr(lv50)) {
            const uint64_t *q = (const uint64_t *)lv50;
            LOGI("[esp v31] cacheList@%p hex: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx",
                 lv50, (unsigned long long)q[0], (unsigned long long)q[1],
                 (unsigned long long)q[2], (unsigned long long)q[3],
                 (unsigned long long)q[4], (unsigned long long)q[5],
                 (unsigned long long)q[6], (unsigned long long)q[7]);
            // If lv50.Context (+0x18) is a managed List, dump its head too.
            void *list = *(void **)((char *)lv50 + 0x18);
            if (is_plausible_ptr(list)) {
                const uint64_t *l = (const uint64_t *)list;
                LOGI("[esp v31] cacheList.Context@%p hex: %016llx %016llx %016llx %016llx %016llx %016llx",
                     list, (unsigned long long)l[0], (unsigned long long)l[1],
                     (unsigned long long)l[2], (unsigned long long)l[3],
                     (unsigned long long)l[4], (unsigned long long)l[5]);
            }
        }
        if (is_plausible_ptr(lv30)) {
            const uint64_t *q = (const uint64_t *)lv30;
            LOGI("[esp v31] tempList@%p hex: %016llx %016llx %016llx %016llx",
                 lv30, (unsigned long long)q[0], (unsigned long long)q[1],
                 (unsigned long long)q[2], (unsigned long long)q[3]);
        }
        lv_dumped = true;
    }

    // Helper: emit one Player as an EspActor by reading its captainLogicPos.
    // Returns true on success. Always checks the Player ptr looks valid first.
    auto emit_player_logicpos = [&](void *player, uint32_t key) -> bool {
        if (!is_plausible_ptr(player)) return false;
        int32_t  camp = *(int32_t  *)((char *)player + 0x008);
        uint32_t cfg  = *(uint32_t *)((char *)player + 0x180);
        // Reject obvious garbage: camp must be 0..15, cfg must be 0..0x100000.
        if (camp < 0 || camp > 15) return false;
        if (cfg > 0x100000) return false;
        int32_t *vp = (int32_t *)((char *)player + 0x408);
        int32_t *vf = (int32_t *)((char *)player + 0x414);
        EspActor a{};
        a.key   = key;
        a.type  = 0;
        a.configId = (int32_t)cfg;
        a.camp     = camp;
        a.objId    = 0;
        a.x = (float)vp[0] / 1000.0f;
        a.y = (float)vp[1] / 1000.0f;
        a.z = (float)vp[2] / 1000.0f;
        a.fwd_x = vf[0]; a.fwd_y = vf[1]; a.fwd_z = vf[2];
        out.push_back(a);
        return true;
    };

    // v31: walk playersCache ListView<Player>.  ListViewBase.Context offset is
    // reflected, not hardcoded -- v28 hardcoded +0x18 and crashed sgame because
    // ListViewBase isn't a generic, so IL2CPP reports the raw offset without
    // the +0x10 header bump.  Each element is class-checked: if its klass ptr
    // doesn't match Player's klass we skip rather than treat random heap as Player.
    static Il2CppClass *cached_player_klass = nullptr;
    static int          cached_lv_ctx_off   = -1;

    // Always emit hostPlayer as a baseline (proven safe in v30).
    void *hostPlayer = *(void **)((char *)gpc + 0x48);
    if (!is_plausible_ptr(hostPlayer)) { status_log(3, "hostPlayer NULL"); return 0; }
    emit_player_logicpos(hostPlayer, 0);
    if (!cached_player_klass) cached_player_klass = il2cpp_object_get_class((Il2CppObject *)hostPlayer);

    // === v32 ActorManager.updatableActorList scan ===
    // playersCache confirmed empty in this sgame build.
    // ActorManager.updatableActorList @ +0x10 is a DictionaryView<UInt32, ActorConfig>
    // (Assets.Scripts.GameLogic) -- memory v6 tested its mscorlib Dictionary layout
    // as count@+0x18, _entries@+0x10, entries[]: hash(0)+next(4)+key(8)+pad+value(16) stride 24.
    // For each ActorConfig: ActorType@+0x18 (0=HERO), inner@+0x50 -> ActorConfigInner.actorLinker@+0x8 -> ActorLinker.
    // Read ActorLinker.position@+0x4C4 Vector3 directly (proven realtime in earlier work).
    static Il2CppClass *cached_am_klass = nullptr;
    static FieldInfo   *cached_am_sinst = nullptr;
    static int          cached_am_dv_ctx = -1;
    if (!cached_am_klass) {
        cached_am_klass = find_class_anywhere("Assets.Scripts.GameLogic", "ActorManager");
        if (cached_am_klass) {
            Il2CppClass *parent = il2cpp_class_get_parent(cached_am_klass);
            cached_am_sinst = il2cpp_class_get_field_from_name(parent, "s_instance");
            if (!cached_am_sinst) cached_am_sinst = il2cpp_class_get_field_from_name(cached_am_klass, "s_instance");
        }
    }
    void *am = nullptr;
    if (cached_am_sinst) il2cpp_field_static_get_value(cached_am_sinst, &am);
    if (is_plausible_ptr(am)) {
        void *dv = *(void **)((char *)am + 0x10);  // updatableActorList
        if (is_plausible_ptr(dv)) {
            if (cached_am_dv_ctx < 0) {
                Il2CppClass *dv_klass = il2cpp_object_get_class((Il2CppObject *)dv);
                FieldInfo *fctx = dv_klass ? il2cpp_class_get_field_from_name(dv_klass, "Context") : nullptr;
                int raw = fctx ? il2cpp_field_get_offset(fctx) : 0x8;
                cached_am_dv_ctx = raw >= 0x10 ? raw : (raw + 0x10);
                LOGI("[esp v32] AM dictview.Context @ +0x%x", cached_am_dv_ctx);
            }
            void *dict = *(void **)((char *)dv + cached_am_dv_ctx);
            if (is_plausible_ptr(dict)) {
                int   count = *(int *)((char *)dict + 0x18);
                void *ents  = *(void **)((char *)dict + 0x10);
                if (is_plausible_ptr(ents) && count > 0 && count < 256) {
                    int hero_emitted = 0;
                    for (int i = 0; i < count + 8 && i < 256; ++i) {
                        char *e = (char *)ents + 0x18 + i * 24;
                        int hash = *(int *)(e + 0);
                        int next = *(int *)(e + 4);
                        if (hash < 0 && next < 0) continue;
                        uint32_t key = *(uint32_t *)(e + 8);
                        void *ac     = *(void **)(e + 16);
                        if (!is_plausible_ptr(ac)) continue;
                        int32_t  atype = *(int32_t  *)((char *)ac + 0x18);
                        if (atype != 0) continue;   // ActorTypeDef.HERO
                        void *inner = *(void **)((char *)ac + 0x50);
                        if (!is_plausible_ptr(inner)) continue;
                        void *al    = *(void **)((char *)inner + 0x08);
                        if (!is_plausible_ptr(al)) continue;
                        // v34: ActorConfig.CmpType@+0x20 and .ConfigID@+0x1c are
                        // ALWAYS 0 in this sgame build (placeholder template ACs).
                        // PC-side hex scan in v33 found the live values live on
                        // ActorLinker instead:
                        //   AL +0x56c = COM_PLAYERCAMP (1=blue, 2=red) verified
                        //   AL +0x38  = hero config ID (114/128/152/... matches sgame hero IDs)
                        int32_t cmp = *(int32_t *)((char *)al + 0x56c);
                        int32_t cfg = *(int32_t *)((char *)al + 0x038);
                        EspActor a{};
                        a.key  = key;
                        a.type = 0;
                        a.configId = cfg;
                        a.camp     = cmp;
                        a.objId    = *(uint32_t *)((char *)al + 0x4AC);

                        // v35 (deprecated): MoveComponent.remotePosition also
                        // turned out to be FOW-frozen.  v36 routes around the
                        // FOW filter entirely by querying SGW.GetDisplayData(),
                        // a static native bridge method that returns the raw
                        // server-sent actor position cache (used by sgame's own
                        // replay system, so guaranteed to include all actors
                        // regardless of visibility).
                        //
                        // The lookup happens once per frame in the scanner-
                        // thread loop below, then we resolve each actor's
                        // entry by actorID == ActorLinker.ObjID@+0x4AC.
                        float *p = (float *)((char *)al + 0x4C4);  // fallback

                        // v45 (Wwise probe): brainstorm angle — Wwise spatial
                        // audio engine must know every emitter's true 3D
                        // position even when actor is FOW'd (otherwise full-map
                        // ultimate sound effects wouldn't pan correctly).
                        // Chain: ActorLinker+0x450 → ActorSoundComponent
                        //        + 0x40 → AkGameObj
                        //        + 0x24 → Vector3 m_position
                        // Just READ — no method call, no .text touch, ACE-safe.
                        // If FOW-filtered actor's AkGameObj.m_position is still
                        // realtime → BREAKTHROUGH.
                        void *sc = *(void **)((char *)al + 0x450);
                        if (is_plausible_ptr(sc)) {
                            void *ako = *(void **)((char *)sc + 0x40);
                            if (is_plausible_ptr(ako)) {
                                uint32_t akid = *(uint32_t *)((char *)ako + 0x50);
                                // v46: call CSharp_GetPosition(akID) — native C export,
                                // not IL2CPP managed. dlsym → direct call.
                                // Wwise is third-party middleware; ACE shouldn't monitor.
                                // Signature (SWIG): void(*)(uint32, AkSoundPosition*)
                                //   AkSoundPosition: 3*Vector3 = position(12) + orientFront(12) + orientTop(12) = 36B
                                static void *(*ak_get_pos)(uint32_t, void *) = nullptr;
                                static bool dlsym_tried = false;
                                if (!dlsym_tried) {
                                    dlsym_tried = true;
                                    void *h = dlopen("libAkSoundEngine.so", RTLD_NOW | RTLD_NOLOAD);
                                    if (h) {
                                        ak_get_pos = (void *(*)(uint32_t, void *))dlsym(h, "CSharp_GetPosition");
                                        LOGI("[esp v46] dlsym CSharp_GetPosition handle=%p fn=%p", h, ak_get_pos);
                                    } else {
                                        LOGI("[esp v46] dlopen libAkSoundEngine.so NOLOAD failed");
                                    }
                                }
                                if (ak_get_pos) {
                                    float ak_pos[9] = {0};
                                    ak_get_pos(akid, ak_pos);
                                    static int wlog_count = 0;
                                    if (wlog_count < 30) {
                                        LOGI("[esp v46] actor key=%u AL=(%.1f,%.1f,%.1f) Wwise=(%.1f,%.1f,%.1f) akID=%u",
                                             key, p[0], p[1], p[2], ak_pos[0], ak_pos[1], ak_pos[2], akid);
                                        wlog_count++;
                                    }
                                }
                            }
                        }
                        if (g_disp_buf && g_disp_count) {
                            uint32_t want = a.objId;
                            // v39: SGW.GetDisplayData() returns a managed wrapper.
                            // Try multiple (base_off, stride) hypotheses for where
                            // the actual DisplayInfoData entries live within buf.
                            //   base=0x10: short managed header (klass+monitor)
                            //   base=0x18: array with inline length at +0x10
                            //   base=0x20: full managed array (klass+monitor+bounds+length)
                            //   base=0x28: alternate sgame wrapper
                            // strides cover DisplayInfoData declared field range (0x38..0x48)
                            const size_t BASES[]   = { 0x10, 0x18, 0x20, 0x28, 0x30 };
                            const size_t STRIDES[] = { 0x30, 0x38, 0x40, 0x48, 0x50 };
                            bool found = false;
                            for (size_t bi = 0; bi < sizeof(BASES)/sizeof(BASES[0]) && !found; ++bi) {
                                const char *base = (const char *)g_disp_buf + BASES[bi];
                                for (size_t si = 0; si < sizeof(STRIDES)/sizeof(STRIDES[0]) && !found; ++si) {
                                    size_t stride = STRIDES[si];
                                    for (uint32_t i = 0; i < 64; ++i) {
                                        const char *e = base + i * stride;
                                        uint32_t aid = *(const uint32_t *)e;
                                        if (aid != want) continue;
                                        // v40: scan multiple position offsets inside the entry
                                        // until we find 3 floats in plausible map range.
                                        static bool logged_hex = false;
                                        if (!logged_hex) {
                                            const uint64_t *q = (const uint64_t *)e;
                                            LOGI("[esp v40] entry @ base=+0x%zx stride=0x%zx i=%u actorID=%u hex: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx",
                                                 BASES[bi], stride, i, want,
                                                 (unsigned long long)q[0], (unsigned long long)q[1],
                                                 (unsigned long long)q[2], (unsigned long long)q[3],
                                                 (unsigned long long)q[4], (unsigned long long)q[5],
                                                 (unsigned long long)q[6], (unsigned long long)q[7]);
                                            logged_hex = true;
                                        }
                                        // Try position offset 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c
                                        for (size_t po = 0x04; po <= 0x1c; po += 0x04) {
                                            float *cand = (float *)(e + po);
                                            float x = cand[0], y = cand[1], z = cand[2];
                                            // valid: not NaN, x and z in (-200,+200), y small
                                            if (x == x && z == z && y == y &&  // NaN check
                                                x > -200.0f && x < 200.0f &&
                                                z > -200.0f && z < 200.0f &&
                                                y > -50.0f && y < 50.0f) {
                                                p = cand; found = true;
                                                static bool logged_pos = false;
                                                if (!logged_pos) {
                                                    LOGI("[esp v40] PICK pos_off=+0x%zx actorID=%u pos=(%.1f,%.1f,%.1f)",
                                                         po, want, x, y, z);
                                                    logged_pos = true;
                                                }
                                                break;
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        a.x = p[0]; a.y = p[1]; a.z = p[2];
                        // v33 RE-mode: overlay doesn't draw forward, so reuse fwd_x/y/z to
                        // carry the ActorConfig + ActorLinker pointers out to PC-side
                        // python so it can process_vm_readv-hexdump them live without
                        // another build cycle.
                        //   fwd_x = ActorConfig low32
                        //   fwd_y = ActorConfig high32
                        //   fwd_z = ActorLinker low32  (high32 implied = ActorConfig high32, same heap region)
                        uintptr_t ac_u = (uintptr_t)ac;
                        uintptr_t al_u = (uintptr_t)al;
                        a.fwd_x = (int32_t)(ac_u & 0xFFFFFFFFu);
                        a.fwd_y = (int32_t)((ac_u >> 32) & 0xFFFFFFFFu);
                        a.fwd_z = (int32_t)(al_u & 0xFFFFFFFFu);
                        out.push_back(a);
                        if (hero_emitted < 6) {
                            LOGI("[esp v33] hero[%d] key=%u cfg=%d camp=%d pos=(%.1f,%.1f,%.1f) obj=%u",
                                 hero_emitted, key, cfg, cmp, a.x, a.y, a.z, a.objId);
                        }
                        hero_emitted++;
                    }
                    if (last_status != 7) {
                        LOGI("[esp v33] AM updatableActorList count=%d heroes_emitted=%d", count, hero_emitted);
                        last_status = 7;
                    }
                } else {
                    if (last_status != 8) {
                        LOGI("[esp v32] AM dict count=%d ents=%p", count, ents);
                        last_status = 8;
                    }
                }
            }
        }
    }

    return (int)out.size();
}

static bool send_snapshot(int fd, const std::vector<EspActor> &actors) {
    EspHeader hdr;
    memcpy(hdr.magic, "ESP1", 4);
    hdr.count = (uint32_t)actors.size();

    struct iovec iov[2];
    iov[0].iov_base = &hdr;
    iov[0].iov_len  = sizeof(hdr);
    iov[1].iov_base = (void *)actors.data();
    iov[1].iov_len  = actors.size() * sizeof(EspActor);

    struct msghdr msg = {};
    msg.msg_iov    = iov;
    msg.msg_iovlen = (actors.empty()) ? 1 : 2;

    ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
    return n > 0;
}

static void server_thread() {
    LOGI("[esp v24] server thread, tid=%d", gettid());
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { LOGE("[esp v24] socket() errno=%d", errno); return; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SOCK_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("[esp v24] bind 127.0.0.1:%d errno=%d", SOCK_PORT, errno);
        close(srv);
        return;
    }
    if (listen(srv, 4) < 0) {
        LOGE("[esp v24] listen errno=%d", errno);
        close(srv);
        return;
    }
    LOGI("[esp v24] listening on 127.0.0.1:%d", SOCK_PORT);

    while (true) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) {
            if (errno == EINTR) continue;
            LOGE("[esp v24] accept errno=%d", errno);
            sleep(1);
            continue;
        }
        LOGI("[esp v24] client connected fd=%d", cli);
        int old = g_client_fd.exchange(cli);
        if (old >= 0) close(old);
    }
}

static void scan_thread() {
    LOGI("[esp v24] scan thread, tid=%d", gettid());
    sleep(20);  // shorter warmup; loop probes GPC and logs stage on each transition
    LOGI("[esp v24] scan loop starting");

    int empty_streak = 0;
    while (true) {
        usleep(150 * 1000);  // ~6.7 Hz

        int fd = g_client_fd.load();
        if (fd < 0) continue;

        refresh_display_data();  // v36: pull native cache before per-actor walk
        std::vector<EspActor> actors;
        int n = scan_heroes(actors);

        // Diagnostic: log heartbeat every ~3s while client is up.
        if (++empty_streak >= 20) {
            empty_streak = 0;
            LOGI("[esp v24] tick n=%d", n);
        }

        if (!send_snapshot(fd, actors)) {
            LOGI("[esp v24] client disconnected, closing fd=%d", fd);
            close(fd);
            g_client_fd.store(-1);
        }
    }
}

void esp_start(const char *game_data_dir) {
    LOGI("[esp v24] esp_start");
    std::thread srv(server_thread);
    srv.detach();
    std::thread scn(scan_thread);
    scn.detach();
}
