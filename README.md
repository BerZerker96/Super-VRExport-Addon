<div align="center">

![SuperVrExport Banner](banner.png)

[![ReShade](https://img.shields.io/badge/ReShade-6.3%2B-purple.svg)](https://reshade.me)
[![Platform](https://img.shields.io/badge/Windows-DX9%20%7C%20DX10%20%7C%20DX11%20%7C%20DX12%20%7C%20Vulkan%20%7C%20OpenGL-0078D4.svg)](#)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**Two ReShade addons that export stereoscopic 3D output directly to KatanaVR / VRScreenCap at full resolution with zero intermediate capture.**

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

Both addons hook into ReShade's effect pipeline and share the stereo 3D frame with KatanaVR or VRScreenCap every frame via a cross-process DXGI shared texture — no screen capture, no resolution loss, no latency beyond one frame.

### SuperVrExport pipeline (SuperDepth3D games)

```
SuperDepth3D (Side by Side mode + Double Buffer, configured automatically)
        │  writes the full-res SBS image into DoubleTex every frame
        │  (DoubleTex = BUFFER_WIDTH×2 — both eyes, full per-eye resolution)
        ▼
SuperVrExport addon
        │  copies DoubleTex directly to a shared GPU texture each frame
        ▼
KatangaMappedFile  ←  KatanaVR / VRScreenCap reads this handle
        ▼
 KatanaVR  →  VR headset (full resolution SBS)
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
| **Direct `DoubleTex` copy** | The addon now copies SuperDepth3D's `DoubleTex` (full-res SBS) straight to KatanaVR. **3DToElse and Frame Sequential mode are no longer used** — this removes the frame-sequential timing flicker and increases per-eye resolution. `texTOT` remains an automatic fallback. |
| **SBS mode (0) instead of Frame Sequential (6)** | `DoubleTex` is only written as a valid SBS image when SuperDepth3D is in Side-by-Side mode, so the addon sets `Stereoscopic_Mode = 0`. `FS_FA` / frame alternation are not used. |
| **Preprocessors set at startup** | `EX_DLP_FS_Mode` and `DoubleBuffer_Mode` are applied once in `init_runtime` before the first compile. The old "DoubleTex not found → forcing reload" recompile cascade has been **removed entirely** — no more reload churn or mid-session mode resets. |
| **Connection survives runtime recreates** | Enabling a virtual controller, alt-tabbing, or scene transitions trigger a runtime destroy/recreate. The shared resource and `KatangaMappedFile` handle are now **kept alive** across these on D3D11 and D3D12 (native + bridge), so KatanaVR no longer drops the connection. A device-change guard rebuilds correctly if the GPU device is genuinely recreated. |
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
KatangaMappedFile  ←  KatanaVR / VRScreenCap reads this handle
        ▼
 KatanaVR  →  VR headset (full resolution SBS)
```

GeoVrExport sets **nothing automatically** — it simply reads the existing texTOT output from 3DToElse and shares it. No SuperDepth3D required. Geo-3D handles stereo generation independently.

---

## 📦 Requirements

| Component | Version / Notes |
|---|---|
| **ReShade** | 6.3.x or newer, installed in **DXGI proxy mode** (`dxgi.dll`) for D3D12 games |
| **[SuperDepth3D.fx](https://github.com/BlueSkyDefender/Depth3D)** | v5.3.8 or newer — **SuperVrExport only.** Must include `EX_DLP_FS_Mode` and `DoubleBuffer_Mode` preprocessors |
| **3DToElse.fx** | **GeoVrExport only.** Not required for SuperVrExport (which reads `DoubleTex` directly). Included in the `Effects/` folder |
| **KatanaVR / VRScreenCap** | 0.4.0-dev5 or newer. Must be started **after** the game |
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
> side-by-side image — that is expected. KatanaVR receives the correct full-res SBS.

### 5 — Launch the game

Start the game. Within 2–3 seconds the addon writes the shared handle to
`KatangaMappedFile`. Look for `SuperVrExport: D3D12 ready` (or `D3D11 ready`) in
`ReShade.log`.

### 6 — Start KatanaVR / VRScreenCap

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

### 7 — Start KatanaVR / VRScreenCap

Launch the viewer **after** the game has loaded.

---

## 🕹️ D3D9 Games — dgVoodoo2 Required

Direct3D 9 games require an extra step because D3D9's shared texture support is limited and cross-process sharing with KatanaVR works most reliably through D3D11.

**Use [dgVoodoo2](https://dege.freeweb.hu/dgVoodoo2/dgVoodoo2/) to translate D3D9 → D3D11:**

1. Download [dgVoodoo2](https://dege.freeweb.hu/dgVoodoo2/dgVoodoo2/) and extract it
2. Copy `D3D9.dll` (from dgVoodoo2's `MS\x64\` folder) into your game folder
3. Install ReShade for the game selecting the **D3D11** API — ReShade will now intercept the translated D3D11 output
4. Follow the relevant setup steps above

> Without dgVoodoo2 the addon will attempt a native D3D9 bridge path but this may fail or produce black frames depending on the game and driver. The D3D11 translation via dgVoodoo2 is the recommended and most reliable approach.

> 📺 **Follow this tutorial for the dgVoodoo2 setup:** [dgVoodoo2 Setup Guide](https://www.youtube.com/watch?v=66shwiE3Jc8)

---

## 🔧 Troubleshooting

| Symptom | Fix |
|---|---|
| KatanaVR shows nothing | Restart KatanaVR **after** the game loads. Check `ReShade.log` for `D3D11 ready` / `D3D12 ready` |
| Black screen in headset | Confirm `first copy fired` in `ReShade.log`. Restart KatanaVR |
| Flat monitor shows squished side-by-side | Expected — SuperVrExport runs SuperDepth3D in SBS mode so `DoubleTex` is valid. KatanaVR still receives correct full-res SBS |
| `DoubleTex` looks mono / stretched in the overlay preview | The addon forces SBS mode automatically; if you manually changed `Stereoscopic_Mode` away from Side by Side, `DoubleTex` stops being valid SBS. Leave the mode alone |
| D3D9 game: black / no connection | Use dgVoodoo2 to translate D3D9 → D3D11. See section above |
| Addon not in ReShade list | Ensure `.addon64` is next to `dxgi.dll`; reinstall ReShade with "Install add-ons" checked |
| Game crashes with SuperVrExport | Confirm the game uses SuperDepth3D. For native Geo3D games use GeoVrExport instead |
| Game crashes with GeoVrExport | Confirm the [Geo-3D](https://github.com/Flugan/Geo3D-Installer) is installed, 3DToElse is enabled, and Stereoscopic Mode Input is set to Frame Sequential |

### Reading ReShade.log

| Message | Meaning |
|---|---|
| `SuperVrExport: D3D12 ready (NATIVE same-device...)` | Best path — same-device D3D12 share. Start KatanaVR now |
| `SuperVrExport: D3D12 ready (D3D11 bridge...)` | Working via the D3D11 bridge. Start KatanaVR now |
| `SuperVrExport: D3D11 ready` | Bridge established — start KatanaVR now |
| `GeoVrExport: D3D11 ready` | Bridge established — start KatanaVR now |
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
