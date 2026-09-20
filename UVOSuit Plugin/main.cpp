// Copyright (c) 2026 [Wamasoet]
// Ultimate VR Optics Suite (UVOSuit) - Core Plugin

#define NOMINMAX
#include <Windows.h>
#include <string>
#include <mutex>
#include <memory>
#include <algorithm> 
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sddl.h> 
#include <atomic> 
#include <thread>

#include "renderlib/imgui/imgui.h"
#include "renderlib/uevr_imgui/imgui_impl_dx11.h"
#include "renderlib/uevr_imgui/imgui_impl_dx12.h"
#include "renderlib/uevr_imgui/imgui_impl_win32.h"
#include "renderlib/rendering/d3d11.hpp"
#include "renderlib/rendering/d3d12.hpp"
#include "uevr/API.h"
#include "uevr/Plugin.hpp"
#include <detours.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
using namespace uevr;

// =========================================================================
// CONSTANTS & OPENXR TYPE DEFINITIONS
// =========================================================================
constexpr float PI_F = 3.14159265358979323846f;
constexpr double PI_D = 3.14159265358979323846;
constexpr float DEG_TO_RAD_F = PI_F / 180.0f;
constexpr double DEG_TO_RAD_D = PI_D / 180.0;

typedef uint64_t MyXrInstance;
typedef int32_t MyXrResult;
typedef uint64_t MyXrSession;
typedef uint64_t MyXrSpace;
typedef int64_t MyXrTime;

typedef void (*PFN_xrVoidFunction)(void);
typedef MyXrResult(*PFN_xrGetInstanceProcAddr)(MyXrInstance instance, const char* name, PFN_xrVoidFunction* function);

struct MyXrFovf { float angleLeft; float angleRight; float angleUp; float angleDown; };
struct MyXrQuaternionf { float x, y, z, w; };
struct MyXrVector3f { float x, y, z; };
struct MyXrPosef { MyXrQuaternionf orientation; MyXrVector3f position; };
struct MyXrView { int32_t type; void* next; MyXrPosef pose; MyXrFovf fov; };
struct MyXrViewLocateInfo { int32_t type; const void* next; int32_t viewConfigurationType; MyXrTime displayTime; MyXrSpace space; };
struct MyXrViewState { int32_t type; void* next; uint64_t viewStateFlags; };

struct MyXrNegotiateLoaderInfo { uint32_t structType; uint32_t structVersion; size_t structSize; uint32_t minInterfaceVersion; uint32_t maxInterfaceVersion; uint64_t minApiVersion; uint64_t maxApiVersion; };
struct MyXrNegotiateRuntimeRequest { uint32_t structType; uint32_t structVersion; size_t structSize; uint32_t runtimeInterfaceVersion; uint64_t runtimeApiVersion; PFN_xrGetInstanceProcAddr getInstanceProcAddr; };

typedef MyXrResult(*PFN_xrNegotiateLoaderRuntimeInterface)(const MyXrNegotiateLoaderInfo*, MyXrNegotiateRuntimeRequest*);
typedef MyXrResult(*PFN_xrLocateViews)(MyXrSession session, const MyXrViewLocateInfo* viewLocateInfo, MyXrViewState* viewState, uint32_t viewCapacityInput, uint32_t* viewCountOutput, MyXrView* views);
typedef FARPROC(WINAPI* PFN_GetProcAddress)(HMODULE, LPCSTR);

// =========================================================================
// MATH OPERATIONS 
// =========================================================================

static MyXrQuaternionf MultiplyQuat(const MyXrQuaternionf& q1, const MyXrQuaternionf& q2) noexcept {
    return {
        q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y,
        q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x,
        q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w,
        q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z
    };
}

static MyXrQuaternionf EulerToQuat(float pitch, float yaw, float roll = 0.0f) noexcept {
    const float cy = std::cos(yaw * 0.5f);
    const float sy = std::sin(yaw * 0.5f);
    const float cp = std::cos(pitch * 0.5f);
    const float sp = std::sin(pitch * 0.5f);
    const float cr = std::cos(roll * 0.5f);
    const float sr = std::sin(roll * 0.5f);

    return {
        cy * sp * cr + sy * cp * sr,
        sy * cp * cr - cy * sp * sr,
        cy * cp * sr - sy * sp * cr,
        cy * cp * cr - sy * sp * sr
    };
}

// =========================================================================
// NAMESPACE: STATE MANAGEMENT & CACHING
// =========================================================================
namespace UVOSuit {

    std::mutex g_FileIOMutex;

    struct VignetteConfig {
        bool enabled;
        float edge_outer;
        float edge_inner;
        float edge_top;
        float edge_bottom;
        float softness;
        float corner_radius;
    };

    HANDLE g_hMapFile_Vignette = nullptr;
    VignetteConfig* g_vignetteConfig = nullptr;

    struct UVOSettings {
        std::atomic<bool> mod_enabled = false;
        std::atomic<bool> enable_custom_fov = false;

        bool enable_3d_boost = false;
        float convergence_angle = 0.0f;
        std::atomic<float> actual_convergence_angle = 0.0f;

        float eye_dominance_shift = 0.0f;
        std::atomic<float> actual_dominance_shift = 0.0f;
        bool enable_dominance_shift = false;

        bool unlock_limits = false;

        float global_fov_scale = 1.0f;
        float outer_fov_scale_left = 1.0f, outer_fov_scale_right = 1.0f;
        float inner_fov_scale_left = 1.0f, inner_fov_scale_right = 1.0f;
        float upper_fov_scale_left = 1.0f, upper_fov_scale_right = 1.0f;
        float lower_fov_scale_left = 1.0f, lower_fov_scale_right = 1.0f;

        float flat_box_vert = 0.0f, flat_box_horiz = 0.0f;
        float flat_left_vert = 0.0f, flat_left_horiz = 0.0f;
        float flat_right_vert = 0.0f, flat_right_horiz = 0.0f;

        float rot_global_pitch = 0.0f, rot_global_yaw = 0.0f;
        float rot_left_pitch = 0.0f, rot_left_yaw = 0.0f;
        float rot_right_pitch = 0.0f, rot_right_yaw = 0.0f;

        bool ui_fov_opened = true;
        bool ui_shift_opened = false;
        bool ui_asym_opened = false;
        bool ui_shifts_opened = false;
        bool ui_rot_opened = false;

        bool enable_lens_mask = false;
        bool vignette_layer_found = false;
        float mask_edge_outer = 0.0f;
        float mask_edge_inner = 0.0f;
        float mask_edge_top = 0.0f;
        float mask_edge_bottom = 0.0f;
        float mask_softness = 0.05f;
        float mask_corner_radius = 0.2f;
        bool ui_mask_opened = false;
    };

    class OptimizationCache {
    public:
        MyXrQuaternionf global_rot = { 0.0f, 0.0f, 0.0f, 1.0f };
        MyXrQuaternionf left_rot = { 0.0f, 0.0f, 0.0f, 1.0f };
        MyXrQuaternionf right_rot = { 0.0f, 0.0f, 0.0f, 1.0f };

        float fov_scale = 1.0f;
        float outer_l = 1.0f, inner_l = 1.0f, up_l = 1.0f, down_l = 1.0f;
        float outer_r = 1.0f, inner_r = 1.0f, up_r = 1.0f, down_r = 1.0f;
        float flat_left_x = 0.0f, flat_left_y = 0.0f;
        float flat_right_x = 0.0f, flat_right_y = 0.0f;

        void Update(const UVOSettings& config) noexcept {
            global_rot = EulerToQuat(config.rot_global_pitch * DEG_TO_RAD_F, config.rot_global_yaw * DEG_TO_RAD_F);
            left_rot = EulerToQuat(config.rot_left_pitch * DEG_TO_RAD_F, config.rot_left_yaw * DEG_TO_RAD_F);
            right_rot = EulerToQuat(config.rot_right_pitch * DEG_TO_RAD_F, config.rot_right_yaw * DEG_TO_RAD_F);

            fov_scale = config.global_fov_scale;
            outer_l = std::max(0.01f, config.outer_fov_scale_left);
            inner_l = std::max(0.01f, config.inner_fov_scale_left);
            up_l = std::max(0.01f, config.upper_fov_scale_left);
            down_l = std::max(0.01f, config.lower_fov_scale_left);

            inner_r = std::max(0.01f, config.inner_fov_scale_right);
            outer_r = std::max(0.01f, config.outer_fov_scale_right);
            up_r = std::max(0.01f, config.upper_fov_scale_right);
            down_r = std::max(0.01f, config.lower_fov_scale_right);

            flat_left_x = (config.flat_box_horiz + config.flat_left_horiz) * DEG_TO_RAD_F;
            flat_left_y = (config.flat_box_vert + config.flat_left_vert) * DEG_TO_RAD_F;
            flat_right_x = (config.flat_box_horiz + config.flat_right_horiz) * DEG_TO_RAD_F;
            flat_right_y = (config.flat_box_vert + config.flat_right_vert) * DEG_TO_RAD_F;
        }
    };

    UVOSettings g_Config;
    OptimizationCache g_Cache;

    PFN_GetProcAddress g_GetProcAddress_original = nullptr;
    PFN_xrNegotiateLoaderRuntimeInterface g_real_negotiate = nullptr;
    PFN_xrGetInstanceProcAddr g_real_get_instance_proc_addr = nullptr;
    PFN_xrLocateViews g_xrLocateViews_original = nullptr;

    enum class DiagLevel { OK, WARNING, CRITICAL_ERR };

    struct DiagnosticState {
        DiagLevel hook_status = DiagLevel::WARNING;
        std::string hook_msg = "Waiting for OpenXR hook...";
        DiagLevel ipc_status = DiagLevel::WARNING;
        std::string ipc_msg = "Waiting for IPC init...";
        DiagLevel layer_status = DiagLevel::WARNING;
        std::string layer_msg = "Waiting for Layer check...";
    };

    DiagnosticState g_Diag;
}

// =========================================================================
// OPENXR HOOKS
// =========================================================================

static MyXrResult Hook_xrLocateViews(MyXrSession session, const MyXrViewLocateInfo* viewLocateInfo, MyXrViewState* viewState, uint32_t viewCapacityInput, uint32_t* viewCountOutput, MyXrView* views) {
    MyXrResult result = UVOSuit::g_xrLocateViews_original(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);

    if (result == 0 && views != nullptr && viewCountOutput != nullptr && *viewCountOutput >= 2 && viewCapacityInput >= 2) {
        if (UVOSuit::g_Config.mod_enabled.load(std::memory_order_relaxed) &&
            UVOSuit::g_Config.enable_custom_fov.load(std::memory_order_relaxed)) {

            const float scale = UVOSuit::g_Cache.fov_scale;
            views[0].fov.angleLeft *= (scale * UVOSuit::g_Cache.outer_l);
            views[0].fov.angleRight *= (scale * UVOSuit::g_Cache.inner_l);
            views[0].fov.angleUp *= (scale * UVOSuit::g_Cache.up_l);
            views[0].fov.angleDown *= (scale * UVOSuit::g_Cache.down_l);

            views[1].fov.angleLeft *= (scale * UVOSuit::g_Cache.inner_r);
            views[1].fov.angleRight *= (scale * UVOSuit::g_Cache.outer_r);
            views[1].fov.angleUp *= (scale * UVOSuit::g_Cache.up_r);
            views[1].fov.angleDown *= (scale * UVOSuit::g_Cache.down_r);

            views[0].fov.angleLeft += UVOSuit::g_Cache.flat_left_x; views[0].fov.angleRight += UVOSuit::g_Cache.flat_left_x;
            views[0].fov.angleUp += UVOSuit::g_Cache.flat_left_y; views[0].fov.angleDown += UVOSuit::g_Cache.flat_left_y;

            views[1].fov.angleLeft += UVOSuit::g_Cache.flat_right_x; views[1].fov.angleRight += UVOSuit::g_Cache.flat_right_x;
            views[1].fov.angleUp += UVOSuit::g_Cache.flat_right_y; views[1].fov.angleDown += UVOSuit::g_Cache.flat_right_y;

            views[0].pose.orientation = MultiplyQuat(views[0].pose.orientation, MultiplyQuat(UVOSuit::g_Cache.global_rot, UVOSuit::g_Cache.left_rot));
            views[1].pose.orientation = MultiplyQuat(views[1].pose.orientation, MultiplyQuat(UVOSuit::g_Cache.global_rot, UVOSuit::g_Cache.right_rot));
        }
    }
    return result;
}

static MyXrResult Hook_xrGetInstanceProcAddr(MyXrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    MyXrResult res = UVOSuit::g_real_get_instance_proc_addr(instance, name, function);
    if (res == 0 && function && *function && name && strcmp(name, "xrLocateViews") == 0) {
        UVOSuit::g_xrLocateViews_original = (PFN_xrLocateViews)*function;
        *function = (PFN_xrVoidFunction)Hook_xrLocateViews;

        if (UVOSuit::g_Diag.hook_status != UVOSuit::DiagLevel::CRITICAL_ERR) {
            UVOSuit::g_Diag.hook_status = UVOSuit::DiagLevel::OK;
            UVOSuit::g_Diag.hook_msg = "OpenXR Pipeline Hooked.";
        }
    }
    return res;
}

static MyXrResult Hook_xrNegotiateLoaderRuntimeInterface(const MyXrNegotiateLoaderInfo* loaderInfo, MyXrNegotiateRuntimeRequest* runtimeRequest) {
    MyXrResult res = UVOSuit::g_real_negotiate(loaderInfo, runtimeRequest);
    if (res == 0 && runtimeRequest->getInstanceProcAddr) {
        UVOSuit::g_real_get_instance_proc_addr = runtimeRequest->getInstanceProcAddr;
        runtimeRequest->getInstanceProcAddr = Hook_xrGetInstanceProcAddr;
    }
    return res;
}

static FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    FARPROC res = UVOSuit::g_GetProcAddress_original(hModule, lpProcName);

    if (res && lpProcName && reinterpret_cast<uintptr_t>(lpProcName) > 0xFFFF) {
        if (strcmp(lpProcName, "xrNegotiateLoaderRuntimeInterface") == 0) {
            UVOSuit::g_real_negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)res;
            return (FARPROC)Hook_xrNegotiateLoaderRuntimeInterface;
        }
        if (strcmp(lpProcName, "xrGetInstanceProcAddr") == 0) {
            UVOSuit::g_real_get_instance_proc_addr = (PFN_xrGetInstanceProcAddr)res;
            return (FARPROC)Hook_xrGetInstanceProcAddr;
        }
    }
    return res;
}

// =========================================================================
// CONFIGURATION I/O PATHS & PARSERS
// =========================================================================

static std::string GetGlobalIniPath() {
    if (const char* appdata = std::getenv("APPDATA")) {
        return std::string(appdata) + "\\UEVR\\UVOSuit_Global.ini";
    }
    return "UVOSuit_Global.ini";
}

static std::string GetLocalIniPath() {
    return API::get()->get_persistent_dir(L"UVOSuit_Local.ini").string();
}

namespace ConfigHelper {
    static void LoadFloat(const std::string& path, const char* section, const char* key, float& out_val) {
        char buf[32];
        if (GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path.c_str())) {
            try { out_val = std::stof(buf); }
            catch (...) {}
        }
    }
    static void LoadBool(const std::string& path, const char* section, const char* key, bool& out_val) {
        char buf[8];
        if (GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path.c_str())) {
            out_val = (std::string(buf) == "1");
        }
    }
    static void SaveFloat(const std::string& path, const char* section, const char* key, float val) {
        WritePrivateProfileStringA(section, key, std::to_string(val).c_str(), path.c_str());
    }
    static void SaveBool(const std::string& path, const char* section, const char* key, bool val) {
        WritePrivateProfileStringA(section, key, val ? "1" : "0", path.c_str());
    }
}

// =========================================================================
// CORE MATH TEMPLATES & HELPERS
// =========================================================================

template <typename T>
void ApplyConvergenceMath(T& pitch, T& yaw, T& roll, float boost_angle, float shift_angle, bool is_left_eye) {
    // Высчитываем итоговый угол для конкретного глаза с учетом доминантности
    // Если shift отрицательный (сдвиг влево), левый глаз расслабляется, правый остается на базе
    // Если shift положительный (сдвиг вправо), правый расслабляется, левый остается на базе
    float effective_boost = boost_angle;

    if (is_left_eye) {
        effective_boost -= std::max(0.0f, -shift_angle);
    }
    else {
        effective_boost -= std::max(0.0f, shift_angle);
    }

    // Защита от ухода в отрицательную конвергенцию, если смещение больше самого буста
    effective_boost = std::max(0.0f, effective_boost);

    const float boost_rad = effective_boost * PI_F / 180.0f;
    const float A = is_left_eye ? boost_rad : -boost_rad;

    const float p = static_cast<float>(pitch) * PI_F / 180.0f;
    const float y = static_cast<float>(yaw) * PI_F / 180.0f;
    const float r = static_cast<float>(roll) * PI_F / 180.0f;

    const float cp = std::cos(p), sp = std::sin(p);
    const float cy = std::cos(y), sy = std::sin(y);
    const float cr = std::cos(r), sr = std::sin(r);

    const float fwd_x = cp * cy;
    const float fwd_y = cp * sy;
    const float fwd_z = sp;

    const float right_x = sr * sp * cy - cr * sy;
    const float right_y = sr * sp * sy + cr * cy;
    const float right_z = -sr * cp;

    const float up_z = cr * cp;

    const float cosA = std::cos(A), sinA = std::sin(A);
    const float new_fwd_x = fwd_x * cosA + right_x * sinA;
    const float new_fwd_y = fwd_y * cosA + right_y * sinA;
    const float new_fwd_z = std::clamp(fwd_z * cosA + right_z * sinA, -1.0f, 1.0f);
    const float new_right_z = right_z * cosA - fwd_z * sinA;

    pitch = static_cast<T>(std::asin(new_fwd_z) * 180.0f / PI_F);
    yaw = static_cast<T>(std::atan2(new_fwd_y, new_fwd_x) * 180.0f / PI_F);
    roll = static_cast<T>(std::atan2(-new_right_z, up_z) * 180.0f / PI_F);
}

static void AppendEnvVarSafe(const char* name, const std::string& new_val) {
    DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
    if (size > 0) {
        std::string current_val(size, '\0');
        GetEnvironmentVariableA(name, &current_val[0], size);
        current_val.resize(size - 1);

        if (current_val.find(new_val) == std::string::npos) {
            std::string combined = current_val + ";" + new_val;
            SetEnvironmentVariableA(name, combined.c_str());
        }
    }
    else {
        SetEnvironmentVariableA(name, new_val.c_str());
    }
}

// =========================================================================
// MAIN PLUGIN CLASS
// =========================================================================

class UVOSuitPlugin : public uevr::Plugin {
public:
    UVOSuitPlugin() = default;

    ~UVOSuitPlugin() override {
        if (UVOSuit::g_GetProcAddress_original) {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourDetach(&(PVOID&)UVOSuit::g_GetProcAddress_original, Hook_GetProcAddress);
            DetourTransactionCommit();
        }

        if (UVOSuit::g_vignetteConfig) {
            UnmapViewOfFile(UVOSuit::g_vignetteConfig);
            UVOSuit::g_vignetteConfig = nullptr;
        }
        if (UVOSuit::g_hMapFile_Vignette) {
            CloseHandle(UVOSuit::g_hMapFile_Vignette);
            UVOSuit::g_hMapFile_Vignette = nullptr;
        }
    }

    static bool CanReadFile(const std::string& path) {
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
            return true;
        }
        return false;
    }

    void on_dllmain() override {
        static bool s_hooked = false;
        if (!s_hooked) {
            s_hooked = true;
            HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
            if (kernel32) {
                UVOSuit::g_GetProcAddress_original = (PFN_GetProcAddress)GetProcAddress(kernel32, "GetProcAddress");
                if (UVOSuit::g_GetProcAddress_original) {
                    DetourTransactionBegin();
                    DetourUpdateThread(GetCurrentThread());
                    DetourAttach(&(PVOID&)UVOSuit::g_GetProcAddress_original, Hook_GetProcAddress);
                    DetourTransactionCommit();
                }
            }

            if (const char* appdata = std::getenv("APPDATA")) {
                std::string layer_dir = std::string(appdata) + "\\UEVR\\VignetteLayer";
                std::string json_path = layer_dir + "\\XR_APILAYER_UVOSUIT_vignette.json";
                std::string dll_path = layer_dir + "\\uvosuit_vignette.dll";

                bool json_exists = CanReadFile(json_path);
                bool dll_exists = CanReadFile(dll_path);

                if (json_exists && dll_exists) {
                    AppendEnvVarSafe("XR_API_LAYER_PATH", layer_dir);
                    AppendEnvVarSafe("XR_ENABLE_API_LAYERS", "XR_APILAYER_UVOSUIT_vignette");
                    UVOSuit::g_Config.vignette_layer_found = true;
                    UVOSuit::g_Diag.layer_status = UVOSuit::DiagLevel::OK;
                    UVOSuit::g_Diag.layer_msg = "Layer activated: JSON manifest and DLL present.";
                }
                else if (json_exists && !dll_exists) {
                    UVOSuit::g_Config.vignette_layer_found = false;
                    UVOSuit::g_Diag.layer_status = UVOSuit::DiagLevel::CRITICAL_ERR;
                    UVOSuit::g_Diag.layer_msg = "Layer error: JSON manifest found, but DLL is missing.";
                }
                else if (!json_exists && dll_exists) {
                    UVOSuit::g_Config.vignette_layer_found = false;
                    UVOSuit::g_Diag.layer_status = UVOSuit::DiagLevel::CRITICAL_ERR;
                    UVOSuit::g_Diag.layer_msg = "Layer error: DLL found, but JSON manifest is missing.";
                }
                else {
                    UVOSuit::g_Config.vignette_layer_found = false;
                    UVOSuit::g_Diag.layer_status = UVOSuit::DiagLevel::WARNING;
                    UVOSuit::g_Diag.layer_msg = "Layer inactive: Files not found.";
                }
            }
        }
    }

    void on_initialize() override {
        ImGui::CreateContext();
        load_configs();
        UVOSuit::g_Config.actual_convergence_angle.store(UVOSuit::g_Config.convergence_angle, std::memory_order_relaxed);
        UVOSuit::g_Config.actual_dominance_shift.store(UVOSuit::g_Config.eye_dominance_shift, std::memory_order_relaxed);

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = FALSE;

        if (ConvertStringSecurityDescriptorToSecurityDescriptorA("D:(A;;GA;;;WD)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
            UVOSuit::g_hMapFile_Vignette = CreateFileMappingA(
                INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                sizeof(UVOSuit::VignetteConfig), "Local\\UVOSuit_Vignette_Data"
            );
            LocalFree(sa.lpSecurityDescriptor);
        }
        else {
            UVOSuit::g_hMapFile_Vignette = CreateFileMappingA(
                INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                sizeof(UVOSuit::VignetteConfig), "Local\\UVOSuit_Vignette_Data"
            );
        }

        if (UVOSuit::g_hMapFile_Vignette != nullptr) {
            UVOSuit::g_vignetteConfig = (UVOSuit::VignetteConfig*)MapViewOfFile(
                UVOSuit::g_hMapFile_Vignette, FILE_MAP_ALL_ACCESS, 0, 0,
                sizeof(UVOSuit::VignetteConfig)
            );

            if (UVOSuit::g_vignetteConfig) {
                UVOSuit::g_Diag.ipc_status = UVOSuit::DiagLevel::OK;
                UVOSuit::g_Diag.ipc_msg = "IPC Shared Memory mapped successfully.";
            }
            else {
                UVOSuit::g_Diag.ipc_status = UVOSuit::DiagLevel::CRITICAL_ERR;
                UVOSuit::g_Diag.ipc_msg = "IPC MapViewOfFile failed.";
            }
        }
        else {
            DWORD err = GetLastError();
            UVOSuit::g_Diag.ipc_status = UVOSuit::DiagLevel::CRITICAL_ERR;
            UVOSuit::g_Diag.ipc_msg = "IPC CreateFileMapping failed. Error Code: " + std::to_string(err);
        }
    }

    void on_present() override {
        std::scoped_lock _{ m_imgui_mutex };
        if (!m_initialized) { if (!initialize_imgui()) return; }

        if (ImGui::GetCurrentContext() == nullptr) return;

        const auto renderer_data = API::get()->param()->renderer;
        const auto vr_active = API::get()->param()->vr->is_hmd_active();

        if (!renderer_data || !renderer_data->swapchain) return;

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            if (!vr_active) ImGui_ImplDX11_NewFrame();
            g_d3d11.render_imgui();
        }
        else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            auto cmd_queue = static_cast<ID3D12CommandQueue*>(renderer_data->command_queue);
            if (cmd_queue != nullptr) {
                if (!vr_active) ImGui_ImplDX12_NewFrame();
                g_d3d12.render_imgui();
            }
        }

        handle_hotkeys();
    }

    void on_device_reset() override {
        std::scoped_lock _{ m_imgui_mutex };
        const auto renderer_data = API::get()->param()->renderer;
        if (!renderer_data) return;

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            ImGui_ImplDX11_Shutdown();
            g_d3d11 = {};
        }
        if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            g_d3d12.reset();
            ImGui_ImplDX12_Shutdown();
            g_d3d12 = {};
        }
        m_initialized = false;
    }

    void on_post_render_vr_framework_dx11(ID3D11DeviceContext* context, ID3D11Texture2D* texture, ID3D11RenderTargetView* rtv) override {
        if (!m_initialized || !API::get()->param()->vr->is_hmd_active() || !context || !rtv) return;
        std::scoped_lock _{ m_imgui_mutex };
        ImGui_ImplDX11_NewFrame();
        g_d3d11.render_imgui_vr(context, rtv);
    }

    void on_post_render_vr_framework_dx12(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* rt, D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {
        if (!m_initialized || !API::get()->param()->vr->is_hmd_active() || !cmd_list || !rtv) return;
        std::scoped_lock _{ m_imgui_mutex };
        ImGui_ImplDX12_NewFrame();
        g_d3d12.render_imgui_vr(cmd_list, rtv);
    }

    bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        return !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard;
    }

    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {
        float target_angle = UVOSuit::g_Config.enable_3d_boost ? UVOSuit::g_Config.convergence_angle : 0.0f;
        float target_shift = (UVOSuit::g_Config.enable_3d_boost && UVOSuit::g_Config.enable_dominance_shift) ? UVOSuit::g_Config.eye_dominance_shift : 0.0f;

        float current_actual = UVOSuit::g_Config.actual_convergence_angle.load(std::memory_order_relaxed);
        float current_shift = UVOSuit::g_Config.actual_dominance_shift.load(std::memory_order_relaxed);

        const float transition_speed = 8.5f;

        // Плавная интерполяция основного буста
        if (current_actual != target_angle) {
            current_actual += (target_angle - current_actual) * std::min(1.0f, transition_speed * delta);
            if (std::abs(current_actual - target_angle) < 0.005f) current_actual = target_angle;
            UVOSuit::g_Config.actual_convergence_angle.store(current_actual, std::memory_order_relaxed);
        }

        // Плавная интерполяция микро-сдвига доминантности
        if (current_shift != target_shift) {
            current_shift += (target_shift - current_shift) * std::min(1.0f, transition_speed * delta);
            if (std::abs(current_shift - target_shift) < 0.0005f) current_shift = target_shift;
            UVOSuit::g_Config.actual_dominance_shift.store(current_shift, std::memory_order_relaxed);
        }

        if (m_initialized) {
            std::scoped_lock _{ m_imgui_mutex };
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            draw_interface();
            ImGui::EndFrame();
            ImGui::Render();
        }
    }

    void on_post_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle device, int view_index, float world_to_meters, UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double) override {
        if (!UVOSuit::g_Config.mod_enabled.load(std::memory_order_relaxed)) return;

        float actual_angle = UVOSuit::g_Config.actual_convergence_angle.load(std::memory_order_relaxed);
        if (actual_angle == 0.0f) return;

        float actual_shift = UVOSuit::g_Config.actual_dominance_shift.load(std::memory_order_relaxed);

        const bool is_left_eye = is_double ? (view_index == 0) : (view_index == 1);
        const bool is_right_eye = is_double ? (view_index == 1) : (view_index == 2);

        if (!is_left_eye && !is_right_eye) return;

        // Преобразование процентов в градусы (лимит 100% = 10 градусов по умолчанию)
        const float actual_boost_angle = (actual_angle / 100.0f) * 10.0f;
        const float actual_shift_angle = (actual_shift / 100.0f) * 10.0f;

        if (is_double) {
            auto rot_d = reinterpret_cast<UEVR_Rotatord*>(rotation);
            ApplyConvergenceMath(rot_d->pitch, rot_d->yaw, rot_d->roll, actual_boost_angle, actual_shift_angle, is_left_eye);
        }
        else {
            auto rot_f = reinterpret_cast<UEVR_Rotatorf*>(rotation);
            ApplyConvergenceMath(rot_f->pitch, rot_f->yaw, rot_f->roll, actual_boost_angle, actual_shift_angle, is_left_eye);
        }
    }

    private:
        HWND m_wnd{};
        bool m_initialized{ false };
        bool m_display_menu = true;
        std::recursive_mutex m_imgui_mutex{};
        std::atomic<bool> m_save_in_progress{ false };

        void save_configs_async() {
            if (m_save_in_progress.exchange(true, std::memory_order_acquire)) {
                return;
            }
            std::thread([this]() {
                this->save_configs();
                m_save_in_progress.store(false, std::memory_order_release);
                }).detach();
        }

        void handle_hotkeys() {
            if (GetForegroundWindow() != m_wnd) return;

            static bool f2_down = false, f3_down = false, end_down = false, del_down = false;
            static bool prior_down = false, next_down = false, minus_down = false, plus_down = false;

            if (GetAsyncKeyState(VK_F2) & 0x8000) { f2_down = true; }
            else if (f2_down) { m_display_menu = !m_display_menu; f2_down = false; }

            if (GetAsyncKeyState(VK_F3) & 0x8000) { f3_down = true; }
            else if (f3_down) {
                bool current = UVOSuit::g_Config.enable_custom_fov.load(std::memory_order_relaxed);
                UVOSuit::g_Config.enable_custom_fov.store(!current, std::memory_order_relaxed);
                save_configs_async();
                f3_down = false;
            }

            if (GetAsyncKeyState(VK_END) & 0x8000) { end_down = true; }
            else if (end_down) {
                UVOSuit::g_Config.enable_3d_boost = !UVOSuit::g_Config.enable_3d_boost;
                save_configs_async();
                end_down = false;
            }

            // DEL - Переключатель Eye-Dominance Balance
            if (GetAsyncKeyState(VK_DELETE) & 0x8000) { del_down = true; }
            else if (del_down) {
                UVOSuit::g_Config.enable_dominance_shift = !UVOSuit::g_Config.enable_dominance_shift;
                save_configs_async();
                del_down = false;
            }

            bool mod_enabled = UVOSuit::g_Config.mod_enabled.load(std::memory_order_relaxed);
            bool boost_enabled = UVOSuit::g_Config.enable_3d_boost;

            if (mod_enabled && boost_enabled) {
                const bool is_shift_pressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                // Шаг при зажатом Shift в 10 раз быстрее (0.2 вместо 0.02)
                const float step = is_shift_pressed ? 0.2f : 0.02f;

                bool is_prior_pressed = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
                bool is_next_pressed = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
                bool is_minus_pressed = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0;
                bool is_plus_pressed = (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0;

                // Регулировка основного 3D Boost (PageUp / PageDown)
                if (is_prior_pressed || is_next_pressed) {
                    if (is_prior_pressed) { UVOSuit::g_Config.convergence_angle += step; prior_down = true; }
                    else { UVOSuit::g_Config.convergence_angle -= step; next_down = true; }

                    const float max_limit = UVOSuit::g_Config.unlock_limits ? 100.0f : 15.0f;
                    UVOSuit::g_Config.convergence_angle = std::clamp(UVOSuit::g_Config.convergence_angle, 0.0f, max_limit);
                    UVOSuit::g_Config.convergence_angle = std::round(UVOSuit::g_Config.convergence_angle * 1000.0f) / 1000.0f;
                    UVOSuit::g_Config.eye_dominance_shift = std::clamp(UVOSuit::g_Config.eye_dominance_shift, -UVOSuit::g_Config.convergence_angle, UVOSuit::g_Config.convergence_angle);
                }
                else if (prior_down || next_down) {
                    save_configs_async();
                    prior_down = false; next_down = false;
                }

                // Регулировка Bias (Клавиши - и +)
                if (UVOSuit::g_Config.enable_dominance_shift) {
                    if (is_minus_pressed || is_plus_pressed) {
                        if (is_minus_pressed) { UVOSuit::g_Config.eye_dominance_shift -= step; minus_down = true; }
                        else { UVOSuit::g_Config.eye_dominance_shift += step; plus_down = true; }

                        float max_shift = UVOSuit::g_Config.convergence_angle;
                        UVOSuit::g_Config.eye_dominance_shift = std::clamp(UVOSuit::g_Config.eye_dominance_shift, -max_shift, max_shift);
                        UVOSuit::g_Config.eye_dominance_shift = std::round(UVOSuit::g_Config.eye_dominance_shift * 1000.0f) / 1000.0f;
                    }
                    else if (minus_down || plus_down) {
                        save_configs_async();
                        minus_down = false; plus_down = false;
                    }
                }
            }
        }

        void load_configs() {
            const std::string local = GetLocalIniPath();
            bool temp_mod = false, temp_fov = false;
            ConfigHelper::LoadBool(local, "State", "ModEnabled", temp_mod);
            ConfigHelper::LoadBool(local, "State", "CustomFovEnabled", temp_fov);
            UVOSuit::g_Config.mod_enabled.store(temp_mod, std::memory_order_relaxed);
            UVOSuit::g_Config.enable_custom_fov.store(temp_fov, std::memory_order_relaxed);

            ConfigHelper::LoadBool(local, "State", "3DBoostEnabled", UVOSuit::g_Config.enable_3d_boost);
            ConfigHelper::LoadBool(local, "State", "UIFovOpen", UVOSuit::g_Config.ui_fov_opened);
            ConfigHelper::LoadBool(local, "State", "UIShiftOpen", UVOSuit::g_Config.ui_shift_opened);
            ConfigHelper::LoadBool(local, "State", "UIAsymOpen", UVOSuit::g_Config.ui_asym_opened);
            ConfigHelper::LoadBool(local, "State", "UIShiftsOpen", UVOSuit::g_Config.ui_shifts_opened);
            ConfigHelper::LoadBool(local, "State", "UIRotOpen", UVOSuit::g_Config.ui_rot_opened);

            ConfigHelper::LoadFloat(local, "3DBoost", "Convergence", UVOSuit::g_Config.convergence_angle);
            ConfigHelper::LoadFloat(local, "3DBoost", "DominanceShift", UVOSuit::g_Config.eye_dominance_shift);
            ConfigHelper::LoadBool(local, "3DBoost", "UnlockLimits", UVOSuit::g_Config.unlock_limits);

            ConfigHelper::LoadBool(local, "3DBoost", "DominanceEnabled", UVOSuit::g_Config.enable_dominance_shift);

            const std::string global = GetGlobalIniPath();
            ConfigHelper::LoadFloat(global, "Optics", "GlobalFov", UVOSuit::g_Config.global_fov_scale);
            ConfigHelper::LoadFloat(global, "Optics", "OutL", UVOSuit::g_Config.outer_fov_scale_left);
            ConfigHelper::LoadFloat(global, "Optics", "OutR", UVOSuit::g_Config.outer_fov_scale_right);
            ConfigHelper::LoadFloat(global, "Optics", "InnL", UVOSuit::g_Config.inner_fov_scale_left);
            ConfigHelper::LoadFloat(global, "Optics", "InnR", UVOSuit::g_Config.inner_fov_scale_right);
            ConfigHelper::LoadFloat(global, "Optics", "UpL", UVOSuit::g_Config.upper_fov_scale_left);
            ConfigHelper::LoadFloat(global, "Optics", "UpR", UVOSuit::g_Config.upper_fov_scale_right);
            ConfigHelper::LoadFloat(global, "Optics", "LowL", UVOSuit::g_Config.lower_fov_scale_left);
            ConfigHelper::LoadFloat(global, "Optics", "LowR", UVOSuit::g_Config.lower_fov_scale_right);

            ConfigHelper::LoadFloat(global, "Shifts", "BoxV", UVOSuit::g_Config.flat_box_vert);
            ConfigHelper::LoadFloat(global, "Shifts", "BoxH", UVOSuit::g_Config.flat_box_horiz);
            ConfigHelper::LoadFloat(global, "Shifts", "LeftV", UVOSuit::g_Config.flat_left_vert);
            ConfigHelper::LoadFloat(global, "Shifts", "LeftH", UVOSuit::g_Config.flat_left_horiz);
            ConfigHelper::LoadFloat(global, "Shifts", "RightV", UVOSuit::g_Config.flat_right_vert);
            ConfigHelper::LoadFloat(global, "Shifts", "RightH", UVOSuit::g_Config.flat_right_horiz);

            ConfigHelper::LoadFloat(global, "Rotation", "GlobP", UVOSuit::g_Config.rot_global_pitch);
            ConfigHelper::LoadFloat(global, "Rotation", "GlobY", UVOSuit::g_Config.rot_global_yaw);
            ConfigHelper::LoadFloat(global, "Rotation", "LeftP", UVOSuit::g_Config.rot_left_pitch);
            ConfigHelper::LoadFloat(global, "Rotation", "LeftY", UVOSuit::g_Config.rot_left_yaw);
            ConfigHelper::LoadFloat(global, "Rotation", "RightP", UVOSuit::g_Config.rot_right_pitch);
            ConfigHelper::LoadFloat(global, "Rotation", "RightY", UVOSuit::g_Config.rot_right_yaw);

            ConfigHelper::LoadBool(local, "State", "UIMaskOpen", UVOSuit::g_Config.ui_mask_opened);
            ConfigHelper::LoadBool(local, "State", "LensMaskEnabled", UVOSuit::g_Config.enable_lens_mask);
            ConfigHelper::LoadFloat(global, "LensMask", "Outer", UVOSuit::g_Config.mask_edge_outer);
            ConfigHelper::LoadFloat(global, "LensMask", "Inner", UVOSuit::g_Config.mask_edge_inner);
            ConfigHelper::LoadFloat(global, "LensMask", "Top", UVOSuit::g_Config.mask_edge_top);
            ConfigHelper::LoadFloat(global, "LensMask", "Bottom", UVOSuit::g_Config.mask_edge_bottom);
            ConfigHelper::LoadFloat(global, "LensMask", "Softness", UVOSuit::g_Config.mask_softness);
            ConfigHelper::LoadFloat(global, "LensMask", "Radius", UVOSuit::g_Config.mask_corner_radius);

            UVOSuit::g_Cache.Update(UVOSuit::g_Config);
        }

        void save_configs() {
            std::lock_guard<std::mutex> file_lock(UVOSuit::g_FileIOMutex);

            bool mod_enabled = UVOSuit::g_Config.mod_enabled.load(std::memory_order_relaxed);
            bool custom_fov = UVOSuit::g_Config.enable_custom_fov.load(std::memory_order_relaxed);

            const std::string local = GetLocalIniPath();
            ConfigHelper::SaveBool(local, "State", "ModEnabled", mod_enabled);
            ConfigHelper::SaveBool(local, "State", "CustomFovEnabled", custom_fov);

            ConfigHelper::SaveBool(local, "State", "3DBoostEnabled", UVOSuit::g_Config.enable_3d_boost);
            ConfigHelper::SaveBool(local, "State", "UIFovOpen", UVOSuit::g_Config.ui_fov_opened);
            ConfigHelper::SaveBool(local, "State", "UIShiftOpen", UVOSuit::g_Config.ui_shift_opened);
            ConfigHelper::SaveBool(local, "State", "UIAsymOpen", UVOSuit::g_Config.ui_asym_opened);
            ConfigHelper::SaveBool(local, "State", "UIShiftsOpen", UVOSuit::g_Config.ui_shifts_opened);
            ConfigHelper::SaveBool(local, "State", "UIRotOpen", UVOSuit::g_Config.ui_rot_opened);

            ConfigHelper::SaveFloat(local, "3DBoost", "Convergence", UVOSuit::g_Config.convergence_angle);
            ConfigHelper::SaveFloat(local, "3DBoost", "DominanceShift", UVOSuit::g_Config.eye_dominance_shift);
            ConfigHelper::SaveBool(local, "3DBoost", "UnlockLimits", UVOSuit::g_Config.unlock_limits);

            ConfigHelper::SaveBool(local, "3DBoost", "DominanceEnabled", UVOSuit::g_Config.enable_dominance_shift);

            const std::string global = GetGlobalIniPath();
            std::filesystem::create_directories(std::filesystem::path(global).parent_path());

            ConfigHelper::SaveFloat(global, "Optics", "GlobalFov", UVOSuit::g_Config.global_fov_scale);
            ConfigHelper::SaveFloat(global, "Optics", "OutL", UVOSuit::g_Config.outer_fov_scale_left);
            ConfigHelper::SaveFloat(global, "Optics", "OutR", UVOSuit::g_Config.outer_fov_scale_right);
            // ... (Сохранение остальных параметров оптики идентично загрузке)
            ConfigHelper::SaveFloat(global, "Optics", "InnL", UVOSuit::g_Config.inner_fov_scale_left);
            ConfigHelper::SaveFloat(global, "Optics", "InnR", UVOSuit::g_Config.inner_fov_scale_right);
            ConfigHelper::SaveFloat(global, "Optics", "UpL", UVOSuit::g_Config.upper_fov_scale_left);
            ConfigHelper::SaveFloat(global, "Optics", "UpR", UVOSuit::g_Config.upper_fov_scale_right);
            ConfigHelper::SaveFloat(global, "Optics", "LowL", UVOSuit::g_Config.lower_fov_scale_left);
            ConfigHelper::SaveFloat(global, "Optics", "LowR", UVOSuit::g_Config.lower_fov_scale_right);

            ConfigHelper::SaveFloat(global, "Shifts", "BoxV", UVOSuit::g_Config.flat_box_vert);
            ConfigHelper::SaveFloat(global, "Shifts", "BoxH", UVOSuit::g_Config.flat_box_horiz);
            ConfigHelper::SaveFloat(global, "Shifts", "LeftV", UVOSuit::g_Config.flat_left_vert);
            ConfigHelper::SaveFloat(global, "Shifts", "LeftH", UVOSuit::g_Config.flat_left_horiz);
            ConfigHelper::SaveFloat(global, "Shifts", "RightV", UVOSuit::g_Config.flat_right_vert);
            ConfigHelper::SaveFloat(global, "Shifts", "RightH", UVOSuit::g_Config.flat_right_horiz);

            ConfigHelper::SaveFloat(global, "Rotation", "GlobP", UVOSuit::g_Config.rot_global_pitch);
            ConfigHelper::SaveFloat(global, "Rotation", "GlobY", UVOSuit::g_Config.rot_global_yaw);
            ConfigHelper::SaveFloat(global, "Rotation", "LeftP", UVOSuit::g_Config.rot_left_pitch);
            ConfigHelper::SaveFloat(global, "Rotation", "LeftY", UVOSuit::g_Config.rot_left_yaw);
            ConfigHelper::SaveFloat(global, "Rotation", "RightP", UVOSuit::g_Config.rot_right_pitch);
            ConfigHelper::SaveFloat(global, "Rotation", "RightY", UVOSuit::g_Config.rot_right_yaw);

            ConfigHelper::SaveBool(local, "State", "UIMaskOpen", UVOSuit::g_Config.ui_mask_opened);
            ConfigHelper::SaveBool(local, "State", "LensMaskEnabled", UVOSuit::g_Config.enable_lens_mask);
            ConfigHelper::SaveFloat(global, "LensMask", "Outer", UVOSuit::g_Config.mask_edge_outer);
            ConfigHelper::SaveFloat(global, "LensMask", "Inner", UVOSuit::g_Config.mask_edge_inner);
            ConfigHelper::SaveFloat(global, "LensMask", "Top", UVOSuit::g_Config.mask_edge_top);
            ConfigHelper::SaveFloat(global, "LensMask", "Bottom", UVOSuit::g_Config.mask_edge_bottom);
            ConfigHelper::SaveFloat(global, "LensMask", "Softness", UVOSuit::g_Config.mask_softness);
            ConfigHelper::SaveFloat(global, "LensMask", "Radius", UVOSuit::g_Config.mask_corner_radius);
        }

        bool DrawResetBtn(const char* id) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.16f, 0.23f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.23f, 0.32f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.64f, 0.72f, 1.0f));
            const bool pressed = ImGui::Button(id);
            ImGui::PopStyleColor(3);
            return pressed;
        }

        void DrawDynamicCheckbox(const char* label, bool* v) {
            const ImVec4 bg_color = *v ? ImVec4(0.31f, 0.78f, 0.47f, 1.0f) : ImVec4(0.45f, 0.18f, 0.22f, 1.0f);
            const ImVec4 hover_color = *v ? ImVec4(0.42f, 0.88f, 0.58f, 1.0f) : ImVec4(0.65f, 0.28f, 0.34f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, bg_color);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, hover_color);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, hover_color);
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.06f, 0.09f, 0.16f, 1.0f));
            if (ImGui::Checkbox(label, v)) {
                UVOSuit::g_Cache.Update(UVOSuit::g_Config);
                save_configs_async();
            }
            ImGui::PopStyleColor(4);
        }

        void DrawDynamicCheckboxAtomic(const char* label, std::atomic<bool>& v) {
            bool temp = v.load(std::memory_order_relaxed);
            const ImVec4 bg_color = temp ? ImVec4(0.31f, 0.78f, 0.47f, 1.0f) : ImVec4(0.45f, 0.18f, 0.22f, 1.0f);
            const ImVec4 hover_color = temp ? ImVec4(0.42f, 0.88f, 0.58f, 1.0f) : ImVec4(0.65f, 0.28f, 0.34f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, bg_color);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, hover_color);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, hover_color);
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.06f, 0.09f, 0.16f, 1.0f));
            if (ImGui::Checkbox(label, &temp)) {
                v.store(temp, std::memory_order_relaxed);
                UVOSuit::g_Cache.Update(UVOSuit::g_Config);
                save_configs_async();
            }
            ImGui::PopStyleColor(4);
        }

        bool initialize_imgui() {
            if (m_initialized) return true;
            std::scoped_lock _{ m_imgui_mutex };
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();

            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowRounding = 8.0f; style.FrameRounding = 4.0f; style.WindowBorderSize = 1.0f;
            style.Colors[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.07f, 0.17f, 0.80f);
            style.Colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
            style.Colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.0f);
            style.Colors[ImGuiCol_FrameBg] = ImVec4(0.06f, 0.09f, 0.16f, 1.0f);
            style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.16f, 0.23f, 1.0f);
            style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.06f, 0.09f, 0.16f, 1.0f);
            style.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.16f, 0.23f, 1.0f);
            style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.23f, 0.32f, 1.0f);
            style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.12f, 0.16f, 0.23f, 1.0f);
            style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.96f, 0.88f, 0.73f, 0.5f);
            style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.96f, 0.88f, 0.73f, 0.8f);
            style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.96f, 0.88f, 0.73f, 1.0f);

            const auto renderer_data = API::get()->param()->renderer;
            if (!renderer_data || !renderer_data->swapchain) return false;

            DXGI_SWAP_CHAIN_DESC swap_desc{};
            (static_cast<IDXGISwapChain*>(renderer_data->swapchain))->GetDesc(&swap_desc);
            m_wnd = swap_desc.OutputWindow;

            if (!ImGui_ImplWin32_Init(m_wnd)) return false;
            if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
                if (!g_d3d11.initialize()) return false;
            }
            else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
                if (!g_d3d12.initialize()) return false;
            }

            m_initialized = true;
            return true;
        }

        void draw_interface() {
            if (!m_display_menu) return;

            // ОБНОВЛЕННАЯ ФУНКЦИЯ-ЩИТ: Жесткая проверка ввода, округление и обрезка
            auto DrawSettingRow = [&](const char* reset_id, const char* label, float* v, float v_min, float v_max, float default_val, int decimals = 3) {
                if (DrawResetBtn(reset_id)) {
                    *v = default_val;
                    UVOSuit::g_Cache.Update(UVOSuit::g_Config);
                    save_configs_async();
                }
                ImGui::SameLine();

                const float step = (decimals == 4) ? 0.0001f : 0.001f;
                const char* format = (decimals == 4) ? "%.4f" : "%.3f";

                bool edited = ImGui::DragFloat(label, v, step, v_min, v_max, format);
                bool deactivated = ImGui::IsItemDeactivatedAfterEdit();

                // Как только человек вписал значение (или просто отпустил мышь) - применяется фильтр
                if (edited || deactivated) {
                    *v = std::clamp(*v, v_min, v_max);

                    float multiplier = (decimals == 4) ? 10000.0f : 1000.0f;
                    *v = std::round(*v * multiplier) / multiplier;

                    UVOSuit::g_Cache.Update(UVOSuit::g_Config);
                }

                if (deactivated) { save_configs_async(); }
                };

            ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, -1.0f), ImVec2(FLT_MAX, FLT_MAX));

            if (ImGui::Begin("Ultimate VR Optics Suite", &m_display_menu, ImGuiWindowFlags_AlwaysAutoResize)) {

                bool mod_enabled = UVOSuit::g_Config.mod_enabled.load(std::memory_order_relaxed);
                if (mod_enabled) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.18f, 0.22f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.22f, 0.28f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 0.98f, 1.0f));
                    if (ImGui::Button("Disable UVOSuit", ImVec2(-1, 0))) {
                        UVOSuit::g_Config.mod_enabled.store(false, std::memory_order_relaxed);
                        save_configs_async();
                    }
                    ImGui::PopStyleColor(3);
                }
                else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.78f, 0.47f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.38f, 0.85f, 0.54f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.09f, 0.16f, 1.0f));
                    if (ImGui::Button("Enable UVOSuit", ImVec2(-1, 0))) {
                        UVOSuit::g_Config.mod_enabled.store(true, std::memory_order_relaxed);
                        save_configs_async();
                    }
                    ImGui::PopStyleColor(3);
                }
                ImGui::Spacing();

                if (mod_enabled) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.82f, 0.38f, 1.0f));
                    ImGui::Text("3D Depth Boost");
                    ImGui::PopStyleColor();

                    DrawDynamicCheckbox("Enable 3D Boost [End]", &UVOSuit::g_Config.enable_3d_boost);
                    const bool was_unlocked = UVOSuit::g_Config.unlock_limits;
                    DrawDynamicCheckbox("Unlock Extreme Limits (>15%)", &UVOSuit::g_Config.unlock_limits);

                    if (was_unlocked && !UVOSuit::g_Config.unlock_limits) {
                        if (UVOSuit::g_Config.convergence_angle > 15.0f) {
                            UVOSuit::g_Config.convergence_angle = 15.0f;
                            // Подтягиваем Shift, если основной буст срезался
                            UVOSuit::g_Config.eye_dominance_shift = std::clamp(UVOSuit::g_Config.eye_dominance_shift, -15.0f, 15.0f);
                        }
                        save_configs_async();
                    }

                    // Основной ползунок Буста
                    float current_max_boost = UVOSuit::g_Config.unlock_limits ? 100.0f : 15.0f;
                    DrawSettingRow(" R ##Boost", "Strength %", &UVOSuit::g_Config.convergence_angle, 0.0f, current_max_boost, 0.0f, 3);

                    // Блокировка Shift, если изменился Boost
                    float max_shift = UVOSuit::g_Config.convergence_angle;
                    if (UVOSuit::g_Config.eye_dominance_shift < -max_shift || UVOSuit::g_Config.eye_dominance_shift > max_shift) {
                        UVOSuit::g_Config.eye_dominance_shift = std::clamp(UVOSuit::g_Config.eye_dominance_shift, -max_shift, max_shift);
                    }

                    // Скрытый список для Eye-Dominance Balance
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.78f, 0.49f, 1.0f));
                    ImGui::SetNextItemOpen(UVOSuit::g_Config.ui_shift_opened, ImGuiCond_Always);
                    const bool shift_curr = ImGui::TreeNode("Eye-Dominance Balance");
                    ImGui::PopStyleColor();

                    if (shift_curr != UVOSuit::g_Config.ui_shift_opened) {
                        UVOSuit::g_Config.ui_shift_opened = shift_curr;
                        save_configs_async();
                    }
                    if (UVOSuit::g_Config.ui_shift_opened) {
                        DrawDynamicCheckbox("Enable Balance [Del]", &UVOSuit::g_Config.enable_dominance_shift);
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.58f, 0.64f, 0.72f, 1.0f), "Negative = Left Eye, Positive = Right Eye");
                        DrawSettingRow(" R ##Bias", "Bias %", &UVOSuit::g_Config.eye_dominance_shift, -max_shift, max_shift, 0.0f, 3);
                        ImGui::TreePop();
                    }

                    ImGui::Spacing(); ImGui::Separator();

                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.82f, 0.38f, 1.0f));
                    ImGui::SetNextItemOpen(UVOSuit::g_Config.ui_fov_opened, ImGuiCond_Always);
                    const bool fov_curr = ImGui::TreeNodeEx("Custom FOV & Optics");
                    ImGui::PopStyleColor();

                    if (fov_curr != UVOSuit::g_Config.ui_fov_opened) {
                        UVOSuit::g_Config.ui_fov_opened = fov_curr;
                        save_configs_async();
                    }

                    if (UVOSuit::g_Config.ui_fov_opened) {
                        DrawDynamicCheckboxAtomic("Enable Custom FOV / Shifts [F3]", UVOSuit::g_Config.enable_custom_fov);
                        DrawSettingRow(" R ##GlobalFOV", "Global Scale", &UVOSuit::g_Config.global_fov_scale, 0.72f, 1.0f, 1.0f, 3);
                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.78f, 0.49f, 1.0f));
                        ImGui::SetNextItemOpen(UVOSuit::g_Config.ui_asym_opened, ImGuiCond_Always);
                        const bool asym_curr = ImGui::TreeNode("Asymmetric FOV Scaling");
                        ImGui::PopStyleColor();

                        if (asym_curr != UVOSuit::g_Config.ui_asym_opened) { UVOSuit::g_Config.ui_asym_opened = asym_curr; save_configs_async(); }
                        if (UVOSuit::g_Config.ui_asym_opened) {
                            if (ImGui::TreeNode("Outer Edge (Temples)")) {
                                DrawSettingRow(" R ##OutL", "Left Eye##OutL", &UVOSuit::g_Config.outer_fov_scale_left, 0.72f, 1.0f, 1.0f, 3);
                                DrawSettingRow(" R ##OutR", "Right Eye##OutR", &UVOSuit::g_Config.outer_fov_scale_right, 0.72f, 1.0f, 1.0f, 3);
                                ImGui::TreePop();
                            }
                            if (ImGui::TreeNode("Inner Edge (Nose)")) {
                                DrawSettingRow(" R ##InnL", "Left Eye##InnL", &UVOSuit::g_Config.inner_fov_scale_left, 0.72f, 1.0f, 1.0f, 3);
                                DrawSettingRow(" R ##InnR", "Right Eye##InnR", &UVOSuit::g_Config.inner_fov_scale_right, 0.72f, 1.0f, 1.0f, 3);
                                ImGui::TreePop();
                            }
                            if (ImGui::TreeNode("Upper Edge (Top)")) {
                                DrawSettingRow(" R ##UpL", "Left Eye##UpL", &UVOSuit::g_Config.upper_fov_scale_left, 0.72f, 1.0f, 1.0f, 3);
                                DrawSettingRow(" R ##UpR", "Right Eye##UpR", &UVOSuit::g_Config.upper_fov_scale_right, 0.72f, 1.0f, 1.0f, 3);
                                ImGui::TreePop();
                            }
                            if (ImGui::TreeNode("Lower Edge (Bottom)")) {
                                DrawSettingRow(" R ##LowL", "Left Eye##LowL", &UVOSuit::g_Config.lower_fov_scale_left, 0.72f, 1.0f, 1.0f, 3);
                                DrawSettingRow(" R ##LowR", "Right Eye##LowR", &UVOSuit::g_Config.lower_fov_scale_right, 0.72f, 1.0f, 1.0f, 3);
                                ImGui::TreePop();
                            }
                            ImGui::TreePop();
                        }

                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.78f, 0.49f, 1.0f));
                        ImGui::SetNextItemOpen(UVOSuit::g_Config.ui_shifts_opened, ImGuiCond_Always);
                        const bool shifts_curr = ImGui::TreeNode("Optical Center Shift");
                        ImGui::PopStyleColor();

                        if (shifts_curr != UVOSuit::g_Config.ui_shifts_opened) { UVOSuit::g_Config.ui_shifts_opened = shifts_curr; save_configs_async(); }
                        if (UVOSuit::g_Config.ui_shifts_opened) {
                            DrawSettingRow(" R ##FBV", "Global Vert (deg)##FB", &UVOSuit::g_Config.flat_box_vert, -45.0f, 45.0f, 0.0f, 3);
                            DrawSettingRow(" R ##FBH", "Global Horiz (deg)##FB", &UVOSuit::g_Config.flat_box_horiz, -45.0f, 45.0f, 0.0f, 3);
                            ImGui::Spacing();
                            DrawSettingRow(" R ##FLV", "Left Vert##FL", &UVOSuit::g_Config.flat_left_vert, -45.0f, 45.0f, 0.0f, 3);
                            DrawSettingRow(" R ##FLH", "Left Horiz##FL", &UVOSuit::g_Config.flat_left_horiz, -45.0f, 45.0f, 0.0f, 3);
                            ImGui::Spacing();
                            DrawSettingRow(" R ##FRV", "Right Vert##FR", &UVOSuit::g_Config.flat_right_vert, -45.0f, 45.0f, 0.0f, 3);
                            DrawSettingRow(" R ##FRH", "Right Horiz##FR", &UVOSuit::g_Config.flat_right_horiz, -45.0f, 45.0f, 0.0f, 3);
                            ImGui::TreePop();
                        }

                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.78f, 0.49f, 1.0f));
                        ImGui::SetNextItemOpen(UVOSuit::g_Config.ui_rot_opened, ImGuiCond_Always);
                        const bool rot_curr = ImGui::TreeNode("Optical Axis Rotation");
                        ImGui::PopStyleColor();

                        if (rot_curr != UVOSuit::g_Config.ui_rot_opened) { UVOSuit::g_Config.ui_rot_opened = rot_curr; save_configs_async(); }
                        if (UVOSuit::g_Config.ui_rot_opened) {
                            DrawSettingRow(" R ##RGP", "Global Pitch##RG", &UVOSuit::g_Config.rot_global_pitch, -45.0f, 45.0f, 0.0f, 3);
                            DrawSettingRow(" R ##RGY", "Global Yaw##RG", &UVOSuit::g_Config.rot_global_yaw, -45.0f, 45.0f, 0.0f, 3);
                            ImGui::Spacing();
                            DrawSettingRow(" R ##RLP", "Left Pitch##RL", &UVOSuit::g_Config.rot_left_pitch, -45.0f, 45.0f, 0.0f, 3);
                            DrawSettingRow(" R ##RLY", "Left Yaw##RL", &UVOSuit::g_Config.rot_left_yaw, -45.0f, 45.0f, 0.0f, 3);
                            ImGui::Spacing();
                            DrawSettingRow(" R ##RRP", "Right Pitch##RR", &UVOSuit::g_Config.rot_right_pitch, -45.0f, 45.0f, 0.0f, 3);
                            DrawSettingRow(" R ##RRY", "Right Yaw##RR", &UVOSuit::g_Config.rot_right_yaw, -45.0f, 45.0f, 0.0f, 3);
                            ImGui::TreePop();
                        }

                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.78f, 0.49f, 1.0f));
                        ImGui::SetNextItemOpen(UVOSuit::g_Config.ui_mask_opened, ImGuiCond_Always);
                        const bool mask_curr = ImGui::TreeNode("Lens Mask (API Layer)");
                        ImGui::PopStyleColor();

                        if (mask_curr != UVOSuit::g_Config.ui_mask_opened) { UVOSuit::g_Config.ui_mask_opened = mask_curr; save_configs_async(); }
                        if (UVOSuit::g_Config.ui_mask_opened) {
                            DrawDynamicCheckbox("Enable Lens Mask", &UVOSuit::g_Config.enable_lens_mask);
                            ImGui::Spacing();
                            ImGui::TextColored(ImVec4(0.58f, 0.64f, 0.72f, 1.0f), "Edge Offsets (Clamp to FOV)");
                            DrawSettingRow(" R ##MOut", "Outer Edge (Temple)", &UVOSuit::g_Config.mask_edge_outer, 0.0f, 0.5f, 0.0f, 3);
                            DrawSettingRow(" R ##MInn", "Inner Edge (Nose)", &UVOSuit::g_Config.mask_edge_inner, 0.0f, 0.5f, 0.0f, 3);
                            DrawSettingRow(" R ##MTop", "Top Edge", &UVOSuit::g_Config.mask_edge_top, 0.0f, 0.5f, 0.0f, 3);
                            DrawSettingRow(" R ##MBot", "Bottom Edge", &UVOSuit::g_Config.mask_edge_bottom, 0.0f, 0.5f, 0.0f, 3);
                            ImGui::Spacing();
                            ImGui::TextColored(ImVec4(0.58f, 0.64f, 0.72f, 1.0f), "Shape & Blending");
                            DrawSettingRow(" R ##MRad", "Corner Radius", &UVOSuit::g_Config.mask_corner_radius, 0.0f, 1.0f, 0.2f, 3);
                            DrawSettingRow(" R ##MSof", "Edge Softness", &UVOSuit::g_Config.mask_softness, 0.001f, 1.0f, 0.05f, 3);
                            ImGui::TreePop();
                        }
                        ImGui::TreePop();
                    }
                }

                if (UVOSuit::g_vignetteConfig != nullptr) {
                    UVOSuit::g_vignetteConfig->enabled = UVOSuit::g_Config.enable_lens_mask;
                    UVOSuit::g_vignetteConfig->edge_outer = UVOSuit::g_Config.mask_edge_outer;
                    UVOSuit::g_vignetteConfig->edge_inner = UVOSuit::g_Config.mask_edge_inner;
                    UVOSuit::g_vignetteConfig->edge_top = UVOSuit::g_Config.mask_edge_top;
                    UVOSuit::g_vignetteConfig->edge_bottom = UVOSuit::g_Config.mask_edge_bottom;
                    UVOSuit::g_vignetteConfig->softness = UVOSuit::g_Config.mask_softness;
                    UVOSuit::g_vignetteConfig->corner_radius = UVOSuit::g_Config.mask_corner_radius;
                }

                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                auto PrintStatus = [](const char* name, UVOSuit::DiagLevel level, const std::string& msg) {
                    ImVec4 color;
                    switch (level) {
                    case UVOSuit::DiagLevel::OK:           color = ImVec4(0.31f, 0.78f, 0.47f, 1.0f); break;
                    case UVOSuit::DiagLevel::WARNING:      color = ImVec4(0.96f, 0.82f, 0.38f, 1.0f); break;
                    case UVOSuit::DiagLevel::CRITICAL_ERR: color = ImVec4(0.55f, 0.15f, 0.22f, 1.0f); break;
                    }
                    ImGui::TextColored(color, "[%s]", name);
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", msg.c_str());
                    };

                ImGui::TextColored(ImVec4(0.58f, 0.64f, 0.72f, 1.0f), "Diagnostics Overview:");
                ImGui::BeginGroup();
                ImGui::Indent(8.0f);
                PrintStatus("OXR", UVOSuit::g_Diag.hook_status, UVOSuit::g_Diag.hook_msg);
                PrintStatus("IPC", UVOSuit::g_Diag.ipc_status, UVOSuit::g_Diag.ipc_msg);
                PrintStatus("LYR", UVOSuit::g_Diag.layer_status, UVOSuit::g_Diag.layer_msg);
                ImGui::Unindent(8.0f);
                ImGui::EndGroup();
            }
            ImGui::End();
        }
};

std::unique_ptr<UVOSuitPlugin> g_plugin{ new UVOSuitPlugin() };