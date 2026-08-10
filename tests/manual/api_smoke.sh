#!/usr/bin/env bash
# Manual smoke test for stage 3.1 Control API.
#
# Prereqs: ./build/liveqx running with default config (api_port=8080,
# default channel id=1).
#
# Usage:
#   ./build/liveqx &        # in another shell, or with config/default.json
#   tests/manual/api_smoke.sh
#
# Exits non-zero on the first unexpected response.

set -euo pipefail

API="${API:-http://127.0.0.1:8080}"

# Tiny pretty-print helper — falls back to cat if jq is missing.
pp() { command -v jq >/dev/null && jq . || cat; }

echo "──> 1. List channels (bootstrap)"
curl -fsS "$API/api/channels" | pp
echo

echo "──> 2. Health"
curl -fsS "$API/api/health" | pp
echo

echo "──> 3. Create new channel id=42 (no auto-play)"
curl -fsS -X POST "$API/api/channels" \
    -H 'Content-Type: application/json' \
    -d '{
        "id": 42,
        "name": "smoke-test",
        "resolution": "320x240",
        "fps": 25,
        "bitrate": 1000000,
        "preset": "ultrafast",
        "default_photo_duration": 3.0,
        "output": {"port": 19042, "latency_ms": 200}
    }' | pp
echo

echo "──> 4. Get status (state must be 'stopped')"
curl -fsS "$API/api/channels/42" | pp
echo

echo "──> 5. Start it"
curl -fsS -X POST "$API/api/channels/42/play" | pp
echo

echo "──> 6. Status (state should be 'running' or healthy variant)"
curl -fsS "$API/api/channels/42" | pp
echo

echo "──> 7. Skip to next clip"
curl -fsS -X POST "$API/api/channels/42/next" | pp
echo

echo "──> 8. PUT updated bitrate"
curl -fsS -X PUT "$API/api/channels/42/config" \
    -H 'Content-Type: application/json' \
    -d '{"bitrate": 1500000}' | pp
echo

echo "──> 9. Stop channel"
curl -fsS -X POST "$API/api/channels/42/stop" | pp
echo

echo "──> 10. Replay → start the same channel again"
curl -fsS -X POST "$API/api/channels/42/play" | pp
echo

echo "──> 11. Stop + delete"
curl -fsS -X POST "$API/api/channels/42/stop" | pp
echo
curl -fsS -X DELETE "$API/api/channels/42" -o /dev/null -w "DELETE → %{http_code}\n"

echo "──> 12. Final list (channel 42 must be gone)"
curl -fsS "$API/api/channels" | pp
echo

echo "smoke OK"
