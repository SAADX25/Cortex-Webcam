# UnityCapture interface

Upstream: https://github.com/schellingb/UnityCapture

Pinned commit: `3ed54c325e0ad71afcf4f246c07e5e17b3d7f2d2`.

`shared.inl` is an altered version of upstream `Source/shared.inl`. Original copyright/permission notice is retained in `LICENSE`. Cortex changes: preserve C++ exception configuration; guard the Windows macro; make the debug assertion conform to strict C++; release mapped views; request mutex modification rights; use bounded sender/open mutex waits, including abandoned-mutex ownership; make the capture-number cast explicit. The receiver ABI and object names are unchanged.

`Install/UnityCaptureFilter32.dll` and `UnityCaptureFilter64.dll` are unmodified upstream binaries from this commit. SHA-256 values are pinned in `scripts/InstallVirtualCamera.ps1`. They are DirectShow filters, not a kernel driver. No Unity runtime is required by the Cortex sender.

Registration is a separate, explicit administrator action. Keep these DLLs in their current location after registration. Unregister before moving or deleting them. The IPC tests use a compatible in-process consumer without installing the filters; they do not replace validation in Zoom, Discord, or OBS.
