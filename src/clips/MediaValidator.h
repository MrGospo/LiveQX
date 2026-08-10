#pragma once
#include <string>

struct ValidationResult {
    bool        valid                 = false;
    bool        has_video             = false;
    bool        has_audio             = false;
    bool        first_frame_is_idr    = false; // critical for seamless loop
    double      video_dur_sec         = 0.0;
    double      audio_dur_sec         = 0.0;
    double      duration_mismatch_sec = 0.0;   // abs(video_dur - audio_dur)
    std::string error;                          // empty when valid=true
};

class MediaValidator {
public:
    // Opens the container with FFmpeg and validates streams.
    // Never throws — failures are reported via ValidationResult::valid/error.
    static ValidationResult validate(const std::string& path);
};
