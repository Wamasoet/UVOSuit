<div align="center">

# 🚀 Ultimate VR Optics Suite (UVOSuit) for UEVR

**Advanced stereoscopic convergence, asymmetric FOV scaling, independent optical axis control, and customizable lens masking for Unreal Engine VR.**

[![C++](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg?style=for-the-badge&logo=c%2B%2B)](https://isocpp.org/)
[![OpenXR](https://img.shields.io/badge/OpenXR-Hook-orange.svg?style=for-the-badge&logo=khronos)](https://www.khronos.org/openxr/)
[![UEVR Plugin](https://img.shields.io/badge/UEVR-Plugin-8A2BE2.svg?style=for-the-badge)](https://uevr.io/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge)](LICENSE)

</div>

---

**Ultimate VR Optics Suite (UVOSuit)** is a high-performance C++ plugin for UEVR built for users looking for deep, granular calibration over optics and stereoscopic depth.

Rather than taking a one-size-fits-all approach, the plugin operates across two distinct rendering layers: it hooks into Unreal Engine's stereo view calculation (`on_post_calculate_stereo_view_offset`) to deliver real, physical camera convergence, while intercepting the OpenXR runtime pipeline (`xrLocateViews`) to grant low-level control over projection boundaries (asymmetric FOV, optical center shifts, and axis rotation). In addition, it communicates with an external OpenXR API Layer via shared memory to render a real-time comfort vignette.

---

### 🔍 Problem & Solution

* **The Problem:**  
  Even with stereoscopy enabled, certain flat-to-VR ports can still feel visually flat or lack physical depth. While dialing down UEVR's built-in *World Scale* does increase depth perception, it shrinks the actual scale of game geometry—leaving you with an unnatural "dollhouse" or miniature effect.
* **The Solution (3D Depth Boost):**  
  UVOSuit implements true stereoscopic convergence. Inside the engine hook (`on_post_calculate_stereo_view_offset`), the plugin recalculates the local rotation of the left and right camera views toward a focal point (*toe-in angle*). This pulls foreground assets closer to your eyes, dramatizes background depth separation, and establishes authentic spatial presence without altering world scale.  
  *(Note: Aggressive 3D Boost values can make foreground geometry feel slightly oversized. You can dial this right back to a sweet spot by slightly increasing UEVR's World Scale slider).*

---

### ⚠️ Rendering Compatibility

> [!WARNING]
> **Oculus Link Incompatibility (FOV & Shifts)**  
> Custom FOV Scaling and Optical Center Shifts are **not supported** when using Oculus Link (Meta Quest Link). The Oculus OpenXR runtime enforces strict hardware FOV limits and utilizes aggressive Asynchronous Timewarp (ATW), which causes heavy visual distortion when attempting to manipulate projection matrices. For full compatibility with optics manipulation, please use **VDXR (Virtual Desktop)** or **Steam Link / SteamVR**.

> [!WARNING]
> **3D Depth Boost REQUIRES Native Stereo**  
> Alternative render methods (**Synchronized Sequential**, **AFR**, **AFW**) depend on temporal reprojection, frame warping, and shared depth buffers that explicitly assume parallel optical axes. Because 3D Boost angles the eye views inward, running these alternate modes causes immediate visual jitter and tearing. Always set UEVR to **Native Stereo** when using 3D Depth Boost.

* **FOV Scaling, Offsets & Vignette:** Projection changes, optical center translation, and the API Layer lens mask execute directly within the OpenXR compositor pipeline and work across **all rendering backends** (excluding Oculus Link limitations noted above).
* **Frame Generation & Custom Builds:**
  * Fully compatible with **OFXR Bridge**.
  * Fully compatible with **joyehoge's custom UEVR builds**.

---

### ✨ Features

* 🕶️ **Engine-Level 3D Depth Boost (Convergence):**
  * Fine-tune camera convergence to enhance perceived 3D depth and volume.
  * **Head-Roll Invariance:** Convergence math (`ApplyConvergenceMath`) runs strictly in local headset space. You can tilt your head sideways at a 90-degree angle without inducing vertical disparity.
  * **Safety Threshold & Smooth Decay:** A safe ceiling of 15% is active by default. For experimentation, you can toggle **Unlock Extreme Limits**. Disabling the toggle smoothly eases the convergence angle back down to the 15% baseline to prevent eye strain or painful ocular pressure.
* 📐 **Comprehensive FOV Scaling (OpenXR):**
  * Global scaling along with isolated margin controls for Outer (temples), Inner (nose), Top, and Bottom half-angles directly in `XrFovf`.
  * *PPD Boost Note:* Narrowing the FOV forces the runtime to map the game's full render resolution into a tighter physical footprint, noticeably increasing perceived PPD (pixels per degree) and image clarity in your focal area. Unrendered regions outside the modified frustum remain black borders.
* 🌑 **Lens Mask (API Layer Vignette):**
  * An optical stencil rendered on top of the final output via an independent OpenXR API Layer. It masks and smooths out the hard rectangular edges produced by aggressive FOV truncations.
  * Granular control over edge offsets (Outer, Inner, Top, Bottom), corner radius rounding, and feathering/edge softness.
  * Zero-latency configuration: updates are pushed from the plugin UI to the API Layer instantly over Win32 Shared Memory (`CreateFileMappingA` / `MapViewOfFile`).
  * **Zero Dependencies & Safe Hooking:** HLSL shaders are precompiled into bytecode, requiring zero additional DirectX redistributables from the user. The plugin also safely appends to the `XR_API_LAYER_PATH` environment variable without overwriting other active OpenXR layers (like eye tracking or VDXR).
* 🎯 **Optical Center Shift:** Translate projection bounds horizontally and vertically.
* 🔄 **Optical Axis Rotation:** Direct orientation adjustment (`XrPosef.orientation`) by multiplying the view quaternion with an optimized rotation cache (independent Pitch and Yaw per eye).
* 📊 **Smart Diagnostics:** An integrated status monitor right in the ImGui window. It tracks whether `xrLocateViews` is actively firing, validates the IPC shared memory buffer, and prints color-coded error codes if something fails.

---

### ⌨️ Hotkeys & Shortcuts

| Hotkey | Action |
| :--- | :--- |
| **`F2`** | Toggle UVOSuit UI window |
| **`F3`** | Toggle custom FOV limits on / off |
| **`End`** | Toggle 3D Depth Boost on / off |
| **`PageUp` / `PageDown`** | Increase / decrease 3D Boost strength |

> [!TIP]
> * Hold **`Ctrl`** and **left-click** any numeric slider to type in a value directly via keyboard.
> * Hold **`Shift`** while dragging any slider to step through values faster.

---

### 💾 Smart Persistence (I/O)

Settings only flush to disk when you release the mouse button (`IsItemDeactivatedAfterEdit`), keeping the render loop free from frame drops and redundant disk writes. Configurations are split logically:

* **Per-Game Config (`UVOSuit_Local.ini`):**
  * **Path:** Located directly inside the game's dedicated UEVR profile folder (`get_persistent_dir`).
  * **Saved State:** Plugin master toggle, open UI subtrees, and all 3D Depth Boost values.
* **Global Config (`%APPDATA%\UEVR\UVOSuit_Global.ini`):**
  * **Path:** Stored globally inside UEVR's main AppData directory.
  * **Saved State:** All FOV multipliers, optical center shifts, axis rotation angles, and Lens Mask parameters.

---

### 🛠️ Practical Tips & Troubleshooting

* **Applying FOV Modifications:**  
  Tweaking projection boundaries in real time can introduce visual warping or mismatched view bounds. To cleanly reset and force the engine to recalculate its projection matrices, click **Reinitialize Runtime** inside the UEVR dashboard.  
  *Note:* Triggering *Reinitialize Runtime* in certain titles (such as *Atomic Heart* or *Clair Obscur: Expedition 33*) can trigger a crash to desktop (CTD). It is best practice to dial in your baseline FOV from a pause menu or stable scene.
* **Visual Artifacts:**  
  Extreme convergence angles or radical projection crops can occasionally conflict with screen-space passes (SSAO, SSR) or engine-level occlusion culling. Throughout testing, properly calibrated values ran clean without game-breaking pipeline bugs.

---

### 📄 License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

### 🤝 Credits
* **Praydog** for the incredible [UEVR](https://github.com/praydog/UEVR) framework.
* The UEVR testing and modding community for ongoing feedback and validation.
* **Ybalrid** — for the initial OpenXR API Layer project template structure.
* **lobotomy-x** — for the initial plugin template structure from UEVR-Dev-Utils.
