#pragma once
//
// Replacement files must keep who may read them: a key file in /etc/liveqx
// is root:liveqx 0640 so the service (user liveqx) can read it, and a rewrite
// done by root must not turn it into root:root 0600.

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>

namespace liveqx::util {

// Gives the open file `fd` the owner, group and permission bits of
// `reference`. Does nothing if `reference` does not exist. Changing the owner
// needs privilege; without it the call is harmless and the owner stays.
inline void copyOwnerAndMode(int fd, const std::filesystem::path& reference) {
    struct stat st{};
    if (::stat(reference.c_str(), &st) != 0) return;
    (void)::fchown(fd, st.st_uid, st.st_gid);
    (void)::fchmod(fd, st.st_mode & 0777);
}

}  // namespace liveqx::util
