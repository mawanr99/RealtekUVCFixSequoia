//
//  kern_start.cpp
//  RealtekUVCFix.kext
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>

#include <sys/vnode.h>

#define MODULE_SHORT "ruvc"

static mach_vm_address_t originalCsValidatePage {};
static bool patchApplied {};

// Original UVCFamily code:
//
//   length = bcdUVC >= 0x0101 ? 34 : 26;
//
// Replacement:
//
//   length = bcdUVC >= 0x0150 ? 48 : 34;
//
static const uint8_t uvcProbeOriginal[] {
    0x31, 0xC9,
    0x66, 0x3D, 0x01, 0x01,
    0x0F, 0x93, 0xC1,
    0x4C, 0x8D, 0x24, 0xCD, 0x1A, 0x00, 0x00, 0x00
};

static const uint8_t uvcProbePatched[] {
    0x31, 0xC9,
    0x66, 0x3D, 0x50, 0x01,
    0x0F, 0x93, 0xC1,
    0x4C, 0x6B, 0xE1, 0x0E,
    0x49, 0x83, 0xC4, 0x22
};

static void patchedCsValidatePage(
    vnode_t vnode,
    memory_object_t pager,
    memory_object_offset_t pageOffset,
    const void *data,
    int *validated,
    int *tainted,
    int *nx
) {
    FunctionCast(
        patchedCsValidatePage,
        originalCsValidatePage
    )(
        vnode,
        pager,
        pageOffset,
        data,
        validated,
        tainted,
        nx
    );

    if (patchApplied || data == nullptr)
        return;

    char path[PATH_MAX] {};
    int pathLength = sizeof(path);

    if (
        vn_getpath(vnode, path, &pathLength) != 0 ||
        !UserPatcher::matchSharedCachePath(path)
    ) {
        return;
    }

    bool replaced = KernelPatcher::findAndReplace(
        const_cast<void *>(data),
        PAGE_SIZE,
        uvcProbeOriginal,
        sizeof(uvcProbeOriginal),
        uvcProbePatched,
        sizeof(uvcProbePatched)
    );

    if (replaced) {
        patchApplied = true;

        SYSLOG(
            MODULE_SHORT,
            "patched UVC 1.5 Probe/Commit length in %s at cache page 0x%llx",
            path,
            static_cast<unsigned long long>(pageOffset)
        );
    }
}

static void pluginStart() {
    DBGLOG(MODULE_SHORT, "RealtekUVCFix starting");

    lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
        KernelPatcher::RouteRequest request(
            "_cs_validate_page",
            patchedCsValidatePage,
            originalCsValidatePage
        );

        if (!patcher.routeMultipleLong(
            KernelPatcher::KernelID,
            &request,
            1
        )) {
            SYSLOG(
                MODULE_SHORT,
                "failed to route _cs_validate_page"
            );
        }
    });
}

static const char *bootargOff[] {
    "-ruvcoff"
};

static const char *bootargDebug[] {
    "-ruvcdbg"
};

static const char *bootargBeta[] {
    "-ruvcbeta"
};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::BigSur,
    KernelVersion::Tahoe,
    pluginStart
};
