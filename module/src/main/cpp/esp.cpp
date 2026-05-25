//
// ESP loop v7: A+B parallel
//   A) chain ActorConfig.inner → ActorLinker → position @ +0x4C4 (validate offset)
//   B) enumerate alternate mgrs (DimensionActorMgr, HeroManager, etc.) to find one with all actors
//
// Design: ONE-SHOT diag (no heartbeat loop, v6 stability proven).
//   - sleep 60s (vs 30s in v6) to give actor spawn more time
//   - dump ActorConfig + chain to ActorLinker + hexdump position window
//   - probe sibling Singleton classes; for each, get s_instance and dump 0x80 bytes
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

#define OFF_OBJID         0x4AC
#define OFF_POSITION      0x4C4
#define OFF_ROTATION      0x4D0

#define OFF_AC_ACTORTYPE  0x18
#define OFF_AC_CONFIGID   0x1c
#define OFF_AC_CMPTYPE    0x20
#define OFF_AC_INNER      0x50

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
        if (k) {
            LOGI("[esp v7] found %s.%s in %s", ns, name, il2cpp_image_get_name(img));
            return k;
        }
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

static void probe_singleton(const char *ns, const char *name) {
    Il2CppClass *k = find_class_anywhere(ns, name);
    if (!k) { LOGI("[esp v7] %s.%s NOT FOUND", ns, name); return; }
    void *inst = get_singleton_instance(k);
    LOGI("[esp v7] %s.%s s_instance=%p", ns, name, inst);
    if (inst) {
        hexdump(name, inst, 0x80);
    }
}

static void part_a_actor_chain(void) {
    LOGI("[esp v7] ========== PART A: actorList chain ==========");
    Il2CppClass *am_klass = find_class_anywhere("Assets.Scripts.GameLogic", "ActorManager");
    if (!am_klass) { LOGE("[esp v7] no ActorManager"); return; }

    void *am = get_singleton_instance(am_klass);
    LOGI("[esp v7] am=%p", am);
    if (!am) return;

    FieldInfo *f_actorList = il2cpp_class_get_field_from_name(am_klass, "actorList");
    int off_actorList = f_actorList ? il2cpp_field_get_offset(f_actorList) : -1;
    if (off_actorList < 0) return;

    void *dictview = *(void **)((char *)am + off_actorList);
    if (!dictview) { LOGE("[esp v7] dictview NULL"); return; }

    Il2CppClass *dv_klass = il2cpp_object_get_class((Il2CppObject *)dictview);
    FieldInfo *f_context = il2cpp_class_get_field_from_name(dv_klass, "Context");
    int off_ctx_raw = f_context ? il2cpp_field_get_offset(f_context) : 0;
    int off_ctx = off_ctx_raw >= 0x10 ? off_ctx_raw : (off_ctx_raw + 0x10);
    void *dict = *(void **)((char *)dictview + off_ctx);
    LOGI("[esp v7] dict=%p", dict);
    if (!dict) return;

    int count = *(int *)((char *)dict + 0x18);
    void *entries = *(void **)((char *)dict + 0x10);
    LOGI("[esp v7] dict count=%d entries=%p", count, entries);

    if (!entries || count <= 0 || count > 256) return;

    for (int i = 0; i < count && i < 12; i++) {
        char *e = (char *)entries + 0x18 + i * 24;
        int hashCode = *(int *)(e + 0);
        int next     = *(int *)(e + 4);
        uint32_t key = *(uint32_t *)(e + 8);
        void *ac     = *(void **)(e + 16);
        LOGI("[esp v7] entry[%d] hash=%d next=%d key=%u ActorConfig=%p", i, hashCode, next, key, ac);

        if (!ac) continue;
        int actorType = *(int *)((char *)ac + OFF_AC_ACTORTYPE);
        int configId  = *(int *)((char *)ac + OFF_AC_CONFIGID);
        int cmpType   = *(int *)((char *)ac + OFF_AC_CMPTYPE);
        void *inner   = *(void **)((char *)ac + OFF_AC_INNER);
        LOGI("[esp v7]   AC[%u]: ActorType=%d ConfigID=%d CmpType=%d inner=%p",
             key, actorType, configId, cmpType, inner);

        char label[48];
        snprintf(label, sizeof(label), "AC_%u", key);
        hexdump(label, ac, 0x80);

        if (inner) {
            snprintf(label, sizeof(label), "AL_%u", key);
            // ActorLinker dump 0x4A0..0x4E0 — covers ObjID, position, rotation
            hexdump(label, (char *)inner + 0x4A0, 0x40);

            uint32_t objId = *(uint32_t *)((char *)inner + OFF_OBJID);
            Vector3 *pos = (Vector3 *)((char *)inner + OFF_POSITION);
            LOGI("[esp v7]   AL[%u]: ObjID=%u pos=(%.2f, %.2f, %.2f)",
                 key, objId, pos->x, pos->y, pos->z);
        }
    }
}

static void part_b_alt_mgrs(void) {
    LOGI("[esp v7] ========== PART B: alternate mgr probes ==========");
    probe_singleton("Assets.Scripts.GameLogic", "DimensionActorMgr");
    probe_singleton("Assets.Scripts.GameLogic", "HeroManager");
    probe_singleton("Assets.Scripts.GameLogic", "SoldierManager");
    probe_singleton("Assets.Scripts.GameLogic", "OrganManager");
    probe_singleton("Assets.Scripts.GameLogic", "MonsterManager");
    probe_singleton("Assets.Scripts.GameLogic", "PlayerManager");
    probe_singleton("Assets.Scripts.GameLogic", "GameLogicAPI");
    probe_singleton("Assets.Scripts.GameLogic", "BattleManager");
    probe_singleton("Assets.Scripts.GameLogic", "DimensionBaseWorld");
    probe_singleton("Assets.Scripts.GameLogic", "DimensionPVPWorld");
}

static void esp_loop(const char *game_data_dir) {
    LOGI("[esp v7] thread start, tid=%d", gettid());
    sleep(60);  // Extended warmup — give 5v5 actors more time to spawn
    LOGI("[esp v7] post-warmup-60s");

    part_a_actor_chain();
    part_b_alt_mgrs();

    LOGI("[esp v7] ========== END DIAG v7 ==========");
}

void esp_start(const char *game_data_dir) {
    std::thread t(esp_loop, game_data_dir);
    t.detach();
}
