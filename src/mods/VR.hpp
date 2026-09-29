#pragma once

#include <algorithm>   // [POST_PASS 28.09.2026] std::clamp
#include <chrono>
#include <cmath>   // [29.09.2026] std::exp2 fuer post_brightness
#include <bitset>
#include <memory>
#include <shared_mutex>
#include <atomic>

#include <openvr.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>

#include "sdk/GameIdentity.hpp"
#include "utility/Patch.hpp"
#include "sdk/Math.hpp"
#include "sdk/helpers/NativeObject.hpp"
#include "sdk/Renderer.hpp"
#include "vr/D3D11Component.hpp"
#include "vr/D3D12Component.hpp"
#include "vr/OverlayComponent.hpp"
#include "vr/runtimes/OpenXR.hpp"
#include "vr/runtimes/OpenVR.hpp"

#include "Mod.hpp"

class REManagedObject;

// [AFW 29.09.2026] PureDarks Frame-Warp-Plugin; der Header selbst nur in VR.cpp
// (er zieht "using namespace pd" nach sich).
namespace pd { struct D3D12RendererAPI; }

class VR : public Mod {
public:
    static std::shared_ptr<VR>& get();

    std::string_view get_name() const override { return "VR"; }

    // Called when the mod is initialized
    std::optional<std::string> on_initialize_d3d_thread() override;

    // [AFW 29.09.2026] nullptr = Plugin fehlt/Dummy -> AFW nie benutzbar.
    pd::D3D12RendererAPI* afw_renderer() const { return m_afw_renderer; }
    void update_afw_camera_data();
    // [AFW_SONDE 29.09.2026] Stimmt die Kamera beim Present mit der des gerenderten Frames
    // ueberein? Snapshot in on_pre_end_rendering, Vergleich in update_afw_camera_data.
    // Lua liest per vrmod:get_afw_probe() (Zaehler seit letztem Abruf, danach 0).
    Matrix4x4f m_afw_end_cam{glm::identity<Matrix4x4f>()};
    std::atomic<int> m_afw_end_frame{-1};
    std::atomic<uint32_t> m_afw_probe_frames{0}, m_afw_probe_diff{0}, m_afw_probe_eye_mismatch{0}, m_afw_probe_frame_gap{0};
    std::atomic<float> m_afw_probe_max_deg{0.0f}, m_afw_probe_max_mm{0.0f};
    // [AFW_SONDE2 29.09.2026] Kommen die Spiel-MVs in unserer Textur an?
    std::atomic<uint32_t> m_afw_mv_copied{0}, m_afw_mv_mismatch{0}, m_afw_mv_none{0}, m_afw_mv_notex{0};
    std::atomic<uint32_t> m_afw_mv_game_w{0}, m_afw_mv_game_h{0}, m_afw_mv_game_fmt{0};
    std::string get_afw_probe();

    void on_lua_state_created(sol::state& lua) override;

    void on_pre_imgui_frame() override;
    void on_present() override;
    void on_post_present() override;
    void on_update_transform(RETransform* transform) override;
    void on_update_camera_controller(RopewayPlayerCameraController* controller) override;
    bool on_pre_gui_draw_element(REComponent* gui_element, void* primitive_context) override;
    void on_gui_draw_element(REComponent* gui_element, void* primitive_context) override;
    void on_pre_update_before_lock_scene(void* ctx) override;
    void on_pre_lightshaft_draw(void* shaft, void* render_context) override;
    void on_lightshaft_draw(void* shaft, void* render_context) override;

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    void on_draw_ui() override;
    void on_device_reset() override;

    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;

    // Application entries
    void on_pre_update_hid(void* entry);
    void on_update_hid(void* entry);
    void on_pre_begin_rendering(void* entry);
    void on_begin_rendering(void* entry);
    void on_pre_end_rendering(void* entry);
    void on_end_rendering(void* entry);
    void on_pre_wait_rendering(void* entry);
    void on_wait_rendering(void* entry);

    template<typename T = VRRuntime>
    T* get_runtime() const {
        return (T*)m_runtime.get();
    }

    auto get_hmd() const {
        return m_openvr->hmd;
    }

    auto& get_openvr_poses() const {
        return m_openvr->render_poses;
    }

    auto get_hmd_width() const {
        return get_runtime()->get_width();
    }

    auto get_hmd_height() const {
        return get_runtime()->get_height();
    }

    auto get_last_controller_update() const {
        return m_last_controller_update;
    }

    int32_t get_frame_count() const;
    int32_t get_game_frame_count() const;

    // [ONI_RENDER] AFR-Schalter fuer die Menue-Kategorie RENDERING (gleicher Config-Wert)
    bool& afr_value() { return m_use_afr->value(); }
    // [AFW 29.09.2026] AFW = AFR-Takt + Warp des anderen Auges (PDAFWPlugin.dll).
    bool& afw_value() { return m_use_afw->value(); }

    // [DLSS_PRO_AUGE 28.09.2026 -- uebernommen von PureDark, RE9-Fork "Fix Upscalers
    // Wobbling"] Das Spiel-DLSS/FSR hat nur EINE Instanz; bei abwechselnden Augen
    // mischt sie die Historie beider Augen -> Brei. Wir legen beim Erzeugen eine
    // zweite Instanz an und leiten jeden Aufruf je nach Auge um; die Bewegungs-
    // vektoren bekommen die alte Matrix DESSELBEN Auges.
    bool& fix_upscalers_value() { return m_fix_upscalers_wobbling->value(); }
    bool is_fix_dlss() const { return m_fix_upscalers_wobbling->value(); }
    // [RENDER-AUGE 29.09.2026] War m_frame_count: den erhoeht der Hauptthread schon fuer
    // den NAECHSTEN Frame, waehrend DLSS noch den vorigen rendert -> im Spiel wechselnd
    // falsches Auge, beide Instanzen bekamen gemischte Augen (Fix wirkte nur bei offenem
    // Menue, User 29.09.). m_render_frame_count ist das Auge des gerade gerenderten
    // Frames -- dieselbe Quelle, die auch die Augenbild-Abgabe benutzt.
    // [ZURUECK AUF EINBAU-FASSUNG 29.09.2026 -- User] Augenwahl wieder ueber m_frame_count
    // (Render-Frame- und Projektions-Varianten brachten keine Verbesserung).
    int afr_eye() const { return (m_frame_count % 2 == m_left_eye_interval) ? 0 : 1; }
    // [AUGE FESTHALTEN 29.09.2026] Auge, mit dem die Bewegungsvektoren DIESES Frames
    // gesetzt wurden (on_scene_layer_update). Die DLSS-Auswertung desselben Frames nimmt
    // genau dieses -- sonst konnte m_frame_count dazwischen schon weitergezaehlt sein
    // (Flackern, weg bei offenem ImGui = anderes Timing; User 29.09.).
    std::atomic<int> m_dlss_eye{-1};
    int dlss_eye() const { const int e = m_dlss_eye.load(); return e >= 0 ? e : afr_eye(); }
    int dlss_frame_count() const { return m_frame_count; }
    void* vrDLSSHandle[2]{nullptr, nullptr};   // NVSDK_NGX_Handle*
    void* vrContexts[2]{nullptr, nullptr};     // ffxContext
    // Frame, in dem zuletzt ein Upscaler-Aufruf ueber UNSERE Pro-Auge-Instanz lief.
    // Nur dann wird die Bewegungsvektor-Korrektur angewandt -- ohne Upscaler bleibt
    // der alte TAA-Fix (sonst Geisterbilder mit dem Spiel-TAA).
    std::atomic<int> m_upscaler_eye_frame{-1000};
    // [DLSS_SONDE 29.09.2026] nur zaehlen, Lua liest per vrmod:get_dlss_probe()
    std::atomic<uint32_t> m_dlss_eval_total{0}, m_dlss_eval_eye[2]{}, m_dlss_eval_other{0},
        m_dlss_eval_fixoff{0}, m_dlss_eval_nosecond{0}, m_dlss_creates{0}, m_dlss_releases{0};
    std::atomic<uint64_t> m_dlss_eye_seq{0};   // letzte 16 Augen der Evaluates als Bits
    std::string get_dlss_probe();
    bool upscaler_per_eye_active() const { return is_fix_dlss() && (m_frame_count - m_upscaler_eye_frame.load()) < 10; }
    void install_upscaler_hooks();

    // [ONI_FARBE 28.09.2026] Engine-Bildregler (via.render.ToneMapping der Spielkamera)
    // fuer die Menue-Kategorie RENDERING. Default = Wert des Spiels; steht ein Regler
    // auf dem Default, wird nichts geschrieben (das Spiel behaelt seine Werte).
    ModSlider& oni_contrast() { return *m_oni_contrast; }
    ModSlider& oni_shadow_contrast() { return *m_oni_shadow_contrast; }
    ModSlider& oni_sharpness() { return *m_oni_sharpness; }
    ModSlider& oni_brightness() { return *m_oni_brightness; }
    ModSlider& oni_gamma() { return *m_oni_gamma; }
    bool& oni_volumetric_fog() { return m_oni_volumetric_fog->value(); }
    bool& oni_ldr_postprocess() { return m_oni_ldr_postprocess->value(); }

    // [POST_PASS 28.09.2026] Eigene Nachbearbeitung auf dem Augenbild (portiert aus
    // dem RE4-Fork). Menue ganze Stufen: Schaerfe 0..10 (0 = aus) -> 0.0..3.0,
    // Saettigung -10..10 (0 = neutral) -> 0.0..2.0, SMAA an/aus.
    int32_t& post_sharpness_step() { return m_post_sharpness->value(); }
    int32_t& post_saturation_step() { return m_post_saturation->value(); }
    bool& post_smaa() { return m_post_smaa->value(); }
    float post_sharpness() const { return (float)std::clamp(m_post_sharpness->value(), 0, 10) * 0.3f; }
    float post_saturation() const { return (float)(std::clamp(m_post_saturation->value(), -10, 10) + 10) * 0.1f; }
    // [BRIGHTNESS/CONTRAST 29.09.2026 -- wie RE4-Fork] Menue -10..10, 0 = neutral.
    int32_t& post_brightness_step() { return m_post_brightness->value(); }
    int32_t& post_contrast_step() { return m_post_contrast->value(); }
    float post_brightness() const { return std::exp2(-(float)std::clamp(m_post_brightness->value(), -10, 10) * 0.1f); }
    float post_contrast() const { return 1.0f + (float)std::clamp(m_post_contrast->value(), -10, 10) * 0.05f; }

    // [AFW 29.09.2026] Nur wirksam mit AFR und geladenem Plugin.
    bool is_using_afw() const {
        return m_use_afr->value() && m_use_afw->value() && m_afw_renderer != nullptr;
    }

    bool is_using_afr() const {
        return m_use_afr->value();
    }

    // Functions that generally use a mutex or have more complex logic
    float get_standing_height();
    Vector4f get_standing_origin();
    void set_standing_origin(const Vector4f& origin);

    glm::quat get_rotation_offset();
    void set_rotation_offset(const glm::quat& offset);
    void recenter_view();

    glm::quat get_gui_rotation_offset();
    void set_gui_rotation_offset(const glm::quat& offset);
    void recenter_gui(const glm::quat& from);

    Vector4f get_current_offset();

    Matrix4x4f get_current_eye_transform(bool flip = false);
    Matrix4x4f get_current_projection_matrix(bool flip = false);

    auto& get_controllers() const {
        return m_controllers;
    }

    bool is_using_controllers() const {
        return !m_controllers.empty() && (std::chrono::steady_clock::now() - m_last_controller_update) <= std::chrono::seconds((int32_t)m_motion_controls_inactivity_timer->value());
    }

    bool is_hmd_active() const {
        return get_runtime()->ready();
    }
    
    bool is_openvr_loaded() const {
        return m_openvr != nullptr && m_openvr->loaded;
    }

    bool is_openxr_loaded() const {
        return m_openxr != nullptr && m_openxr->loaded;
    }

    bool is_using_hmd_oriented_audio() {
        return m_hmd_oriented_audio->value();
    }

    void toggle_hmd_oriented_audio() {
        m_hmd_oriented_audio->toggle();
    }

    const Matrix4x4f& get_last_render_matrix() {
        return m_render_camera_matrix;
    }

    Vector4f get_position(uint32_t index)  const;
    Vector4f get_velocity(uint32_t index)  const;
    Vector4f get_angular_velocity(uint32_t index)  const;
    Matrix4x4f get_rotation(uint32_t index)  const;
    Matrix4x4f get_transform(uint32_t index) const;
    vr::HmdMatrix34_t get_raw_transform(uint32_t index) const;

    const auto& get_eyes() const {
        return get_runtime()->eyes;
    }

    void apply_hmd_transform(glm::quat& rotation, Vector4f& position);
    void apply_hmd_transform(::REJoint* camera_joint);
    
    bool is_hand_behind_head(VRRuntime::Hand hand, float sensitivity = 0.2f) const;
    bool is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle) const;
    Vector2f get_joystick_axis(vr::VRInputValueHandle_t handle) const;

    Vector2f get_left_stick_axis() const;
    Vector2f get_right_stick_axis() const;

    // [ONI_MENU 26.09.2026, aus dem RE9-Fork] Solange das Menue offen ist (und kurz
    // danach, bis alles losgelassen ist), liefern is_action_active und
    // get_*_stick_axis NICHTS -- die zentrale Stelle, ueber die alle Module und Lua
    // die Controller lesen. Das Menue selbst liest ueber die _raw-Varianten.
    bool is_action_active_raw(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle) const;
    bool is_menu_input_blocked() const;
    void set_menu_release_guard(bool on) { m_menu_release_guard = on; }
    bool is_menu_release_guard() const { return m_menu_release_guard; }
    bool m_menu_release_guard{false};

    Vector2f get_joystick_axis_raw(vr::VRInputValueHandle_t handle) const;
    Vector2f get_left_stick_axis_raw() const;
    Vector2f get_right_stick_axis_raw() const;
    Vector2f get_right_touchpad_axis() const;   // [ONI_MENU] wie RE9: vorerst immer 0

    void trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle);
    
    auto get_action_set() const { return m_action_set; }
    auto& get_active_action_set() const { return m_active_action_set; }
    auto get_action_trigger() const { return m_action_trigger; }
    auto get_action_grip() const { return m_action_grip; }
    auto get_action_joystick() const { return m_action_joystick; }
    auto get_action_joystick_click() const { return m_action_joystick_click; }
    auto get_action_a_button() const { return m_action_a_button; }
    auto get_action_b_button() const { return m_action_b_button; }
    auto get_action_weapon_dial() const { return m_action_weapon_dial; }
    auto get_action_minimap() const { return m_action_minimap; }
    auto get_action_block() const { return m_action_block; }
    auto get_action_dpad_up() const { return m_action_dpad_up; }
    auto get_action_dpad_down() const { return m_action_dpad_down; }
    auto get_action_dpad_left() const { return m_action_dpad_left; }
    auto get_action_dpad_right() const { return m_action_dpad_right; }
    auto get_action_heal() const { return m_action_heal; }
    auto get_left_joystick() const { return m_left_joystick; }
    auto get_right_joystick() const { return m_right_joystick; }

    const auto& get_action_handles() const { return m_action_handles;}

    auto get_ui_scale() const { return m_ui_scale_option->value(); }
    const auto& get_raw_projections() const { return get_runtime()->raw_projections; }

    void unhide_crosshair() {
        m_last_crosshair_hide = std::chrono::steady_clock::now();
    }

private:
    Vector4f get_position_unsafe(uint32_t index) const;
    Vector4f get_velocity_unsafe(uint32_t index) const;
    Vector4f get_angular_velocity_unsafe(uint32_t index) const;

private:
    // Hooks
    void on_view_get_size(REManagedObject* scene_view, float* result) override;
    static void inputsystem_update_hook(void* ctx, REManagedObject* input_system);
    void on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) override;
    static Matrix4x4f* gui_camera_get_projection_matrix_hook(REManagedObject* camera, Matrix4x4f* result);
    void on_camera_get_view_matrix(REManagedObject* camera, Matrix4x4f* result) override;

    bool on_pre_overlay_layer_update(sdk::renderer::layer::Overlay* layer, void* render_context) override;
    bool on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override;

    bool on_pre_post_effect_layer_update(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
    bool on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
    void on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
    uint32_t m_previous_distortion_type{};
    bool m_set_next_post_effect_distortion_type{false};

    bool on_pre_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
    void on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
    bool on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) override;
    bool m_set_next_scene_layer_data{false};

    struct SceneLayerData {
        SceneLayerData() = default;
        SceneLayerData(sdk::renderer::SceneInfo* info) 
            : scene_info(info)
        {
            if (scene_info != nullptr) {
                this->view_projection_matrix = scene_info->view_projection_matrix;
            }
        }

        sdk::renderer::SceneInfo* scene_info{};
        Matrix4x4f view_projection_matrix{};
    };

    std::array<SceneLayerData, 5> m_scene_layer_data {};
    // [DLSS_PRO_AUGE] View-Matrix des letzten Frames je Auge, je SceneInfo-Slot
    // (m_scene_layer_data wird pro Frame neu gebaut, deshalb getrennt gehalten).
    std::array<std::array<Matrix4x4f, 2>, 5> m_old_view_matrix{};
    bool m_ngx_hooked{false};
    bool m_ffx_hooked{false};
    int m_upscaler_hook_try_frame{0};

    static void wwise_listener_update_hook(void* listener);

    //static float get_sharpness_hook(void* tonemapping);

    // initialization functions
    std::optional<std::string> initialize_openvr();
    std::optional<std::string> initialize_openvr_input();
    std::optional<std::string> initialize_openxr();
    std::optional<std::string> initialize_openxr_input();
    std::optional<std::string> initialize_openxr_swapchains();
    std::optional<std::string> hijack_resolution();
    std::optional<std::string> hijack_input();
    std::optional<std::string> hijack_camera();
    std::optional<std::string> hijack_wwise_listeners(); // audio hook

    std::optional<std::string> reinitialize_openvr() {
        spdlog::info("Reinitializing OpenVR");
        std::scoped_lock _{m_openvr_mtx};

        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        m_openvr.reset();

        // Reinitialize openvr input, hopefully this fixes the issue
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openvr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenVR: {}", *e);
        }

        return e;
    }

    std::optional<std::string> reinitialize_openxr() {
        spdlog::info("Reinitializing OpenXR");
        std::scoped_lock _{m_openvr_mtx};

        if (m_is_d3d12) {
            m_d3d12.openxr().destroy_swapchains();
        } else {
            m_d3d11.openxr().destroy_swapchains();
        }

        m_openxr.reset();
        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openxr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenXR: {}", *e);
        }

        return e;
    }

    bool detect_controllers();
    bool is_any_action_down();
    void update_hmd_state();
    void update_action_states();
    void update_camera(); // if not in firstperson mode
    void update_camera_origin(); // every frame
    void update_audio_camera();
    void update_render_matrix();
    void apply_oni_tonemap();   // [ONI_FARBE]
    void restore_audio_camera(); // after wwise listener update
    void restore_camera(); // After rendering
    void set_lens_distortion(bool value);
    void disable_bad_effects();
    void fix_temporal_effects();

    // input functions
    // Purpose: "Emulate" OpenVR input to the game
    // By setting things like input flags based on controller state
    void openvr_input_to_re2_re3(REManagedObject* input_system);
    void openvr_input_to_re_engine(); // generic, can be used on any game

    // Sets overlay layer to return instantly
    // causes world-space gui elements to render properly
    Patch::Ptr m_overlay_draw_patch{};
    
    mutable std::recursive_mutex m_openvr_mtx{};
    mutable std::recursive_mutex m_wwise_mtx{};
    mutable std::shared_mutex m_gui_mtx{};
    mutable std::shared_mutex m_rotation_mtx{};

    vr::VRTextureBounds_t m_right_bounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    vr::VRTextureBounds_t m_left_bounds{ 0.0f, 0.0f, 1.0f, 1.0f };

    glm::vec3 m_overlay_rotation{-1.550f, 0.0f, -1.330f};
    glm::vec4 m_overlay_position{0.0f, 0.06f, -0.07f, 1.0f};

    float m_nearz{ 0.1f };
    float m_farz{ 3000.0f };

    std::shared_ptr<VRRuntime> m_runtime{std::make_shared<VRRuntime>()}; // will point to the real runtime if it exists
    std::shared_ptr<runtimes::OpenVR> m_openvr{std::make_shared<runtimes::OpenVR>()};
    std::shared_ptr<runtimes::OpenXR> m_openxr{std::make_shared<runtimes::OpenXR>()};

    Vector4f m_standing_origin{ 0.0f, 1.5f, 0.0f, 0.0f };
    glm::quat m_rotation_offset{ glm::identity<glm::quat>() };
    glm::quat m_gui_rotation_offset{ glm::identity<glm::quat>() };

    std::vector<int32_t> m_controllers{};
    std::unordered_set<int32_t> m_controllers_set{};

    // Action set handles
    vr::VRActionSetHandle_t m_action_set{};
    vr::VRActiveActionSet_t m_active_action_set{};

    // Action handles
    vr::VRActionHandle_t m_action_trigger{ };
    vr::VRActionHandle_t m_action_grip{ };
    vr::VRActionHandle_t m_action_joystick{};
    vr::VRActionHandle_t m_action_joystick_click{};
    vr::VRActionHandle_t m_action_a_button{};
    vr::VRActionHandle_t m_action_b_button{};
    vr::VRActionHandle_t m_action_dpad_up{};
    vr::VRActionHandle_t m_action_dpad_right{};
    vr::VRActionHandle_t m_action_dpad_down{};
    vr::VRActionHandle_t m_action_dpad_left{};
    vr::VRActionHandle_t m_action_system_button{};
    vr::VRActionHandle_t m_action_weapon_dial{};
    vr::VRActionHandle_t m_action_re3_dodge{};
    vr::VRActionHandle_t m_action_re2_quickturn{};
    vr::VRActionHandle_t m_action_re2_firstperson_toggle{};
    vr::VRActionHandle_t m_action_re2_reset_view{};
    vr::VRActionHandle_t m_action_re2_change_ammo{};
    vr::VRActionHandle_t m_action_re2_toggle_flashlight{};
    vr::VRActionHandle_t m_action_minimap{};
    vr::VRActionHandle_t m_action_block{};
    vr::VRActionHandle_t m_action_haptic{};
    vr::VRActionHandle_t m_action_heal{};

    bool m_was_firstperson_toggle_down{false};
    bool m_was_flashlight_toggle_down{false};
    
    
    std::unordered_map<std::string, std::reference_wrapper<vr::VRActionHandle_t>> m_action_handles {
        { "/actions/default/in/Trigger", m_action_trigger },
        { "/actions/default/in/Grip", m_action_grip },
        { "/actions/default/in/Joystick", m_action_joystick },
        { "/actions/default/in/JoystickClick", m_action_joystick_click },
        { "/actions/default/in/AButton", m_action_a_button },
        { "/actions/default/in/BButton", m_action_b_button },
        { "/actions/default/in/DPad_Up", m_action_dpad_up },
        { "/actions/default/in/DPad_Right", m_action_dpad_right },
        { "/actions/default/in/DPad_Down", m_action_dpad_down },
        { "/actions/default/in/DPad_Left", m_action_dpad_left },
        { "/actions/default/in/SystemButton", m_action_system_button },
        { "/actions/default/in/WeaponDial_Start", m_action_weapon_dial },
        { "/actions/default/in/RE3_Dodge", m_action_re3_dodge },
        { "/actions/default/in/RE2_Quickturn", m_action_re2_quickturn },
        { "/actions/default/in/RE2_FirstPerson_Toggle", m_action_re2_firstperson_toggle },
        { "/actions/default/in/RE2_Reset_View", m_action_re2_reset_view },
        { "/actions/default/in/RE2_Change_Ammo", m_action_re2_change_ammo },
        { "/actions/default/in/RE2_Toggle_Flashlight", m_action_re2_toggle_flashlight },
        { "/actions/default/in/MiniMap", m_action_minimap },
        { "/actions/default/in/Block", m_action_block },
        { "/actions/default/in/Heal", m_action_heal },

        // Out
        { "/actions/default/out/Haptic", m_action_haptic },
    };

    // Input sources
    vr::VRInputValueHandle_t m_left_joystick{};
    vr::VRInputValueHandle_t m_right_joystick{};

    // Input system history
    std::bitset<64> m_button_states_down{};
    std::bitset<64> m_button_states_on{};
    std::bitset<64> m_button_states_up{};
    std::chrono::steady_clock::time_point m_last_controller_update{};
    std::chrono::steady_clock::time_point m_last_interaction_display{};
    std::chrono::steady_clock::time_point m_last_crosshair_hide{};
    uint32_t m_backbuffer_inconsistency_start{};
    std::chrono::nanoseconds m_last_input_delay{};
    std::chrono::nanoseconds m_avg_input_delay{};

    HANDLE m_present_finished_event{CreateEvent(nullptr, TRUE, FALSE, nullptr)};

    Vector4f m_raw_projections[2]{};

    vrmod::D3D11Component m_d3d11{};
    vrmod::D3D12Component m_d3d12{};
    vrmod::OverlayComponent m_overlay_component{};

    Vector4f m_original_camera_position{ 0.0f, 0.0f, 0.0f, 0.0f };
    glm::quat m_original_camera_rotation{ glm::identity<glm::quat>() };

    Matrix4x4f m_original_camera_matrix{ glm::identity<Matrix4x4f>() };

    Vector4f m_original_audio_camera_position{ 0.0f, 0.0f, 0.0f, 0.0f };
    glm::quat m_original_audio_camera_rotation{ glm::identity<glm::quat>() };

    Matrix4x4f m_render_camera_matrix{ glm::identity<Matrix4x4f>() };

    sdk::helpers::NativeObject m_via_hid_gamepad{ "via.hid.GamePad" };

    // options
    int m_frame_count{};
    int m_render_frame_count{};
    int m_last_frame_count{-1};
    int m_left_eye_frame_count{0};
    int m_right_eye_frame_count{0};

    bool m_submitted{false};
    //bool m_disable_sharpening{true};

    bool m_needs_camera_restore{false};
    bool m_needs_audio_restore{false};
    bool m_in_render{false};
    // [AFR_LATCH 29.09.2026] Spielkamera des ersten Auges, gilt fuer das zweite Auge
    Matrix4x4f m_afr_latch_view{ glm::identity<Matrix4x4f>() };
    int m_afr_latch_frame{-1};
    // [ONI_UIBUF] zuletzt gesehener UI-Puffer der Overlay-Schicht (in D3D12Component geleert)
    std::atomic<ID3D12Resource*> m_ui_buffer_tex{nullptr};
    bool m_in_lightshaft{false};
    bool m_positional_tracking{true};
    bool m_is_d3d12{false};
    bool m_backbuffer_inconsistency{false};
    bool m_init_finished{false};
    bool m_has_hw_scheduling{false}; // hardware accelerated GPU scheduling

    // on the backburner
    bool m_depth_aided_reprojection{false};

    // == 1 or == 0
    uint8_t m_left_eye_interval{0};
    uint8_t m_right_eye_interval{1};

    static std::string actions_json;
    static std::string binding_rift_json;
    static std::string bindings_oculus_touch_json;
    static std::string binding_vive;
    static std::string bindings_vive_controller;
    static std::string bindings_knuckles;

    const std::unordered_map<std::string, std::string> m_binding_files {
        { "actions.json", actions_json },
        { "binding_rift.json", binding_rift_json },
        { "bindings_oculus_touch.json", bindings_oculus_touch_json },
        { "binding_vive.json", binding_vive },
        { "bindings_vive_controller.json", bindings_vive_controller },
        { "bindings_knuckles.json", bindings_knuckles }
    };

    const ModKey::Ptr m_set_standing_key{ ModKey::create(generate_name("SetStandingOriginKey")) };
    const ModKey::Ptr m_recenter_view_key{ ModKey::create(generate_name("RecenterViewKey")) };
    const ModToggle::Ptr m_decoupled_pitch{ ModToggle::create(generate_name("DecoupledPitch"), false) };
    pd::D3D12RendererAPI* m_afw_renderer{nullptr};   // [AFW 29.09.2026]

    const ModToggle::Ptr m_use_afw{ ModToggle::create(generate_name("AlternateFrameWarping"), false) };   // [AFW 29.09.2026]
    const ModToggle::Ptr m_use_afr{ ModToggle::create(generate_name("AlternateFrameRendering"), true) };   // [DEFAULT AN 29.09.2026] User: AFR + DLSS-Fix sieht super aus
    const ModToggle::Ptr m_fix_upscalers_wobbling{ ModToggle::create(generate_name("FixUpscalersWobbling"), true) };   // [DLSS_PRO_AUGE]
    const ModToggle::Ptr m_use_custom_view_distance{ ModToggle::create(generate_name("UseCustomViewDistance"), false) };
    const ModToggle::Ptr m_hmd_oriented_audio{ ModToggle::create(generate_name("HMDOrientedAudio"), true) };
    // [ONI_FARBE] Defaults = Werte des Spiels (Sonde oni_render_sonde 28.09.2026)
    const ModSlider::Ptr m_oni_contrast{ ModSlider::create(generate_name("OniContrast"), 0.0f, 1.0f, 0.30f) };
    const ModSlider::Ptr m_oni_shadow_contrast{ ModSlider::create(generate_name("OniShadowContrast"), 0.0f, 2.0f, 0.95f) };
    const ModSlider::Ptr m_oni_sharpness{ ModSlider::create(generate_name("OniSharpness"), 0.0f, 3.0f, 0.0f) };
    // [ONI_FARBE] Brightness/Gamma = ace.cDisplaySettings OutputLowerLimit/Gamma
    // (app.GraphicsManager._AppGraphicsSettingController._DisplaySettings), wirkt nur mit
    // updateRequest() danach (Farbtest 28.09.2026). Defaults = gemessene Spielwerte.
    const ModSlider::Ptr m_oni_brightness{ ModSlider::create(generate_name("OniBrightness"), 0.0f, 0.3f, 0.083f) };
    const ModSlider::Ptr m_oni_gamma{ ModSlider::create(generate_name("OniGamma"), 0.5f, 3.0f, 1.865f) };

    struct OniDisplayParam {
        std::optional<float> orig{};   // Spielwert vor unserem ersten Schreiben
        bool overriding{false};
    };
    OniDisplayParam m_oni_ds_brightness{};
    OniDisplayParam m_oni_ds_gamma{};

    // [ONI_FARBE] Volumetrischer Nebel flackert im HMD -> Default AUS (User 28.09.2026)
    // [DEFAULT AN 29.09.2026 -- User: Engine-Toggles bleiben, erstmal normal = an]
    // Neuer Config-Name (_V2), damit ein gespeichertes "aus" nicht weiter gilt.
    const ModToggle::Ptr m_oni_volumetric_fog{ ModToggle::create(generate_name("OniVolumetricFog_V2"), true) };
    bool m_oni_vfog_forced_off{false};
    // [ONI_FARBE] LDRPostProcess flackert zusammen mit dem Nebel -> Default AUS (User 28.09.2026)
    const ModToggle::Ptr m_oni_ldr_postprocess{ ModToggle::create(generate_name("OniLDRPostProcess_V2"), true) };
    bool m_oni_ldr_forced_off{false};
    // [POST_PASS 28.09.2026] s. post_sharpness()/post_saturation()/post_smaa()
    const ModInt32::Ptr m_post_sharpness{ ModInt32::create(generate_name("PostSharpness"), 0) };
    const ModInt32::Ptr m_post_saturation{ ModInt32::create(generate_name("PostSaturation_V2"), 0) };
    const ModToggle::Ptr m_post_smaa{ ModToggle::create(generate_name("PostSMAA"), false) };
    const ModInt32::Ptr m_post_brightness{ ModInt32::create(generate_name("PostBrightness"), 0) };   // [29.09.2026]
    const ModInt32::Ptr m_post_contrast{ ModInt32::create(generate_name("PostContrast"), 0) };       // [29.09.2026]
    const ModSlider::Ptr m_view_distance{ ModSlider::create(generate_name("CustomViewDistance"), 10.0f, 3000.0f, 500.0f) };
    const ModSlider::Ptr m_motion_controls_inactivity_timer{ ModSlider::create(generate_name("MotionControlsInactivityTimer"), 30.0f, 100.0f, 30.0f) };
    const ModSlider::Ptr m_joystick_deadzone{ ModSlider::create(generate_name("JoystickDeadzone"), 0.01f, 0.9f, 0.15f) };
    const ModSlider::Ptr m_ui_scale_option{ ModSlider::create(generate_name("2DUIScale"), 1.0f, 100.0f, 12.0f) };
    const ModSlider::Ptr m_ui_distance_option{ ModSlider::create(generate_name("2DUIDistance"), 0.01f, 100.0f, 1.3f) };   // [ONI_UIDIST] Default 1.3 statt 1.0 (User 27.09.2026)
    const ModSlider::Ptr m_world_ui_scale_option{ ModSlider::create(generate_name("WorldSpaceUIScale"), 1.0f, 100.0f, 15.0f) };
    const ModSlider::Ptr m_resolution_scale{ ModSlider::create(generate_name("OpenXRResolutionScale"), 0.1f, 5.0f, 1.0f) };

    const ModToggle::Ptr m_force_fps_settings{ ModToggle::create(generate_name("ForceFPS"), true) };
    const ModToggle::Ptr m_force_aa_settings{ ModToggle::create(generate_name("ForceAntiAliasing"), true) };
    const ModToggle::Ptr m_force_motionblur_settings{ ModToggle::create(generate_name("ForceMotionBlur"), true) };
    const ModToggle::Ptr m_force_vsync_settings{ ModToggle::create(generate_name("ForceVSync"), true) };
    const ModToggle::Ptr m_force_lensdistortion_settings{ ModToggle::create(generate_name("ForceLensDistortion"), true) };
    const ModToggle::Ptr m_force_volumetrics_settings{ ModToggle::create(generate_name("ForceVolumetrics"), true) };
    const ModToggle::Ptr m_force_lensflares_settings{ ModToggle::create(generate_name("ForceLensFlares"), true) };
    const ModToggle::Ptr m_force_dynamic_shadows_settings{ ModToggle::create(generate_name("ForceDynamicShadows"), true) };

#ifdef REFRAMEWORK_UNIVERSAL
    const ModToggle::Ptr m_allow_engine_overlays{ ModToggle::create(generate_name("AllowEngineOverlays_V2"), sdk::GameIdentity::get().tdb_ver() < 73) };
#else
#if TDB_VER < 73
    const ModToggle::Ptr m_allow_engine_overlays{ ModToggle::create(generate_name("AllowEngineOverlays_V2"), true) };
#else
    const ModToggle::Ptr m_allow_engine_overlays{ ModToggle::create(generate_name("AllowEngineOverlays_V2"), false) };
#endif
#endif

    const ModToggle::Ptr m_desktop_fix{ ModToggle::create(generate_name("DesktopRecordingFix"), true) };
    const ModToggle::Ptr m_desktop_fix_skip_present{ ModToggle::create(generate_name("DesktopRecordingFixSkipPresent"), true) };

#ifdef REFRAMEWORK_UNIVERSAL
    const ModToggle::Ptr m_enable_asynchronous_rendering{ ModToggle::create(generate_name("AsyncRendering_V3"), sdk::GameIdentity::get().tdb_ver() < 73) };
#else
#if TDB_VER >= 73
    const ModToggle::Ptr m_enable_asynchronous_rendering{ ModToggle::create(generate_name("AsyncRendering_V3"), false) };
#else
    const ModToggle::Ptr m_enable_asynchronous_rendering{ ModToggle::create(generate_name("AsyncRendering_V3"), true) };
#endif
#endif

    bool m_disable_projection_matrix_override{ false };
    bool m_disable_gui_camera_projection_matrix_override{ false };
    bool m_disable_view_matrix_override{false};
    bool m_disable_backbuffer_size_override{false};
    bool m_disable_temporal_fix{false};
    bool m_disable_post_effect_fix{false};

    ValueList m_options{
        *m_set_standing_key,
        *m_recenter_view_key,
        *m_decoupled_pitch,
        *m_use_afr,
        *m_use_afw,   // [AFW 29.09.2026]
        *m_fix_upscalers_wobbling,   // [DLSS_PRO_AUGE]
        *m_oni_contrast,          // [ONI_FARBE]
        *m_oni_shadow_contrast,   // [ONI_FARBE]
        *m_oni_sharpness,         // [ONI_FARBE]
        *m_oni_brightness,        // [ONI_FARBE]
        *m_oni_gamma,             // [ONI_FARBE]
        *m_oni_volumetric_fog,    // [ONI_FARBE]
        *m_oni_ldr_postprocess,   // [ONI_FARBE]
        *m_post_sharpness,        // [POST_PASS]
        *m_post_saturation,       // [POST_PASS]
        *m_post_smaa,             // [POST_PASS]
        *m_post_brightness,       // [29.09.2026]
        *m_post_contrast,         // [29.09.2026]
        *m_use_custom_view_distance,
        *m_hmd_oriented_audio,
        *m_view_distance,
        *m_motion_controls_inactivity_timer,
        *m_joystick_deadzone,
        *m_force_fps_settings,
        *m_force_aa_settings,
        *m_force_motionblur_settings,
        *m_force_vsync_settings,
        *m_force_lensdistortion_settings,
        *m_force_volumetrics_settings,
        *m_force_lensflares_settings,
        *m_force_dynamic_shadows_settings,
        *m_ui_scale_option,
        *m_ui_distance_option,
        *m_world_ui_scale_option,
        *m_allow_engine_overlays,
        *m_resolution_scale,
        *m_desktop_fix,
        *m_desktop_fix_skip_present,
        *m_enable_asynchronous_rendering
    };

    bool m_use_rotation{true};

    friend class vrmod::D3D11Component;
    friend class vrmod::D3D12Component;
    friend class vrmod::OverlayComponent;
};
