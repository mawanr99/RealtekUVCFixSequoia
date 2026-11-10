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
static vnode_t cacheVnode {};

struct UVCPatch {
    const uint8_t *original;
    const uint8_t *replacement;
    size_t size;
    const char *name;
    uint64_t pageOffset;  // Learned on first match. 0 means not found yet.
};

// macOS Tahoe 26.5
//
// Original:
//   length = bcdUVC >= 0x0101 ? 34 : 26;
//
// Replacement:
//   length = bcdUVC >= 0x0150 ? 48 : 34;
//
// Result register: r12
static const uint8_t tahoeOriginal[] {
    0x31, 0xC9,
    0x66, 0x3D, 0x01, 0x01,
    0x0F, 0x93, 0xC1,
    0x4C, 0x8D, 0x24, 0xCD, 0x1A, 0x00, 0x00, 0x00
};

static const uint8_t tahoeReplacement[] {
    0x31, 0xC9,
    0x66, 0x3D, 0x50, 0x01,
    0x0F, 0x93, 0xC1,
    0x4C, 0x6B, 0xE1, 0x0E,
    0x49, 0x83, 0xC4, 0x22
};

// macOS Sequoia
//
// Original:
//   length = bcdUVC >= 0x0101 ? 34 : 26;
//
// Replacement:
//   length = bcdUVC >= 0x0150 ? 48 : 34;
//
// Result register: rbx
static const uint8_t sequoiaOriginal[] {
    0x31, 0xC9,
    0x66, 0x3D, 0x01, 0x01,
    0x0F, 0x93, 0xC1,
    0x48, 0x8D, 0x1C, 0xCD, 0x1A, 0x00, 0x00, 0x00
};

static const uint8_t sequoiaReplacement[] {
    0x31, 0xC9,
    0x66, 0x3D, 0x50, 0x01,
    0x0F, 0x93, 0xC1,
    0x48, 0x6B, 0xD9, 0x0E,
    0x48, 0x83, 0xC3, 0x22
};

// macOS Tahoe 26.5 UVCExtension IR pixel-format diagnostic mapping
//
// Realtek IR exposes:
//   GUID 00000032-0002-0010-8000-00AA00389B71
//
// _getPixelFormatFromGUID() normally returns 0 for this GUID.
// The diagnostic mapping below changes the unknown-format fallback:
//
//   vendorID == 0x0BDA -> kCVPixelFormatType_OneComponent8 ('L008')
//   otherwise          -> 0
//
// The existing cold logging helper is reused as a trampoline target.
// Its only direct caller is disabled before the helper body is replaced.
//
// This is still diagnostic: it applies L008 to any unknown GUID from
// vendor 0x0BDA. A release-quality patch should identify the IR GUID
// more narrowly.

// Tail of _getPixelFormatFromGUID():
//
//   mov rax, r12
//   pop rbx
//   pop r12
//   pop r14
//   pop r15
//   pop rbp
//   ret
//   xor r12d, r12d
//   jmp cleanup
//
// Replace the unknown-format fallback with:
//   jmp helper
static const uint8_t irFallbackOriginal[] {
    0x4C, 0x89, 0xE0,
    0x5B,
    0x41, 0x5C,
    0x41, 0x5E,
    0x41, 0x5F,
    0x5D,
    0xC3,
    0x45, 0x31, 0xE4,
    0xEB, 0xE6
};

static const uint8_t irFallbackReplacement[] {
    0x4C, 0x89, 0xE0,
    0x5B,
    0x41, 0x5C,
    0x41, 0x5E,
    0x41, 0x5F,
    0x5D,
    0xC3,
    0xE9, 0x4D, 0x2D, 0x00, 0x00
};

// Disable the original sole caller of the cold helper before repurposing it.
static const uint8_t irColdCallOriginal[] {
    0x49, 0x83, 0xC6, 0x20,
    0x4C, 0x89, 0xF7,
    0x4C, 0x89, 0xE6,
    0xE8, 0x6B, 0x62, 0x01, 0x00,
    0xEB, 0x9F
};

static const uint8_t irColdCallReplacement[] {
    0x49, 0x83, 0xC6, 0x20,
    0x4C, 0x89, 0xF7,
    0x4C, 0x89, 0xE6,
    0x90, 0x90, 0x90, 0x90, 0x90,
    0xEB, 0x9F
};

// Beginning of the now-unused cold helper.
//
// Replacement:
//
//   cmp  r14, 0x0BDA
//   jne  non_realtek
//   mov  r12d, 'L008'
//   jmp  _getPixelFormatFromGUID cleanup
// non_realtek:
//   xor  r12d, r12d
//   jmp  _getPixelFormatFromGUID cleanup
static const uint8_t irHelperOriginal[] {
    0x55, 0x48, 0x89, 0xE5,
    0x48, 0x83, 0xEC, 0x20,
    0x48, 0x8B, 0x05, 0x02, 0xEC, 0xC5, 0x3F,
    0x48, 0x8B, 0x00,
    0x48, 0x89, 0x45, 0xF8,
    0x48, 0x8B, 0x07,
    0x4C, 0x8D, 0x45
};

static const uint8_t irHelperReplacement[] {
    0x49, 0x81, 0xFE, 0xDA, 0x0B, 0x00, 0x00,
    0x75, 0x0B,
    0x41, 0xBC, 0x38, 0x30, 0x30, 0x4C,
    0xE9, 0x85, 0xD2, 0xFF, 0xFF,
    0x45, 0x31, 0xE4,
    0xE9, 0x7D, 0xD2, 0xFF, 0xFF
};

static_assert(
    sizeof(irFallbackOriginal) == sizeof(irFallbackReplacement),
    "IR fallback patch size mismatch"
);

static_assert(
    sizeof(irColdCallOriginal) == sizeof(irColdCallReplacement),
    "IR cold-call patch size mismatch"
);

static_assert(
    sizeof(irHelperOriginal) == sizeof(irHelperReplacement),
    "IR helper patch size mismatch"
);

// macOS Tahoe 26.5 UVCExtension legacy-ID collision fix
//
// Realtek 0BDA:58F4 exposes two UVC functions:
//
//   RGB interface 0
//   IR  interface 2
//
// UVCExtension's original legacy ID omits the interface number, so
// both functions receive the same AVCaptureDevice uniqueID.
//
// Preserve the original CMOVE semantics for all devices. For this
// Realtek camera only, include the interface number in the low legacy
// component:
//
//   RGB: PID 0x58F4 + 0 -> 0x58F4
//   IR : PID 0x58F4 + 2 -> 0x58F6

static const uint8_t legacyCallsiteOriginal[] {
    0x80, 0x7D, 0xBF, 0x00,
    0x4C, 0x0F, 0x44, 0x6D, 0xA0,
    0x48, 0x8B, 0x45, 0xB0,
    0x48, 0xC1, 0xE0, 0x20,
    0x41, 0xC1, 0xE6, 0x10,
    0x49, 0x09, 0xC6,
    0x4D, 0x09, 0xEE
};

static const uint8_t legacyCallsiteReplacement[] {
    0x80, 0x7D, 0xBF, 0x00,
    0xE9, 0xB8, 0xF8, 0xFF, 0xFF,
    0x48, 0x8B, 0x45, 0xB0,
    0x48, 0xC1, 0xE0, 0x20,
    0x41, 0xC1, 0xE6, 0x10,
    0x49, 0x09, 0xC6,
    0x4D, 0x09, 0xEE
};

// Unreachable tail of the cold helper already repurposed by the
// L008 mapping. There are no external references into this region.
static const uint8_t legacyHelperOriginal[] {
    0xE0, 0x41, 0xC7, 0x00, 0x02, 0x01, 0x40, 0x08,
    0x49, 0x89, 0x40, 0x04,
    0x48, 0x8D, 0x3D, 0x7A, 0x7C, 0xFD, 0xFF,
    0x48, 0x8D, 0x0D, 0xD3, 0x00, 0x01, 0x00,
    0x6A, 0x11,
    0x5A,
    0x6A, 0x0C,
    0x41, 0x59,
    0xE8, 0x01, 0xAF, 0x00, 0x00,
    0x48, 0x8B, 0x05, 0xC8, 0xEB
};

static const uint8_t legacyHelperReplacement[] {
    // Save packed PID/interface.
    0x4C, 0x89, 0xE8,

    // Original:
    // cmove r13, [rbp - 0x60]
    0x4C, 0x0F, 0x44, 0x6D, 0xA0,

    // vendorID == 0x0BDA?
    0x41, 0x81, 0xFE, 0xDA, 0x0B, 0x00, 0x00,
    0x75, 0x15,

    // EDX = PID from packed high 16 bits.
    0x89, 0xC2,
    0xC1, 0xEA, 0x10,

    // PID == 0x58F4?
    0x81, 0xFA, 0xF4, 0x58, 0x00, 0x00,
    0x75, 0x08,

    // ECX = interface number.
    0x0F, 0xB7, 0xC8,

    // PID += interface.
    0x01, 0xCA,
    0x41, 0x89, 0xD5,

    // Return to original legacy-ID construction.
    0xE9, 0x1D, 0x07, 0x00, 0x00
};

static_assert(
    sizeof(legacyCallsiteOriginal) ==
        sizeof(legacyCallsiteReplacement),
    "legacy callsite patch size mismatch"
);

static_assert(
    sizeof(legacyHelperOriginal) ==
        sizeof(legacyHelperReplacement),
    "legacy helper patch size mismatch"
);


// macOS Tahoe 26.5 UVCFrameProcessor L008 stride fix
//
// UVCFrameProcessorGetBytesPerRowAlignmentForFormat() assumes that
// every non-planar native format uses two bytes per pixel:
//
//   align64(width << 1)
//
// That is valid for formats such as YUV422 but not for L008, which is
// one-component 8-bit.
//
// For L008, the UVC source is one byte per pixel, so its source
// bytes-per-row is the stream width. Keep that value so UVCNativeFrame
// can repack into CoreVideo's independently aligned destination stride.
//
// On the tested Realtek 0BDA:58F4 IR stream:
//
//   width:                       340
//   raw UVC bytes per row:       340
//   CoreVideo L008 bytesPerRow:  352
//   original derived value:      704

static const uint8_t l008StrideCallsiteOriginal[] {
    0x85, 0xC0,
    0x75, 0x59,

    0x48, 0x8B, 0x7D, 0xD0,
    0xE8, 0x92, 0x6C, 0x00, 0x00,

    0x84, 0xC0,
    0x41, 0x0F, 0x94, 0xC7
};

static const uint8_t l008StrideCallsiteReplacement[] {
    0x85, 0xC0,
    0x75, 0x59,

    // jmp UVCFrameProcessorGetBytesPerRowAlignmentForFormat.cold.1
    0xE9, 0x11, 0x6A, 0x00, 0x00,
    0x90, 0x90, 0x90, 0x90,

    0x84, 0xC0,
    0x41, 0x0F, 0x94, 0xC7
};

// The cold helper has one original caller, used only to log the
// CVPixelBufferCreate() error. Disable that call before repurposing
// the helper. The subsequent return-value logic remains unchanged.
static const uint8_t l008StrideColdCallerOriginal[] {
    0x89, 0xC7,
    0xE8, 0xB6, 0x69, 0x00, 0x00,
    0x49, 0xC7, 0xC6, 0xFF, 0xFF, 0xFF, 0xFF
};

static const uint8_t l008StrideColdCallerReplacement[] {
    0x89, 0xC7,
    0x90, 0x90, 0x90, 0x90, 0x90,
    0x49, 0xC7, 0xC6, 0xFF, 0xFF, 0xFF, 0xFF
};

// Repurpose the beginning of the now-unused cold helper.
//
// r15d = requested pixel format
// [rbp - 0x30] = temporary CVPixelBuffer
//
// L008:
//   r14 already contains stream width
//   -> use width as source bytes-per-row
//   -> original cleanup/return path
//
// Other formats:
//   CVPixelBufferIsPlanar(tempBuffer)
//   -> original alignment calculation
static const uint8_t l008StrideHelperOriginal[] {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48,
    0x83, 0xEC, 0x10, 0x48, 0x8B, 0x05, 0x21, 0x45,
    0xC1, 0x3F, 0x48, 0x8B, 0x00, 0x48, 0x89, 0x45,
    0xE8, 0x41, 0x89, 0xFE, 0x6A, 0x01, 0x5F, 0xE8,
    0x06, 0x9E, 0xFF, 0xFF, 0x48, 0x89, 0xC7, 0xE8,
    0x55, 0x03, 0x00, 0x00, 0x48, 0x89, 0xC3, 0x6A,
    0x10, 0x5E, 0x48, 0x89, 0xC7, 0xE8, 0x65, 0x03,
    0x00, 0x00, 0x84, 0xC0, 0x74, 0x2C, 0x4C, 0x8D,
    0x45, 0xE0, 0x41, 0xC7, 0x00, 0x00, 0x01, 0x00,
    0x04, 0x45, 0x89, 0x70, 0x04, 0x48, 0x8D, 0x3D,
    0x77, 0x35, 0xFF, 0xFF, 0x48, 0x8D, 0x0D, 0x90,
    0x1A, 0x00, 0x00, 0x6A, 0x10, 0x5A, 0x6A, 0x08,
    0x41, 0x59, 0x48, 0x89, 0xDE, 0xE8, 0x81, 0x02,
    0x00, 0x00, 0x48, 0x89, 0xDF, 0xFF, 0x15, 0x58,
    0x91, 0xC0, 0x3F, 0x48, 0x8B, 0x05, 0xB9, 0x44,
    0xC1, 0x3F, 0x48, 0x8B, 0x00, 0x48, 0x3B, 0x45,
    0xE8, 0x75, 0x09, 0x48, 0x83, 0xC4, 0x10, 0x5B,
    0x41, 0x5E, 0x5D, 0xC3, 0xE8, 0x4E, 0x02, 0x00,
    0x00,
};

static const uint8_t l008StrideHelperReplacement[] {
    0x41, 0x81, 0xFF, 0x38, 0x30, 0x30, 0x4C, 0x75,
    0x05, 0xE9, 0x0C, 0x96, 0xFF, 0xFF, 0x48, 0x8B,
    0x7D, 0xD0, 0xE8, 0x6E, 0x02, 0x00, 0x00, 0xE9,
    0xD7, 0x95, 0xFF, 0xFF, 0x81, 0xBB, 0xB0, 0x00,
    0x00, 0x00, 0x38, 0x30, 0x30, 0x4C, 0x75, 0x58,
    0x41, 0x56, 0x41, 0x57, 0x4C, 0x8B, 0x73, 0x18,
    0x4D, 0x8B, 0x76, 0x30, 0x4C, 0x89, 0xF7, 0x6A,
    0x01, 0x5E, 0xE8, 0x4C, 0x02, 0x00, 0x00, 0x85,
    0xC0, 0x75, 0x33, 0x4C, 0x89, 0xF7, 0xE8, 0x04,
    0x02, 0x00, 0x00, 0x44, 0x0F, 0xB6, 0x38, 0x41,
    0x83, 0xE7, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x4C,
    0x89, 0xF7, 0x6A, 0x01, 0x5E, 0xE8, 0x39, 0x02,
    0x00, 0x00, 0x45, 0x85, 0xFF, 0x0F, 0x94, 0xC0,
    0x90, 0x90, 0x90, 0x90, 0xEB, 0x02, 0x31, 0xC0,
    0x41, 0x5F, 0x41, 0x5E, 0x84, 0xC0, 0x75, 0x0C,
    0x48, 0x8B, 0x7B, 0x20, 0x48, 0x85, 0xFF, 0xE9,
    0xCF, 0xAF, 0xFF, 0xFF, 0xE9, 0xD5, 0xAF, 0xFF,
    0xFF,
};

static_assert(
    sizeof(l008StrideCallsiteOriginal) ==
        sizeof(l008StrideCallsiteReplacement),
    "L008 stride callsite patch size mismatch"
);

static_assert(
    sizeof(l008StrideColdCallerOriginal) ==
        sizeof(l008StrideColdCallerReplacement),
    "L008 stride cold-caller patch size mismatch"
);

static_assert(
    sizeof(l008StrideHelperOriginal) ==
        sizeof(l008StrideHelperReplacement),
    "L008 stride helper patch size mismatch"
);

// macOS Tahoe 26.5 UVCFrameProcessor L008 IR illumination filter.
//
// Realtek 0BDA:58F4 alternates illuminated and unilluminated IR frames.
//
// Realtek DeviceMFT encodes the illumination state in the Y-tag.
// For native L008, raw[0].bit0 maps to Y-tag bit 15:
//
//   raw[0].bit0 == 1 -> illuminated -> dispatch
//   raw[0].bit0 == 0 -> OFF/DROP    -> skip
//
// This is independent of the UVC FID phase, which may change between
// stream sessions.
//
// Redirect dispatchPreviousFrame's handler check into the helper
// installed in the repurposed cold region above.

static const uint8_t l008YTagHandlerCallsiteOriginal[] {
    0x4C, 0x89, 0xFF, 0xFF, 0x15, 0x77, 0xE1, 0xC0,
    0x3F, 0x48, 0x8B, 0x7B, 0x20, 0x48, 0x85, 0xFF,
    0x74, 0x09, 0x48, 0x8B, 0x73, 0x18,
};

static const uint8_t l008YTagHandlerCallsiteReplacement[] {
    0x4C, 0x89, 0xFF, 0xFF, 0x15, 0x77, 0xE1, 0xC0,
    0x3F, 0xE9, 0xC3, 0x4F, 0x00, 0x00, 0x90, 0x90,
    0x74, 0x09, 0x48, 0x8B, 0x73, 0x18,
};

static_assert(
    sizeof(l008YTagHandlerCallsiteOriginal) ==
        sizeof(l008YTagHandlerCallsiteReplacement),
    "L008 Y-tag handler patch size mismatch"
);

// sites[0] is the Probe/Commit patch. pluginStart swaps in the Sequoia
// variant on older kernels, so an unused pattern is never scanned.
static UVCPatch sites[] {
    { tahoeOriginal, tahoeReplacement, sizeof(tahoeOriginal), "Probe/Commit" },
    { irFallbackOriginal, irFallbackReplacement, sizeof(irFallbackOriginal), "IR fallback trampoline" },
    { irColdCallOriginal, irColdCallReplacement, sizeof(irColdCallOriginal), "IR cold-helper caller" },
    { irHelperOriginal, irHelperReplacement, sizeof(irHelperOriginal), "IR L008 helper" },
    { legacyCallsiteOriginal, legacyCallsiteReplacement, sizeof(legacyCallsiteOriginal), "legacy-ID trampoline" },
    { legacyHelperOriginal, legacyHelperReplacement, sizeof(legacyHelperOriginal), "legacy-ID helper" },
    { l008StrideCallsiteOriginal, l008StrideCallsiteReplacement, sizeof(l008StrideCallsiteOriginal), "L008 stride trampoline" },
    { l008StrideColdCallerOriginal, l008StrideColdCallerReplacement, sizeof(l008StrideColdCallerOriginal), "L008 stride cold caller" },
    { l008StrideHelperOriginal, l008StrideHelperReplacement, sizeof(l008StrideHelperOriginal), "L008 row-width helper" },
    { l008YTagHandlerCallsiteOriginal, l008YTagHandlerCallsiteReplacement, sizeof(l008YTagHandlerCallsiteOriginal), "L008 Y-tag filter" },
};

static bool isSharedCacheVnode(vnode_t vnode) {
    char path[PATH_MAX] {};
    int pathLength = sizeof(path);

    return vn_getpath(vnode, path, &pathLength) == 0 &&
        UserPatcher::matchSharedCachePath(path);
}

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

    if (data == nullptr)
        return;

    // Patches live on clean file-backed pages, so macOS can drop them and
    // reload the original bytes from disk at any time. Every page-in must be
    // patched again, which makes this filter the hot path.
    //
    // ponytail: assumes all sites share one cache file and that its vnode
    // lives for the whole boot (the shared region maps it). Keep one vnode
    // per site if a macOS build ever splits the UVC frameworks across files.
    if (cacheVnode != nullptr) {
        if (vnode != cacheVnode)
            return;
    } else if (!isSharedCacheVnode(vnode)) {
        return;
    }

    for (auto &site : sites) {
        if (site.pageOffset != 0 && site.pageOffset != pageOffset)
            continue;

        bool replaced = KernelPatcher::findAndReplace(
            const_cast<void *>(data),
            PAGE_SIZE,
            site.original,
            site.size,
            site.replacement,
            site.size
        );

        if (!replaced)
            continue;

        if (site.pageOffset == 0) {
            SYSLOG(
                MODULE_SHORT,
                "applied %s patch at offset 0x%llx",
                site.name,
                static_cast<unsigned long long>(pageOffset)
            );
        } else {
            DBGLOG(
                MODULE_SHORT,
                "reapplied %s patch at offset 0x%llx",
                site.name,
                static_cast<unsigned long long>(pageOffset)
            );
        }

        site.pageOffset = pageOffset;
        cacheVnode = vnode;
    }
}

static void pluginStart() {
    DBGLOG(MODULE_SHORT, "RealtekUVCFix starting");

    if (getKernelVersion() < KernelVersion::Tahoe) {
        sites[0].original = sequoiaOriginal;
        sites[0].replacement = sequoiaReplacement;
        sites[0].size = sizeof(sequoiaOriginal);
    }

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
