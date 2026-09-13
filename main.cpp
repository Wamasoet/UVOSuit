#define NOMINMAX
#include <Windows.h>
#include <string>
#include <mutex>
#include <memory>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "renderlib/imgui/imgui.h"
#include "renderlib/uevr_imgui/imgui_impl_dx11.h"
#include "renderlib/uevr_imgui/imgui_impl_dx12.h"
#include "renderlib/uevr_imgui/imgui_impl_win32.h"
#include "renderlib/rendering/d3d11.hpp"
#include "renderlib/rendering/d3d12.hpp"
#include "uevr/API.h"
#include "uevr/Plugin.hpp"
#include <detours.h>

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
[[nodiscard]] MyXrQuaternionf MultiplyQuat(const MyXrQuaternionf& q1, const MyXrQuaternionf& q2) noexcept {
    return {
        q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y,
        q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x,
        q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w,
        q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z
    };
}

[[nodiscard]] MyXrQuaternionf EulerToQuat(float pitch, float yaw, float roll = 0.0f) noexcept {
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
    struct UVOSettings {
        bool mod_enabled = false;
        bool enable_custom_fov = false;
        bool enable_3d_boost = false;

        float convergence_angle = 0.0f;
        float actual_convergence_angle = 0.0f;
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

        // UI State
        bool ui_fov_opened = true;
        bool ui_asym_opened = false;
        bool ui_shifts_opened = false;
        bool ui_rot_opened = false;
    };

    class OptimizationCache {
    public:
        MyXrQuaternionf global_rot = { 0.0f, 0.0f, 0.0f, 1.0f };
        MyXrQuaternionf left_rot = { 0.0f, 0.0f, 0.0f, 1.0f };
        MyXrQuaternionf right_rot = { 0.0f, 0.0f, 0.0f, 1.0f };

        // Real-time update for fast rendering during UI drag events
        void Update(const UVOSettings& config) noexcept {
            global_rot = EulerToQuat(config.rot_global_pitch * DEG_TO_RAD_F, config.rot_global_yaw * DEG_TO_RAD_F);
            left_rot = EulerToQuat(config.rot_left_pitch * DEG_TO_RAD_F, config.rot_left_yaw * DEG_TO_RAD_F);
            right_rot = EulerToQuat(config.rot_right_pitch * DEG_TO_RAD_F, config.rot_right_yaw * DEG_TO_RAD_F);
        }
    };

    // Global instances strictly contained within namespace
    UVOSettings g_Config;
    OptimizationCache g_Cache;

    // C-Style Hook Variables
    PFN_GetProcAddress g_GetProcAddress_original = nullptr;
    PFN_xrNegotiateLoaderRuntimeInterface g_real_negotiate = nullptr;
    PFN_xrGetInstanceProcAddr g_real_get_instance_proc_addr = nullptr;
    PFN_xrLocateViews g_xrLocateViews_original = nullptr;

    uint64_t g_hook_fire_count = 0;
    std::string g_hook_status_msg = "Waiting for VR initialization...";
}

// =========================================================================
// OPENXR HOOKS
// =========================================================================
MyXrResult Hook_xrLocateViews(MyXrSession session, const MyXrViewLocateInfo* viewLocateInfo, MyXrViewState* viewState, uint32_t viewCapacityInput, uint32_t* viewCountOutput, MyXrView* views) {
    UVOSuit::g_hook_fire_count++;
    MyXrResult result = UVOSuit::g_xrLocateViews_original(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);

    // Validation ensures we don't access invalid memory during early game startup
    if (UVOSuit::g_Config.mod_enabled && UVOSuit::g_Config.enable_custom_fov && result == 0 && views != nullptr && viewCountOutput != nullptr && *viewCountOutput >= 2) {

        // 1. FOV Scaling Process
        views[0].fov.angleLeft *= (UVOSuit::g_Config.global_fov_scale * UVOSuit::g_Config.outer_fov_scale_left);
        views[0].fov.angleRight *= (UVOSuit::g_Config.global_fov_scale * UVOSuit::g_Config.inner_fov_scale_left);
        views[0].fov.angleUp *= (UVOSuit::g_Config.global_fov_scale * UVOSuit::g_Config.upper_fov_scale_left);
        views[0].fov.angleDown *= (UVOSuit::g_Config.global_fov_scale * UVOSuit::g_Config.lower_fov_scale_left);

        views[1].fov.angleLeft *= (UVOSuit::g_Config.global_fov_scale * UVOSuit::g_Config.inner_fov_scale_right);
        views[1].fov.angleRight *= (UVOSuit::g_Config.global_fov_scale * UVOSuit::g_Config.outer_fov_scale_right);
        views[1].fov.angleUp *= (UVOSuit::g_Config.global_fov_scale * UVOSuit::g_Config.upper_fov_scale_right);
        views[1].fov.angleDown *= (UVOSuit::g_Config.global_fov_scale * UVOSuit::g_Config.lower_fov_scale_right);

        // 2. Optical Center (Flat) Shifts
        const float flat_left_x = (UVOSuit::g_Config.flat_box_horiz + UVOSuit::g_Config.flat_left_horiz) * DEG_TO_RAD_F;
        const float flat_left_y = (UVOSuit::g_Config.flat_box_vert + UVOSuit::g_Config.flat_left_vert) * DEG_TO_RAD_F;
        const float flat_right_x = (UVOSuit::g_Config.flat_box_horiz + UVOSuit::g_Config.flat_right_horiz) * DEG_TO_RAD_F;
        const float flat_right_y = (UVOSuit::g_Config.flat_box_vert + UVOSuit::g_Config.flat_right_vert) * DEG_TO_RAD_F;

        views[0].fov.angleLeft += flat_left_x; views[0].fov.angleRight += flat_left_x;
        views[0].fov.angleUp += flat_left_y; views[0].fov.angleDown += flat_left_y;

        views[1].fov.angleLeft += flat_right_x; views[1].fov.angleRight += flat_right_x;
        views[1].fov.angleUp += flat_right_y; views[1].fov.angleDown += flat_right_y;

        // 3. Optical Axis Rotation (Utilizing Optimization Cache)
        views[0].pose.orientation = MultiplyQuat(views[0].pose.orientation, MultiplyQuat(UVOSuit::g_Cache.global_rot, UVOSuit::g_Cache.left_rot));
        views[1].pose.orientation = MultiplyQuat(views[1].pose.orientation, MultiplyQuat(UVOSuit::g_Cache.global_rot, UVOSuit::g_Cache.right_rot));
    }
    return result;
}

MyXrResult Hook_xrGetInstanceProcAddr(MyXrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    MyXrResult res = UVOSuit::g_real_get_instance_proc_addr(instance, name, function);
    if (res == 0 && function && *function && name && strcmp(name, "xrLocateViews") == 0) {
        UVOSuit::g_xrLocateViews_original = (PFN_xrLocateViews)*function;
        *function = (PFN_xrVoidFunction)Hook_xrLocateViews;
        UVOSuit::g_hook_status_msg = "SUCCESS: UVOSuit Active!";
    }
    return res;
}

MyXrResult Hook_xrNegotiateLoaderRuntimeInterface(const MyXrNegotiateLoaderInfo* loaderInfo, MyXrNegotiateRuntimeRequest* runtimeRequest) {
    MyXrResult res = UVOSuit::g_real_negotiate(loaderInfo, runtimeRequest);
    if (res == 0 && runtimeRequest->getInstanceProcAddr) {
        UVOSuit::g_real_get_instance_proc_addr = runtimeRequest->getInstanceProcAddr;
        runtimeRequest->getInstanceProcAddr = Hook_xrGetInstanceProcAddr;
    }
    return res;
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    FARPROC res = UVOSuit::g_GetProcAddress_original(hModule, lpProcName);

    // Safety check against invalid memory pointers in legacy APIs
    if (lpProcName && reinterpret_cast<uintptr_t>(lpProcName) > 0xFFFF) {
        if (strcmp(lpProcName, "xrNegotiateLoaderRuntimeInterface") == 0 && res) {
            UVOSuit::g_real_negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)res;
            return (FARPROC)Hook_xrNegotiateLoaderRuntimeInterface;
        }
        if (strcmp(lpProcName, "xrGetInstanceProcAddr") == 0 && res) {
            UVOSuit::g_real_get_instance_proc_addr = (PFN_xrGetInstanceProcAddr)res;
            return (FARPROC)Hook_xrGetInstanceProcAddr;
        }
    }
    return res;
}// =========================================================================
// CONFIGURATION I/O PATHS & PARSERS
// =========================================================================
std::string GetGlobalIniPath() {
    if (const char* appdata = std::getenv("APPDATA")) {
        return std::string(appdata) + "\\UEVR\\UVOSuit_Global.ini";
    }
    return "UVOSuit_Global.ini";
}

std::string GetLocalIniPath() {
    return API::get()->get_persistent_dir(L"UVOSuit_Local.ini").string();
}

namespace ConfigHelper {
    void LoadFloat(const std::string& path, const char* section, const char* key, float& out_val) {
        char buf[32];
        if (GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path.c_str())) {
            try { out_val = std::stof(buf); }
            catch (...) {}
        }
    }
    void LoadBool(const std::string& path, const char* section, const char* key, bool& out_val) {
        char buf[8];
        if (GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path.c_str())) {
            out_val = (std::string(buf) == "1");
        }
    }
    void SaveFloat(const std::string& path, const char* section, const char* key, float val) {
        WritePrivateProfileStringA(section, key, std::to_string(val).c_str(), path.c_str());
    }
    void SaveBool(const std::string& path, const char* section, const char* key, bool val) {
        WritePrivateProfileStringA(section, key, val ? "1" : "0", path.c_str());
    }
}

// =========================================================================
// CORE MATH TEMPLATES
// =========================================================================
template <typename T>
void ApplyConvergenceMath(T& pitch, T& yaw, T& roll, float boost_angle, bool is_left_eye) {
    const T boost_rad = static_cast<T>(boost_angle) * static_cast<T>(PI_D / 180.0);
    const T A = is_left_eye ? boost_rad : -boost_rad;

    const T p = pitch * static_cast<T>(PI_D / 180.0);
    const T y = yaw * static_cast<T>(PI_D / 180.0);
    const T r = roll * static_cast<T>(PI_D / 180.0);

    const T cp = std::cos(p), sp = std::sin(p);
    const T cy = std::cos(y), sy = std::sin(y);
    const T cr = std::cos(r), sr = std::sin(r);

    const T fwd_x = cp * cy;
    const T fwd_y = cp * sy;
    const T fwd_z = sp;

    const T right_x = sr * sp * cy - cr * sy;
    const T right_y = sr * sp * sy + cr * cy;
    const T right_z = -sr * cp;

    const T up_z = cr * cp;

    const T cosA = std::cos(A), sinA = std::sin(A);
    const T new_fwd_x = fwd_x * cosA + right_x * sinA;
    const T new_fwd_y = fwd_y * cosA + right_y * sinA;
    const T new_fwd_z = std::clamp(fwd_z * cosA + right_z * sinA, static_cast<T>(-1.0), static_cast<T>(1.0));
    const T new_right_z = right_z * cosA - fwd_z * sinA;

    pitch = std::asin(new_fwd_z) * static_cast<T>(180.0 / PI_D);
    yaw = std::atan2(new_fwd_y, new_fwd_x) * static_cast<T>(180.0 / PI_D);
    roll = std::atan2(-new_right_z, up_z) * static_cast<T>(180.0 / PI_D);
}

// =========================================================================
// MAIN PLUGIN CLASS
// =========================================================================
class UVOSuitPlugin : public uevr::Plugin {
public:
    UVOSuitPlugin() = default;

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
        }
    }

    void on_initialize() override {
        ImGui::CreateContext();
        load_configs();
        UVOSuit::g_Config.actual_convergence_angle = UVOSuit::g_Config.convergence_angle;
    }

    void on_present() override {
        std::scoped_lock _{ m_imgui_mutex };
        if (!m_initialized) { if (!initialize_imgui()) return; }

        const auto renderer_data = API::get()->param()->renderer;
        const auto vr_active = API::get()->param()->vr->is_hmd_active();

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            if (!vr_active) ImGui_ImplDX11_NewFrame();
            g_d3d11.render_imgui();
        }
        else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            auto cmd_queue = (ID3D12CommandQueue*)renderer_data->command_queue;
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
        if (!m_initialized || !API::get()->param()->vr->is_hmd_active()) return;
        std::scoped_lock _{ m_imgui_mutex };
        ImGui_ImplDX11_NewFrame();
        g_d3d11.render_imgui_vr(context, rtv);
    }

    void on_post_render_vr_framework_dx12(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* rt, D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {
        if (!m_initialized || !API::get()->param()->vr->is_hmd_active()) return;
        std::scoped_lock _{ m_imgui_mutex };
        ImGui_ImplDX12_NewFrame();
        g_d3d12.render_imgui_vr(cmd_list, rtv);
    }

    bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        return !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard;
    }

    void on_pre_engine_tick(API::UGameEngine* engine, float delta) override {
        // Smooth interpolation for 3D convergence to prevent eye strain
        if (UVOSuit::g_Config.actual_convergence_angle != UVOSuit::g_Config.convergence_angle) {
            const float transition_speed = 3.0f;
            UVOSuit::g_Config.actual_convergence_angle += (UVOSuit::g_Config.convergence_angle - UVOSuit::g_Config.actual_convergence_angle) * std::min(1.0f, transition_speed * delta);

            if (std::abs(UVOSuit::g_Config.actual_convergence_angle - UVOSuit::g_Config.convergence_angle) < 0.005f) {
                UVOSuit::g_Config.actual_convergence_angle = UVOSuit::g_Config.convergence_angle;
            }
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
        if (!UVOSuit::g_Config.mod_enabled || !UVOSuit::g_Config.enable_3d_boost || UVOSuit::g_Config.actual_convergence_angle == 0.0f) return;

        const bool is_left_eye = is_double ? (view_index == 0) : (view_index == 1);
        const bool is_right_eye = is_double ? (view_index == 1) : (view_index == 2);

        if (!is_left_eye && !is_right_eye) return;

        const float actual_boost_angle = (UVOSuit::g_Config.actual_convergence_angle / 100.0f) * 10.0f;

        if (is_double) {
            auto rot_d = (UEVR_Rotatord*)rotation;
            ApplyConvergenceMath(rot_d->pitch, rot_d->yaw, rot_d->roll, actual_boost_angle, is_left_eye);
        }
        else {
            auto rot_f = (UEVR_Rotatorf*)rotation;
            ApplyConvergenceMath(rot_f->pitch, rot_f->yaw, rot_f->roll, actual_boost_angle, is_left_eye);
        }
    }

private:
    HWND m_wnd{};
    bool m_initialized{ false };
    bool m_display_menu = true;
    std::recursive_mutex m_imgui_mutex{};

    void handle_hotkeys() {
        if (GetForegroundWindow() != m_wnd) return;

        static bool f2_down = false, f3_down = false, end_down = false;

        if (GetAsyncKeyState(VK_F2) & 0x8000) {
            if (!f2_down) { m_display_menu = !m_display_menu; f2_down = true; }
        }
        else f2_down = false;

        if (GetAsyncKeyState(VK_F3) & 0x8000) {
            if (!f3_down) {
                UVOSuit::g_Config.enable_custom_fov = !UVOSuit::g_Config.enable_custom_fov;
                save_configs();
                f3_down = true;
            }
        }
        else f3_down = false;

        if (GetAsyncKeyState(VK_END) & 0x8000) {
            if (!end_down) {
                UVOSuit::g_Config.enable_3d_boost = !UVOSuit::g_Config.enable_3d_boost;
                save_configs();
                end_down = true;
            }
        }
        else end_down = false;

        if (UVOSuit::g_Config.mod_enabled && UVOSuit::g_Config.enable_3d_boost) {
            const bool is_shift_pressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const float step = is_shift_pressed ? 0.2f : 0.02f;
            bool value_changed = false;

            if (GetAsyncKeyState(VK_PRIOR) & 0x8000) { UVOSuit::g_Config.convergence_angle += step; value_changed = true; }
            if (GetAsyncKeyState(VK_NEXT) & 0x8000) { UVOSuit::g_Config.convergence_angle -= step; value_changed = true; }

            if (value_changed) {
                const float max_limit = UVOSuit::g_Config.unlock_limits ? 100.0f : 15.0f;
                UVOSuit::g_Config.convergence_angle = std::clamp(UVOSuit::g_Config.convergence_angle, 0.0f, max_limit);
                save_configs();
            }
        }
    }

    void load_configs() {
        const std::string local = GetLocalIniPath();
        ConfigHelper::LoadBool(local, "State", "ModEnabled", UVOSuit::g_Config.mod_enabled);
        ConfigHelper::LoadBool(local, "State", "CustomFovEnabled", UVOSuit::g_Config.enable_custom_fov);
        ConfigHelper::LoadBool(local, "State", "3DBoostEnabled", UVOSuit::g_Config.enable_3d_boost);

        ConfigHelper::LoadBool(local, "State", "UIFovOpen", UVOSuit::g_Config.ui_fov_opened);
        ConfigHelper::LoadBool(local, "State", "UIAsymOpen", UVOSuit::g_Config.ui_asym_opened);
        ConfigHelper::LoadBool(local, "State", "UIShiftsOpen", UVOSuit::g_Config.ui_shifts_opened);
        ConfigHelper::LoadBool(local, "State", "UIRotOpen", UVOSuit::g_Config.ui_rot_opened);

        ConfigHelper::LoadFloat(local, "3DBoost", "Convergence", UVOSuit::g_Config.convergence_angle);
        ConfigHelper::LoadBool(local, "3DBoost", "UnlockLimits", UVOSuit::g_Config.unlock_limits);

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

        // Initialize cache on startup
        UVOSuit::g_Cache.Update(UVOSuit::g_Config);
    }

    void save_configs() {
        const std::string local = GetLocalIniPath();
        ConfigHelper::SaveBool(local, "State", "ModEnabled", UVOSuit::g_Config.mod_enabled);
        ConfigHelper::SaveBool(local, "State", "CustomFovEnabled", UVOSuit::g_Config.enable_custom_fov);
        ConfigHelper::SaveBool(local, "State", "3DBoostEnabled", UVOSuit::g_Config.enable_3d_boost);

        ConfigHelper::SaveBool(local, "State", "UIFovOpen", UVOSuit::g_Config.ui_fov_opened);
        ConfigHelper::SaveBool(local, "State", "UIAsymOpen", UVOSuit::g_Config.ui_asym_opened);
        ConfigHelper::SaveBool(local, "State", "UIShiftsOpen", UVOSuit::g_Config.ui_shifts_opened);
        ConfigHelper::SaveBool(local, "State", "UIRotOpen", UVOSuit::g_Config.ui_rot_opened);

        ConfigHelper::SaveFloat(local, "3DBoost", "Convergence", UVOSuit::g_Config.convergence_angle);
        ConfigHelper::SaveBool(local, "3DBoost", "UnlockLimits", UVOSuit::g_Config.unlock_limits);

        const std::string global = GetGlobalIniPath();
        std::filesystem::create_directories(std::filesystem::path(global).parent_path());

        ConfigHelper::SaveFloat(global, "Optics", "GlobalFov", UVOSuit::g_Config.global_fov_scale);
        ConfigHelper::SaveFloat(global, "Optics", "OutL", UVOSuit::g_Config.outer_fov_scale_left);
        ConfigHelper::SaveFloat(global, "Optics", "OutR", UVOSuit::g_Config.outer_fov_scale_right);
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
            save_configs();
        }

        ImGui::PopStyleColor(4);
    }

    bool initialize_imgui() {
        if (m_initialized) return true;
        std::scoped_lock _{ m_imgui_mutex };
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 8.0f;
        style.FrameRounding = 4.0f;
        style.WindowBorderSize = 1.0f;

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
        DXGI_SWAP_CHAIN_DESC swap_desc{};
        ((IDXGISwapChain*)renderer_data->swapchain)->GetDesc(&swap_desc);
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

        // Reactive UI Helper: Handles rendering, instant memory cache updates, and deferred disk I/O
        auto DrawSettingRow = [&](const char* reset_id, const char* label, float* v, float v_min, float v_max, float default_val) {
            if (DrawResetBtn(reset_id)) {
                *v = default_val;
                UVOSuit::g_Cache.Update(UVOSuit::g_Config);
                save_configs();
            }
            ImGui::SameLine();

            // DragFloat updates the reference pointer directly in memory
            if (ImGui::DragFloat(label, v, 0.001f, v_min, v_max, "%.3f")) {
                // Instantly update the optimization cache for smooth VR rendering
                UVOSuit::g_Cache.Update(UVOSuit::g_Config);
            }

            // Commit to HDD/SSD only after user interaction completes
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                save_configs();
            }
            };

        if (ImGui::Begin("Ultimate VR Optics Suite", &m_display_menu, ImGuiWindowFlags_AlwaysAutoResize)) {

            // Main Toggle Button
            if (UVOSuit::g_Config.mod_enabled) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.18f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.22f, 0.28f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 0.98f, 1.0f));
                if (ImGui::Button("Disable UVOSuit", ImVec2(-1, 0))) {
                    UVOSuit::g_Config.mod_enabled = false;
                    save_configs();
                }
                ImGui::PopStyleColor(3);
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.78f, 0.47f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.38f, 0.85f, 0.54f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.09f, 0.16f, 1.0f));
                if (ImGui::Button("Enable UVOSuit", ImVec2(-1, 0))) {
                    UVOSuit::g_Config.mod_enabled = true;
                    save_configs();
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::Spacing();

            // 3D Depth Boost Category
            if (UVOSuit::g_Config.mod_enabled) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.82f, 0.38f, 1.0f));
                ImGui::Text("3D Depth Boost");
                ImGui::PopStyleColor();

                DrawDynamicCheckbox("Enable 3D Depth Boost [End]", &UVOSuit::g_Config.enable_3d_boost);

                const bool was_unlocked = UVOSuit::g_Config.unlock_limits;
                DrawDynamicCheckbox("Unlock Extreme Limits (>15%)", &UVOSuit::g_Config.unlock_limits);

                // Enforce safety limits dynamically
                if (was_unlocked && !UVOSuit::g_Config.unlock_limits) {
                    if (UVOSuit::g_Config.convergence_angle > 15.0f) {
                        UVOSuit::g_Config.convergence_angle = 15.0f;
                        save_configs();
                    }
                }

                DrawSettingRow(" R ##Boost", "Strength %", &UVOSuit::g_Config.convergence_angle, 0.0f, UVOSuit::g_Config.unlock_limits ? 100.0f : 15.0f, 0.0f);

                ImGui::Spacing(); ImGui::Separator();

                // Custom FOV Main Tree
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.82f, 0.38f, 1.0f));
                ImGui::SetNextItemOpen(UVOSuit::g_Config.ui_fov_opened, ImGuiCond_Always);
                const bool fov_curr = ImGui::TreeNodeEx("Custom FOV");
                ImGui::PopStyleColor();

                if (fov_curr != UVOSuit::g_Config.ui_fov_opened) {
                    UVOSuit::g_Config.ui_fov_opened = fov_curr;
                    save_configs();
                }

                if (UVOSuit::g_Config.ui_fov_opened) {
                    DrawDynamicCheckbox("Enable Custom FOV / Shifts [F3]", &UVOSuit::g_Config.enable_custom_fov);

                    DrawSettingRow(" R ##GlobalFOV", "Global Scale", &UVOSuit::g_Config.global_fov_scale, 0.72f, 1.0f, 1.0f);

                    ImGui::Spacing();

                    // Asymmetric FOV Scaling Tree
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.78f, 0.49f, 1.0f));
                    ImGui::SetNextItemOpen(UVOSuit::g_Config.ui_asym_opened, ImGuiCond_Always);
                    const bool asym_curr = ImGui::TreeNode("Asymmetric FOV Scaling");
                    ImGui::PopStyleColor();

                    if (asym_curr != UVOSuit::g_Config.ui_asym_opened) {
                        UVOSuit::g_Config.ui_asym_opened = asym_curr;
                        save_configs();
                    }
                    if (UVOSuit::g_Config.ui_asym_opened) {
                        if (ImGui::TreeNode("Outer Edge (Temples)")) {
                            DrawSettingRow(" R ##OutL", "Left Eye##OutL", &UVOSuit::g_Config.outer_fov_scale_left, 0.72f, 1.0f, 1.0f);
                            DrawSettingRow(" R ##OutR", "Right Eye##OutR", &UVOSuit::g_Config.outer_fov_scale_right, 0.72f, 1.0f, 1.0f);
                            ImGui::TreePop();
                        }
                        if (ImGui::TreeNode("Inner Edge (Nose)")) {
                            DrawSettingRow(" R ##InnL", "Left Eye##InnL", &UVOSuit::g_Config.inner_fov_scale_left, 0.72f, 1.0f, 1.0f);
                            DrawSettingRow(" R ##InnR", "Right Eye##InnR", &UVOSuit::g_Config.inner_fov_scale_right, 0.72f, 1.0f, 1.0f);
                            ImGui::TreePop();
                        }
                        if (ImGui::TreeNode("Upper Edge (Top)")) {
                            DrawSettingRow(" R ##UpL", "Left Eye##UpL", &UVOSuit::g_Config.upper_fov_scale_left, 0.72f, 1.0f, 1.0f);
                            DrawSettingRow(" R ##UpR", "Right Eye##UpR", &UVOSuit::g_Config.upper_fov_scale_right, 0.72f, 1.0f, 1.0f);
                            ImGui::TreePop();
                        }
                        if (ImGui::TreeNode("Lower Edge (Bottom)")) {
                            DrawSettingRow(" R ##LowL", "Left Eye##LowL", &UVOSuit::g_Config.lower_fov_scale_left, 0.72f, 1.0f, 1.0f);
                            DrawSettingRow(" R ##LowR", "Right Eye##LowR", &UVOSuit::g_Config.lower_fov_scale_right, 0.72f, 1.0f, 1.0f);
                            ImGui::TreePop();
                        }
                        ImGui::TreePop();
                    }

                    // Optical Center Shift Tree
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.78f, 0.49f, 1.0f));
                    ImGui::SetNextItemOpen(UVOSuit::g_Config.ui_shifts_opened, ImGuiCond_Always);
                    const bool shifts_curr = ImGui::TreeNode("Optical Center Shift");
                    ImGui::PopStyleColor();

                    if (shifts_curr != UVOSuit::g_Config.ui_shifts_opened) {
                        UVOSuit::g_Config.ui_shifts_opened = shifts_curr;
                        save_configs();
                    }
                    if (UVOSuit::g_Config.ui_shifts_opened) {
                        DrawSettingRow(" R ##FBV", "Global Vert (deg)##FB", &UVOSuit::g_Config.flat_box_vert, -45.0f, 45.0f, 0.0f);
                        DrawSettingRow(" R ##FBH", "Global Horiz (deg)##FB", &UVOSuit::g_Config.flat_box_horiz, -45.0f, 45.0f, 0.0f);

                        ImGui::Spacing();
                        DrawSettingRow(" R ##FLV", "Left Vert##FL", &UVOSuit::g_Config.flat_left_vert, -45.0f, 45.0f, 0.0f);
                        DrawSettingRow(" R ##FLH", "Left Horiz##FL", &UVOSuit::g_Config.flat_left_horiz, -45.0f, 45.0f, 0.0f);

                        ImGui::Spacing();
                        DrawSettingRow(" R ##FRV", "Right Vert##FR", &UVOSuit::g_Config.flat_right_vert, -45.0f, 45.0f, 0.0f);
                        DrawSettingRow(" R ##FRH", "Right Horiz##FR", &UVOSuit::g_Config.flat_right_horiz, -45.0f, 45.0f, 0.0f);
                        ImGui::TreePop();
                    }

                    // Optical Axis Rotation Tree
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.78f, 0.49f, 1.0f));
                    ImGui::SetNextItemOpen(UVOSuit::g_Config.ui_rot_opened, ImGuiCond_Always);
                    const bool rot_curr = ImGui::TreeNode("Optical Axis Rotation");
                    ImGui::PopStyleColor();

                    if (rot_curr != UVOSuit::g_Config.ui_rot_opened) {
                        UVOSuit::g_Config.ui_rot_opened = rot_curr;
                        save_configs();
                    }
                    if (UVOSuit::g_Config.ui_rot_opened) {
                        DrawSettingRow(" R ##RGP", "Global Pitch##RG", &UVOSuit::g_Config.rot_global_pitch, -45.0f, 45.0f, 0.0f);
                        DrawSettingRow(" R ##RGY", "Global Yaw##RG", &UVOSuit::g_Config.rot_global_yaw, -45.0f, 45.0f, 0.0f);

                        ImGui::Spacing();
                        DrawSettingRow(" R ##RLP", "Left Pitch##RL", &UVOSuit::g_Config.rot_left_pitch, -45.0f, 45.0f, 0.0f);
                        DrawSettingRow(" R ##RLY", "Left Yaw##RL", &UVOSuit::g_Config.rot_left_yaw, -45.0f, 45.0f, 0.0f);

                        ImGui::Spacing();
                        DrawSettingRow(" R ##RRP", "Right Pitch##RR", &UVOSuit::g_Config.rot_right_pitch, -45.0f, 45.0f, 0.0f);
                        DrawSettingRow(" R ##RRY", "Right Yaw##RR", &UVOSuit::g_Config.rot_right_yaw, -45.0f, 45.0f, 0.0f);
                        ImGui::TreePop();
                    }
                    ImGui::TreePop();
                }
            }

            ImGui::Spacing();
            if (UVOSuit::g_hook_fire_count > 0) {
                ImGui::TextColored(ImVec4(0.31f, 0.78f, 0.47f, 1.0f), "Status: %s", UVOSuit::g_hook_status_msg.c_str());
            }
            else {
                ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.14f, 1.0f), "Status: %s", UVOSuit::g_hook_status_msg.c_str());
            }
        }
        ImGui::End();
    }
};

std::unique_ptr<UVOSuitPlugin> g_plugin{ new UVOSuitPlugin() };