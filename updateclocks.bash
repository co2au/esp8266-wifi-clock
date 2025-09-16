#!/bin/bash
set -euo pipefail

# --- Config -------------------------------------------------------
BIN_DEFAULT="build/esp8266.esp8266.generic/wifi_clock_mqtt.ino.bin"
BIN_PATH="${1:-$BIN_DEFAULT}"         # allow BIN path override: ./updateclocks.bash out.bin
CLOCKS=(10.246.40.70 10.246.40.69)    # add more IPs here

# If you enabled HTTP OTA auth in the web UI, set these (or export before running):
HOTA_USER="${HOTA_USER:-admin}"
HOTA_PASS="${HOTA_PASS:-}"             # leave blank if no auth

# curl timeouts/retries
CONNECT_TIMEOUT=5
MAX_TIME=120
RETRIES=2
RETRY_DELAY=2

# --- Helpers ------------------------------------------------------
ok()   { printf "\033[1;32m✓\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m!\033[0m %s\n" "$*" >&2; }
err()  { printf "\033[1;31m✗ %s\033[0m\n" "$*" >&2; }

need() {
  command -v "$1" >/dev/null 2>&1 || { err "missing dependency: $1"; exit 127; }
}

check_bin() {
  [[ -f "$BIN_PATH" ]] || { err "binary not found: $BIN_PATH"; exit 1; }
  [[ -s "$BIN_PATH" ]] || { err "binary is empty: $BIN_PATH"; exit 1; }
  ok "Using firmware: $BIN_PATH ($(du -h "$BIN_PATH" | awk '{print $1}'))"
}

probe() {
  local ip=$1
  curl -sS -m "$CONNECT_TIMEOUT" "http://$ip/config.json" \
    --fail >/dev/null || return 1
}

push_update() {
  local ip=$1
  local url="http://$ip/update"

  # Build curl auth args only if password set (to avoid digest negotiation when not needed)
  local auth_args=()
  if [[ -n "$HOTA_PASS" ]]; then
    auth_args=(--digest -u "$HOTA_USER:$HOTA_PASS")
  fi

  local tmp_body
  tmp_body=$(mktemp)
  # Capture HTTP code to a variable; body to a temp file
  local code
  code=$(curl -sS \
    --connect-timeout "$CONNECT_TIMEOUT" \
    --max-time "$MAX_TIME" \
    --retry "$RETRIES" --retry-delay "$RETRY_DELAY" \
    -F "update=@${BIN_PATH}" \
    "${auth_args[@]}" \
    "$url" \
    -w "%{http_code}" -o "$tmp_body") || { rm -f "$tmp_body"; return 2; }

  # Show a friendly one-liner with both code and short body
  local body
  body=$(tr -d '\r' < "$tmp_body" | head -c 160)
  rm -f "$tmp_body"

  echo "${code}! HTTP status ${body} from $ip"

  # Treat 200/202 as success
  if [[ "$code" == "200" || "$code" == "202" ]]; then
    return 0
  else
    return 3
  fi

  # Alternative success test
  if [[ "$code" =~ ^20(0|2)$ ]] && [[ "$body" == *"Update Success!"* ]]; then
    return 0
  fi
}


# --- Main ---------------------------------------------------------
need curl
check_bin

for ip in "${CLOCKS[@]}"; do
  echo "== Updating $ip =="

  if probe "$ip"; then
    ok "Clock reachable at $ip"
  else
    warn "Clock $ip not reachable (no /config.json); attempting update anyway…"
  fi

  if push_update "$ip"; then
    ok "OTA pushed to $ip — device will reboot"
  else
    err "OTA failed for $ip"
  fi
  echo
done

ok "All done."

