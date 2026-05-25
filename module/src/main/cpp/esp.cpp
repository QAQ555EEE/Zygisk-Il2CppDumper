//
// ESP loop v5: reflect Dictionary layout + dump ActorConfig
// Approach: ActorManager.actorList is DictionaryView<UInt32, ActorConfig>, the
// 11 TinyValueLists are empty in this sgame build. The real actor data is in
// actorList.Context (Dictionary). Use IL2CPP API to find _entries / _count
// true offsets (Tencent may have shuffled fields), then walk entries to dump
// ActorConfig payload to confirm ConfigID + ActorLinker (inner @ +0x50) path.
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

// Probe a candidate field by name on a class; log offset if found
static int probe_offset(Il2CppClass *klass, const char *name) {
    FieldInfo *f = il2cpp_class_get_field_from_name(klass, name);
    if (!f) { LOGI("[esp]   field '%s' NOT FOUND on klass", name); return -1; }
    int off = il2cpp_field_get_offset(f);
    LOGI("[esp]   field '%s' offset=0x%x", name, off);
    return off;
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

    // Probe ActorManager.actorList offset (instance field)
    FieldInfo *f_actorList = il2cpp_class_get_field_from_name(am_klass, "actorList");
    int off_actorList = f_actorList ? il2cpp_field_get_offset(f_actorList) : -1;
    LOGI("[esp] ActorManager.actorList offset=0x%x", off_actorList);

    int iter = 0;
    bool diag_done = false;
    while (true) {
        sleep(1);
        iter++;

        void *am = nullptr;
        il2cpp_field_static_get_value(s_inst_field, &am);
        if (!am) {
            if (iter % 10 == 0) LOGI("[esp] iter %d: am NULL", iter);
            continue;
        }

        if (!diag_done && off_actorList >= 0) {
            LOGI("[esp] ===== DIAG v5 =====");
            void *dictview = *(void **)((char *)am + off_actorList);
            LOGI("[esp] am=%p dictview=%p", am, dictview);
            if (!dictview) { LOGE("[esp] dictview NULL"); diag_done = true; continue; }

            // dictview: DictionaryView<UInt32, ActorConfig> — has one field 'Context' (Dictionary<,>)
            Il2CppClass *dv_klass = il2cpp_object_get_class((Il2CppObject *)dictview);
            LOGI("[esp] dv_klass=%p name=%s", dv_klass, dv_klass ? il2cpp_class_get_name(dv_klass) : "?");
            FieldInfo *f_context = il2cpp_class_get_field_from_name(dv_klass, "Context");
            int off_ctx = f_context ? il2cpp_field_get_offset(f_context) : 0x10;
            LOGI("[esp] DictionaryView.Context offset=0x%x", off_ctx);
            void *dict = *(void **)((char *)dictview + off_ctx);
            LOGI("[esp] dict=%p", dict);
            if (!dict) { LOGE("[esp] dict NULL"); diag_done = true; continue; }

            // Reflect Dictionary<TKey,TValue> field offsets
            Il2CppClass *dict_klass = il2cpp_object_get_class((Il2CppObject *)dict);
            LOGI("[esp] dict_klass=%p name=%s", dict_klass, dict_klass ? il2cpp_class_get_name(dict_klass) : "?");
            int off_count = probe_offset(dict_klass, "_count");
            int off_entries = probe_offset(dict_klass, "_entries");
            int off_buckets = probe_offset(dict_klass, "_buckets");
            int off_comparer = probe_offset(dict_klass, "_comparer");
            int off_freelist = probe_offset(dict_klass, "_freeList");
            int off_freecount = probe_offset(dict_klass, "_freeCount");

            // Wider hexdump of dict to align bytes with reported offsets
            hexdump("dict", dict, 0x80);

            // Read count + entries via reflected offsets
            if (off_count >= 0 && off_entries >= 0) {
                int count = *(int *)((char *)dict + off_count);
                void *entries = *(void **)((char *)dict + off_entries);
                LOGI("[esp] count=%d entries=%p", count, entries);
                if (entries) {
                    // IL2CPP array: +0x18 = max_length (size_t), +0x20 = first element
                    uint64_t len = *(uint64_t *)((char *)entries + 0x18);
                    LOGI("[esp] entries.length=%llu", (unsigned long long)len);
                    hexdump("entries_hdr", entries, 0x40);

                    // Probe Entry struct stride: standard mscorlib Entry is
                    // { int hashCode; int next; TKey key; TValue value }
                    // For Dictionary<UInt32, ActorConfig>: hashCode(4)+next(4)+key(4)+pad(4)+value(8) = 24 bytes
                    // For sgame variants it may differ — dump first few raw bytes.
                    hexdump("entries_e0", (char *)entries + 0x20, 0x60);
                    hexdump("entries_e1", (char *)entries + 0x20 + 0x18, 0x60);
                    hexdump("entries_e2", (char *)entries + 0x20 + 0x30, 0x60);
                }
            }

            // Try to find ActorConfig class and reflect 'inner' offset
            Il2CppClass *acfg_klass = il2cpp_class_from_name(img, "Assets.Scripts.GameLogic", "ActorConfig");
            if (acfg_klass) {
                LOGI("[esp] ActorConfig klass=%p", acfg_klass);
                probe_offset(acfg_klass, "inner");
                probe_offset(acfg_klass, "ConfigID");
                probe_offset(acfg_klass, "ActorType");
                probe_offset(acfg_klass, "CmpType");
            } else {
                LOGE("[esp] ActorConfig class not found in image");
            }

            LOGI("[esp] ===== END DIAG v5 =====");
            diag_done = true;
        }

        // Periodic heartbeat — show dict count to confirm it's filling
        if (diag_done && iter % 5 == 0 && off_actorList >= 0) {
            void *dictview = *(void **)((char *)am + off_actorList);
            if (dictview) {
                void *dict = *(void **)((char *)dictview + 0x10); // fallback offset
                if (dict) {
                    // _count probed inline below since we don't keep state — re-reflect once per heartbeat to stay correct
                    static Il2CppClass *cached_dk = nullptr;
                    static int cached_off_count = -1;
                    if (!cached_dk) {
                        cached_dk = il2cpp_object_get_class((Il2CppObject *)dict);
                        FieldInfo *fc = il2cpp_class_get_field_from_name(cached_dk, "_count");
                        if (fc) cached_off_count = il2cpp_field_get_offset(fc);
                    }
                    int c = cached_off_count >= 0 ? *(int *)((char *)dict + cached_off_count) : -1;
                    LOGI("[esp] iter %d dict count=%d", iter, c);
                }
            }
        }
    }
}

void esp_start(const char *game_data_dir) {
    std::thread t(esp_loop, game_data_dir);
    t.detach();
}
