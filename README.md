# RealtekUVCFix

Experimental Lilu plugin that fixes UVC 1.5 stream negotiation for certain Realtek USB webcams on Intel Hackintosh systems running macOS Tahoe.

## Problem

Some Realtek webcams are detected by macOS and expose valid camera formats, but fail to start streaming:

- no camera LED
- no first video frame
- QuickTime, Photo Booth, FaceTime, and AVFoundation wait indefinitely

The camera tested during development has:

- Vendor ID: `0x0BDA`
- Product ID: `0x58F4`
- UVC version: `0x0150`

The sensor, USB mapping, endpoint configuration, and MJPEG frame data were verified to work with a patched build of libuvc.

## Root cause

On the tested macOS Tahoe build, `UVCFamily.framework` calculates the Video Streaming Probe/Commit control length as:

```c
length = bcdUVC >= 0x0101 ? 34 : 26;
```

The tested UVC 1.5 camera requires the 48-byte UVC 1.5 control block.

RealtekUVCFix replaces the logic at runtime with:

```c
length = bcdUVC >= 0x0150 ? 48 : 34;
```

## How it works

RealtekUVCFix is a Lilu plugin.

It hooks the macOS shared-cache page validation path and replaces one verified 17-byte instruction pattern in `UVCFamily.framework` while the dyld shared cache is mapped.

It does not modify the System volume or dyld shared cache on disk.

## Compatibility

Currently tested only on:

- Intel x86_64 Hackintosh
- macOS Tahoe 26.5
- Darwin 25.5.0
- Realtek webcam VID `0x0BDA`, PID `0x58F4`
- Lilu 1.7.2

The original byte pattern was verified to occur exactly once in the tested `dyld_shared_cache_x86_64h`.

Do not assume compatibility with another macOS build without verifying the pattern first.

## Installation

1. Download or build `RealtekUVCFix.kext`.
2. Place it in `EFI/OC/Kexts`.
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

Then test the camera in QuickTime, Photo Booth, FaceTime, or another AVFoundation application.

For debug logs:

```bash
log show --last boot \
  --info \
  --debug \
  --style compact \
  --predicate 'eventMessage CONTAINS[c] "ruvc" OR eventMessage CONTAINS[c] "RealtekUVCFix"'
```

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

Build with:

```bash
xcodebuild \
  -project RealtekUVCFix.xcodeproj \
  -target RealtekUVCFix \
  -configuration Release \
  CODE_SIGNING_ALLOWED=NO \
  build
```

## Safety warning

This project patches executable code loaded from the macOS dyld shared cache.

A mismatched pattern or unsupported macOS update could cause instability, camera service crashes, or boot failure.

Keep a working EFI backup and an easy way to disable or remove the kext.

## Credits

- Acidanthera for Lilu and MacKernelSDK
- FeatureUnlock as a reference implementation for shared-cache patching
- libuvc developers
- USB-IF UVC specification

## License

See `LICENSE`.
