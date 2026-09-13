<div align="center">

# 🚀 Ultimate VR Optics Suite (UVOSuit) for UEVR

**Advanced stereoscopic convergence, asymmetric FOV scaling, and independent optical axis control for Unreal Engine VR.**

[![C++](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg?style=for-the-badge&logo=c%2B%2B)](https://isocpp.org/)
[![OpenXR](https://img.shields.io/badge/OpenXR-Hook-orange.svg?style=for-the-badge&logo=khronos)](https://www.khronos.org/openxr/)
[![UEVR Plugin](https://img.shields.io/badge/UEVR-Plugin-8A2BE2.svg?style=for-the-badge)](https://uevr.io/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge)](LICENSE)

</div>

---

**Ultimate VR Optics Suite (UVOSuit)** is a high-performance C++ plugin for UEVR tailored for users who require deeper optical and stereoscopic adjustments than stock profiles provide.

By hooking directly into the OpenXR pipeline (`xrLocateViews`), UVOSuit gives you complete real-time authority over stereoscopic 3D depth, independent asymmetric FOV limits, and optical center / rotation alignment for each eye individually.

---

### 🔍 Problem & Solution

* **The Problem:**  
  While UEVR is incredible, certain games can still appear visually flat or lack physical presence. Adjusting UEVR's built-in *World Scale* increases depth perception, but at the cost of shrinking the game world geometry—resulting in an unnatural "miniature" or "dollhouse" effect.
* **The Solution (3D Depth Boost):**  
  UVOSuit introduces true stereoscopic convergence control. The plugin dynamically computes and tilts the camera stereo-pair locally (*toe-in angle*). This brings foreground objects forward, dramatizes background depth, and delivers an authentic sense of spatial volume without warping overall world scale.  
  *(Note: At higher 3D Boost values, foreground geometry may appear slightly enlarged. You can easily balance this by nudging World Scale up in UEVR to achieve your ideal visual sweet spot).*

---

### ⚠️ Rendering Compatibility

> [!WARNING]
> **Native Stereo is REQUIRED for 3D Depth Boost**  
> Alternative rendering methods (**Synchronized Sequential**, **AFR**, **AFW**) rely on temporal reprojection, frame-warping, and shared temporal buffers engineered strictly for parallel optical axes. Because 3D Boost dynamically alters camera toe-in geometry, using these alternative modes will result in severe jitter and visual tearing. Always ensure **Native Stereo** is selected in UEVR.

* **FOV Scaling:** Field-of-view modification and optical axis offsets work seamlessly across **all rendering backends**.
* **Frame Generation & Engine Builds:**
  * Fully compatible with **OFXR Bridge**.
  * Fully compatible with **joyehoge's UEVR builds**.

---

### ✨ Core Features

* 🕶️ **3D Depth Boost (Convergence):**
  * Fine-grained control over camera convergence.
  * **Head-Roll Safety (Local Rotation):** All calculations are locked strictly to headset-local coordinates. You can tilt your head 90 degrees without vertical disparity or visual strain.
  * **Safety Limits & Smooth Recovery:** A safe cap of 15% is enforced by default. Enthusiasts can check **Unlock Extreme Limits**. Unchecking the box smoothly interpolates convergence back to the safe 15% threshold to avoid eye fatigue.
* 📐 **Comprehensive FOV Scaling:**
  * Global scaling alongside independent adjustment for Outer (temple), Inner (nose), Top, and Bottom margins per eye.
  * *Pixel Density Note:* Decreasing FOV renders the same native resolution into a tighter projection footprint, significantly boosting perceived PPD (pixels per degree) and sharpness. The trade-off is unrendered black borders at the periphery, as no rasterization occurs outside the restricted projection matrix.
* 🎯 **Optical Center Shift:** Translate the projection center horizontally and vertically to compensate for facial interface fit and individual IPD variance.
* 🔄 **Optical Axis Rotation:** Precise Pitch and Yaw angular offsets for left and right eyes independently.

---

### ⌨️ Hotkeys & Controls

| Shortcut | Description |
| :--- | :--- |
| **`F2`** | Toggle UVOSuit interface |
| **`F3`** | Toggle custom FOV limits on / off |
| **`End`** | Toggle 3D Depth Boost on / off |
| **`PageUp` / `PageDown`** | Increase / decrease 3D Boost intensity on the fly |

> [!TIP]
> * Hold **`Ctrl`** and **click** on any numeric value to type a precise number directly.
> * Hold **`Shift`** while adjusting any slider to accelerate the adjustment speed.

---

### 💾 Smart I/O Persistence

Settings are written to disk only when you release a slider, preventing unnecessary disk I/O during tuning. Configurations are split logically:

* **Game Profile (`UVOSuit_Local.ini`):**
  * **Location:** Stored inside the game's profile folder created by UEVR.
  * **Saved Data:** Plugin toggle state, open GUI tabs, and all 3D Depth Boost parameters.
  * **Purpose:** Convergence requirements differ significantly across game engines and visual art styles.
* **Universal Profile (`%APPDATA%\UnrealVRMod\UEVR\UVOSuit_Global.ini`):**
  * **Location:** Saved globally in UEVR's main configuration folder.
  * **Saved Data:** All FOV Scaling factors, axis shifts, and rotation values.
  * **Purpose:** Asymmetric FOV and optical alignment are properties of your specific VR headset and facial interface, not the game. Calibrate your optics once and use them everywhere.

---

### 🛠️ Practical Tips & Troubleshooting

* **Applying FOV Adjustments:**  
  Live changes to projection bounds may cause temporary distortion while dragging sliders. To cleanly refresh the projection matrix, click **Reinitialize Runtime** in the UEVR dashboard (or trigger an in-game camera shift / cutscene transition, e.g., in *Little Nightmares II*).  
  *Note:* Frequently pressing *Reinitialize Runtime* can cause crashes in heavier titles. It is recommended to dial in base FOV values in stable game areas.
* **Visual Artifacts:**  
  Extremely high convergence angles or radical FOV values can theoretically interact with screen-space shadows or object occlusion culling. During testing across titles like *Atomic Heart*, *Lies of P*, *Little Nightmares II*, and *Stray*, no critical pipeline anomalies were observed under normal calibrated use.

---

### 🗺️ Roadmap (WIP)

- [ ] Status panel diagnostics: automated OpenXR initialization error reporting and context logs.
- [ ] Customizable Comfort Vignette to visually blend outer projection boundaries when using aggressive FOV narrowing.

---

### 📄 License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

### 🤝 Credits
* **Praydog** for [UEVR](https://github.com/praydog/UEVR).
* The UEVR testing community for ongoing feedback and insights.
