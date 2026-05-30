# GeoD3D9Proxy — `geod3d9.dll`

A tiny **D3D9 → D3D9Ex** upgrading proxy that lets **GeoVrExport** use its fast GPU-side
shared-surface path on legacy **D3D9 + Geo-3D** games (e.g. Dragon Age: Origins) —
**without dgVoodoo2 and without any CPU readback.**

## What it does for Geo-3D / D3D9

Geo-3D games render stereo through `3DToElse.fx`, and GeoVrExport shares the resulting
`texTOT` surface across to KatanaVR. On modern APIs that share is a pure GPU copy. But old
D3D9 games create a **plain `IDirect3DDevice9`**, and a plain (non-Ex) device **cannot create
or open shared textures**. GeoVrExport is then forced onto a slow full-resolution CPU readback
every frame (`GetRenderTargetData` → system memory → `UpdateSubresource`), which at 4K is a
heavy PCIe round-trip — the cause of the poor performance you see, logged as:

```
GeoVrExport: D3D9 ready (CPU staging full-res, non-Ex device)
```

Only an `IDirect3DDevice9Ex` (created via `Direct3DCreate9Ex` + `CreateDeviceEx`) supports the
GPU-side shared render target the fast path needs.

This proxy is installed **as the game's `d3d9.dll`**. It transparently upgrades the game's calls:

```
Direct3DCreate9   ->  Direct3DCreate9Ex
CreateDevice      ->  CreateDeviceEx
```

and chainloads ReShade (kept named `d3d9.dll`, inside a `ReShade\` subfolder). Because the
upgrade happens **above** ReShade, ReShade itself ends up creating the device via
`CreateDeviceEx`, so its device wrapper is "extended" and GeoVrExport's
`QueryInterface(IDirect3DDevice9Ex)` succeeds → the **GPU shared path runs, zero readback**.
Success shows in the log as:

```
GeoVrExport: D3D9 ready (D3D9Ex shared)
```

## Load order

```
game.exe  ->  d3d9.dll (THIS proxy)  ->  ReShade\d3d9.dll (chainloaded)  ->  system d3d9
```

The proxy must sit **in front of** ReShade. An upgrade placed *below* ReShade would be hidden:
GeoVrExport sees ReShade's wrapped device, and ReShade only exposes `IDirect3DDevice9Ex` if
**ReShade** created the device as Ex — which only happens when something above it (this proxy)
made the original call `CreateDeviceEx`.

## Build

Run `build_geod3d9.bat` → produces a 32-bit `geod3d9.dll` (Dragon Age: Origins and most D3D9
games are 32-bit). For a 64-bit game, change `/MACHINE:X86` to `/MACHINE:X64` in the script.

## Install

The goal: **only the proxy stays next to the game `.exe`.** Everything ReShade needs — ReShade
itself, the addons, and both INI files — moves into a `ReShade\` subfolder.

1. In the game folder (next to e.g. `DAOrigins.exe`), create a subfolder named **`ReShade`**.
2. **Move ReShade and all its files into `ReShade\`:**
   - `d3d9.dll` — ReShade itself. It **must** keep the name `d3d9.dll` (that's how ReShade
     detects the Direct3D 9 API); the subfolder is what prevents the clash with this proxy.
   - `GeoVrExport.addon32` (and `Geo3D.addon32` if you use the separate Geo-3D addon)
   - `ReShade.ini`
   - `ReShadePreset.ini`
3. **Copy `geod3d9.dll` into the game folder and rename it to `d3d9.dll`.** This is now the only
   DirectX DLL beside the executable.
4. *(Optional)* If ReShade isn't at the default `ReShade\d3d9.dll`, create a `geod3d9.ini` next to
   the proxy pointing at it — see `geod3d9.ini.example`.

Resulting layout:

```
<game>\DAOrigins.exe
<game>\d3d9.dll                      <- geod3d9.dll renamed to d3d9.dll  (ONLY this beside the EXE)
<game>\geod3d9.ini                   <- optional
<game>\ReShade\d3d9.dll              <- ReShade (kept named d3d9.dll)
<game>\ReShade\GeoVrExport.addon32
<game>\ReShade\Geo3D.addon32         <- if used
<game>\ReShade\ReShade.ini
<game>\ReShade\ReShadePreset.ini
```

> Note: ReShade resolves its shader/preset paths from `ReShade.ini`. After moving the INIs into
> `ReShade\`, confirm the `EffectSearchPaths` / `TextureSearchPaths` / `PresetPath` entries still
> point to valid locations (absolute paths keep working; relative paths are relative to the game
> EXE, not the subfolder).

## Verifying it worked

In `ReShade.log`:

| Log line | Meaning |
|---|---|
| `GeoVrExport: D3D9 ready (D3D9Ex shared)` | ✅ Fast GPU path — the proxy is working. |
| `GeoVrExport: D3D9 ready (CPU staging full-res, non-Ex device)` | ❌ Proxy not in front of ReShade — recheck load order / layout. |
| `GeoVrExport: D3D9 GPU share unavailable, falling back` | ❌ ReShade couldn't make a shared resource on this device (still a plain device) — proxy isn't upgrading it. |

## Notes & caveats

- **Never makes a game worse:** if the Ex upgrade fails for any reason, the proxy transparently
  falls back to a plain `CreateDevice`, exactly as the game asked.
- **Does not touch the addons:** it neither modifies nor depends on GeoVrExport / SuperVrExport.
- **`D3DPOOL_MANAGED` caveat:** D3D9Ex devices do not support the managed memory pool. Games that
  allocate in `D3DPOOL_MANAGED` may fail on an Ex device (this minimal proxy upgrades device
  creation only; it does not translate the pool). If a game black-screens after switching to the
  proxy, that's the likely cause — use **dgVoodoo2** for that title instead, or request the
  extended proxy variant that wraps the device and remaps `MANAGED → DEFAULT`.
- **dgVoodoo2 vs the proxy:** dgVoodoo2 (D3D9 → D3D11) is the broadest, zero-build option. This
  proxy is the lighter, fully-native alternative specifically for Geo-3D D3D9 games.
