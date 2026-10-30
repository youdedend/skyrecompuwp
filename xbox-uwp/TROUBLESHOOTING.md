# Troubleshooting the smoke package on console

For a console that installed the package but crashed immediately (reported against v0.1.1).

## The two things that were wrong

**1. Activation metadata was missing (probable root cause of the crash).**
The project never set `<GenerateWindowsMetadata>true</GenerateWindowsMetadata>`, so the linker
never ran `/WINMD` and no WinRT activation metadata was emitted. Symptoms match exactly what
you saw: `Add-AppxPackage` succeeds (the manifest is valid, the signature is valid, CI stays
green), the tile appears, and the process dies at activation before any of our code runs.
Microsoft's Xbox ATG UWP samples set this flag on every configuration. Fixed in v0.2.0.

**2. Empty LocalState was my defect, not a second fault.**
v0.1.x only logged through `OutputDebugStringW`, which has no listener on a console, and never
opened a file. Empty LocalState was therefore guaranteed by design. Fixed in v0.2.0.

## What v0.2.0 adds

| Change | Why |
|---|---|
| `<LocalState>\smoke.log`, appended per line with `CreateFile2(FILE_APPEND_DATA)` and closed each write | No `FlushFileBuffers` (not in the UWP API allowlist), no handle lifetime to get wrong, and a crash between lines cannot lose what was already written |
| Every `IFrameworkView` entry point wrapped in `try`/`catch`, logging the `HRESULT` | An escaping C++/CX exception normally terminates the process silently |
| A failure no longer terminates: it logs, paints red, and exits after 30 s | Keeps the evidence on screen and in the file |
| Screen colour encodes the furthest stage reached | A screenshot alone localises the failure |

## Reading the screen colour

| Colour | Stage reached | Meaning if it stops here |
|---|---|---|
| Dark grey | `Start` | Process started; died before `Initialize` finished |
| Blue | `AppInitialized` | Died between `Initialize` and `SetWindow` |
| Teal | `WindowSet` | Died between `SetWindow` and device creation |
| Indigo | `DeviceCreated` | D3D11 device exists; swap chain/RTV failed |
| Green | `SwapChainCreated` | Everything created; died entering the present loop |
| Animated | `Running` | Working |
| Red | `Failed` | See `smoke.log` — the HRESULT is in there |

If you get a **red** screen, the log will name the failing call. If you get **black** or the app
vanishes with no red, the failure is before our first render, so the log is still the only
source — fetch it as below.

## Fetching `smoke.log` from the console

1. Console in Developer Mode, on the same network as your PC.
2. Open the Xbox Device Portal: `https://<console-ip>:11443`, accept the certificate warning,
   sign in with the credentials set in *Dev Home → Settings → Remote Access*.
3. **File Explorer** (left nav) → pick the app sandbox → `LocalAppData` →
   `LocalState` → `smoke.log`.
4. Download it and paste its contents.

The file is ASCII, one line per event, each prefixed with seconds since process start, e.g.:

```
[   0.000] === SkyRecomp UWP smoke 0.2.0 main() ===
[   0.001] log: \\?\C:\Data\Users\...\LocalState\smoke.log
[   0.002] === SkyRecomp UWP smoke 0.2.0 ===
[   0.310] SetWindow: bounds=1920x1080
[   0.412] D3D11CreateDevice ok, feature level 0xB000
[   0.530] swap chain created 1920x1080 (flip sequential)
[   0.531] stage -> Running
```

A file that stops after `main()` but before `stage -> AppInitialized` means the failure is in
activation or event wiring, not in D3D. A file that does not appear at all means the process is
dying before `main()`, which points back at packaging/activation rather than code.

## Other things worth checking on console

- **App type must be `Game`** in *Dev Home → the app → View details*. In `App` mode the memory
  budget is much smaller and graphics-oriented apps can be terminated at launch.
- **VCLibs must be installed first.** The app links the DLL CRT (`/MD`, mandatory with C++/CX).
  Install `dependencies\Microsoft.VCLibs.x64.14.00.appx` before the `.msix`.
- **Certificate trust.** An untrusted signer normally fails at deploy (`0x800B0109`), but a
  partial trust state can let install succeed and activation fail.
- **Xbox One vs Series.** The package is x64 and requests D3D feature level 11.0 first, falling
  back to 10.x. Both console families are x64 for UWP; only Series X|S is guaranteed to expose
  FL11.0 to apps.
