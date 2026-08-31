#pragma once

// Payload sanitisation for audit rows. Producers (middleware, domain
// handlers) call buildAuditDetailsFromBody() to turn a raw HTTP request
// body into the JSON string stored in audit_events.details_json.
//
// Guarantees:
//   * Recursively redacts keys whose name resembles a credential
//     (password/token/secret/…). Over-redacts on purpose — leaking a
//     password to the audit trail is worse than hiding a UI setting.
//   * Caps stored size at ~4KB. Larger bodies collapse to a size note
//     so the row still has forensic signal without bloating the DB.
//   * Never throws — parse failures land as {"body_note":"parse_error"}.
//
// This is the ONLY place secrets should be filtered before they hit
// AuditDb. Keep the sensitive-key list here canonical.

#include <string>

#include <nlohmann/json.hpp>

namespace liveqx::audit {

// In-place redaction. Values under sensitive keys become "[REDACTED]".
// Non-object/array values are unchanged. Case-insensitive substring match.
void redactSensitiveKeys(nlohmann::json& v);

// Build the details JSON string for an audit row from an HTTP request body.
// content_type is the raw Content-Type header value (may be empty).
// Return value is a valid JSON string, never empty ("{}" if nothing to say).
std::string buildAuditDetailsFromBody(const std::string& body,
                                      const std::string& content_type);

}  // namespace liveqx::audit
