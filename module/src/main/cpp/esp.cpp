//
// ESP loop v11: periodic actor scan + abstract Unix socket server
// Streams binary EspActor[] packets to any client connected to @sgame_esp
//
// Protocol (host-endian little, all sgame is aarch64):
//   Each packet:
//     magic[4]    "ESP1"
//     count       uint32  (number of EspActor entries)
//     actors[]    EspActor (52 bytes each)
//
// Period: 500 ms (low enough to be smooth, high enough to avoid timing detect)
//

#include "esp.h"
#include "log.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#define DO_API(r, n, p) extern r (*n) p
#include "il2cpp-api-functions.h"
#undef DO_API

// ===== layout offsets (verified by v10) =====
// AM+0x8 = actorList (Dict, all actors including HERO);
// AM+0x10 = updatableActorList (Dict, only updatable ORGAN/MONSTER/SOLDIER - no HERO)
#define AM_UPDATABLE_LIST   0x8
#define AC_ACTORTYPE        0x18
#define AC_CONFIGID         0x1c
#define AC_CMPTYPE          0x20
#define AC_BATTLEORDER      0x24
#define AC_INNER            0x50
#define INNER_LINKER        0x8
#define AL_OBJID            0x4AC
#define AL_FORWARD          0x4B8
#define AL_POSITION         0x4C4

// ===== protocol =====
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

// SELinux blocks untrusted_app -> untrusted_app abstract unix sockets,
// so we use TCP loopback instead.
#define SOCK_PORT 47291

// ===== shared state =====
static std::mutex g_actors_mtx;
static std::vector<EspActor> g_actors;
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

static void *get_singleton_instance(Il2CppClass *klass) {
    if (!klass) return nullptr;
    Il2CppClass *parent = il2cpp_class_get_parent(klass);
    FieldInfo *f = il2cpp_class_get_field_from_name(parent, "s_instance");
    if (!f) f = il2cpp_class_get_field_from_name(klass, "s_instance");
    if (!f) return nullptr;
    void *inst = nullptr;
    il2cpp_field_static_get_value(f, &inst);
    return inst;
}

// Scan ActorManager.updatableActorList 鈫?populate output vector
// Returns count of actors collected (0 if AM not ready).
static int scan_actors(std::vector<EspActor> &out) {
    out.clear();

    static Il2CppClass *cached_am_klass = nullptr;
    static FieldInfo *cached_s_inst = nullptr;
    static Il2CppClass *cached_dv_klass = nullptr;
    static int cached_off_ctx = -1;

    if (!cached_am_klass) {
        cached_am_klass = find_class_anywhere("Assets.Scripts.GameLogic", "ActorManager");
        if (!cached_am_klass) return 0;
        Il2CppClass *parent = il2cpp_class_get_parent(cached_am_klass);
        cached_s_inst = il2cpp_class_get_field_from_name(parent, "s_instance");
        if (!cached_s_inst) cached_s_inst = il2cpp_class_get_field_from_name(cached_am_klass, "s_instance");
        if (!cached_s_inst) return 0;
    }

    void *am = nullptr;
    il2cpp_field_static_get_value(cached_s_inst, &am);
    if (!am) return 0;

    void *dictview = *(void **)((char *)am + AM_UPDATABLE_LIST);
    if (!dictview) return 0;

    if (!cached_dv_klass) {
        cached_dv_klass = il2cpp_object_get_class((Il2CppObject *)dictview);
        FieldInfo *f_ctx = il2cpp_class_get_field_from_name(cached_dv_klass, "Context");
        int raw = f_ctx ? il2cpp_field_get_offset(f_ctx) : 0;
        cached_off_ctx = raw >= 0x10 ? raw : (raw + 0x10);
    }

    void *dict = *(void **)((char *)dictview + cached_off_ctx);
    if (!dict) return 0;

    int count = *(int *)((char *)dict + 0x18);
    void *entries = *(void **)((char *)dict + 0x10);
    if (!entries || count <= 0 || count > 256) return 0;

    out.reserve(count);
    for (int i = 0; i < count + 8 && (int)out.size() < count; i++) {
        if (i >= 256) break;  // safety
        char *e = (char *)entries + 0x18 + i * 24;
        int hashCode = *(int *)(e + 0);
        int next     = *(int *)(e + 4);
        uint32_t key = *(uint32_t *)(e + 8);
        void *ac     = *(void **)(e + 16);
        if (hashCode < 0 && next < 0) continue;
        if (!ac) continue;

        void *inner = *(void **)((char *)ac + AC_INNER);
        if (!inner) continue;
        void *al = *(void **)((char *)inner + INNER_LINKER);
        if (!al) continue;

        EspActor a;
        a.key         = key;
        a.type        = *(int *)((char *)ac + AC_ACTORTYPE);
        a.configId    = *(int *)((char *)ac + AC_CONFIGID);
        a.camp        = *(int *)((char *)ac + AC_CMPTYPE);
        a.battleOrder = *(int *)((char *)ac + AC_BATTLEORDER);
        a.objId       = *(uint32_t *)((char *)al + AL_OBJID);
        float *p = (float *)((char *)al + AL_POSITION);
        a.x = p[0]; a.y = p[1]; a.z = p[2];
        int   *f = (int   *)((char *)al + AL_FORWARD);
        a.fwd_x = f[0]; a.fwd_y = f[1]; a.fwd_z = f[2];
        out.push_back(a);
    }
    return (int)out.size();
}

// Send the current snapshot to a single client fd. Returns false on socket error.
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
    msg.msg_iovlen = 2;

    ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
    return n > 0;
}

// Server thread: bind TCP loopback, accept one client at a time, hand fd to global
static void server_thread() {
    LOGI("[esp v12] server thread, tid=%d", gettid());
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { LOGE("[esp v12] socket() errno=%d", errno); return; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SOCK_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("[esp v12] bind 127.0.0.1:%d errno=%d", SOCK_PORT, errno);
        close(srv);
        return;
    }
    if (listen(srv, 4) < 0) {
        LOGE("[esp v12] listen errno=%d", errno);
        close(srv);
        return;
    }
    LOGI("[esp v12] listening on 127.0.0.1:%d", SOCK_PORT);

    while (true) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) {
            if (errno == EINTR) continue;
            LOGE("[esp v12] accept errno=%d", errno);
            sleep(1);
            continue;
        }
        LOGI("[esp v12] client connected fd=%d", cli);
        int old = g_client_fd.exchange(cli);
        if (old >= 0) close(old);
    }
}

// Scanner thread: every 500 ms, scan + send if client connected
static void scan_thread() {
    LOGI("[esp v11] scan thread, tid=%d", gettid());
    sleep(45);  // shorter warmup; ActorManager usually ready by 30s but be safe
    LOGI("[esp v11] scan loop starting");

    while (true) {
        usleep(150 * 1000);  // 150 ms period (~6.7 Hz)

        int fd = g_client_fd.load();
        if (fd < 0) continue;  // no client, skip work

        std::vector<EspActor> actors;
        int n = scan_actors(actors);
        if (n == 0) continue;

        if (!send_snapshot(fd, actors)) {
            LOGI("[esp v11] client disconnected, closing fd=%d", fd);
            close(fd);
            g_client_fd.store(-1);
        }
    }
}

void esp_start(const char *game_data_dir) {
    LOGI("[esp v11] esp_start");
    std::thread srv(server_thread);
    srv.detach();
    std::thread scn(scan_thread);
    scn.detach();
}
