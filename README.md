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

* **The Problem:** Even with stereoscopy enabled, certain flat-to-VR ports can still feel visually flat or lack physical depth. While dialing down UEVR's built-in *World Scale* does increase depth perception, it shrinks the actual scale of game geometry—leaving you with an unnatural "dollhouse" or miniature effect.
* **The Solution (3D Depth Boost):** UVOSuit implements true stereoscopic convergence. Inside the engine hook (`on_post_calculate_stereo_view_offset`), the plugin recalculates the local rotation of the left and right camera views toward a focal point (*toe-in angle*). This pulls foreground assets closer to your eyes, dramatizes background depth separation, and establishes authentic spatial presence without altering world scale. *(Note: Aggressive 3D Boost values can make foreground geometry feel slightly oversized. You can dial this right back to a sweet spot by slightly increasing UEVR's World Scale slider).*

---

### ⚠️ Rendering Compatibility

> [!WARNING]
> **Oculus Link Incompatibility (FOV & Shifts)**
> Custom FOV Scaling and Optical Center Shifts are **not supported** when using Oculus Link (Meta Quest Link). The Oculus OpenXR runtime enforces strict hardware FOV limits and utilizes aggressive Asynchronous Timewarp (ATW), which causes heavy visual distortion when attempting to manipulate projection matrices. For full compatibility with optics manipulation, please use **VDXR (Virtual Desktop)** or **Steam Link / SteamVR**.

> [!WARNING]
> **3D Depth Boost REQUIRES Native Stereo**
> Alternative render methods (**Synchronized Sequential**, **AFR**, **AFW**) depend on temporal reprojection, frame warping, and shared depth buffers that explicitly assume parallel optical axes. Because 3D Boost angles the eye views inward, running these alternate modes causes immediate visual jitter and tearing. Always set UEVR to **Native Stereo** when using 3D Depth Boost.

* **FOV Scaling, Offsets & Vignette:** Projection changes, optical center translation, and the API Layer lens mask execute directly within the OpenXR compositor pipeline and work across **all rendering backends** (excluding Oculus Link limitations noted above).
* **Frame Generation & Custom Builds:** Fully compatible with **OFXR Bridge** and **joyehoge's custom UEVR builds**.

---

### ✨ Features

* ⚡ **Zero-Stutter Lock-Free Architecture:** The plugin utilizes a completely lock-free design for its OpenXR rendering path. By leveraging `std::atomic` variables for cache reading, UVOSuit guarantees zero mutex contention or micro-stutters in the headset, maintaining absolute frame pacing even during aggressive UI adjustments.
* 🕶️ **Engine-Level 3D Depth Boost (Convergence):** Fine-tune camera convergence to enhance perceived 3D depth and volume.
  * **Eye-Dominance Balance (Advanced Eye Strain Relief):** Instead of forcing mathematically rigid symmetry, you can dynamically offset the optical convergence to align perfectly with your naturally dominant eye. This drastically reduces eye fatigue during long sessions by allowing your visual system to rest in its natural asymmetric state. The bias slider is strictly clamped to your current 3D Boost limits to ensure safety.
  * **Head-Roll Invariance:** Convergence math (`ApplyConvergenceMath`) runs strictly in local headset space. You can tilt your head sideways at a 90-degree angle without inducing vertical disparity.
  * **Safety Threshold & Smooth Decay:** A safe ceiling of 15% is active by default. For experimentation, you can toggle **Unlock Extreme Limits**. Disabling the toggle instantly caps the target angle, while the rendering engine smoothly eases the convergence back to the 15% baseline to prevent eye strain.
* 📐 **Comprehensive FOV Scaling (OpenXR):** Global scaling along with isolated margin controls for Outer (temples), Inner (nose), Top, and Bottom half-angles directly in `XrFovf`. Narrowing the FOV maps the game's full render resolution into a tighter physical footprint, noticeably increasing perceived PPD (pixels per degree).
* 🌑 **Lens Mask (API Layer Vignette):** An optical stencil rendered on top of the final output via an independent OpenXR API Layer to smooth hard rectangular edges. Features granular control over offsets, corner radius, and softness with zero-latency configuration via Win32 Shared Memory.
  * **Zero Dependencies & Safe Hooking:** The plugin safely registers the `XR_API_LAYER_PATH` environment variable **only** after utilizing strict WinAPI validation (`CreateFileA` with read/write sharing) to confirm file presence, eliminating OpenXR initialization crashes (Error -36).
* 🎯 **Optical Center Shift & Axis Rotation:** Translate projection bounds horizontally and vertically, or apply direct orientation adjustment (`XrPosef.orientation`) via an optimized rotation cache.
* 📊 **Smart Diagnostics:** An integrated UI monitor independently tracks and displays the real-time health of OpenXR Hooks, IPC Shared Memory, and API Layer files.

---

### ⌨️ Hotkeys & Shortcuts

| Hotkey | Action |
| :--- | :--- |
| **`F2`** | Toggle UVOSuit UI window |
| **`F3`** | Toggle custom FOV limits on / off |
| **`End`** | Toggle 3D Depth Boost on / off |
| **`PageUp` / `PageDown`** | Increase / decrease 3D Boost strength |
| **`Del`** | Toggle Eye-Dominance Balance on / off |
| **`-` / `=`** | Decrease / increase Eye-Dominance Bias |

> [!TIP]
> * Hold **`Ctrl`** and **left-click** any numeric slider to type in a value directly via keyboard. **Bulletproof Input Protection:** All manual entries are now strictly clamped to their dynamic minimum and maximum limits in real-time, and mathematically rounded to the nearest thousandth (0.001) to prevent floating-point drift, invalid settings, or engine crashes.
> * Hold **`Shift`** while dragging any slider (or using keyboard shortcuts) to step through values faster.

---

### 💾 Smart Persistence (I/O)

Settings are written to disk utilizing a **thread-safe, asynchronous background process**. The save function only triggers when you release the mouse button (`IsItemDeactivatedAfterEdit`) and incorporates atomic spam-protection. This keeps the engine's render loop entirely free from frame drops and redundant disk writes.

* **Per-Game Config (`UVOSuit_Local.ini`):** Located directly inside the game's dedicated UEVR profile folder. Saves Plugin master toggle, open UI subtrees, and all 3D Depth Boost values.
* **Global Config (`%APPDATA%\UEVR\UVOSuit_Global.ini`):** Stored globally inside UEVR's main AppData directory. Saves all FOV multipliers, optical center shifts, axis rotation angles, and Lens Mask parameters.

---

### 🛠️ Practical Tips & Troubleshooting

* **Applying FOV Modifications:** Tweaking projection boundaries in real time can introduce visual warping or mismatched view bounds. To cleanly reset and force the engine to recalculate its projection matrices, click **Reinitialize Runtime** inside the UEVR dashboard. *Note:* Triggering *Reinitialize Runtime* in certain titles (such as *Atomic Heart* or *Clair Obscur: Expedition 33*) can trigger a crash to desktop (CTD).
* **Visual Artifacts:** Extreme convergence angles or radical projection crops can occasionally conflict with screen-space passes (SSAO, SSR) or engine-level occlusion culling.

---

### 📄 License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

### 🤝 Credits
* **Praydog** for the incredible UEVR framework.
* The UEVR testing and modding community for ongoing feedback and validation.
* **Ybalrid** — for the initial OpenXR API Layer project template structure.
* **lobotomy-x** — for the initial plugin template structure from UEVR-Dev-Utils.
