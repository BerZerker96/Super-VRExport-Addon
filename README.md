<div align="center">

![SuperVrExport Banner](banner.png)

[![ReShade](https://img.shields.io/badge/ReShade-6.3%2B-purple.svg)](https://reshade.me)
[![Platform](https://img.shields.io/badge/Windows-DX9%20%7C%20DX10%20%7C%20DX11%20%7C%20DX12%20%7C%20Vulkan%20%7C%20OpenGL-0078D4.svg)](#)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**Two ReShade addons that export stereoscopic 3D output directly to KatangaVR / VRScreenCap at full resolution with zero intermediate capture.**

</div>

---

## 🙏 Credits

| | |
|---|---|
| **[artumino](https://github.com/artumino/ReshadeVRExport)** | Original ReshadeVRExport — architecture, shared-texture pipeline, and Geo3D support that this project is built on |
| **[BlueSkyDefender](https://github.com/BlueSkyDefender/Depth3D)** | Author of SuperDepth3D and the Frame Alternation / DoubleBuffer additions that make the direct export pipeline possible |
| **[bo3b](https://github.com/bo3b/katanga)** | Author of Katanga VR — the VR display application this addon feeds. Reading the Katanga source code directly informed the correct IPC protocol (`KatangaMappedFile`, KMT handle size, `OpenSharedResource` requirements) and led to several critical bug fixes in this addon |
| **[Flugan](https://github.com/Flugan/Geo3D-Installer)** | Author of Geo3D — the D3D11 stereoscopic mod that GeoVrExport is designed to work with |

---

## 🗂️ Which Addon Do I Need?

| Addon | Use for |
|---|---|
| **`SuperVrExport.addon64`** | Games using **[SuperDepth3D](https://github.com/BlueSkyDefender/Depth3D)** to generate stereo |
| **`GeoVrExport.addon64`** | Games using the **[Geo-3D](https://github.com/Flugan/Geo3D-Installer)** for stereoscopic output (3DToElse only, no SuperDepth3D) |

**Not sure?** If you are adding stereoscopic 3D to a game yourself using the **[SuperDepth3D](https://github.com/BlueSkyDefender/Depth3D)** shader, use **SuperVrExport**. If you are using the **[Geo-3D](https://github.com/Flugan/Geo3D-Installer)** to inject stereo into a D3D11 game, use **GeoVrExport**.

---

## 💡 How It Works

Both addons hook into ReShade's effect pipeline and share the stereo 3D frame with KatangaVR or VRScreenCap every frame via a cross-process DXGI shared texture — no screen capture, no resolution loss, no latency beyond one frame.

### SuperVrExport pipeline (SuperDepth3D games)

```
SuperDepth3D (Side by Side mode + Double Buffer, configured automatically)
        │  writes the full-res SBS image into DoubleTex every frame
        │  (DoubleTex = BUFFER_WIDTH×2 — both eyes, full per-eye resolution)
        ▼
SuperVrExport addon
        │  copies DoubleTex directly to a shared GPU texture each frame
        ▼
KatangaMappedFile  ←  KatangaVR / VRScreenCap reads this handle
        ▼
 KatangaVR  →  VR headset (full resolution SBS)
```

**What SuperVrExport sets automatically:**

| Preprocessor / Uniform | Value | Why |
|---|---|---|
| `EX_DLP_FS_Mode` | `1` | Set once at startup so SuperDepth3D's extended modes are available |
| `DoubleBuffer_Mode` | `1` | Creates `DoubleTex` — the full-res SBS texture the addon copies |
| `Stereoscopic_Mode` | `0` (Side by Side) | Required so SuperDepth3D writes a valid SBS image into `DoubleTex` (the DoubleTex write path only runs in SBS mode) |

> Both preprocessors are set automatically at startup before the first shader
> compile, so they are always in place even if you haven't set them manually.

#### What changed in SuperVrExport

| Change | Detail |
|---|---|
| **Direct `DoubleTex` copy** | The addon now copies SuperDepth3D's `DoubleTex` (full-res SBS) straight to KatangaVR. **3DToElse and Frame Sequential mode are no longer used** — this removes the frame-sequential timing flicker and increases per-eye resolution. `texTOT` remains an automatic fallback. |
| **SBS mode (0) instead of Frame Sequential (6)** | `DoubleTex` is only written as a valid SBS image when SuperDepth3D is in Side-by-Side mode, so the addon sets `Stereoscopic_Mode = 0`. `FS_FA` / frame alternation are not used. |
| **Preprocessors set at startup** | `EX_DLP_FS_Mode` and `DoubleBuffer_Mode` are applied once in `init_runtime` before the first compile. The old "DoubleTex not found → forcing reload" recompile cascade has been **removed entirely** — no more reload churn or mid-session mode resets. |
| **Connection survives runtime recreates** | Enabling a virtual controller, alt-tabbing, or scene transitions trigger a runtime destroy/recreate. The shared resource and `KatangaMappedFile` handle are now **kept alive** across these on D3D11 and D3D12 (native + bridge), so KatangaVR no longer drops the connection. A device-change guard rebuilds correctly if the GPU device is genuinely recreated. |
| **Native D3D12 same-device path** | When possible the addon shares a same-device D3D12 resource (named `DX12VRStream`) with zero cross-device copy, falling back to the D3D11 bridge only when the driver rejects the native path. |


### GeoVrExport pipeline (Geo-3D games)

```
Geo-3D (injects frame-sequential stereo into D3D11 games)
        │  alternating L/R full-frame output each frame
        ▼
3DToElse.fx (Frame Sequential input, set by user once)
        │  reconstructs current + previous frame into full SBS (texTOT)
        ▼
GeoVrExport addon
        │  copies texTOT to a shared GPU texture each frame
        ▼
KatangaMappedFile  ←  KatangaVR / VRScreenCap reads this handle
        ▼
 KatangaVR  →  VR headset (full resolution SBS)
```

GeoVrExport sets **nothing automatically** — it simply reads the existing texTOT output from 3DToElse and shares it. No SuperDepth3D required. Geo-3D handles stereo generation independently.

---

## 📦 Requirements

| Component | Version / Notes |
|---|---|
| **ReShade** | 6.3.x or newer, installed in **DXGI proxy mode** (`dxgi.dll`) for D3D12 games |
| **[SuperDepth3D.fx](https://github.com/BlueSkyDefender/Depth3D)** | v5.3.8 or newer — **SuperVrExport only.** Must include `EX_DLP_FS_Mode` and `DoubleBuffer_Mode` preprocessors |
| **3DToElse.fx** | **GeoVrExport only.** Included in the `Effects/` folder |
| **KatangaVR / VRScreenCap** | 0.4.0-dev5 or newer. Must be started **after** the game |
| **OS** | Windows 10 2004+ or Windows 11 |

---

## 🚀 Setup — SuperVrExport (SuperDepth3D games)

### 1 — Install ReShade

Download [ReShade](https://reshade.me) and install it for your game selecting the **DXGI** API. This places `dxgi.dll` next to the game executable.

### 2 — Copy shader files

Copy **[`SuperDepth3D.fx`](https://github.com/BlueSkyDefender/Depth3D)** (v5.3.8+) into your `reshade-shaders\Shaders\` folder and enable the **SuperDepth3D** technique in the ReShade overlay.

### 3 — Install the addon

Copy **`SuperVrExport.addon64`** to the same folder as `dxgi.dll`. ReShade automatically loads all `.addon64` files from that directory.

### 4 — That's it for configuration

There is **nothing to configure manually.** On launch the addon automatically sets
SuperDepth3D to **Side by Side** mode with **Double Buffer** enabled, so `DoubleTex`
holds a full-res SBS image every frame. Just tune your SuperDepth3D depth/3D settings
to taste as normal.

> Because SuperDepth3D is in SBS mode, your flat monitor will show a squished
> side-by-side image — that is expected. KatangaVR receives the correct full-res SBS.

### 5 — Launch the game

Start the game. Within 2–3 seconds the addon writes the shared handle to
`KatangaMappedFile`. Look for `SuperVrExport: D3D12 ready` (or `D3D11 ready`) in
`ReShade.log`.

### 6 — Start KatangaVR / VRScreenCap

Launch the viewer **after** the game has loaded. It reads `KatangaMappedFile` and opens the shared texture automatically.

> If the viewer was already running before the game, restart it after the game loads.

---

## 🚀 Setup — GeoVrExport (Geo-3D games)

### 1 — Install ReShade

Download [ReShade](https://reshade.me) and install it for your game selecting the **DXGI** API.

### 2 — Install Geo-3D

Download and run the [Geo3D installer](https://github.com/Flugan/Geo3D-Installer) for your game. This injects the stereo frame-sequential output that GeoVrExport reads.

### 3 — Copy shader file

Copy **`3DToElse.fx`** into your `reshade-shaders\Shaders\` folder and enable the **To_Else** technique in the ReShade overlay. SuperDepth3D is **not needed**.

### 4 — Install the addon

Copy **`GeoVrExport.addon64`** to the same folder as `dxgi.dll`.

### 5 — Configure 3DToElse

Open the ReShade overlay, select the **To_Else** technique, and set:

| Setting | Value |
|---|---|
| **Stereoscopic Mode Input** | **Frame Sequential** (index 5) |
| **3D Display Mode** | **Side by Side** (index 0) |

### 6 — Launch the game

Start the game. The addon reads texTOT from 3DToElse immediately — no recompile required.

### 7 — Start KatangaVR / VRScreenCap

Launch the viewer **after** the game has loaded.

---

## 🕹️ D3D9 Games — dgVoodoo2 Required

Direct3D 9 games require an extra step because D3D9's shared texture support is limited and cross-process sharing with KatangaVR works most reliably through D3D11.

**Use [dgVoodoo2](https://dege.freeweb.hu/dgVoodoo2/dgVoodoo2/) to translate D3D9 → D3D11:**

1. Download [dgVoodoo2](https://dege.freeweb.hu/dgVoodoo2/dgVoodoo2/) and extract it
2. Copy `D3D9.dll` (from dgVoodoo2's `MS\x64\` folder) into your game folder
3. Install ReShade for the game selecting the **D3D11** API — ReShade will now intercept the translated D3D11 output
4. Follow the relevant setup steps above

> Without dgVoodoo2 the addon will attempt a native D3D9 bridge path but this may fail or produce black frames depending on the game and driver. The D3D11 translation via dgVoodoo2 is the recommended and most reliable approach.

> 📺 **Follow this tutorial for the dgVoodoo2 setup:** [dgVoodoo2 Setup Guide](https://www.youtube.com/watch?v=66shwiE3Jc8)

---

## 🧩 D3D9 + SuperDepth3D or Geo-3D — Native Fast Path (`geod3d9.dll`, no dgVoodoo2)

If you're running a **SuperDepth3D or Geo-3D** game on **D3D9** (e.g. Dragon Age: Origins) and want to stay
fully native — no dgVoodoo2 translation layer — use the **`geod3d9.dll`** proxy shipped in
the `GeoD3D9Proxy/` folder. It makes GeoVrExport's fast GPU path work on plain D3D9 games.

### Why it's needed

Legacy D3D9 games create a *plain* `IDirect3DDevice9`. A plain (non-Ex) device **cannot create
or open shared textures**, so GeoVrExport is forced onto a slow full-resolution CPU readback
every frame (you'll see `D3D9 ready (CPU staging full-res, non-Ex device)` in the log, with poor
performance). Only an `IDirect3DDevice9Ex` supports the GPU-side shared render target the fast
path needs.

`geod3d9.dll` sits **in front of** ReShade and transparently upgrades the game's calls
(`Direct3DCreate9 → Direct3DCreate9Ex`, `CreateDevice → CreateDeviceEx`). Because the upgrade
happens above ReShade, ReShade itself creates an *extended* device, so GeoVrExport detects
`IDirect3DDevice9Ex` and runs its GPU shared-surface path — **zero CPU readback**.

```
game.exe  ->  d3d9.dll (geod3d9 proxy)  ->  ReShade\d3d9.dll (chainloaded)  ->  system d3d9
```

### Setup

The goal: **only the proxy stays next to the game `.exe`**; ReShade and everything it needs move
into a `ReShade\` subfolder.

1. **Build the proxy.** In `GeoD3D9Proxy/`, run `build_geod3d9.bat` → produces a 32-bit
   `geod3d9.dll` (DA:O and most D3D9 games are 32-bit; for a 64-bit game change `/MACHINE:X86`
   to `/MACHINE:X64`).
2. **Create a `ReShade\` subfolder** in the game folder (next to `DAOrigins.exe`).
3. **Move ReShade and its files into `ReShade\`** so nothing but the proxy is left beside the EXE:
   - `d3d9.dll`  ← ReShade itself (it **must** keep the `d3d9.dll` name so it detects the D3D9 API)
   - `GeoVrExport.addon32` (and `Geo3D.addon32` if used)
   - `ReShade.ini`
   - `ReShadePreset.ini`

   Resulting layout:
   ```
   <game>\DAOrigins.exe
   <game>\d3d9.dll                  <- geod3d9.dll renamed to d3d9.dll  (ONLY this beside the EXE)
   <game>\ReShade\d3d9.dll          <- ReShade
   <game>\ReShade\GeoVrExport.addon32
   <game>\ReShade\Geo3D.addon32
   <game>\ReShade\ReShade.ini
   <game>\ReShade\ReShadePreset.ini
   ```
4. **Put the proxy beside the EXE.** Copy `geod3d9.dll` into the game folder and rename it to
   **`d3d9.dll`**. This is now the only DirectX DLL next to the game executable.
5. *(Optional)* If you put ReShade somewhere other than `ReShade\d3d9.dll`, drop a `geod3d9.ini`
   next to the proxy pointing at it — see `GeoD3D9Proxy/geod3d9.ini.example`.
6. **Launch.** In `ReShade.log` you should now see the fast branch:
   `GeoVrExport: D3D9 ready (D3D9Ex shared)` — instead of the CPU-staging line.

> The proxy never makes a game worse: if the Ex upgrade fails for any reason it silently falls
> back to a normal `CreateDevice`. It does not modify the GeoVrExport / SuperVrExport addons.

> **dgVoodoo2 vs the proxy:** dgVoodoo2 (D3D9 → D3D11) is the broadest, zero-build option and is
> still recommended if you just want it working or if a game misbehaves with the proxy. The
> `geod3d9.dll` proxy is the lighter, fully-native alternative for Geo-3D D3D9 games — see
> `GeoD3D9Proxy/README.md` for full details and caveats.

---

## 🔧 Troubleshooting

| Symptom | Fix |
|---|---|
| KatangaVR shows nothing | Restart KatangaVR **after** the game loads. Check `ReShade.log` for `D3D11 ready` / `D3D12 ready` |
| Black screen in headset | Confirm `first copy fired` in `ReShade.log`. Restart KatangaVR |
| Flat monitor shows squished side-by-side | Expected — SuperVrExport runs SuperDepth3D in SBS mode so `DoubleTex` is valid. KatangaVR still receives correct full-res SBS |
| `DoubleTex` looks mono / stretched in the overlay preview | The addon forces SBS mode automatically; if you manually changed `Stereoscopic_Mode` away from Side by Side, `DoubleTex` stops being valid SBS. Leave the mode alone |
| D3D9 game: black / no connection | Use dgVoodoo2 to translate D3D9 → D3D11. See section above |
| D3D9 Geo-3D game: very poor performance | Log shows `CPU staging full-res` — install the `geod3d9.dll` proxy so GeoVrExport gets the fast `D3D9Ex shared` path. See "D3D9 + Geo-3D — Native Fast Path" |
| Addon not in ReShade list | Ensure `.addon64` is next to `dxgi.dll`; reinstall ReShade with "Install add-ons" checked |
| Game crashes with SuperVrExport | Confirm the game uses SuperDepth3D. For native Geo3D games use GeoVrExport instead |
| Game crashes with GeoVrExport | Confirm the [Geo-3D](https://github.com/Flugan/Geo3D-Installer) is installed, 3DToElse is enabled, and Stereoscopic Mode Input is set to Frame Sequential |

### Reading ReShade.log

| Message | Meaning |
|---|---|
| `SuperVrExport: D3D12 ready (NATIVE same-device...)` | Best path — same-device D3D12 share. Start KatangaVR now |
| `SuperVrExport: D3D12 ready (D3D11 bridge...)` | Working via the D3D11 bridge. Start KatangaVR now |
| `SuperVrExport: D3D11 ready` | Bridge established — start KatangaVR now |
| `GeoVrExport: D3D11 ready` | Bridge established — start KatangaVR now |
| `GeoVrExport: D3D9 ready (D3D9Ex shared)` | **Fast** native D3D9 GPU path — the `geod3d9.dll` proxy is working. Best case for D3D9 |
| `GeoVrExport: D3D9 ready (CPU staging full-res, non-Ex device)` | Slow fallback — plain D3D9 device, CPU readback each frame. Install the `geod3d9.dll` proxy (see the D3D9 + Geo-3D section) or use dgVoodoo2 |
| `GeoVrExport: D3D9 GPU share unavailable, falling back` | ReShade couldn't make a shared resource on this device — proxy isn't in front of ReShade. Re-check the load order / folder layout |
| `first copy fired, src=... dst=...` | GPU copy is working |

---

## 🏗️ Building

Requires Visual Studio 2019 or 2022 with the C++ Desktop workload.

```bat
build.bat
```

If `include_v6.3.3\` and `include_latest\` are already present, builds immediately. Otherwise downloads ReShade headers automatically. Produces:

```
bin\reshade_6.3.x\SuperVrExport.addon32   bin\reshade_6.3.x\SuperVrExport.addon64
bin\reshade_6.3.x\GeoVrExport.addon32     bin\reshade_6.3.x\GeoVrExport.addon64
bin\reshade_latest\SuperVrExport.addon32  bin\reshade_latest\SuperVrExport.addon64
bin\reshade_latest\GeoVrExport.addon32    bin\reshade_latest\GeoVrExport.addon64
```

Use `.addon64` for 64-bit games (most modern games) and `.addon32` for 32-bit games. Check your ReShade version in the overlay top-left corner to pick the right folder.

---

<div align="center">

Original addon by **[artumino](https://github.com/artumino/ReshadeVRExport)** · SuperDepth3D by **[BlueSkyDefender](https://github.com/BlueSkyDefender/Depth3D)** · Katanga VR by **[bo3b](https://github.com/bo3b/katanga)** · Geo3D by **[Flugan](https://github.com/Flugan/Geo3D-Installer)**

</div>
