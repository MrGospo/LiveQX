#pragma once
//
// The small media samples some tests read: two pictures and a two-second clip, kept in
// tests/data/media/ (see the README there). The path comes from CMake, so a test does not
// depend on the directory it is run from.

#include <string>

#ifndef LIVEQX_TEST_MEDIA_DIR
#error "LIVEQX_TEST_MEDIA_DIR is set by tests/CMakeLists.txt"
#endif

inline std::string testMedia(const std::string& name) {
    return std::string(LIVEQX_TEST_MEDIA_DIR) + "/" + name;
}
