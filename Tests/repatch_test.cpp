// Host-side regression test for the shared-cache page hook.
//
// Run from the repository root:
//   clang++ -std=c++17 -I Tests/stubs -DPRODUCT_NAME=RealtekUVCFix \
//     -DMODULE_VERSION=0.0.0 Tests/repatch_test.cpp -o /tmp/repatch_test && /tmp/repatch_test

#include "../RealtekUVCFix/kern_start.cpp"

#include <cstdio>
#include <vector>

struct Patch {
    const char *name;
    const uint8_t *original;
    const uint8_t *replacement;
    size_t size;
    size_t page;
};

// irHelper and legacyHelper share a page, as they do in the real cache.
static const Patch cachePatches[] {
    {"probe", tahoeOriginal, tahoeReplacement, sizeof(tahoeOriginal), 0},
    {"irFallback", irFallbackOriginal, irFallbackReplacement, sizeof(irFallbackOriginal), 1},
    {"irColdCall", irColdCallOriginal, irColdCallReplacement, sizeof(irColdCallOriginal), 2},
    {"irHelper", irHelperOriginal, irHelperReplacement, sizeof(irHelperOriginal), 3},
    {"legacyHelper", legacyHelperOriginal, legacyHelperReplacement, sizeof(legacyHelperOriginal), 3},
    {"legacyCallsite", legacyCallsiteOriginal, legacyCallsiteReplacement, sizeof(legacyCallsiteOriginal), 4},
    {"l008Callsite", l008StrideCallsiteOriginal, l008StrideCallsiteReplacement, sizeof(l008StrideCallsiteOriginal), 5},
    {"l008ColdCaller", l008StrideColdCallerOriginal, l008StrideColdCallerReplacement, sizeof(l008StrideColdCallerOriginal), 5},
    {"l008Helper", l008StrideHelperOriginal, l008StrideHelperReplacement, sizeof(l008StrideHelperOriginal), 6},
    {"l008YTag", l008YTagHandlerCallsiteOriginal, l008YTagHandlerCallsiteReplacement, sizeof(l008YTagHandlerCallsiteOriginal), 7},
};

static constexpr size_t pageCount = 8;
static constexpr size_t patchSlot = 0x100;
static constexpr uint64_t firstPageOffset = 0x450000;

static VnodeStub *const cacheVnodeStub = reinterpret_cast<VnodeStub *>(0x1000);
static VnodeStub *const otherVnodeStub = reinterpret_cast<VnodeStub *>(0x2000);

static int failures = 0;

static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
}

static void passThrough(vnode_t, memory_object_t, memory_object_offset_t,
                        const void *, int *, int *, int *) {}

static std::vector<uint8_t> freshPage(size_t page) {
    std::vector<uint8_t> bytes(PAGE_SIZE, 0xCC);
    size_t slot = 1;
    for (const auto &patch : cachePatches) {
        if (patch.page == page)
            memcpy(bytes.data() + patchSlot * slot++, patch.original, patch.size);
    }
    return bytes;
}

static bool pageIsPatched(const std::vector<uint8_t> &bytes, size_t page) {
    size_t slot = 1;
    for (const auto &patch : cachePatches) {
        if (patch.page == page &&
            memcmp(bytes.data() + patchSlot * slot++, patch.replacement, patch.size) != 0)
            return false;
    }
    return true;
}

static void validate(vnode_t vnode, uint64_t offset, std::vector<uint8_t> &bytes) {
    patchedCsValidatePage(vnode, nullptr, offset, bytes.data(), nullptr, nullptr, nullptr);
}

static bool loadCachePage(size_t page) {
    auto bytes = freshPage(page);
    validate(cacheVnodeStub, firstPageOffset + page * PAGE_SIZE, bytes);
    return pageIsPatched(bytes, page);
}

int main() {
    originalCsValidatePage = reinterpret_cast<mach_vm_address_t>(passThrough);
    sharedCacheVnode = cacheVnodeStub;

    auto foreign = freshPage(0);
    validate(otherVnodeStub, firstPageOffset, foreign);
    check(foreign == freshPage(0), "page from another file is left untouched");

    check(loadCachePage(0), "first cache page is patched");

    int pathLookups = vnGetPathCalls;
    auto later = freshPage(0);
    validate(otherVnodeStub, 0x9000, later);
    check(vnGetPathCalls == pathLookups, "no path lookup for other files once the cache is known");

    bool allPatched = true;
    for (size_t page = 1; page < pageCount; page++)
        allPatched = loadCachePage(page) && allPatched;
    check(allPatched, "every remaining cache page is patched");

    bool allRepatched = true;
    for (size_t page = 0; page < pageCount; page++)
        allRepatched = loadCachePage(page) && allRepatched;
    check(allRepatched, "pages reloaded from disk are patched again");

    int scans = findAndReplaceCalls;
    std::vector<uint8_t> unrelated(PAGE_SIZE, 0xCC);
    validate(cacheVnodeStub, 0x900000, unrelated);
    check(findAndReplaceCalls == scans, "unrelated cache pages are not scanned once offsets are known");

    printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
