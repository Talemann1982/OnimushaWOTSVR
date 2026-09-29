#pragma once

#include <array>
#include <cstdint>

#include <d3d12.h>

#include "ComPtr.hpp"

namespace d3d12 {
// ============================================================================
// [POST_PASS 28.09.2026 -- Ansage des Users: "AA / Sharpness / Saturation auch
// OHNE Upscaling, wie ReShade"]
//
// Eigener Nachbearbeitungs-Pass auf dem FERTIGEN Augenbild, direkt bevor es an
// OpenXR/OpenVR geht (D3D12Component::apply_post_pass). Portiert aus dem
// RE4-Fork (afw). Onimusha rendert per AFR (ein Auge pro Frame) und hat keinen
// Upscaler -- der Pass laeuft daher jeden Frame auf dem gerade fertigen Auge.
// Schaerfe (LumaSharpen-Art) + Saettigung; SMAA als eigener Pass (unten).
// ============================================================================
struct PostPass {
    bool setup(ID3D12Device* device);

    // Bearbeitet `src` an Ort und Stelle. `src_state` ist der Zustand, in dem
    // die Textur uebergeben wird, und in dem sie auch zurueckbleibt.
    // sharpness 0..3 (0 = aus), saturation 0..2 (1 = unveraendert).
    bool dispatch_inplace(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                          ID3D12Resource* src, D3D12_RESOURCE_STATES src_state,
                          float sharpness, float saturation,
                          float brightness = 1.0f, float contrast = 1.0f);   // [29.09.2026] 1 = unveraendert

    void reset();

private:
    static constexpr uint32_t SLOT_COUNT = 2;
    static constexpr uint32_t RING_COUNT = 8;

    struct Slot {
        ComPtr<ID3D12Resource> target{};
        ID3D12Resource* src{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};      // Format der Quelle
        DXGI_FORMAT uav_format{DXGI_FORMAT_UNKNOWN};  // Format der Zwischentextur (SRGB -> UNORM)
    };

    int32_t ensure_target(ID3D12Device* device, ID3D12Resource* src);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};

    std::array<Slot, SLOT_COUNT> m_slots{};

    uint32_t m_ring{0};
    uint32_t m_next_slot{0};
    uint32_t m_descriptor_size{0};
    bool m_failed{false};   // Shader/PSO einmal gescheitert -> nie wieder versuchen
};

// ============================================================================
// [POST_PASS SMAA 28.09.2026] SMAA 1x (Jimenez et al., MIT -- Daten und Shader
// in SmaaData.inl, aus ReShade uebernommen). Drei Grafik-Durchgaenge:
// Kanten (Luma) -> Mischgewichte (AreaTex/SearchTex) -> Nachbarn mischen.
// Laeuft wie PostPass an Ort und Stelle auf einem Augenbild; die Quelle bleibt
// in src_state zurueck.
// ============================================================================
struct SmaaPass {
    bool dispatch_inplace(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                          ID3D12Resource* src, D3D12_RESOURCE_STATES src_state);

    void reset();

private:
    static constexpr uint32_t SLOT_COUNT = 2;
    static constexpr uint32_t RING_COUNT = 8;
    static constexpr uint32_t SRV_PER_DRAW = 5;   // t0 Farbe, t1 Kanten, t2 Gewichte, t3 Area, t4 Search

    struct Slot {
        ComPtr<ID3D12Resource> edges{};    // R8G8_UNORM
        ComPtr<ID3D12Resource> blend{};    // R8G8B8A8_UNORM
        ComPtr<ID3D12Resource> out{};      // Format der Quelle
        ID3D12Resource* src{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        uint32_t rtv_base{0};              // 3 RTVs ab hier im RTV-Heap
    };

    bool setup(ID3D12Device* device);
    bool ensure_pso_out(ID3D12Device* device, DXGI_FORMAT format);
    bool upload_luts(ID3D12Device* device, ID3D12GraphicsCommandList* cmd);
    int32_t ensure_slot(ID3D12Device* device, ID3D12Resource* src);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso_edges{};
    ComPtr<ID3D12PipelineState> m_pso_weights{};
    ComPtr<ID3D12PipelineState> m_pso_out{};
    DXGI_FORMAT m_pso_out_format{DXGI_FORMAT_UNKNOWN};
    ComPtr<ID3DBlob> m_vs_edges{}, m_ps_edges{}, m_vs_weights{}, m_ps_weights{}, m_vs_blend{}, m_ps_blend{};

    ComPtr<ID3D12DescriptorHeap> m_srv_heap{};
    ComPtr<ID3D12DescriptorHeap> m_rtv_heap{};
    uint32_t m_srv_size{0};
    uint32_t m_rtv_size{0};

    ComPtr<ID3D12Resource> m_area{};
    ComPtr<ID3D12Resource> m_search{};
    ComPtr<ID3D12Resource> m_upload{};
    bool m_luts_ready{false};

    std::array<Slot, SLOT_COUNT> m_slots{};
    uint32_t m_ring{0};
    uint32_t m_next_slot{0};
    bool m_failed{false};
};
}
