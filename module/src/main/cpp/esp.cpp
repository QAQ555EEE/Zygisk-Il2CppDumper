//
// ESP loop v4: hex-dump am_instance + HeroActors object to diagnose empty lists
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

#define OFF_POSITION_IN_LINKER    0x4C4
#define OFF_OBJID_IN_LINKER       0x4AC

struct Vector3 { float x, y, z; };

static const char *TINY_LISTS[] = {
    "HeroActors", "OrganActors", "TowerActors", "SoldierActors",
    "DragonActors", "VehicleActors", "BuffMonsterActors", "SpringActors",
    "CallMonsterActors", "CallActors", "SacredAnimalActors", nullptr
};

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

static const Il2CppImage *find_image_with_actormanager() {
    Il2CppDomain *domain = il2cpp_domain_get();
    if (!domain) { LOGE("[esp] no domain"); return nullptr; }
    size_t n = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        const Il2CppImage *img = il2cpp_assembly_get_image(assemblies[i]);
        Il2CppClass *k = il2cpp_class_from_name(img, "Assets.Scripts.GameLogic", "ActorManager");
        if (k) {
            LOGI("[esp] ActorManager found in %s", il2cpp_image_get_name(img));
            return img;
        }
    }
    return nullptr;
}

static void esp_loop(const char *game_data_dir) {
    LOGI("[esp] thread start, tid=%d", gettid());
    sleep(30);
    LOGI("[esp] post-warmup-30s");

    const Il2CppImage *img = find_image_with_actormanager();
    if (!img) { LOGE("[esp] no image"); return; }

    Il2CppClass *am_klass = il2cpp_class_from_name(img, "Assets.Scripts.GameLogic", "ActorManager");
    if (!am_klass) { LOGE("[esp] no ActorManager class"); return; }

    Il2CppClass *singleton_klass = il2cpp_class_get_parent(am_klass);
    FieldInfo *s_inst_field = il2cpp_class_get_field_from_name(singleton_klass, "s_instance");
    if (!s_inst_field) s_inst_field = il2cpp_class_get_field_from_name(am_klass, "s_instance");
    if (!s_inst_field) { LOGE("[esp] s_instance not found"); return; }

    FieldInfo *list_fields[16] = {0};
    size_t list_offsets[16] = {0};
    int n_lists = 0;
    for (int i = 0; TINY_LISTS[i]; i++) {
        FieldInfo *f = il2cpp_class_get_field_from_name(am_klass, TINY_LISTS[i]);
        if (f) {
            list_fields[n_lists] = f;
            list_offsets[n_lists] = il2cpp_field_get_offset(f);
            n_lists++;
        }
    }

    int iter = 0;
    bool dumped_once = false;
    int last_nonzero_idx = -1;
    while (true) {
        sleep(1);
        iter++;

        void *am = nullptr;
        il2cpp_field_static_get_value(s_inst_field, &am);
        if (!am) {
            if (iter % 10 == 0) LOGI("[esp] iter %d: am NULL", iter);
            continue;
        }

        if (!dumped_once) {
            LOGI("[esp] ===== DIAGNOSTIC DUMP =====");
            LOGI("[esp] am=%p", am);
            hexdump("am", am, 0x90);

            for (int i = 0; i < n_lists; i++) {
                void *list_obj = *(void **)((char *)am + list_offsets[i]);
                char label[64];
                snprintf(label, sizeof(label), "%s_obj(%p)", TINY_LISTS[i], list_obj);
                hexdump(label, list_obj, 0x30);
            }

            void *dictview = *(void **)((char *)am + 0x8);
            hexdump("actorList_dv", dictview, 0x30);
            if (dictview) {
                void *dict = *(void **)((char *)dictview + 0x10);
                hexdump("actorList_dict", dict, 0x60);
            }

            LOGI("[esp] ===== END DUMP =====");
            dumped_once = true;
        }

        if (iter % 5 == 0) {
            char buf[768];
            int p = 0;
            for (int i = 0; i < n_lists; i++) {
                void *list_obj = *(void **)((char *)am + list_offsets[i]);
                int32_t sz = list_obj ? *(int32_t *)((char *)list_obj + 0x18) : -1;
                p += snprintf(buf+p, sizeof(buf)-p, "%s=%d ", TINY_LISTS[i], sz);
                if (sz > 0 && sz < 200) last_nonzero_idx = i;
            }
            LOGI("[esp] iter %d am=%p %s", iter, am, buf);
        }

        if (last_nonzero_idx >= 0) {
            void *list_obj = *(void **)((char *)am + list_offsets[last_nonzero_idx]);
            if (!list_obj) continue;
            int32_t sz = *(int32_t *)((char *)list_obj + 0x18);
            if (sz <= 0 || sz > 64) continue;
            void *backing = *(void **)((char *)list_obj + 0x10);
            if (!backing) continue;
            void **elements = (void **)((char *)backing + 0x20);
            for (int32_t i = 0; i < sz && i < 16; i++) {
                void *linker = elements[i];
                if (!linker) continue;
                uint32_t objId = *(uint32_t *)((char *)linker + OFF_OBJID_IN_LINKER);
                Vector3 *pos = (Vector3 *)((char *)linker + OFF_POSITION_IN_LINKER);
                LOGI("[esp]   %s[%d] obj=%u pos=(%.1f,%.1f,%.1f)",
                     TINY_LISTS[last_nonzero_idx], i, objId, pos->x, pos->y, pos->z);
            }
        }
    }
}

void esp_start(const char *game_data_dir) {
    std::thread t(esp_loop, game_data_dir);
    t.detach();
}
