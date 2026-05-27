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

---

## 🗂️ Which Addon Do I Need?

| Addon | Use for |
|---|---|
| **`SuperVrExport.addon64`** | Games using **SuperDepth3D** to generate stereo (SD3D + 3DToElse) |
| **`GeoVrExport.addon64`** | Games with **native frame-sequential / Geo3D stereo output** (3DToElse only, no SuperDepth3D) |

**Not sure?** If you are adding stereoscopic 3D to a game yourself using the SuperDepth3D shader, use **SuperVrExport**. If the game already outputs stereo natively (e.g. Geo-3D titles, Yakuza series, OPUS Prism Peak), use **GeoVrExport**.

---

## 💡 How It Works

Both addons hook into ReShade's effect pipeline and share the stereo 3D frame with KatanaVR or VRScreenCap every frame via a cross-process DXGI shared texture — no screen capture, no resolution loss, no latency beyond one frame.

### SuperVrExport pipeline (SuperDepth3D games)

```
SuperDepth3D (Frame Sequential mode, configured automatically)
        │  alternating L/R full-frame output each frame
        ▼
3DToElse.fx (Frame Sequential input, set by user once)
        │  reconstructs current + previous frame into full SBS (texTOT)
        ▼
SuperVrExport addon
        │  copies texTOT to a shared GPU texture each frame
        ▼
KatangaMappedFile  ←  KatanaVR / VRScreenCap reads this handle
        ▼
 KatanaVR  →  VR headset (full resolution SBS)
```

**What SuperVrExport sets automatically:**

| Preprocessor / Uniform | Value | Why |
|---|---|---|
| `EX_DLP_FS_Mode` | `1` | Adds Frame Sequential (mode 6) to SuperDepth3D's Stereoscopic Mode dropdown |
| `DoubleBuffer_Mode` | `1` | Creates the DoubleTex buffer as a direct SBS source |
| `Stereoscopic_Mode` | `6` (Frame Sequential) | Set automatically after compile |
| `FS_FA` | `true` | Enables addon-driven frame alternation |

### GeoVrExport pipeline (Geo3D / native frame-sequential games)

```
Game (native frame-sequential stereo output)
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

GeoVrExport sets **nothing automatically** — it simply reads the existing texTOT output from 3DToElse and shares it. No SuperDepth3D required.

---

## 📦 Requirements

| Component | Version / Notes |
|---|---|
| **ReShade** | 6.3.x or newer, installed in **DXGI proxy mode** (`dxgi.dll`) for D3D12 games |
| **SuperDepth3D.fx** | v5.3.8 or newer — **SuperVrExport only.** Must include `EX_DLP_FS_Mode` and `DoubleBuffer_Mode` preprocessors |
| **3DToElse.fx** | Required for both addons. Included in the `Effects/` folder |
| **KatanaVR / VRScreenCap** | 0.4.0-dev5 or newer. Must be started **after** the game |
| **OS** | Windows 10 2004+ or Windows 11 |

---

## 🚀 Setup — SuperVrExport (SuperDepth3D games)

### 1 — Install ReShade

Download [ReShade](https://reshade.me) and install it for your game selecting the **DXGI** API. This places `dxgi.dll` next to the game executable.

### 2 — Copy shader files

Copy **`SuperDepth3D.fx`** (v5.3.8+) and **`3DToElse.fx`** into your `reshade-shaders\Shaders\` folder and enable both techniques in the ReShade overlay.

> **Technique order matters:** `SuperDepth3D` must appear **before** `To_Else` in the technique list.

### 3 — Install the addon

Copy **`SuperVrExport.addon64`** to the same folder as `dxgi.dll`. ReShade automatically loads all `.addon64` files from that directory.

### 4 — Configure 3DToElse

Open the ReShade overlay, select the **To_Else** technique, and set:

| Setting | Value |
|---|---|
| **Stereoscopic Mode Input** | **Frame Sequential** (index 5) |
| **3D Display Mode** | **Side by Side** (index 0) |

> The addon automatically sets SuperDepth3D to Frame Sequential mode — you only need to configure the 3DToElse input side.

### 5 — Launch the game

Start the game. Within 2–3 seconds the addon will configure Frame Sequential mode and write the shared handle to `KatangaMappedFile`.

### 6 — Start KatanaVR / VRScreenCap

Launch the viewer **after** the game has loaded. It reads `KatangaMappedFile` and opens the shared texture automatically.

> If the viewer was already running before the game, restart it after the game loads.

---

## 🚀 Setup — GeoVrExport (Geo3D / native frame-sequential games)

### 1 — Install ReShade

Download [ReShade](https://reshade.me) and install it for your game selecting the **DXGI** API.

### 2 — Copy shader file

Copy **`3DToElse.fx`** into your `reshade-shaders\Shaders\` folder and enable the **To_Else** technique in the ReShade overlay. SuperDepth3D is **not needed**.

### 3 — Install the addon

Copy **`GeoVrExport.addon64`** to the same folder as `dxgi.dll`.

### 4 — Configure 3DToElse

Open the ReShade overlay, select the **To_Else** technique, and set:

| Setting | Value |
|---|---|
| **Stereoscopic Mode Input** | **Frame Sequential** (index 5) |
| **3D Display Mode** | **Side by Side** (index 0) |

### 5 — Launch the game

Start the game. The addon reads texTOT from 3DToElse immediately — no recompile required.

### 6 — Start KatanaVR / VRScreenCap

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
| D3D9 game: black / no connection | Use dgVoodoo2 to translate D3D9 → D3D11. See section above |
| Addon not in ReShade list | Ensure `.addon64` is next to `dxgi.dll`; reinstall ReShade with "Install add-ons" checked |
| `DoubleTex not found` loop | Normal on first launch of SuperVrExport — resolves within 3 seconds |
| Game crashes with SuperVrExport | Confirm the game uses SuperDepth3D. For native Geo3D games use GeoVrExport instead |
| Game crashes with GeoVrExport | Confirm 3DToElse is enabled and Stereoscopic Mode Input is set to Frame Sequential |

### Reading ReShade.log

| Message | Meaning |
|---|---|
| `SuperVrExport: D3D12 ready` | Bridge established — start KatanaVR now |
| `SuperVrExport: D3D11 ready` | Bridge established — start KatanaVR now |
| `GeoVrExport: D3D11 ready` | Bridge established — start KatanaVR now |
| `first copy fired, src=... dst=...` | GPU copy is working |
| `DoubleTex not found — forcing reload` | SuperVrExport startup — normal, triggers one shader recompile |

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

Original addon by **[artumino](https://github.com/artumino/ReshadeVRExport)** · SuperDepth3D by **[BlueSkyDefender](https://github.com/BlueSkyDefender/Depth3D)**

</div>
