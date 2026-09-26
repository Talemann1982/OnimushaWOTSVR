#pragma once

#include <string>
#include <optional>
#include <cstdint>

#include "imgui.h"

// ============================================================================
// [ONI_MENU 26.09.2026] 1:1 aus dem RE9-Fork ([MENUE 21.09.2026, nach dem RE4-Fork]). Die VR-Tafel des Mod-Menues (OpenVR).
//
// Das Original-Verhalten von REFramework ist RAUS: keine Flaeche an der linken
// Hand, kein Laser, kein Oeffnen/Schliessen durch Zielen (Gesten). Geoeffnet
// wird das Menue ueber LT + linkes B (REFramework::run_vr_menu_frame) bzw. Insert; die Tafel
// wird beim Oeffnen EINMAL vor den Kopf gestellt und bleibt im Raum stehen,
// bis das Menue zugeht. Sie zeigt die ganze Textur des VR-Menue-Kontexts
// (REFramework::VR_MENU_WIDTH x VR_MENU_HEIGHT). Breite und Abstand live aus
// REFramework (get_vr_menu_panel_width/_distance).
// ============================================================================
namespace vrmod{
class OverlayComponent {
public:
    void on_reset();
    std::optional<std::string> on_initialize_openvr();

    void on_pre_imgui_frame();
    void on_post_compositor_submit();

private:
    struct {
        bool full_bounds_set{false};   // Overlay zeigt die ganze Textur
    } m_overlay_data;

    // overlay handle
    vr::VROverlayHandle_t m_overlay_handle{};
    vr::VROverlayHandle_t m_thumbnail_handle{};

    // [MENUE VOR DEM KOPF 11.09.2026] Beim Oeffnen einmal vor den Kopf gestellt
    // (Blickrichtung nur um die Hochachse, damit sie aufrecht steht). Aendert sich
    // der Abstand bei offenem Menue, rueckt die Tafel entlang der beim Oeffnen
    // gemerkten Blickrichtung vor/zurueck.
    void update_panel_anchor();

    Matrix4x4f m_panel_anchor{1.0f};
    bool m_panel_anchored{false};
    Vector3f m_panel_head_pos{0.0f, 0.0f, 0.0f};   // Kopfposition beim Oeffnen
    Vector3f m_panel_back{0.0f, 0.0f, 1.0f};       // waagerechte Blickrichtung (nach hinten)
    float m_panel_distance_used{0.0f};
    float m_overlay_width_set{-1.0f};              // zuletzt gesetzte Breite
    bool m_overlay_shown{false};

    void update_overlay();
};}
