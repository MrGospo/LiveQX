#pragma once
//
// A cap on long-lived responses (the event stream, the log stream, the host
// metrics stream).
//
// The HTTP server gives every open connection a worker thread for as long as it
// is open, and a stream stays open until the browser tab is closed. With a pool
// of a dozen workers, a dozen open streams are enough for every other request —
// the health check and the login among them — to wait for a worker that never
// comes. One tab holds up to four streams, so three colleagues were enough.
//
// So streams are counted: a total, so that most workers stay free for ordinary
// requests, and a per-user cap, so that one person (or one script) cannot take
// the whole allowance. Over the cap a stream is refused at once (the caller
// answers 429) and the client tries again later; it never waits for a worker.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace liveqx::api {

class StreamLimiter {
public:
    StreamLimiter(int max_total, int max_per_user) : st_(std::make_shared<State>()) {
        configure(max_total, max_per_user);
    }

    // May be called while streams are open; open ones are never cut, only new ones are judged.
    void configure(int max_total, int max_per_user) {
        std::lock_guard<std::mutex> lk(st_->mu);
        st_->max_total = max_total < 1 ? 1 : max_total;
        st_->max_user  = max_per_user < 1 ? 1 : max_per_user;
    }

    // A place for one more stream of `user` (0 = not known), or null when the server or the user
    // is at the cap. The place is given back when the returned pointer is released — keep it
    // inside whatever lives as long as the stream does.
    std::shared_ptr<void> acquire(std::int64_t user) {
        {
            std::lock_guard<std::mutex> lk(st_->mu);
            if (st_->total >= st_->max_total) return nullptr;
            int& mine = st_->per_user[user];
            if (mine >= st_->max_user) return nullptr;
            ++mine;
            ++st_->total;
        }
        return std::shared_ptr<void>(new char(0), [st = st_, user](void* p) {
            delete static_cast<char*>(p);
            std::lock_guard<std::mutex> lk(st->mu);
            --st->total;
            if (auto it = st->per_user.find(user); it != st->per_user.end() && --it->second <= 0)
                st->per_user.erase(it);
        });
    }

    int active() const {
        std::lock_guard<std::mutex> lk(st_->mu);
        return st_->total;
    }
    int maxTotal() const {
        std::lock_guard<std::mutex> lk(st_->mu);
        return st_->max_total;
    }

private:
    struct State {
        std::mutex mu;
        int max_total{24};
        int max_user{6};
        int total{0};
        std::map<std::int64_t, int> per_user;
    };
    std::shared_ptr<State> st_;      // shared with the places handed out, so they may outlive the limiter
};

}  // namespace liveqx::api
