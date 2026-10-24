#include "players/CommandService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>

namespace liveqx::players {
namespace {

std::int64_t nowSeconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::string rfc3339(std::int64_t unix_s) {
    std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// A restarted machine has been up for less than the time since the command,
// allowing for clocks that differ by a minute.
constexpr std::int64_t kClockSlackS = 60;

// Lower case, single spaces: a phrase typed by a person is compared forgivingly
// about case and spacing, strictly about the words.
std::string normalisePhrase(const std::string& in) {
    std::string out;
    bool space = true;
    for (unsigned char c : in) {
        if (std::isspace(c)) { space = true; continue; }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

}  // namespace

CommandService::CommandService(PlayersDb& db, PlayersService& players, CommandServiceOptions opts)
    : db_(db), players_(players), opts_(std::move(opts)) {}

bool CommandService::supportedType(const std::string& type) {
    return type == kRestartPlayer || type == kRebootScreen || type == kReloadPlaylist;
}

int CommandService::confirmTimeoutFor(const PlayersSettings& s, const std::string& type) {
    if (type == kRebootScreen)  return s.confirm_timeout_reboot_s;
    if (type == kReloadPlaylist) return s.confirm_timeout_reload_s;
    return s.confirm_timeout_restart_s;
}

void CommandService::announce(const CommandRow& c) {
    if (!opts_.events) return;
    nlohmann::json payload = {{"player_id", c.player_id}, {"command_id", c.id}, {"type", c.type},
                              {"state", c.state}, {"result", c.result}};
    // Where the screen sits now, so that whoever sees only part of the tree
    // gets only its own news.
    if (auto place = db_.placeOfPlayer(c.player_id)) {
        payload["group_id"]  = place->group_id;
        payload["object_id"] = place->object_id;
    }
    opts_.events->publish(events::EventType::PlayerCommand, -1, std::move(payload));
}

void CommandService::announceJob(const std::string& job_id) {
    if (!opts_.events) return;
    auto job = db_.findJob(job_id);
    if (!job) return;
    nlohmann::json payload = {{"job_id", job->id}, {"state", job->state}};
    if (job->created_by) payload["created_by"] = *job->created_by;
    opts_.events->publish(events::EventType::PlayerCommandJob, -1, std::move(payload));
}

CommandOutcome CommandService::sendToPlayer(const std::string& player_id, const std::string& type,
                                            std::optional<std::int64_t> issued_by, std::int64_t now) {
    CommandOutcome out;
    if (!supportedType(type)) { out.error = "unsupported_type"; return out; }
    auto p = db_.findPlayer(player_id);
    if (!p) { out.error = "player_not_found"; return out; }
    if (p->state != PlayerState::Active) { out.error = "player_not_active"; return out; }
    if (!opts_.publish) { out.error = "broker_unavailable"; return out; }
    if (now == 0) now = nowSeconds();

    const PlayersSettings s = players_.settings();
    CommandRow row;
    row.id          = PlayersDb::newId();
    row.player_id   = player_id;
    row.type        = type;
    row.state       = "pending";
    row.issued_at   = now;
    row.ttl_seconds = s.command_ttl_s;
    row.deadline_at = now + confirmTimeoutFor(s, type);
    row.issued_by   = issued_by;

    // Recorded before it is sent: the block must exist the moment the command
    // can arrive, and a second request meets it.
    switch (db_.createCommand(row, &out.busy_id)) {
        case CommandCreate::Ok: break;
        case CommandCreate::Busy: out.error = "command_in_progress"; return out;
        case CommandCreate::Storage: out.error = "storage_error"; return out;
    }

    mqtt::Message m;
    m.topic   = "liveqx/v1/players/" + player_id + "/commands";
    m.payload = nlohmann::json{{"id", row.id}, {"issued_at", rfc3339(now)},
                               {"ttl_seconds", row.ttl_seconds}, {"type", type}}.dump();
    m.qos       = 1;
    m.retain    = false;                                   // a retained command would repeat at every reconnect
    m.expiry_s  = static_cast<std::uint32_t>(row.ttl_seconds);
    const mqtt::PublishResult pr = opts_.publish(m);
    if (pr != mqtt::PublishResult::Ok) {
        // It never left: do not leave the screen blocked for it.
        auto settled = db_.settleCommand(row.id, "not_sent",
                                         pr == mqtt::PublishResult::NotConnected ? "broker_not_connected"
                                                                                 : "broker_error", now);
        out.error = "broker_unavailable";
        if (settled) { out.row = *settled; announce(*settled); }
        return out;
    }

    out.ok  = true;
    out.row = row;
    if (auto st = db_.status(player_id)) out.player_online = st->online;
    db_.addEvent(player_id, "command_sent",
                 nlohmann::json{{"command_id", row.id}, {"type", type}}.dump(), now);
    announce(row);
    return out;
}

void CommandService::onResult(const std::string& player_id, const std::string& command_id,
                              const std::string& status, std::optional<std::int64_t> os_uptime_s,
                              std::int64_t now) {
    if (now == 0) now = nowSeconds();
    auto cmd = db_.findCommand(command_id);
    // Only the screen the command went to can settle it; a repeated report of
    // an already settled command changes nothing.
    if (!cmd || cmd->player_id != player_id || cmd->state != "pending") return;

    std::string state, result = status;
    if (status == "executed" || status == "duplicate") {
        state = "executed";
        // A screen that was told to reboot must have rebooted: the machine's
        // own uptime shows it. A player that does not report uptime is believed.
        if (cmd->type == kRebootScreen && os_uptime_s &&
            now - *os_uptime_s < cmd->issued_at - kClockSlackS) {
            state  = "failed";
            result = "os_not_rebooted";
        }
    } else {
        state = "failed";                               // expired, unsupported, failed
    }
    if (auto settled = db_.settleCommand(command_id, state, result, now)) {
        db_.addEvent(player_id, "command_" + state,
                     nlohmann::json{{"command_id", command_id}, {"type", cmd->type}, {"result", result}}.dump(), now);
        announce(*settled);
    }
}

std::size_t CommandService::sweep(std::int64_t now) {
    const auto timed_out = db_.timeoutCommands(now);
    for (const auto& c : timed_out) {
        db_.addEvent(c.player_id, "command_timeout",
                     nlohmann::json{{"command_id", c.id}, {"type", c.type}}.dump(), now);
        announce(c);
    }
    return timed_out.size();
}

std::optional<CommandRow> CommandService::release(const std::string& command_id, std::int64_t now) {
    auto settled = db_.settleCommand(command_id, "released", "released_by_admin", now);
    if (!settled) return std::nullopt;
    db_.addEvent(settled->player_id, "command_released",
                 nlohmann::json{{"command_id", command_id}, {"type", settled->type}}.dump(), now);
    announce(*settled);
    return settled;
}

}  // namespace liveqx::players

namespace liveqx::players {

std::string CommandService::requiredPhrase(const std::string& type, TargetKind kind, const std::string& name) {
    switch (kind) {
        case TargetKind::Group:  return type + " group " + name;
        case TargetKind::Object: return type + " object " + name;
        case TargetKind::All:    return type + " all";
    }
    return type;
}

BulkPlan CommandService::plan(const CommandTarget& target, const std::string& type) {
    BulkPlan p;
    ListFilter f;
    f.state = PlayerState::Active;
    std::string name;
    switch (target.kind) {
        case TargetKind::Group: {
            auto g = db_.findGroup(target.id);
            if (!g) return p;
            auto o = db_.findObject(g->object_id);
            name = (o ? o->name : std::string()) + " / " + g->name;
            f.group_id = target.id;
            break;
        }
        case TargetKind::Object: {
            auto o = db_.findObject(target.id);
            if (!o) return p;
            name = o->name;
            f.object_id = target.id;
            break;
        }
        case TargetKind::All:
            name = "all";
            break;
    }
    p.found = true;
    p.target_name = name;
    p.required_phrase = requiredPhrase(type, target.kind, target.kind == TargetKind::All ? "" : name);
    for (const auto& pl : db_.listPlayers(f)) {
        ++p.total;
        auto st = db_.status(pl.id);
        if (!st || !st->online) { p.offline.push_back(pl.id); continue; }
        p.online.push_back(pl.id);
        for (const auto& c : db_.pendingCommandsOf(pl.id))
            if (c.type == type) p.busy.push_back({pl.id, c.id});
    }
    return p;
}

BulkOutcome CommandService::sendToTarget(const CommandTarget& target, const std::string& type,
                                         const std::string& confirm_phrase, std::size_t expected_count,
                                         std::optional<std::int64_t> issued_by, std::int64_t now,
                                         const std::string& actor_name, const std::string& actor_role) {
    BulkOutcome out;
    if (!supportedType(type)) { out.error = "unsupported_type"; return out; }
    out.plan = plan(target, type);
    if (!out.plan.found) { out.error = "target_not_found"; return out; }
    if (out.plan.total == 0) { out.error = "no_targets"; return out; }
    if (normalisePhrase(confirm_phrase) != normalisePhrase(out.plan.required_phrase)) {
        out.error = "confirm_phrase_mismatch";
        return out;
    }
    if (expected_count != out.plan.total) { out.error = "expected_count_mismatch"; return out; }
    if (!out.plan.busy.empty()) { out.error = "screens_busy"; return out; }
    if (!opts_.publish) { out.error = "broker_unavailable"; return out; }
    if (now == 0) now = nowSeconds();

    const PlayersSettings s = players_.settings();
    JobRow job;
    job.id          = PlayersDb::newId();
    job.type        = type;
    job.target_kind = target.kind == TargetKind::Group ? "group" : target.kind == TargetKind::Object ? "object" : "all";
    job.target_id   = target.id;
    job.target_name = out.plan.target_name;
    job.batch_size  = s.command_batch_size;
    job.stop_percent = s.command_batch_stop_percent;
    job.created_at  = now;
    job.created_by  = issued_by;
    job.created_by_name = actor_name;
    job.created_by_role = actor_role;

    std::vector<JobItem> items;
    for (const auto& id : out.plan.offline) items.push_back({job.id, id, -1, "skipped_offline", "", "offline"});
    for (std::size_t i = 0; i < out.plan.online.size(); ++i)
        items.push_back({job.id, out.plan.online[i], static_cast<std::int64_t>(i / static_cast<std::size_t>(job.batch_size)),
                         "queued", "", ""});

    std::lock_guard<std::mutex> lk(jobs_mu_);
    if (!db_.createJob(job, items)) { out.error = "storage_error"; return out; }
    advance(job, now, /*fresh=*/true);
    announceJob(job.id);

    out.job_id     = job.id;
    out.batch_size = job.batch_size;
    for (const auto& it : db_.jobItems(job.id)) {
        if (it.state == "sent") out.sent.push_back({it.player_id, it.command_id});
        else if (it.state == "failed") out.failed.push_back({it.player_id, it.error});
        else if (it.state == "queued") ++out.queued;
    }
    // The broker being away is not a per-screen failure: take the job back
    // rather than leave a trail of failures for the whole target.
    if (out.sent.empty() && !out.failed.empty() &&
        std::all_of(out.failed.begin(), out.failed.end(),
                    [](const BulkOutcome::Failed& f) { return f.error == "broker_unavailable"; })) {
        db_.finishJob(job.id, "cancelled", "broker_unavailable", now);
        out.failed.clear();
        out.error = "broker_unavailable";
        return out;
    }
    out.ok = true;
    return out;
}

void CommandService::endJob(const std::string& job_id, const std::string& state, const std::string& reason,
                            std::int64_t now, bool notify) {
    waiting_for_broker_.erase(job_id);
    if (!db_.finishJob(job_id, state, reason, now)) return;
    announceJob(job_id);
    if (!notify || !opts_.on_job_end) return;
    if (auto v = jobView(job_id)) opts_.on_job_end(v->job, v->summary);
}

void CommandService::advance(JobRow job, std::int64_t now, bool fresh) {
    for (;;) {
        const auto items = db_.jobItems(job.id);

        // 1. A batch is out: nothing more until every screen of it is settled.
        for (const auto& it : items) {
            if (it.state != "sent") continue;
            auto c = db_.findCommand(it.command_id);
            if (c && c->state == "pending") return;
        }

        // 2. The batch that just finished: did enough of it come back?
        if (job.next_batch > 0) {
            std::int64_t members = 0, bad = 0, broker_away = 0;
            for (const auto& it : items) {
                if (it.batch_no != job.next_batch - 1) continue;
                if (it.state == "failed") {
                    ++members; ++bad;
                    if (it.error == "broker_unavailable") ++broker_away;
                }
                else if (it.state == "sent") {
                    ++members;
                    auto c = db_.findCommand(it.command_id);
                    if (!c || c->state != "executed") ++bad;
                }
            }
            if (members > 0 && bad * 100 > job.stop_percent * members) {
                // Told apart: a batch that never left because the broker stayed away is not screens that did not answer.
                endJob(job.id, "stopped", broker_away == bad ? "broker_unavailable" : "too_many_unconfirmed", now, true);
                return;
            }
        }

        // 3. The next batch, or the end.
        std::int64_t next = -1;
        for (const auto& it : items)
            if (it.state == "queued" && (next < 0 || it.batch_no < next)) next = it.batch_no;
        if (next < 0) { endJob(job.id, "done", "", now, true); return; }

        for (const auto& it : items) {
            if (it.state != "queued" || it.batch_no != next) continue;
            // Things change while a job runs: check the screen again at its turn.
            auto p = db_.findPlayer(it.player_id);
            auto st = db_.status(it.player_id);
            if (!p || p->state != PlayerState::Active || !st || !st->online) {
                db_.updateJobItem(job.id, it.player_id, "skipped_offline", "", p ? "offline" : "gone");
                continue;
            }
            auto one = sendToPlayer(it.player_id, job.type, job.created_by, now);
            if (one.ok) {
                waiting_for_broker_.erase(job.id);
                db_.updateJobItem(job.id, it.player_id, "sent", one.row.id, "");
                if (opts_.on_bulk_sent) opts_.on_bulk_sent(one.row, job);
            } else if (one.error == "broker_unavailable" && !fresh) {
                // Nothing went out, and nothing is wrong with the screen: the broker is not there yet (the
                // server has just started, or the broker is being restarted). Failing the batch for that
                // would stop a job that did nothing wrong, so it waits; this screen and the rest of the
                // batch stay queued, and the next tick tries again. Past broker_wait_s it fails as before.
                const std::int64_t since = waiting_for_broker_.try_emplace(job.id, now).first->second;
                if (now - since < opts_.broker_wait_s) return;
                db_.updateJobItem(job.id, it.player_id, "failed", "", one.error);
            } else {
                db_.updateJobItem(job.id, it.player_id, "failed", "", one.error);
            }
        }
        db_.setNextBatch(job.id, next + 1);
        job.next_batch = next + 1;
        announceJob(job.id);                              // a new batch is out
    }
}

void CommandService::tick(std::int64_t now) {
    if (now == 0) now = nowSeconds();
    std::lock_guard<std::mutex> lk(jobs_mu_);
    for (auto& job : db_.runningJobs()) advance(job, now);
}

std::optional<JobRow> CommandService::cancelJob(const std::string& job_id, std::int64_t now) {
    if (now == 0) now = nowSeconds();
    std::lock_guard<std::mutex> lk(jobs_mu_);
    auto job = db_.findJob(job_id);
    if (!job || job->state != "running") return std::nullopt;
    endJob(job_id, "cancelled", "cancelled_by_user", now, true);
    return db_.findJob(job_id);
}

std::optional<JobView> CommandService::jobView(const std::string& job_id) {
    auto job = db_.findJob(job_id);
    if (!job) return std::nullopt;
    JobView v;
    v.job = *job;
    for (auto& it : db_.jobItems(job_id)) {
        JobItemView iv;
        iv.item = it;
        ++v.summary.total;
        if (it.state == "queued") ++v.summary.queued;
        else if (it.state == "skipped_offline") ++v.summary.skipped_offline;
        else if (it.state == "failed") ++v.summary.failed;
        else if (it.state == "cancelled") ++v.summary.cancelled;
        else if (it.state == "sent") {
            if (auto c = db_.findCommand(it.command_id)) {
                iv.command_state = c->state;
                iv.command_result = c->result;
                if (c->state == "pending") ++v.summary.pending;
                else if (c->state == "executed") ++v.summary.executed;
                else ++v.summary.unconfirmed;
            }
        }
        v.items.push_back(std::move(iv));
    }
    return v;
}

std::optional<JobView> CommandService::jobOverview(const std::string& job_id) {
    auto job = db_.findJob(job_id);
    if (!job) return std::nullopt;
    JobView v;
    v.job = *job;
    for (const auto& c : db_.jobItemCounts(job_id)) {
        const auto n = static_cast<std::size_t>(c.n);
        v.summary.total += n;
        if (c.item_state == "queued") v.summary.queued += n;
        else if (c.item_state == "skipped_offline") v.summary.skipped_offline += n;
        else if (c.item_state == "failed") v.summary.failed += n;
        else if (c.item_state == "cancelled") v.summary.cancelled += n;
        else if (c.item_state == "sent" && c.has_command) {
            if (c.command_state == "pending") v.summary.pending += n;
            else if (c.command_state == "executed") v.summary.executed += n;
            else v.summary.unconfirmed += n;
        }
    }
    return v;
}

}  // namespace liveqx::players
