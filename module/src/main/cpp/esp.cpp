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
                        int32_t  cmp   = *(int32_t  *)((char *)ac + 0x20);
                        int32_t  cfg   = *(int32_t  *)((char *)ac + 0x1c);
                        if (atype != 0) continue;   // ActorTypeDef.HERO
                        void *inner = *(void **)((char *)ac + 0x50);
                        if (!is_plausible_ptr(inner)) continue;
                        void *al    = *(void **)((char *)inner + 0x08);
                        if (!is_plausible_ptr(al)) continue;
                        EspActor a{};
                        a.key  = key;
                        a.type = 0;
                        a.configId = cfg;
                        a.camp     = cmp;
                        a.objId    = *(uint32_t *)((char *)al + 0x4AC);
                        float *p = (float *)((char *)al + 0x4C4);
                        a.x = p[0]; a.y = p[1]; a.z = p[2];
                        int32_t *f = (int32_t *)((char *)al + 0x4B8);
                        a.fwd_x = f[0]; a.fwd_y = f[1]; a.fwd_z = f[2];
                        out.push_back(a);
                        hero_emitted++;
                    }
                    if (last_status != 7) {
                        LOGI("[esp v32] AM updatableActorList count=%d heroes_emitted=%d", count, hero_emitted);
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
