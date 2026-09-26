#pragma once

#include "Mod.hpp"

class Mods {
public:
    Mods();
    virtual ~Mods() {}

    std::optional<std::string> on_initialize() const;
    std::optional<std::string> on_initialize_d3d_thread() const;

    void on_pre_imgui_frame() const;
    void on_frame() const;
    void on_present() const;
    void on_post_frame() const;
    void on_draw_ui() const;

    // [ONI_MENU 26.09.2026, aus dem RE9-Fork] Die REFramework-EIGENEN Trees (VR,
    // Camera, Graphics, ...), Kategorie "REFramework Options" des Menues.
    void draw_ref_trees() const;
    void on_device_reset() const;

    const auto& get_mods() const {
        return m_mods;
    }

private:
    std::vector<std::shared_ptr<Mod>> m_mods;
};