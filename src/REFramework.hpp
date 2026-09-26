#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>
#include <map>

#include <spdlog/spdlog.h>
#include <imgui.h>
#include <utility/Patch.hpp>

#include <../../directxtk12-src/Inc/GraphicsMemory.h>
#include "mods/vr/d3d12/CommandContext.hpp"

#include <sdk/GameIdentity.hpp>
class Mods;
class REGlobals;
class RETypes;

#include "D3D11Hook.hpp"
#include "D3D12Hook.hpp"
#include "DInputHook.hpp"
#include "WindowsMessageHook.hpp"

// Global facilitator
class REFramework {
private:
    void hook_monitor();
    std::atomic<uint32_t> m_do_not_hook_d3d_count{0};

public:
    struct DoNotHook {
        DoNotHook(std::atomic<uint32_t>& count) : m_count(count) {
            ++m_count;
        }
    
        ~DoNotHook() {
            --m_count;
        }

    private:
        std::atomic<uint32_t>& m_count;
    };

    DoNotHook acquire_do_not_hook_d3d() {
        return DoNotHook{m_do_not_hook_d3d_count};
    }


public:
    REFramework(HMODULE reframework_module);
    virtual ~REFramework();

    static auto get_reframework_module() { return s_reframework_module; }
    static void set_reframework_module(HMODULE module) { s_reframework_module = module; }

    bool is_valid() const { return m_valid; }

    bool is_dx11() const { return m_is_d3d11; }

    bool is_dx12() const { return m_is_d3d12; }

    const auto& get_mods() const { return m_mods; }

    const auto& get_mouse_delta() const { return m_mouse_delta; }
    const auto& get_keyboard_state() const { return m_last_keys; }

    Address get_module() const { return m_game_module; }

    bool is_ready() const { return m_initialized && m_game_data_initialized; }
    bool is_game_data_initialized() const { return m_game_data_initialized; }
    bool is_ui_focused() const { return m_is_ui_focused; }

    void run_imgui_frame(bool from_present);

    void on_frame_d3d11();
    void on_post_present_d3d11();
    void on_frame_d3d12();
    void on_post_present_d3d12();
    void on_reset();

    void patch_set_cursor_pos();
    void remove_set_cursor_pos_patch();

    bool on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param);
    void on_direct_input_keys(const std::array<uint8_t, 256>& keys);

    static inline bool s_fallback_appdata{false};
    static inline bool s_checked_file_permissions{false};
    static std::filesystem::path get_persistent_dir();
    static std::filesystem::path get_persistent_dir(const std::string& dir) {
        return get_persistent_dir() / dir;
    }

    void request_save_config() {
        m_wants_save_config = true;
    }

    enum class RendererType : uint8_t {
        D3D11,
        D3D12
    };
    
    auto get_renderer_type() const { return m_renderer_type; }
    auto& get_d3d11_hook() const { return m_d3d11_hook; }
    auto& get_d3d12_hook() const { return m_d3d12_hook; }

    auto get_window() const { return m_wnd; }
    auto get_last_window_pos() const { return m_last_window_pos; } // REFramework imgui window
    auto get_last_window_size() const { return m_last_window_size; } // REFramework imgui window

    static const char* get_game_name() {
        return sdk::GameIdentity::get().game_name().data();
    }

    bool is_drawing_ui() const {
        return m_draw_ui;
    }

    void set_draw_ui(bool state, bool should_save = true);

    auto& get_hook_monitor_mutex() {
        return m_hook_monitor_mutex;
    }

    auto& get_startup_mutex() {
        return m_startup_mutex;
    }

    void set_font_size(float size) {
        if (m_font_size != size) {
            m_font_size = size;
        }

        if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().DisplaySize.y > 0.0f) {
            m_font_display_height = ImGui::GetIO().DisplaySize.y;
        }
    }

    void set_font_size_for_display(float size, float source_display_height);

    auto get_font_size() const { return m_font_size; }
    auto get_main_window_display_size() const { return m_main_window_display_size; }
    auto get_default_font() const { return m_default_font; }

    void set_font(std::string path) { 
        m_default_font_file = path;
        m_fonts_need_init = true;
    }

    int add_font(const std::filesystem::path& filepath, float size);
    // [UEBERSCHRIFTEN 11.09.2026] Abschnitts-Ueberschrift im Menue: mittig,
    // erster Buchstabe knallrot, Rest weiss, in re4-title,
    // HEADING_FONT_EXTRA px groesser als der uebrige Text, darunter
    // eine halbe Zeile Abstand. Genutzt von "Upscaling" (TemporalUpscaler),
    // "Rendering Technique" (VR) und den Ueberschriften in "Mod Options" --
    // Aussehen nur hier aendern. gap_before = DAVOR Absatz + roter Trennstrich
    // + Absatz (fuer jede Ueberschrift, die nicht ganz oben steht).
    static constexpr int HEADING_FONT_EXTRA = 10;   // [11.09.2026] erst 2, dann 6 -- re4-title braucht Groesse, sonst gequetscht
    void draw_menu_heading(const char* text, bool gap_before = false);

    // Abstand zwischen den Eintraegen einer Auswahl-Reihe (nebeneinander).
    // [LINKSBUENDIG 11.09.2026] Die Helfer fuer mittige Reihen sind entfallen.
    static constexpr float MENU_BUTTON_GAP = 12.0f;

    // [WEISSER RAHMEN 11.09.2026] Toggle (ImGui::Checkbox) mit weissem Rahmen.
    // [ROTER HAKEN 11.09.2026] Haken knallrot -- dieselbe Farbe fuer alle Toggles
    // und Auswahlen im Menue, auch die, die selbst zeichnen (ModToggle).
    // [ONI_MENU 26.09.2026] DIE Akzentfarbe des Menues -- Gelb statt Rot (RE4) bzw.
    // Blau (RE9). Nur hier aendern: Haken, Rahmen, Markierungen, Ueberschriften-
    // Anfangsbuchstaben, Titel und alle Akzente im Theme (set_imgui_style) leiten
    // sich davon ab (menu_accent).
    static constexpr ImVec4 MENU_ACCENT_COLOR{1.0f, 0.78f, 0.0f, 1.0f};

    // Akzent in Helligkeit k (0..1, 1 = MENU_ACCENT_COLOR) mit Deckkraft a --
    // ersetzt die RE9-Blautoene (0, 0, k, a) 1:1.
    static constexpr ImVec4 menu_accent(float k = 1.0f, float a = 1.0f) {
        return ImVec4{MENU_ACCENT_COLOR.x * k, MENU_ACCENT_COLOR.y * k, MENU_ACCENT_COLOR.z * k, a};
    }

    static constexpr ImVec4 MENU_CHECKMARK_COLOR = MENU_ACCENT_COLOR;   // [ONI_MENU] RE9: blau
    bool draw_menu_checkbox(const char* label, bool* v);

    // [MENUE-TOGGLE 11.09.2026] Der Stil dazu, auch fuer Stellen, die selbst zeichnen
    // (ModToggle): Rahmen 1 px dicker als im Theme, eckig, weiss bzw. border_color,
    // Haken knallrot. Immer paarweise mit pop_menu_toggle_style.
    void push_menu_toggle_style();
    void push_menu_toggle_style(const ImVec4& border_color);
    void pop_menu_toggle_style();

    // [MENUE-AUSWAHL 11.09.2026] Auswahl statt Knopf, sieht aus wie ein Toggle
    // (Kaestchen, weisser Rahmen, Haken bei der gewaehlten Option).
    // Gibt true beim Anklicken.
    // Die zweite Form faerbt den Rahmen (Farbauswahl: in der jeweiligen Farbe).
    bool draw_menu_radio(const char* label, bool active);
    bool draw_menu_radio(const char* label, bool active, const ImVec4& border_color);

    // [VR-MENUE-KONTEXT 11.09.2026] Das Menue hat in VR einen EIGENEN ImGui-
    // Kontext mit fester Groesse -- unabhaengig von der Desktop-Aufloesung
    // (Vorbild: re7_vr_settings_overlay.dll). Er zeichnet das Menuefenster
    // bildschirmfuellend in RTV::IMGUI, das genau VR_MENU_WIDTH x VR_MENU_HEIGHT
    // gross ist; OpenVR-Overlay und OpenXR-Quad zeigen diese Textur GANZ.
    // Der Desktop behaelt sein normales Fenster, beide gehen gemeinsam auf/zu.
    static constexpr float VR_MENU_WIDTH = 1920.0f;
    static constexpr float VR_MENU_HEIGHT = 1080.0f;
    // [ABSTAND SPALTEN 11.09.2026] Luft zwischen linker Spalte und rechtem Inhalt, px bei 1080.
    static constexpr float MENU_DETAIL_GAP = 24.0f;

    // [VR-MENUE-GROESSE 11.09.2026] Breite und Abstand der VR-Tafel in Metern.
    // [ONI_MENU] Einstellbar unter DEVELOPER -> "Menu Editor" (live), gespeichert
    // in reframework/data/oni_vr/oni_vr_menu.txt. Sobald die Werte passen, wandern
    // sie hier als Defaults fest hinein.
    static constexpr float VR_MENU_PANEL_WIDTH_DEFAULT = 1.0f;
    static constexpr float VR_MENU_PANEL_DISTANCE_DEFAULT = 1.0f;

    float get_vr_menu_panel_width() const { return m_vr_menu_panel_width; }
    float get_vr_menu_panel_distance() const { return m_vr_menu_panel_distance; }
    void set_vr_menu_panel_width(float meters) { m_vr_menu_panel_width = std::clamp(meters, 0.25f, 5.0f); }
    void set_vr_menu_panel_distance(float meters) { m_vr_menu_panel_distance = std::clamp(meters, 0.25f, 5.0f); }

    // [VR-SCHRIFT 11.09.2026] Textgroessen im VR-Menue, 1.0 = wie bisher.
    //   nav    = linke Spalte (Kategorien in re4-title), die Spalte waechst mit
    //   detail = rechter Bereich: Text, Ueberschriften, Innenraender und Abstaende
    //            wachsen GEMEINSAM, das Verhaeltnis bleibt
    // Das VR-Menue nutzt dafuer eigene Schriften, VR_FONT_OVERSAMPLE-fach so gross
    // gerastert und herunterskaliert -- bis 3.0 bleibt es damit scharf. Seit dem
    // Vollbild-Umbau gelten die Werte auch am Desktop, dort zusaetzlich mit der
    // Fensterhoehe / 1080 skaliert.
    static constexpr int VR_FONT_OVERSAMPLE = 3;
    float get_vr_menu_nav_text_scale() const { return m_vr_menu_nav_text_scale; }
    float get_vr_menu_detail_text_scale() const { return m_vr_menu_detail_text_scale; }
    // [BIS 5.0 16.09.2026] War 3.0. Die Schrift ist VR_FONT_OVERSAMPLE-fach (3) gerastert --
    // oberhalb von 3.0 wird sie hochskaliert und damit etwas weicher.
    void set_vr_menu_nav_text_scale(float scale) { m_vr_menu_nav_text_scale = std::clamp(scale, 0.5f, 5.0f); }
    void set_vr_menu_detail_text_scale(float scale) { m_vr_menu_detail_text_scale = std::clamp(scale, 0.5f, 3.0f); }

    // [STICK-DEADZONE 11.09.2026] Ab welchem Ausschlag die Sticks im VR-Menue
    // navigieren, scrollen und Slider verstellen (0..1). Frueher fest 0.55.
    // [KATEGORIE-RUNDUNG 11.09.2026] Eckenrundung der roten Markierung in der
    // Kategorien-Spalte, in Pixeln bei Massstab 1 (waechst mit "Category Text Size"
    // und der Aufloesung mit). Frueher fest style.FrameRounding (5).
    float get_vr_menu_nav_rounding() const { return m_vr_menu_nav_rounding; }
    void set_vr_menu_nav_rounding(float px) { m_vr_menu_nav_rounding = std::clamp(px, 0.0f, 40.0f); }

    // [ONI_MENU 26.09.2026] Logo oben in der Kategorien-Spalte: Breite als Faktor auf
    // die Innenbreite der Spalte, Abstand zum oberen Rand in px bei Massstab 1
    // (waechst wie die Spalte mit). Menu Editor, gespeichert in oni_vr/oni_vr_menu.txt.
    float get_vr_menu_logo_scale() const { return m_vr_menu_logo_scale; }
    void set_vr_menu_logo_scale(float s) { m_vr_menu_logo_scale = std::clamp(s, 0.1f, 3.0f); }
    float get_vr_menu_logo_top_gap() const { return m_vr_menu_logo_top_gap; }
    void set_vr_menu_logo_top_gap(float px) { m_vr_menu_logo_top_gap = std::clamp(px, 0.0f, 400.0f); }

    float get_vr_menu_stick_deadzone() const { return m_vr_menu_stick_deadzone; }
    void set_vr_menu_stick_deadzone(float deadzone) { m_vr_menu_stick_deadzone = std::clamp(deadzone, 0.2f, 0.95f); }

    // [STICK-WIEDERHOLUNG 11.09.2026] Stick gehalten: erst nach "delay" Sekunden
    // wiederholt sich der Schritt, danach alle "interval" Sekunden. Gemeint ist das
    // Weiterspringen des Cursors; das Slider-Verstellen laeuft in ImGui fest
    // 2,67-mal so schnell. ImGuis Vorgabe war 0,2 s / 0,04 s -- viel zu hektisch.
    float get_vr_menu_repeat_delay() const { return m_vr_menu_repeat_delay; }
    // [SCROLLTEMPO 14.09.2026 -- Ansage "Slider fuer Scrollgeschwindigkeit"]
    // Pixel pro Sekunde bei voll ausgelenktem rechten Stick.
    float get_vr_menu_scroll_speed() const { return m_vr_menu_scroll_speed; }
    void set_vr_menu_scroll_speed(float px) { m_vr_menu_scroll_speed = std::clamp(px, 200.0f, 4000.0f); }

    // [TRACKPAD-SCROLL 15.09.2026] Eigener Faktor: das Pad liefert kleinere
    // Auslenkungen als der Stick, mit dem Stick-Tempo fuehlt es sich zaeh an.
    float get_vr_menu_trackpad_scale() const { return m_vr_menu_trackpad_scale; }
    void set_vr_menu_trackpad_scale(float f) { m_vr_menu_trackpad_scale = std::clamp(f, 0.5f, 8.0f); }

    float get_vr_menu_repeat_interval() const { return m_vr_menu_repeat_interval; }
    void set_vr_menu_repeat_delay(float seconds) { m_vr_menu_repeat_delay = std::clamp(seconds, 0.1f, 1.0f); }
    void set_vr_menu_repeat_interval(float seconds) { m_vr_menu_repeat_interval = std::clamp(seconds, 0.03f, 0.6f); }

    // [ONI_MENU] Deckkraft des Hintergrundbilds (RE4: Menu.png) entfaellt -- kein Bild.

    // Feste Pixelabstaende im Menue (Knopfabstand, Zeilenluft) -- im skalierten
    // rechten VR-Bereich mitgewachsen, sonst unveraendert.
    float menu_px(float px) const { return px * m_menu_px_scale; }

    // [SPLASH 13.09.2026] Reine Lese-Getter fuer Zeichner ausserhalb, die EXAKT
    // dieselben Schriften und denselben Massstab wie das Menue brauchen.
    ImFont* vr_font_base() const { return m_vr_font_base; }
    ImFont* vr_font_heading() const { return m_vr_font_heading; }
    float menu_draw_scale() const { return m_menu_draw_scale; }
    // [MENUE-SOUNDS 11.09.2026] Klick-Sounds des Mod-Menues. REFramework erkennt
    // die Aktion (Desktop- wie VR-Kontext) und reiht sie ein; abholen per
    // take_menu_sounds. [ONI_MENU] In Onimusha spielt sie (noch) niemand ab.
    enum class MenuSound : uint8_t {
        Open,       // Menue auf/zu (LT + linkes B, Insert, X)
        Tree,       // Tree / CollapsingHeader auf- oder zuklappen
        Toggle,     // Checkbox, Auswahlpunkt, Knopf
        Slider,     // Slider/Drag verschieben (hoechstens alle 50 ms)
        Category,   // Kategorie links wechseln
        Cursor,     // [12.09.2026] Markierung wandert (Stick hoch/runter, Spaltenwechsel)
    };

    std::vector<MenuSound> take_menu_sounds();

    // Aus ImGuiTestEngineHook_ItemInfo -- flags = ImGuiItemStatusFlags.
    void on_imgui_item_info(ImGuiContext* ctx, ImGuiID id, int flags);


    ImFont* get_font(int index) const {
        if (index >= 0 && index < m_additional_fonts.size()) {
            return m_additional_fonts[index].font;
        } else {
            return nullptr;
        }
    }

    auto get_font_size(int index) const {
        if (index >= 0 && index < m_additional_fonts.size()) {
            return m_additional_fonts[index].size;
        } else {
            return m_font_size;
        }
    }

private:
    void save_config();
    void consume_input();
    void init_fonts();
    void invalidate_device_objects();

    void draw_ui();
    void draw_about();
    void preserve_main_window_position(const char* window_name);
    void scale_font_for_display(float display_height);
    void track_manual_ui_layout_changes();
    void ensure_ui_layout_baseline();
    void process_ui_layout_save(bool from_present);

    // [MENUE-KATEGORIEN 11.09.2026] Hauptfenster: links die Kategorien, rechts
    // der Inhalt der gewaehlten. Die Auswahl lebt nur fuer die laufende
    // Sitzung, beim Spielstart steht "Mod Options" (der oberste Eintrag).
    // [ONI_MENU 26.09.2026] Onimusha: nur Mod Options (vorerst leer), Developer
    // (Menu Editor) und REFramework Options.
    // [ONI_DEV_UI] true = Dev-Fassung (DEVELOPER-Kategorie sichtbar), false = Public.
    static constexpr bool ONI_DEV_UI = false;

    enum class MenuCategory : int {
        ModOptions,
        Bindings,     // [ONI_BIND] vorerst leer
        Rendering,    // [ONI_RENDER] AFR-Schalter
        Developer,
        RefOptions,
    };

    // Anzahl der Eintraege oben -- bei einer neuen Kategorie mitziehen
    // (m_vr_menu.detail_last_id/-scroll haengen daran).
    static constexpr size_t MENU_CATEGORY_COUNT = 5;

    void draw_menu_nav();
    void draw_menu_detail();
    void draw_ref_options();   // Inhalt der Kategorie "REFramework Options"

    // [ONI_MENU 26.09.2026] Kategorie "Developer": der Menu Editor (Port aus
    // RE9VRMenu::draw_menu_editor), seine Werte in reframework/data/oni_vr/oni_vr_menu.txt.
    void draw_menu_editor();
    static std::filesystem::path menu_editor_cfg_path();
    void load_menu_editor_cfg();
    void save_menu_editor_cfg();
    bool m_menu_editor_cfg_loaded{false};

    // [ONI_MENU 26.09.2026] Logo der Kategorien-Spalte (img_oni_logo.hpp):
    // Textur laden (init_d3d12) und fuer ImGui::Image holen -- Textur-ID und
    // Seitenverhaeltnis Hoehe/Breite; leer ohne D3D12-Textur.
    void create_menu_images_d3d12(ID3D12Device* device);
    std::optional<std::pair<ImTextureID, float>> get_menu_logo_texture();
    void draw_bindings_image();   // [ONI_BIND]

    // Das Menuefenster selbst (Titel, Kategorien, Inhalt) -- fuer den Desktop
    // (vr = false, 600x500, schliessbar) und im VR-Kontext (vr = true,
    // bildschirmfuellend). Rueckgabe: Fenster noch offen.
    bool draw_menu_window(bool vr);


    // Ein Frame des VR-Menue-Kontexts; laeuft in run_imgui_frame direkt nach
    // dem Desktop-Frame (m_imgui_mtx gehalten), gerendert in on_frame_d3d12.
    void run_vr_menu_frame();

    // [TASTATUR-NAVIGATION 16.09.2026 -- Ansage des Users] Das Desktop-Menue mit
    // der Tastatur bedienen, nach denselben Regeln wie der Controller im VR-Menue:
    // Pfeiltasten = Richtung, Leertaste = auswaehlen, Backspace = zurueck (in der
    // Kategorien-Spalte: Menue zu). Laeuft in run_imgui_frame VOR ImGui::NewFrame.
    void run_desk_menu_keyboard();
    // Liefert nach dem Desktop-Frame true, wenn Backspace das Menue schliessen soll.
    bool m_desk_menu_close{false};

    // [ONI_PAD] Xbox-Pad fuers Spiel gesperrt (Menue offen oder noch nicht losgelassen)
    bool is_pad_blocked_for_game() const { return m_draw_ui || m_pad_release_guard; }
    std::atomic<bool> m_pad_release_guard{false};
    float m_pad_ls[2]{}, m_pad_rs[2]{};   // [ONI_PAD] fuers VR-Menue
    bool m_pad_a{false}, m_pad_b{false};

    // Die Tasten, die die Tastatur-Navigation selbst an ImGui gibt. Ihre echten
    // Nachrichten gehen dann NICHT an ImGui (on_message) -- ausser ein Textfeld ist aktiv.
    static bool is_desk_menu_nav_key(WPARAM vk) {
        return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT
               || vk == VK_SPACE || vk == VK_BACK;
    }

    struct MenuNavState {
        ImGuiContext* ctx{nullptr};
        ImFontAtlas* atlas{nullptr};   // geteilt mit dem Desktop-Kontext
        bool has_draw_data{false};

        // [MENUE-STEUERUNG 11.09.2026] Controller-Navigation (kein Laser mehr).
        bool drawing{false};            // gerade im VR-Kontext am Zeichnen
        bool was_open{false};           // Menuezustand im letzten Frame
        void* nav_window{nullptr};      // ImGuiWindow* der Kategorien-Spalte
        int nav_item_flags{0};          // ItemStatusFlags unter dem Nav-Cursor (letzter Frame)
        int frame_nav_item_flags{0};    // ... im laufenden Frame gesammelt
        bool jump_to_nav{false};        // Cursor auf die gewaehlte Kategorie setzen
        float scroll_delta{0.0f};       // rechter Stick -> rechter Bereich
        bool tweaking{false};           // markierter Slider wird gerade verstellt
        bool face_down_pulse_prev{false};
        bool l_a_prev{false};
        int dir_prev{0};

        // [SPALTENWECHSEL 11.09.2026] ImGui findet links/rechts nur Elemente auf
        // gleicher Hoehe -- den Wechsel zwischen den Spalten machen wir selbst.
        bool jump_to_detail{false};     // Cursor auf den ersten bedienbaren Eintrag rechts
        ImGuiID left_probe_id{0};       // NavId beim Druck nach links ...
        int left_probe_frames{0};       // ... hat sie sich nach 3 Frames nicht bewegt -> zur Kategorie
        bool rs_left_prev{false};

        // [SPALTENWECHSEL MERKEN 11.09.2026] Letzter Cursor-Eintrag und Scroll im
        // rechten Bereich, je Kategorie -- "rechts" aus der Kategorien-Spalte kehrt
        // dorthin zurueck statt nach ganz oben. Ist der Eintrag nicht mehr da (Tree
        // zu), gilt der erste bedienbare Eintrag im Frame (fallback).
        ImGuiID detail_last_id[MENU_CATEGORY_COUNT]{};
        float detail_last_scroll[MENU_CATEGORY_COUNT]{};
        ImGuiID detail_fallback_id{0};
        void* detail_fallback_window{nullptr};   // ImGuiWindow* des Fallback-Eintrags

        // [KATEGORIE-RAND 11.09.2026] Oberster/unterster Eintrag links. "hoch" am
        // obersten bzw. "runter" am untersten tut nichts -- vorher sprang der
        // Cursor aus der Spalte und die Markierung war weg. Kein Rundlauf links.
        ImGuiID nav_first_id{0};
        ImGuiID nav_last_id{0};

        // [CURSOR-SOUND 12.09.2026] NavId des letzten Frames -- wandert die
        // Markierung, gibt es einen Ton.
        ImGuiID nav_prev_id{0};

        // [TASTATUR-NAVIGATION 16.09.2026] Nur Desktop: Flanken von Leertaste/Backspace.
        bool key_select_prev{false};
        bool key_back_prev{false};
        bool cancel_pulse_prev{false};   // Escape-Impuls im letzten Frame gesendet

        // [ONI_MENU 26.09.2026] Nur VR: LT + linkes B im letzten Frame (Menue auf/zu).
        bool open_combo_prev{false};
    };

    MenuNavState m_vr_menu{};
    // [TASTATUR-NAVIGATION 16.09.2026] Derselbe Zustand fuer das Desktop-Menue.
    MenuNavState m_desk_menu{};

    // Zustand des Kontexts, der GERADE das Menue zeichnet -- sonst nullptr.
    MenuNavState* active_menu_nav() {
        if (m_vr_menu.drawing) {
            return &m_vr_menu;
        }

        return m_desk_menu.drawing ? &m_desk_menu : nullptr;
    }

    // [VR-MENUE-GROESSE 11.09.2026]
    float m_vr_menu_panel_width{VR_MENU_PANEL_WIDTH_DEFAULT};
    float m_vr_menu_panel_distance{VR_MENU_PANEL_DISTANCE_DEFAULT};

    // [VR-SCHRIFT 11.09.2026]
    float m_vr_menu_nav_text_scale{1.0f};
    float m_vr_menu_detail_text_scale{1.0f};
    float m_vr_menu_stick_deadzone{0.7f};   // [STICK-DEADZONE] war fest 0.55 -- zu empfindlich
    float m_vr_menu_nav_rounding{5.0f};     // [KATEGORIE-RUNDUNG] = bisheriges FrameRounding
    float m_vr_menu_logo_scale{0.9f};       // [ONI_MENU] Logo-Breite / Innenbreite der Spalte
    float m_vr_menu_logo_top_gap{16.0f};    // [ONI_MENU] px bei Massstab 1
    float m_vr_menu_repeat_delay{0.45f};    // [STICK-WIEDERHOLUNG] Sekunden bis zur ersten Wiederholung
    float m_vr_menu_repeat_interval{0.2f};  // [STICK-WIEDERHOLUNG] Sekunden zwischen Wiederholungen
    float m_vr_menu_scroll_speed{900.0f};   // [SCROLLTEMPO 14.09.2026] Pixel/Sekunde, rechter Stick
    float m_vr_menu_trackpad_scale{2.5f};   // [TRACKPAD-SCROLL 15.09.2026] Faktor auf das Stick-Tempo
    float m_menu_px_scale{1.0f};
    float m_menu_draw_scale{0.0f};   // [VOLLBILD] Massstab waehrend draw_menu_window, sonst 0

    // [MENUE-SOUNDS 11.09.2026]
    void queue_menu_sound(MenuSound sound);

    std::mutex m_menu_sound_mtx{};
    std::vector<MenuSound> m_menu_sounds{};
    // Letzter Checked-/Opened-Zustand je Widget, getrennt je ImGui-Kontext.
    std::unordered_map<ImGuiContext*, std::unordered_map<ImGuiID, uint8_t>> m_menu_item_states{};
    std::chrono::steady_clock::time_point m_last_slider_sound{};
    std::optional<bool> m_menu_sound_prev_draw_ui{};

    // [ONI_DEV_UI] Public ohne MOD OPTIONS -> startet auf BINDINGS
    MenuCategory m_menu_category{ONI_DEV_UI ? MenuCategory::ModOptions : MenuCategory::Bindings};

public:
    bool hook_d3d11();
    bool hook_d3d12();
    void open_console();

private:
    bool initialize();
    bool initialize_game_data();
    bool initialize_windows_message_hook();

    bool first_frame_initialize();

    void call_on_frame();

    static inline HMODULE s_reframework_module{};

    bool m_first_frame{true};
    bool m_first_frame_d3d_initialize{true};
    bool m_is_d3d12{false};
    bool m_is_d3d11{false};
    bool m_valid{false};
    bool m_initialized{false};
    bool m_created_default_cfg{false};
    bool m_started_game_data_thread{false};
    std::atomic<bool> m_terminating{false}; // Destructor is called
    std::atomic<bool> m_game_data_initialized{false};
    std::atomic<bool> m_mods_fully_initialized{false};
    
    // UI
    bool m_has_frame{false};
    bool m_wants_device_object_cleanup{false};
    bool m_wants_save_config{false};
    std::atomic<bool> m_wants_save_imgui_config{false};
    // [ONI_MENU 26.09.2026] [MENUE ZU BEIM START, aus dem RE9-Fork] War true:
    // REFramework oeffnete das Menue beim Spielstart von selbst. Jetzt zu --
    // geoeffnet wird ueber LT + linkes B (VR) oder Insert. (Mit "Remember Menu
    // State" in der Config kann es weiter offen starten.)
    bool m_draw_ui{false};
    bool m_last_draw_ui{m_draw_ui};
    bool m_is_ui_focused{false};
    bool m_cursor_state{false};
    bool m_cursor_state_changed{true};
    bool m_ui_option_transparent{true};
    bool m_ui_passthrough{false};
    
    ImVec2 m_last_window_pos{};
    ImVec2 m_last_window_size{};
    ImVec2 m_main_window_display_size{};
    bool m_loaded_saved_ui_display_size{false};
    ImVec2 m_saved_ui_display_size{};
    bool m_ui_layout_save_pending{false};
    std::chrono::steady_clock::time_point m_ui_layout_last_changed{};

    struct UIWindowGeometry {
        ImVec2 position{};
        ImVec2 size{};
    };

    std::map<ImGuiID, UIWindowGeometry> m_ui_window_geometries{};
    bool m_manual_ui_geometry_dirty{false};

    struct AdditionalFont {
        std::filesystem::path filepath{};
        float size{16};
        ImFont* font{};
    };

    std::string m_default_font_file = "DEFAULT";
    bool m_fonts_need_init{true};
    float m_font_size{16};
    float m_font_display_height{};
    ImFont* m_default_font;
    std::map<std::string, ImFont*> loaded_fonts{};
    std::vector<AdditionalFont> m_additional_fonts{};
    ImFont* m_heading_font{nullptr};   // Ueberschriften, wird in update_fonts neu gesetzt
    ImFont* m_vr_font_base{nullptr};      // [VR-SCHRIFT] Grundschrift, VR_FONT_OVERSAMPLE-fach gerastert
    ImFont* m_vr_font_heading{nullptr};   // [VR-SCHRIFT] re4-title, VR_FONT_OVERSAMPLE-fach gerastert
    ImFont* m_vr_font_nav{nullptr};       // [KATEGORIE-SCHRIFT] = m_vr_font_heading (re4-title)

    std::mutex m_input_mutex{};
    std::recursive_mutex m_config_mtx{};
    std::recursive_mutex m_imgui_mtx{};
    std::recursive_mutex m_patch_mtx{};

    HWND m_wnd{0};
    HMODULE m_game_module{0};

    float m_accumulated_mouse_delta[2]{};
    float m_mouse_delta[2]{};
    std::array<uint8_t, 256> m_last_keys{0};
    std::unique_ptr<D3D11Hook> m_d3d11_hook{};
    std::unique_ptr<D3D12Hook> m_d3d12_hook{};
    std::unique_ptr<WindowsMessageHook> m_windows_message_hook;
    std::unique_ptr<DInputHook> m_dinput_hook;
    std::shared_ptr<spdlog::logger> m_logger;
    spdlog::sink_ptr m_dist_sink;
    Patch::Ptr m_set_cursor_pos_patch{};

    std::string m_error{""};

    // Game-specific stuff
    std::unique_ptr<Mods> m_mods;

    std::recursive_mutex m_hook_monitor_mutex{};
    std::recursive_mutex m_startup_mutex{};
    std::unique_ptr<std::jthread> m_d3d_monitor_thread{};
    std::chrono::steady_clock::time_point m_last_present_time{};
    std::chrono::steady_clock::time_point m_last_message_time{};
    std::chrono::steady_clock::time_point m_last_sendmessage_time{};
    std::chrono::steady_clock::time_point m_last_chance_time{};
    uint32_t m_frames_since_init{0};
    bool m_has_last_chance{true};
    bool m_first_initialize{true};

    bool m_sent_message{false};
    bool m_message_hook_requested{false};
    bool m_console_setup{false};

    RendererType m_renderer_type{RendererType::D3D11};

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

private: // D3D misc
    void set_imgui_style() noexcept;

private: // D3D11 Init
    bool init_d3d11();
    void deinit_d3d11();

private: // D3D12 Init
    bool init_d3d12();
    void deinit_d3d12();

private: // D3D11 members
    struct D3D11 {
        ComPtr<ID3D11Texture2D> blank_rt{};
		ComPtr<ID3D11Texture2D> rt{};
        ComPtr<ID3D11RenderTargetView> blank_rt_rtv{};
		ComPtr<ID3D11RenderTargetView> rt_rtv{};
		ComPtr<ID3D11ShaderResourceView> rt_srv{};
        uint32_t rt_width{};
        uint32_t rt_height{};
		ComPtr<ID3D11RenderTargetView> bb_rtv{};
    } m_d3d11{};

public:
    auto& get_blank_rendertarget_d3d11() { return m_d3d11.blank_rt; }
    auto& get_rendertarget_d3d11() { return m_d3d11.rt; }
    auto get_rendertarget_width_d3d11() const { return m_d3d11.rt_width; }
    auto get_rendertarget_height_d3d11() const { return m_d3d11.rt_height; }

private: // D3D12 members
    struct D3D12 {
        std::vector<std::unique_ptr<d3d12::CommandContext>> cmd_ctxs{};
        uint32_t cmd_ctx_index{0};

        enum class RTV : int{
            BACKBUFFER_0,
            BACKBUFFER_1,
            BACKBUFFER_2,
            BACKBUFFER_3,
            BACKBUFFER_4,
            BACKBUFFER_5,
            BACKBUFFER_6,
            BACKBUFFER_7,
            BACKBUFFER_8,
            BACKBUFFER_LAST = BACKBUFFER_8,
            IMGUI,
            BLANK,
            COUNT,
        };

        enum class SRV : int {
            IMGUI_FONT_BACKBUFFER,
            IMGUI_FONT_VR,
            IMGUI_VR,
            BLANK,
            ONI_MENU_LOGO,   // [ONI_MENU 26.09.2026] Logo der Kategorien-Spalte
            ONI_MENU_BINDINGS,   // [ONI_BIND 27.09.2026] Bild der Kategorie BINDINGS
            COUNT
        };

        // [ONI_MENU 26.09.2026] Textur des Logos (Slot SRV::ONI_MENU_LOGO), leer,
        // wenn das Laden scheiterte.
        ComPtr<ID3D12Resource> menu_logo{};
        ComPtr<ID3D12Resource> menu_bindings{};   // [ONI_BIND]

        ComPtr<ID3D12DescriptorHeap> rtv_desc_heap{};
        ComPtr<ID3D12DescriptorHeap> srv_desc_heap{};
        ComPtr<ID3D12Resource> rts[(int)RTV::COUNT]{};

        auto& get_rt(RTV rtv) { return rts[(int)rtv]; }

        D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_rtv(ID3D12Device* device, RTV rtv) {
            return {rtv_desc_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                    (SIZE_T)rtv * (SIZE_T)device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
        }

        D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_srv(ID3D12Device* device, SRV srv) {
            return {srv_desc_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                    (SIZE_T)srv * (SIZE_T)device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
        }

        D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_srv(ID3D12Device* device, SRV srv) {
            return {srv_desc_heap->GetGPUDescriptorHandleForHeapStart().ptr +
                    (SIZE_T)srv * (SIZE_T)device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
        }

        uint32_t rt_width{};
        uint32_t rt_height{};

        std::array<void*, 2> imgui_backend_datas{};
        std::unique_ptr<DirectX::DX12::GraphicsMemory> graphics_memory{}; // for use in several places around REF
    } m_d3d12{};

public:
    auto& get_blank_rendertarget_d3d12() { return m_d3d12.get_rt(D3D12::RTV::BLANK); }
    auto& get_rendertarget_d3d12() { return m_d3d12.get_rt(D3D12::RTV::IMGUI); }
    auto get_rendertarget_width_d3d12() { return m_d3d12.rt_width; }
    auto get_rendertarget_height_d3d12() { return m_d3d12.rt_height; }

private:
};

extern std::unique_ptr<REFramework> g_framework;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam); // Use ImGui::GetCurrentContext()
