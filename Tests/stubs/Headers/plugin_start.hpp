// Host-side stand-ins for the Lilu and XNU APIs used by kern_start.cpp.
#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define xStringify(a) #a
#define ADDPR(a) a
#define SYSLOG(...) do {} while (0)
#define DBGLOG(...) do {} while (0)

typedef uint64_t mach_vm_address_t;
typedef struct VnodeStub *vnode_t;
typedef void *memory_object_t;
typedef uint64_t memory_object_offset_t;

inline int findAndReplaceCalls = 0;
inline int vnGetPathCalls = 0;
inline vnode_t sharedCacheVnode = nullptr;

template <typename T, size_t N>
constexpr size_t arrsize(const T (&)[N]) { return N; }

template <typename T>
T FunctionCast(T, mach_vm_address_t original) {
    return reinterpret_cast<T>(original);
}

enum class KernelVersion { BigSur = 20, Sequoia = 24, Tahoe = 25 };
inline KernelVersion getKernelVersion() { return KernelVersion::Tahoe; }

inline int vn_getpath(vnode_t vnode, char *path, int *) {
    vnGetPathCalls++;
    strcpy(path, vnode == sharedCacheVnode ? "cache" : "other");
    return 0;
}

struct UserPatcher {
    static bool matchSharedCachePath(const char *path) {
        return strcmp(path, "cache") == 0;
    }
};

struct KernelPatcher {
    static constexpr size_t KernelID = 0;

    struct RouteRequest {
        template <typename T>
        RouteRequest(const char *, T, mach_vm_address_t &) {}
    };

    bool routeMultipleLong(size_t, RouteRequest *, size_t) { return true; }

    static bool findAndReplace(void *data, size_t dataSize,
                               const void *find, size_t findSize,
                               const void *replace, size_t replaceSize) {
        findAndReplaceCalls++;
        auto *bytes = static_cast<uint8_t *>(data);
        for (size_t i = 0; i + findSize <= dataSize; i++) {
            if (memcmp(bytes + i, find, findSize) == 0) {
                memcpy(bytes + i, replace, replaceSize);
                return true;
            }
        }
        return false;
    }
};

struct LiluAPI {
    enum Requirements { AllowNormal };
    void onPatcherLoadForce(void (*)(void *, KernelPatcher &)) {}
};
inline LiluAPI lilu;

inline size_t parseModuleVersion(const char *) { return 0; }

struct PluginConfiguration {
    const char *product;
    size_t version;
    LiluAPI::Requirements requirements;
    const char **disableArgs;
    size_t disableArgCount;
    const char **debugArgs;
    size_t debugArgCount;
    const char **betaArgs;
    size_t betaArgCount;
    KernelVersion minKernel;
    KernelVersion maxKernel;
    void (*start)();
};
