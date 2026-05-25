<div align="center">

![SuperVrExport Banner](banner.png)

[![ReShade](https://img.shields.io/badge/ReShade-6.3%2B-purple.svg)](https://reshade.me)
[![Platform](https://img.shields.io/badge/Windows-DX9%20%7C%20DX10%20%7C%20DX11%20%7C%20DX12%20%7C%20Vulkan%20%7C%20OpenGL-0078D4.svg)](#)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**A ReShade addon that exports SuperDepth3D's stereoscopic output directly to KatanaVR / VRScreenCap at full resolution with zero intermediate capture.**

</div>

---

## 🙏 Credits

| | |
|---|---|
| **[artumino](https://github.com/artumino/ReshadeVRExport)** | Original ReshadeVRExport — architecture, shared-texture pipeline, and Geo3D support that this project is built on |
| **[BlueSkyDefender](https://github.com/BlueSkyDefender/Depth3D)** | Author of SuperDepth3D and the Frame Alternation / DoubleBuffer additions that make the direct export pipeline possible |

---

## 💡 What It Does

SuperVrExport hooks into ReShade's effect pipeline and shares the stereo 3D frame with KatanaVR or VRScreenCap every frame via a cross-process DXGI shared texture — no screen capture, no resolution loss, no latency beyond one frame.

The addon **automatically configures SuperDepth3D** on startup: it sets the required preprocessors, detects whether you are using a regular or VR-preprocessor build, and sets the correct output mode. You do not need to change any shader settings manually.

---

## 🎮 How It Works — SuperDepth3D Pipeline

SuperDepth3D does **not** output SBS natively in a form KatanaVR can read in a single frame. The addon uses a **Frame Sequential → 3DToElse → KatanaVR** pipeline:

```
SuperDepth3D (Frame Sequential mode)
        │  alternating L/R full-frame output
        ▼
3DToElse.fx (Frame Sequential input)
        │  reconstructs current + previous frame into full SBS (texTOT)
        ▼
SuperVrExport addon
        │  copies texTOT to a shared GPU texture each frame
        ▼
KatangaMappedFile  ←  KatanaVR / VRScreenCap reads this handle
        │
        ▼
 KatanaVR  →  VR headset (full resolution SBS)
```

### What the addon sets automatically

| Preprocessor / Uniform | Value | Why |
|---|---|---|
| `EX_DLP_FS_Mode` | `1` | Adds Frame Sequential (mode 6) to SuperDepth3D's Stereoscopic Mode dropdown |
| `DoubleBuffer_Mode` | `1` | Creates the DoubleTex buffer (used as fallback for VR-mode builds) |
| `Stereoscopic_Mode` | `6` (Frame Sequential) | Set automatically after compile for regular builds |
| `FS_FA` | `true` | Enables addon-driven frame alternation |

For **VR preprocessor builds** (`Virtual_Reality_Mode=1`), the addon detects this via the absence of the `FS_FA` uniform and instead sets `Stereoscopic_Mode=0` (SBS), reading from `DoubleTex` directly — no 3DToElse needed.

---

## 📦 Requirements

| Component | Version / Notes |
|---|---|
| **ReShade** | 6.3.x or newer, installed in **DXGI proxy mode** (`dxgi.dll`) for D3D12 games |
| **SuperDepth3D.fx** | v5.3.8 or newer — must include `EX_DLP_FS_Mode` and `DoubleBuffer_Mode` preprocessors |
| **3DToElse.fx** | Required for regular (non-VR) builds. Included in the `Effects/` folder |
| **KatanaVR / VRScreenCap** | 0.4.0-dev5 or newer. Must be started **after** the game |
| **OS** | Windows 10 2004+ or Windows 11 |

---

## 🚀 Setup — Regular SuperDepth3D Build

### 1 — Install ReShade

Download [ReShade](https://reshade.me) and install it for your game selecting the **DXGI** API (for D3D12 games). This places `dxgi.dll` next to the game executable.

### 2 — Copy shader files

Copy **`SuperDepth3D.fx`** (v5.3.8+) and **`3DToElse.fx`** into your `reshade-shaders\Shaders\` folder and enable both techniques in the ReShade overlay.

> **Technique order matters:** `SuperDepth3D` must appear **before** `To_Else` in the technique list so 3DToElse receives the frame sequential output.

### 3 — Install the addon

Copy **`SuperVrExport.addon64`** to the same folder as `dxgi.dll` (the game executable folder). ReShade automatically loads all `.addon64` files from that directory.

### 4 — Configure 3DToElse

Open the ReShade overlay, select the **To_Else** technique, and set:

| Setting | Value |
|---|---|
| **Stereoscopic Mode Input** | **Frame Sequential** (index 5) |
| **3D Display Mode** | **Side by Side** (index 0) |

> The addon automatically sets SuperDepth3D to Frame Sequential mode — you only need to set the 3DToElse input side.

### 5 — Launch the game

Start the game. Within 2–3 seconds the addon will:
- Set `EX_DLP_FS_Mode=1` and `DoubleBuffer_Mode=1` (triggers a one-time shader recompile)
- Detect the build type and set `Stereoscopic_Mode=6` + `FS_FA=true`
- Write the shared handle to `KatangaMappedFile`
- Log `SuperVrExport: D3D12 ready` (or `D3D11 ready`) to `ReShade.log`

### 6 — Start KatanaVR / VRScreenCap

Launch the viewer **after** the game has loaded. It reads `KatangaMappedFile` and opens the shared texture automatically. The full-resolution SBS image appears in the headset.

> If the viewer was already running before the game, restart it after the game loads.

---

## 🥽 Setup — VR Preprocessor Build (Virtual_Reality_Mode=1)

If you have compiled SuperDepth3D with `Virtual_Reality_Mode=1`:

1. 3DToElse is **not needed**
2. The addon detects the VR build automatically (FS_FA uniform is absent)
3. It sets `Stereoscopic_Mode=0` (Side by Side) and reads from `DoubleTex` directly
4. Follow steps 3, 5, and 6 above — skip the 3DToElse configuration

---

## 🌐 Geo3D / Frame Sequential Games

For games that natively output frame sequential stereo (e.g. Geo-3D titles):

1. Enable **3DToElse.fx** in ReShade
2. Set **Stereoscopic Mode Input = Frame Sequential**
3. The addon reads `texTOT` from 3DToElse — no SuperDepth3D required
4. Start KatanaVR after the game

---

## 🔧 Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| KatanaVR shows nothing | Viewer started before game/addon | Restart KatanaVR after `D3D12 ready` appears in `ReShade.log` |
| Black screen in headset | Copy not flushing / stale handle | Restart KatanaVR; confirm `first copy fired` in `ReShade.log` |
| Wrong colours / rainbow | Format mismatch (handled automatically in latest build) | Update to latest `SuperVrExport.addon64` |
| Addon not in ReShade list | Wrong folder or ReShade without addon support | Ensure `.addon64` is next to `dxgi.dll`; reinstall ReShade with "Install add-ons" checked |
| `DoubleTex not found` loop | Shader compiling without `DoubleBuffer_Mode=1` | Normal on first launch — resolves within 3 seconds |
| Virtual controller freeze | ReShade runtime recreate on HID insert | Wait 2 seconds — reload guard prevents infinite loop |
| D3D12 shared handle failed | RE Engine / similar blocks `D3D12_HEAP_FLAG_SHARED` | Handled automatically via D3D11 reversed bridge in latest build |

### Reading ReShade.log

All addon messages appear in `ReShade.log` (next to the game executable):

| Message | Meaning |
|---|---|
| `D3D12 ready (D3D11 bridge, ...)` | Bridge established — start KatanaVR now |
| `first copy fired, src=... dst=...` | GPU copy is working |
| `VR buffer not found` | texTOT / DoubleTex temporarily unavailable during reload — resolves automatically |
| `DoubleTex not found — forcing reload` | Normal on startup, triggers one shader recompile |
| `AcquireSync timed out` | Mutex wait exceeded 100ms — usually transient |

---

## 🏗️ Building

Requires Visual Studio 2019 or 2022 with the C++ Desktop workload.

```bat
build.bat
```

Downloads ReShade headers automatically and produces four binaries:

```
bin\x86_api14\SuperVrExport.addon    ← x86, ReShade 6.3.x (API 14)
bin\x64_api14\SuperVrExport.addon64  ← x64, ReShade 6.3.x (API 14)
bin\x86_api16\SuperVrExport.addon    ← x86, ReShade 6.4+  (API 16)
bin\x64_api16\SuperVrExport.addon64  ← x64, ReShade 6.4+  (API 16)
```

Optional: set `VULKAN_SDK` environment variable before building to enable the Vulkan interop path.

---

<div align="center">

Original addon by **[artumino](https://github.com/artumino/ReshadeVRExport)** · SuperDepth3D by **[BlueSkyDefender](https://github.com/BlueSkyDefender/Depth3D)**

</div>
