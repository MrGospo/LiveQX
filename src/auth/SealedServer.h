#pragma once
//
// Sealed start: while the master key is passphrase-wrapped and nobody has
// typed the passphrase yet, the real service cannot run (every secret in the
// databases is encrypted with that key). Instead of failing, the process
// serves this tiny HTTP(S) server on the normal API port:
//
//   GET  /api/seal/status   -> {"sealed": true|false}
//   POST /api/seal/unlock   {"passphrase": "..."} -> 200 | 400 | 401 | 413 | 429
//   GET  <anything else not under /api/>  -> a self-contained page with a
//                                            passphrase field (RU/EN)
//   everything else          -> 503 {"error":"sealed"}
//
// Once the key is unlocked, waitUnlocked() returns, the caller stops this
// server and continues with the normal start on the same port.
//
// Brute force: attempts are serialised (one Argon2 derivation at a time —
// each takes hundreds of milliseconds and a lot of memory), and every address
// backs off exponentially after a few failures. During a lockout even the
// correct passphrase is refused, so the server never acts as an oracle.

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace liveqx::auth {

class MasterKey;

// Per-address exponential backoff for passphrase attempts.
//   failures 1 .. free_attempts-1   -> no delay
//   failure number f >= free_attempts -> locked for min(max_delay, 2^(f-free_attempts+1)) s
// An address that stays quiet for `forget_after_s` starts from zero again.
class UnlockLimiter {
public:
    using Clock = std::function<std::int64_t()>;   // unix seconds

    explicit UnlockLimiter(Clock now = nullptr, int free_attempts = 3,
                           int max_delay_s = 300, int forget_after_s = 3600);

    // 0 if an attempt is allowed now, otherwise seconds to wait.
    int  retryAfter(const std::string& address);
    void recordFailure(const std::string& address);
    void recordSuccess(const std::string& address);

private:
    struct Entry {
        int          failures{0};
        std::int64_t locked_until{0};
        std::int64_t last_failure{0};
    };
    std::int64_t nowSec() const;
    void         forgetIdle(std::int64_t now);

    Clock now_;
    int   free_attempts_;
    int   max_delay_s_;
    int   forget_after_s_;
    std::mutex mu_;
    std::map<std::string, Entry> entries_;
};

class SealedServer {
public:
    struct Options {
        std::string           bind{"0.0.0.0"};
        int                   port{8080};          // 0 = pick a free port
        std::filesystem::path cert_path;           // both empty = plain HTTP
        std::filesystem::path key_path;
        int                   free_attempts{3};
    };

    SealedServer(MasterKey& key, Options options);
    ~SealedServer();
    SealedServer(const SealedServer&) = delete;
    SealedServer& operator=(const SealedServer&) = delete;

    // Binds and starts serving in a background thread. False (with `err`) if
    // the port cannot be bound or the certificate cannot be loaded.
    bool start(std::string& err);

    // The port actually bound (useful when Options::port was 0).
    int port() const noexcept { return port_; }

    // Blocks until the key is unlocked (true) or `should_abort()` returns
    // true (false). Polls `should_abort` a few times per second.
    bool waitUnlocked(const std::function<bool()>& should_abort);

    // Stops serving and joins the thread. Idempotent.
    void stop();

private:
    void installHandlers();

    MasterKey&                        key_;
    Options                           opts_;
    std::unique_ptr<httplib::Server>  server_;
    std::thread                       thread_;
    int                               port_{0};
    UnlockLimiter                     limiter_;
    std::mutex                        unlock_mu_;   // one derivation at a time
    std::mutex                        wait_mu_;
    std::condition_variable           wait_cv_;
    bool                              unlocked_{false};
};

}  // namespace liveqx::auth
