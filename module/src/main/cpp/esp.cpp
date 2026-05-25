//
// ESP loop v9: iterate updatableActorList → ActorConfig → ActorLinker → position
// v8 finding: updatableActorList @ AM+0x10 has 70 real actor entries (5v5)
//
// v9 plan:
//   1. read updatableActorList → dict → entries (Dictionary<UInt32, ActorConfig>)
//   2. iterate entries (stride 24, elements from entries+0x18)
//   3. for each ActorConfig: read ActorType@+0x18 / ConfigID@+0x1c / CmpType@+0x20 / inner@+0x50
//   4. if inner non-NULL: read ObjID@+0x4AC, position@+0x4C4, rotation@+0x4D0
//   5. log per-entry; also hexdump first 2 entries' AC + AL for offset validation
//

#include "esp.h"
#include "log.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"
#include <thread>
#include <unistd.h>
#include <cstring>
#include <cstdio>

#define DO_API(r, n, p) extern r (*n) p

#include "il2cpp-api-functions.h"

#undef DO_API

#define OFF_AC_ACTORTYPE  0x18
#define OFF_AC_CONFIGID   0x1c
#define OFF_AC_CMPTYPE    0x20
#define OFF_AC_BATTLEORDER 0x24
#define OFF_AC_INNER      0x50

#define OFF_AL_OBJID      0x4AC
#define OFF_AL_POSITION   0x4C4
#define OFF_AL_ROTATION   0x4D0

struct Vector3 { float x, y, z; };

static void hexdump(const char *label, const void *p, size_t len) {
    if (!p) { LOGI("[esp] %s: NULL", label); return; }
    char line[256];
    for (size_t off = 0; off < len; off += 16) {
        int pos = snprintf(line, sizeof(line), "[esp] %s+%03zx:", label, off);
        for (size_t j = 0; j < 16 && off+j < len; j++) {
            pos += snprintf(line+pos, sizeof(line)-pos, " %02x", ((const uint8_t*)p)[off+j]);
        }
        LOGI("%s", line);
    }
}

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

static void esp_loop(const char *game_data_dir) {
    LOGI("[esp v9] thread start, tid=%d", gettid());
    sleep(60);
    LOGI("[esp v9] post-warmup-60s");

    Il2CppClass *am_klass = find_class_anywhere("Assets.Scripts.GameLogic", "ActorManager");
    if (!am_klass) { LOGE("[esp v9] no ActorManager klass"); return; }

    void *am = get_singleton_instance(am_klass);
    LOGI("[esp v9] am=%p", am);
    if (!am) return;

    // updatableActorList @ AM+0x10 (confirmed by v8: COUNT=70)
    void *dictview = *(void **)((char *)am + 0x10);
    if (!dictview) { LOGE("[esp v9] updatableActorList NULL"); return; }

    Il2CppClass *dv_klass = il2cpp_object_get_class((Il2CppObject *)dictview);
    FieldInfo *f_context = il2cpp_class_get_field_from_name(dv_klass, "Context");
    int off_ctx_raw = f_context ? il2cpp_field_get_offset(f_context) : 0;
    int off_ctx = off_ctx_raw >= 0x10 ? off_ctx_raw : (off_ctx_raw + 0x10);
    void *dict = *(void **)((char *)dictview + off_ctx);
    if (!dict) { LOGE("[esp v9] dict NULL"); return; }

    int count = *(int *)((char *)dict + 0x18);
    void *entries = *(void **)((char *)dict + 0x10);
    LOGI("[esp v9] updatableActorList dict=%p COUNT=%d entries=%p", dict, count, entries);
    if (!entries || count <= 0 || count > 256) return;

    int dumped = 0;
    int iter_cap = count < 20 ? count : 20;  // iterate up to 20 to capture all real actors

    for (int i = 0; i < iter_cap; i++) {
        char *e = (char *)entries + 0x18 + i * 24;
        int hashCode = *(int *)(e + 0);
        int next     = *(int *)(e + 4);
        uint32_t key = *(uint32_t *)(e + 8);
        void *ac     = *(void **)(e + 16);

        // Skip free-list entries (hashCode == -1 means deleted)
        if (hashCode < 0 && next < 0) continue;
        if (!ac) {
            LOGI("[esp v9] entry[%d] key=%u AC=NULL", i, key);
            continue;
        }

        int actorType = *(int *)((char *)ac + OFF_AC_ACTORTYPE);
        int configId  = *(int *)((char *)ac + OFF_AC_CONFIGID);
        int cmpType   = *(int *)((char *)ac + OFF_AC_CMPTYPE);
        int battleOrder = *(int *)((char *)ac + OFF_AC_BATTLEORDER);
        void *inner   = *(void **)((char *)ac + OFF_AC_INNER);

        if (inner) {
            uint32_t objId = *(uint32_t *)((char *)inner + OFF_AL_OBJID);
            Vector3 *pos = (Vector3 *)((char *)inner + OFF_AL_POSITION);
            LOGI("[esp v9] [%d] key=%u Type=%d Cfg=%d Camp=%d BO=%d AL=%p ObjID=%u pos=(%.1f,%.1f,%.1f)",
                 i, key, actorType, configId, cmpType, battleOrder,
                 inner, objId, pos->x, pos->y, pos->z);
        } else {
            LOGI("[esp v9] [%d] key=%u Type=%d Cfg=%d Camp=%d BO=%d AL=NULL",
                 i, key, actorType, configId, cmpType, battleOrder);
        }

        // Hexdump first 2 AC + AL for offset validation
        if (dumped < 2 && inner) {
            char label[48];
            snprintf(label, sizeof(label), "AC_%u", key);
            hexdump(label, ac, 0x60);
            snprintf(label, sizeof(label), "AL_%u_pos", key);
            hexdump(label, (char *)inner + 0x4A0, 0x40);
            dumped++;
        }
    }

    LOGI("[esp v9] ========== END DIAG v9 ==========");
}

void esp_start(const char *game_data_dir) {
    std::thread t(esp_loop, game_data_dir);
    t.detach();
}
