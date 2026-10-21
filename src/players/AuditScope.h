#pragma once
//
// Where an audit row about players happened (B13): the object and group of the
// screen, group or object it is about, as they are when the row is written.
// Used as the audit logger's scope resolver.
//
// Things that are already deleted cannot be asked; the handler that deletes them
// places the row itself (setAuditScopeOverride). An object is placed without any
// lookup, so even the row about deleting it carries its place.

#include "audit/AuditTypes.h"
#include "players/PlayersDb.h"

#include <optional>
#include <string>

namespace liveqx::players {

inline std::optional<audit::AuditScope> auditScopeOf(PlayersDb& db, const audit::AuditEvent& e) {
    audit::AuditScope sc;
    try {
        if (e.target_type == "player") {
            if (auto place = db.placeOfPlayer(e.target_id)) {
                sc.object_id = place->object_id;
                sc.group_id  = place->group_id;
                return sc;
            }
        } else if (e.target_type == "player_group" && !e.target_id.empty()) {
            if (auto g = db.findGroup(std::stoll(e.target_id))) {
                sc.object_id = g->object_id;
                sc.group_id  = g->id;
                return sc;
            }
        } else if (e.target_type == "player_object" && !e.target_id.empty()) {
            sc.object_id = std::stoll(e.target_id);
            return sc;
        }
    } catch (...) {
        // a target id that is not a number: no place
    }
    return std::nullopt;
}

}  // namespace liveqx::players
