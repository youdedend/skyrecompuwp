# skyrecompuwp

Xbox (Developer Mode) UWP work around [SkyRecomp](https://github.com/Apolinario-coder/SkyRecomp),
a static recompilation of *The Legend of Zelda: Skyward Sword HD* for PC.

**Upstream SkyRecomp has no Xbox or UWP support.** No `xbox-uwp.zip` exists in its releases
or CI artifacts, and its only Windows build is a desktop Win32 `.exe` that links Vulkan,
which cannot run on an Xbox. The audit that establishes this is in
[`BUILD_NOTES.md`](BUILD_NOTES.md).

## What is in this repository

| Path | What it is |
|---|---|
| [`BUILD_NOTES.md`](BUILD_NOTES.md) | The audit: what upstream supports, what prebuilt packages exist, the four hard blockers, toolchain reality, Dev Mode sideloading steps. Read this first. |
| [`XBOX-UWP-PORT-PLAN.md`](XBOX-UWP-PORT-PLAN.md) | Staged engineering plan to actually get there (D3D12 backend, UWP platform layer, off-console game library, memory budget, MSIX packaging). |
| [`xbox-uwp/`](xbox-uwp/README.md) | Working packaging scaffold: Xbox manifest, certificate + packaging scripts, a D3D11 smoke-test UWP app that proves the pipeline, and the gated real-runtime project. |
| [`audit/commands.log`](audit/commands.log) | Every command used in the audit, with its output. |
| [`.github/workflows/build-xbox-uwp.yml`](.github/workflows/build-xbox-uwp.yml) | CI on `windows-2022` (VS 2022 + Windows SDK 10.0.26100.0): builds, signs and packages the smoke test into `xbox-uwp-smoketest.zip`. |

## Status in one line

The packaging pipeline is real and testable today; the game runtime port is not started and
cannot be shortcut — see `BUILD_NOTES.md` §4 and §7.

## No game files

Nothing here contains or downloads game assets, keys, or decryption tooling. Upstream
SkyRecomp requires the user's own legally obtained *Skyward Sword HD* v1.0.0 files.
