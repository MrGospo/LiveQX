#pragma once
#include <string>
#include "transitions/ITransition.h"

struct PlaylistItem {
    std::string      path;
    double           display_duration = 10.0; // seconds; ignored for VideoClip
    TransitionConfig transition;
    // NUMA node hint propagated by ChannelInstance/ContentSync. -1 = no
    // pinning (default). VideoClip pins its decode threads to this node;
    // ImageClip ignores it (synchronous, runs on caller thread).
    int              numa_node = -1;
};
