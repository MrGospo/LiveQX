#pragma once
//
// `liveqx --unseal`: opens a sealed service from the machine it runs on.
//
// The web page on the API port is the usual way to enter the passphrase. This
// is the second way, with no network surface at all: it talks to a Unix socket
// in the state directory (mode 0600, so only the service user and root can
// use it), served by SealedServer for as long as the service is sealed. It
// helps when the API port cannot be reached or does not come up.
//
// Protocol: the client sends one line {"passphrase":"..."}, the server answers
// one line: {"ok":true} or {"error":"<code>", "retry_after":<seconds>}.

#include <filesystem>

#include "auth/KeyProtectCli.h"

namespace liveqx::auth {

// Returns the process exit code (0 = unlocked).
int unsealViaSocket(const std::filesystem::path& socket_path,
                    const PassphraseSource& passphrase);

}  // namespace liveqx::auth
