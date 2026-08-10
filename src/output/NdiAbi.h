#pragma once
//
// Минимальные ABI-совместимые объявления типов NDI 5.x SDK. Мы НЕ
// бандлим NDI SDK headers (NewTek redistribution license запрещает
// без явного opt-in оператора), поэтому объявляем сами те структуры,
// которые передаём в libndi через dlsym'нутые функции. Layout
// должен 1:1 совпадать с Processing.NDI.Lib.h из NDI SDK 5.x.
//
// При обновлении NDI SDK до 6.x — сверять с NewTek release notes;
// до сих пор NDI ABI оставался стабильным с v3.
//
#include <cstdint>

namespace liveqx::ndi::abi {

using send_instance_t = void*;
using recv_instance_t = void*;
using find_instance_t = void*;

// FourCC tags (LE). NDI SDK называет это
// NDIlib_FourCC_video_type_e — мы используем сырые int'ы, чтобы не
// тащить header.
enum FourCC : int {
    FourCC_UYVY = 0x59565955,  // 'U','Y','V','Y'
    FourCC_BGRA = 0x41524742,  // 'B','G','R','A'
    FourCC_BGRX = 0x58524742,
    FourCC_RGBA = 0x41424752,
};

enum frame_format_e : int {
    frame_format_progressive  = 1,
    frame_format_interleaved  = 0,
    frame_format_field_0      = 2,
    frame_format_field_1      = 3,
};

// Match NDIlib_send_create_t exactly.
struct send_create_t {
    const char* p_ndi_name;
    const char* p_groups;
    bool        clock_video;
    bool        clock_audio;
};

// Match NDIlib_video_frame_v2_t exactly. line_stride_in_bytes shares a
// union slot with data_size_in_bytes — for raw UYVY/BGRA we always
// populate stride.
struct video_frame_v2_t {
    int             xres;
    int             yres;
    int             FourCC;
    int             frame_rate_N;
    int             frame_rate_D;
    float           picture_aspect_ratio;
    int             frame_format_type;
    std::int64_t    timecode;
    std::uint8_t*   p_data;
    int             line_stride_in_bytes;
    const char*     p_metadata;
    std::int64_t    timestamp;
};

// NDIlib_recv/send источник: используем в c8 для NdiInput. p_url_address
// формально — union с p_ip_address, но layout-совместимо с одним
// pointer-полем.
struct source_t {
    const char* p_ndi_name;
    const char* p_url_address;
};

// NDI 5 receive surface — used by NdiInput. Layout 1:1 с
// Processing.NDI.Lib.h. ABI стабильна с v3.
struct audio_frame_v2_t {
    int          sample_rate;
    int          no_channels;
    int          no_samples;
    std::int64_t timecode;
    float*       p_data;
    int          channel_stride_in_bytes;
    const char*  p_metadata;
    std::int64_t timestamp;
};

enum recv_color_format_e : int {
    recv_color_format_BGRX_BGRA = 0,
    recv_color_format_UYVY_BGRA = 1,
    recv_color_format_RGBX_RGBA = 2,
    recv_color_format_UYVY_RGBA = 3,
    recv_color_format_fastest   = 100,
    recv_color_format_best      = 101,
};

enum recv_bandwidth_e : int {
    recv_bandwidth_metadata_only = -10,
    recv_bandwidth_audio_only    = 10,
    recv_bandwidth_lowest        = 0,
    recv_bandwidth_highest       = 100,
};

enum frame_type_e : int {
    frame_type_none           = 0,
    frame_type_video          = 1,
    frame_type_audio          = 2,
    frame_type_metadata       = 3,
    frame_type_error          = 4,
    frame_type_status_change  = 100,
};

struct recv_create_v3_t {
    source_t    source_to_connect_to;
    int         color_format;
    int         bandwidth;
    bool        allow_video_fields;
    const char* p_ndi_recv_name;
};

}  // namespace liveqx::ndi::abi
