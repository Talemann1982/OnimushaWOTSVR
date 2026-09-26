#include "../VR.hpp"

#include "OverlayComponent.hpp"

// ============================================================================
// [ONI_MENU 26.09.2026] 1:1 aus dem RE9-Fork ([MENUE 21.09.2026, nach dem RE4-Fork]). Kopfkommentar: OverlayComponent.hpp.
// Das Original (Flaeche an der linken Hand, Laser, Oeffnen durch Zielen) steht
// in OverlayComponent.cpp.bak_2026-09-26_menu.
// ============================================================================

namespace {
// [VR-MENUE-GROESSE 11.09.2026] Breite der VR-Tafel in Metern, live aus REFramework
// ("Menu Editor"). Frueher die Konstante 0.25.
float panel_width_m() {
    return g_framework->get_vr_menu_panel_width();
}
}

namespace vrmod {
void OverlayComponent::on_reset() {
    m_overlay_data = {};
    m_overlay_width_set = -1.0f;
}

std::optional<std::string> OverlayComponent::on_initialize_openvr() {
    m_overlay_data = {};
    m_overlay_width_set = -1.0f;
    m_overlay_shown = false;
    m_panel_anchored = false;

    auto overlay_error = vr::VROverlay()->CreateOverlay("REFramework", "REFramework", &m_overlay_handle);

    if (overlay_error != vr::VROverlayError_None) {
        return "VROverlay failed to create overlay: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
    }

    overlay_error = vr::VROverlay()->SetOverlayWidthInMeters(m_overlay_handle, panel_width_m());

    if (overlay_error != vr::VROverlayError_None) {
        return "VROverlay failed to set overlay width: " + std::string{vr::VROverlay()->GetOverlayErrorNameFromEnum(overlay_error)};
    }

    m_overlay_width_set = panel_width_m();

    // Unsichtbar, bis das Menue aufgeht -- es gibt keine Zeigetests mehr, fuer die
    // die Flaeche stehen bleiben muesste.
    vr::VROverlay()->HideOverlay(m_overlay_handle);

    spdlog::info("Made overlay with handle {}", m_overlay_handle);

    return std::nullopt;
}

void OverlayComponent::on_pre_imgui_frame() {
    // [MENUE 21.09.2026] Keine Laser-/Maus-Eingabe mehr: das VR-Menue wird per
    // Controller bedient (REFramework::run_vr_menu_frame).
}

void OverlayComponent::on_post_compositor_submit() {
    this->update_overlay();
}

// [MENUE VOR DEM KOPF 11.09.2026] Beim Oeffnen einmal vor den Kopf gestellt,
// danach steht die Flaeche still im Raum.
void OverlayComponent::update_panel_anchor() {
    auto& vr = VR::get();

    // Menue zu -> beim naechsten Oeffnen neu vor den Kopf stellen.
    if (!g_framework->is_drawing_ui()) {
        m_panel_anchored = false;
        return;
    }

    const auto distance = g_framework->get_vr_menu_panel_distance();

    // [VR-MENUE-GROESSE 11.09.2026] Steht schon: nur einem geaenderten Abstand
    // folgen, entlang der beim Oeffnen gemerkten Richtung und Kopfposition.
    if (m_panel_anchored) {
        if (distance != m_panel_distance_used) {
            m_panel_distance_used = distance;
            m_panel_anchor[3] = Vector4f{m_panel_head_pos - (m_panel_back * distance), 1.0f};
        }

        return;
    }

    // Noch keine gueltige Kopfpose -> im naechsten Frame.
    if (!vr->is_hmd_active()) {
        return;
    }

    const auto hmd = vr->get_transform(0);

    // Nur die Blickrichtung um die Hochachse -- schaut man beim Oeffnen nach unten,
    // soll die Flaeche trotzdem aufrecht vor einem stehen. Spalte 2 ist die +Z-Achse
    // des Kopfes, also die Richtung NACH HINTEN.
    auto back = Vector3f{hmd[2]};
    back.y = 0.0f;

    if (glm::length(back) < 0.001f) {
        back = Vector3f{0.0f, 0.0f, 1.0f};   // exakt senkrecht geschaut
    }

    back = glm::normalize(back);

    const auto yaw = std::atan2(back.x, back.z);

    // Die Flaeche schaut nach +Z, also zum Spieler hin; Mitte auf Augenhoehe.
    m_panel_anchor = Matrix4x4f{glm::angleAxis(yaw, Vector3f{0.0f, 1.0f, 0.0f})};
    m_panel_head_pos = Vector3f{hmd[3]};
    m_panel_back = back;
    m_panel_distance_used = distance;
    m_panel_anchor[3] = Vector4f{m_panel_head_pos - (back * distance), 1.0f};

    m_panel_anchored = true;
}

void OverlayComponent::update_overlay() {
    if (!VR::get()->get_runtime()->is_openvr()) {
        return;
    }

    update_panel_anchor();

    const bool open = g_framework->is_drawing_ui() && m_panel_anchored;

    if (!open) {
        if (m_overlay_shown) {
            vr::VROverlay()->HideOverlay(m_overlay_handle);
            m_overlay_shown = false;
        }

        return;
    }

    // [VR-MENUE-GROESSE 11.09.2026] Breite live nachziehen -- SteamVR kennt sie
    // sonst nur vom Anlegen des Overlays.
    if (const auto width = panel_width_m(); width != m_overlay_width_set) {
        vr::VROverlay()->SetOverlayWidthInMeters(m_overlay_handle, width);
        m_overlay_width_set = width;
    }

    // [VR-MENUE-KONTEXT 11.09.2026] Die Textur IST das VR-Menue -- ganz zeigen.
    // Einmal setzen; on_reset leert den Merker.
    if (!m_overlay_data.full_bounds_set) {
        vr::VRTextureBounds_t bounds{};
        bounds.uMin = 0.0f;
        bounds.vMin = 0.0f;
        bounds.uMax = 1.0f;
        bounds.vMax = 1.0f;

        vr::VROverlay()->SetOverlayTextureBounds(m_overlay_handle, &bounds);
        m_overlay_data.full_bounds_set = true;
    }

    const auto steamvr_transform = Matrix3x4f{glm::rowMajor4(m_panel_anchor)};
    vr::VROverlay()->SetOverlayTransformAbsolute(m_overlay_handle, vr::ETrackingUniverseOrigin::TrackingUniverseStanding,
                                                 (vr::HmdMatrix34_t*)&steamvr_transform);

    if (g_framework->get_renderer_type() == REFramework::RendererType::D3D11) {
        vr::Texture_t imgui_tex{(void*)g_framework->get_rendertarget_d3d11().Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
        vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);
    } else {
        auto& hook = g_framework->get_d3d12_hook();

        vr::D3D12TextureData_t texture_data {
            g_framework->get_rendertarget_d3d12().Get(),
            hook->get_command_queue(),
            0
        };

        vr::Texture_t imgui_tex{(void*)&texture_data, vr::TextureType_DirectX12, vr::ColorSpace_Auto};
        vr::VROverlay()->SetOverlayTexture(m_overlay_handle, &imgui_tex);
    }

    if (!m_overlay_shown) {
        vr::VROverlay()->ShowOverlay(m_overlay_handle);
        m_overlay_shown = true;
    }
}
}
