//
// ESP loop v6: fix v5 SIGSEGV + use real dict layout (+0x50 wrapper prefix)
// v5 findings:
//   - am ✓ (singleton is real, not empty shell)
//   - actorList @ ActorManager+0x8, DictionaryView.Context @ +0x8 (NOT +0x10!)
//   - dict count=7 confirmed for 5v5 match (real layout offsets shifted +0x50)
//   - probe_offset reports raw declared offsets; real values are at probe + 0x50
//   - ActorConfig NOT in same image as ActorManager (it's in Scripts.Base.dll)
//   - v5 SIGSEGV was from heartbeat using hardcoded dictview+0x10 (should be +0x8)
// v6 design:
//   - ONE-SHOT diag, no heartbeat loop (safer)
//   - search all images for ActorConfig class
//   - use real dict offsets (count @ wrapper_end+0x68 etc) — but verify by scanning
//   - iterate 7 entries, dump ActorConfig payload
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

// Find class across all loaded assemblies (handles cross-DLL refs like ActorConfig in Scripts.Base.dll)
static Il2CppClass *find_class_anywhere(const char *ns, const char *name) {
    Il2CppDomain *domain = il2cpp_domain_get();
    if (!domain) return nullptr;
    size_t n = 0;
    const Il2CppAssembly **assemblies = il2cpp_domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        const Il2CppImage *img = il2cpp_assembly_get_image(assemblies[i]);
        Il2CppClass *k = il2cpp_class_from_name(img, ns, name);
        if (k) {
            LOGI("[esp] found %s.%s in %s", ns, name, il2cpp_image_get_name(img));
            return k;
        }
    }
    LOGE("[esp] class %s.%s NOT FOUND in any image", ns, name);
    return nullptr;
}

static int probe_offset(Il2CppClass *klass, const char *name) {
    FieldInfo *f = il2cpp_class_get_field_from_name(klass, name);
    if (!f) { LOGI("[esp]   field '%s' NOT FOUND", name); return -1; }
    int off = il2cpp_field_get_offset(f);
    LOGI("[esp]   field '%s' offset=0x%x", name, off);
    return off;
}

// Scan a memory region for a 4-byte value at 8-byte alignment, return offset or -1
static int scan_for_int32(const void *base, size_t len, uint32_t needle) {
    for (size_t off = 0; off + 4 <= len; off += 4) {
        if (*(uint32_t *)((const char *)base + off) == needle) return (int)off;
    }
    return -1;
}

static void esp_loop(const char *game_data_dir) {
    LOGI("[esp v6] thread start, tid=%d", gettid());
    sleep(30);
    LOGI("[esp v6] post-warmup-30s");

    Il2CppClass *am_klass = find_class_anywhere("Assets.Scripts.GameLogic", "ActorManager");
    if (!am_klass) return;

    Il2CppClass *singleton_klass = il2cpp_class_get_parent(am_klass);
    FieldInfo *s_inst_field = il2cpp_class_get_field_from_name(singleton_klass, "s_instance");
    if (!s_inst_field) s_inst_field = il2cpp_class_get_field_from_name(am_klass, "s_instance");
    if (!s_inst_field) { LOGE("[esp v6] s_instance not found"); return; }

    FieldInfo *f_actorList = il2cpp_class_get_field_from_name(am_klass, "actorList");
    int off_actorList = f_actorList ? il2cpp_field_get_offset(f_actorList) : -1;
    LOGI("[esp v6] ActorManager.actorList offset=0x%x", off_actorList);
    if (off_actorList < 0) return;

    // Pre-resolve ActorConfig from any image (it's in Scripts.Base.dll, not GameCore)
    Il2CppClass *acfg_klass = find_class_anywhere("Assets.Scripts.GameLogic", "ActorConfig");
    if (acfg_klass) {
        LOGI("[esp v6] ActorConfig klass=%p", acfg_klass);
        probe_offset(acfg_klass, "inner");
        probe_offset(acfg_klass, "ConfigID");
        probe_offset(acfg_klass, "ActorType");
        probe_offset(acfg_klass, "CmpType");
    }

    // Wait for am to become non-NULL — at most 60 iterations (1 minute)
    void *am = nullptr;
    for (int i = 0; i < 60; i++) {
        sleep(1);
        il2cpp_field_static_get_value(s_inst_field, &am);
        if (am) break;
        if (i % 10 == 9) LOGI("[esp v6] still waiting for am, i=%d", i);
    }
    if (!am) { LOGE("[esp v6] am stayed NULL"); return; }

    LOGI("[esp v6] ===== DIAG v6 =====");
    LOGI("[esp v6] am=%p", am);
    void *dictview = *(void **)((char *)am + off_actorList);
    LOGI("[esp v6] dictview=%p", dictview);
    if (!dictview) { LOGE("[esp v6] dictview NULL"); return; }

    // DictionaryView.Context offset reflected (was 0x8 in v5)
    Il2CppClass *dv_klass = il2cpp_object_get_class((Il2CppObject *)dictview);
    FieldInfo *f_context = il2cpp_class_get_field_from_name(dv_klass, "Context");
    int off_ctx_raw = f_context ? il2cpp_field_get_offset(f_context) : 0;
    LOGI("[esp v6] DictionaryView.Context raw_offset=0x%x", off_ctx_raw);
    // If raw_offset < 0x10 then it's missing IL2CPP header offset — try both
    int off_ctx = off_ctx_raw >= 0x10 ? off_ctx_raw : (off_ctx_raw + 0x10);
    void *dict = *(void **)((char *)dictview + off_ctx);
    LOGI("[esp v6] dict @ +0x%x = %p", off_ctx, dict);
    if (!dict) {
        // Fallback: try the raw offset directly
        off_ctx = off_ctx_raw;
        dict = *(void **)((char *)dictview + off_ctx);
        LOGI("[esp v6] dict (fallback @ +0x%x) = %p", off_ctx, dict);
    }
    if (!dict) { LOGE("[esp v6] dict NULL"); return; }

    // Dump first 0x100 bytes of dict to find real _count / _entries / _buckets positions
    hexdump("dict", dict, 0x100);

    // Heuristic: locate _count by scanning for a plausible (count <= 64) followed by 0xffffffff (freeList=-1)
    // Pattern in v5: at +0x68 we saw "07 00 00 00 ff ff ff ff"
    int found_count_off = -1;
    int found_count_val = -1;
    for (int off = 0x10; off < 0xF0; off += 4) {
        uint32_t v = *(uint32_t *)((char *)dict + off);
        uint32_t next = *(uint32_t *)((char *)dict + off + 4);
        if (v > 0 && v < 64 && next == 0xFFFFFFFFu) {
            found_count_off = off;
            found_count_val = (int)v;
            LOGI("[esp v6] count pattern found @ +0x%x val=%d", off, (int)v);
            break;
        }
    }
    if (found_count_off < 0) {
        // Also try without -1 sentinel (post-fill freeList may not be -1)
        for (int off = 0x10; off < 0xF0; off += 4) {
            uint32_t v = *(uint32_t *)((char *)dict + off);
            if (v > 0 && v < 64) {
                LOGI("[esp v6] candidate small int @ +0x%x = %d", off, (int)v);
            }
        }
    }

    // If we found count, _entries = count_off - 0x10 (standard mscorlib Dictionary stride: buckets,entries,count)
    if (found_count_off >= 0) {
        int entries_off = found_count_off - 0x8;
        int buckets_off = found_count_off - 0x10;
        void *entries = *(void **)((char *)dict + entries_off);
        void *buckets = *(void **)((char *)dict + buckets_off);
        LOGI("[esp v6] inferred entries @ +0x%x = %p", entries_off, entries);
        LOGI("[esp v6] inferred buckets @ +0x%x = %p", buckets_off, buckets);

        if (entries) {
            uint64_t arr_len = *(uint64_t *)((char *)entries + 0x18);
            LOGI("[esp v6] entries.length(arr_header+0x18)=%llu", (unsigned long long)arr_len);
            hexdump("entries_hdr", entries, 0x40);
            // Dump first 3 entry candidates at standard mscorlib stride 24
            // Entry { int hashCode; int next; TKey key; TValue value }
            // Dictionary<UInt32, ActorConfig>: hash(4)+next(4)+key(4)+pad(4)+value(8) = 24
            for (int i = 0; i < 8 && i < found_count_val + 2; i++) {
                char label[48];
                snprintf(label, sizeof(label), "e[%d]", i);
                hexdump(label, (char *)entries + 0x20 + i * 24, 24);
            }
        }
    }

    LOGI("[esp v6] ===== END DIAG v6 (no further reads) =====");
    // INTENTIONALLY no heartbeat loop — avoids crash & reduces footprint
}

void esp_start(const char *game_data_dir) {
    std::thread t(esp_loop, game_data_dir);
    t.detach();
}
