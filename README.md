# RealtekUVCFix

Experimental Lilu plugin for enabling the built-in Realtek infrared camera on the Dell Precision 5540 under macOS Tahoe.

## Tested hardware and software

- Dell Precision 5540
- Built-in Realtek IR camera
- USB vendor ID: `0x0BDA`
- USB product ID: `0x58F4`
- UVC version: `0x0150`
- macOS Tahoe 26.5
- Darwin 25.5.0
- Intel x86_64 Hackintosh
- Lilu 1.7.2

Dell documents the Precision 5540 infrared camera as an optional camera device separate from the standard HD camera.

## Problem

On the tested Dell Precision 5540, macOS detects the built-in IR camera and exposes its camera formats, but streaming never starts:

- the IR camera LED remains off
- no first video frame is received
- QuickTime, Photo Booth, FaceTime, and other AVFoundation applications wait indefinitely

The camera hardware, USB mapping, endpoints, and MJPEG output were verified independently with a patched build of libuvc.

## Root cause

In the tested macOS Tahoe build, `UVCFamily.framework` calculates the Video Streaming Probe/Commit control length as:

```c
length = bcdUVC >= 0x0101 ? 34 : 26;
```

The Realtek UVC 1.5 camera used by the Dell Precision 5540 requires a 48-byte control block.

RealtekUVCFix changes the runtime logic to:

```c
length = bcdUVC >= 0x0150 ? 48 : 34;
```

## How it works

RealtekUVCFix is a Lilu plugin.

It hooks the shared-cache page validation path and replaces one verified 17-byte instruction pattern in `UVCFamily.framework` while the dyld shared cache is mapped.

It does not modify the System volume or the dyld shared cache on disk.

## Installation

1. Download `RealtekUVCFix.kext` from the latest release.
2. Copy it to `EFI/OC/Kexts`.
3. Add it to `Kernel -> Add` after `Lilu.kext`.
4. Reboot.

Required load order:

```text
Lilu.kext
RealtekUVCFix.kext
```

Optional debug boot arguments:

```text
-liludbgall -ruvcdbg keepsyms=1 debug=0x100
```

Disable the plugin with:

```text
-ruvcoff
```

## Verification

Check that both kexts loaded:

```bash
kmutil showloaded | grep -iE 'Lilu|RealtekUVCFix'
```

Then test the IR camera in QuickTime, Photo Booth, FaceTime, or another AVFoundation application.

## Building

Dependencies:

- Xcode
- Lilu Debug SDK
- MacKernelSDK

Place these in the repository root:

```text
Lilu.kext
MacKernelSDK
```

Build the Release configuration:

```bash
xcodebuild \
  -project RealtekUVCFix.xcodeproj \
  -target RealtekUVCFix \
  -configuration Release \
  CODE_SIGNING_ALLOWED=NO \
  build
```

## Compatibility

Currently verified only on:

- Dell Precision 5540
- Realtek IR camera `0BDA:58F4`
- macOS Tahoe 26.5
- Intel x86_64

The patch is tied to a byte pattern found in the tested macOS build. Compatibility with other Dell models, other Realtek cameras, or later macOS builds has not yet been verified.

## Safety warning

This project patches executable code loaded from the macOS dyld shared cache.

A mismatched macOS build or changed byte pattern may cause camera-service crashes, system instability, or boot failure.

Keep a working EFI backup and a reliable method to disable or remove the kext.

## Credits

- Acidanthera for Lilu and MacKernelSDK
- FeatureUnlock as a reference implementation for shared-cache patching
- libuvc developers
- USB-IF UVC specification

## License

See `LICENSE`.
