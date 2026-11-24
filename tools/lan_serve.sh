#!/usr/bin/env bash
# Start the LAN server detached, so it outlives the shell/agent/terminal that started it.
#
# Every previous "it works from localhost, but nobody else can reach it" report here traced back to
# the server having been started as a plain background job of an interactive shell: when that shell
# exited, the job got SIGHUP, and the multiplayer child was taken with it. curl then reports
# "connection refused" on 8123/8443/8133 - which looks exactly like a firewall or a wrong address,
# and is neither.
#
#   tools/lan_serve.sh start|stop|restart|status|logs
#
# It refuses to report success unless the listeners are actually accepting afterwards, because the
# whole point is that a message saying "it is up" was not enough evidence.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
ROOT="$PWD"
PIDFILE="build/serve.pid"
LOG="build/logs/serve.log"
HTTP_PORT="${SWBF2_HTTP_PORT:-8123}"
TLS_PORT="${SWBF2_TLS_PORT:-8443}"
MP_PORT="${SWBF2_MP_PORT:-8133}"
mkdir -p build/logs

alive() { [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; }

# A listener test is the only honest readiness check: a pid can exist while bind failed.
listening() {
  local port="$1"
  (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null && { exec 3>&- 3<&-; return 0; } || return 1
}

status() {
  if alive; then
    echo "serve.py: running, pid $(cat "$PIDFILE")"
  else
    echo "serve.py: NOT running"
  fi
  for p in "$HTTP_PORT" "$TLS_PORT" "$MP_PORT"; do
    if listening "$p"; then echo "  port $p: accepting"; else echo "  port $p: nothing listening"; fi
  done
  ss -ltn 2>/dev/null | grep -E ":(${HTTP_PORT}|${TLS_PORT}|${MP_PORT})\b" | sed 's/^/  /'
}

case "${1:-status}" in
  start)
    if alive; then echo "already running, pid $(cat "$PIDFILE")"; status; exit 0; fi
    # shellcheck disable=SC2086
    setsid nohup python3 tools/serve.py "$HTTP_PORT" --host 0.0.0.0 --tls-port "$TLS_PORT" \
      --mp-port "$MP_PORT" ${SWBF2_SERVE_ARGS:-} >"$LOG" 2>&1 < /dev/null &
    echo $! > "$PIDFILE"
    # Give the child time to bind, then prove it bound instead of assuming it did.
    ok=0
    for _ in $(seq 1 40); do
      sleep 0.25
      if listening "$HTTP_PORT"; then ok=1; break; fi
      if ! alive; then break; fi
    done
    if [[ $ok -ne 1 ]]; then
      echo "FAILED to start; last lines of $LOG:" >&2
      tail -20 "$LOG" >&2
      rm -f "$PIDFILE"
      exit 1
    fi
    if ! listening "$TLS_PORT"; then
      echo "WARNING: HTTPS port $TLS_PORT is not accepting. LAN devices reaching this server over" >&2
      echo "         plain http will not get WebGPU (no Secure Context). See $LOG." >&2
    fi
    if ! listening "$MP_PORT"; then
      echo "WARNING: multiplayer signaling port $MP_PORT is not accepting; /api and /ws will 502." >&2
    fi
    echo "started, pid $(cat "$PIDFILE") (detached: survives this shell)"
    sed 's/^/  /' "$LOG"
    ;;
  stop)
    if alive; then
      pid="$(cat "$PIDFILE")"
      # The whole process group, so the node multiplayer child goes too, not just the parent.
      kill -TERM -"$(ps -o pgid= "$pid" | tr -d ' ')" 2>/dev/null || kill -TERM "$pid" 2>/dev/null
      for _ in $(seq 1 20); do alive || break; sleep 0.25; done
      alive && { pkill -f "serve.py $HTTP_PORT" 2>/dev/null; pkill -f "multiplayer_server.mjs $MP_PORT" 2>/dev/null; }
      echo "stopped"
    elif listening "$HTTP_PORT" || listening "$MP_PORT"; then
      pkill -f "serve.py $HTTP_PORT" 2>/dev/null || true
      pkill -f "multiplayer_server.mjs $MP_PORT" 2>/dev/null || true
      sleep 0.5
      echo "stopped orphaned server on ports $HTTP_PORT/$MP_PORT"
    else
      echo "was not running"
    fi
    rm -f "$PIDFILE"
    ;;
  restart) "$0" stop; sleep 0.5; "$0" start ;;
  logs) tail -n "${2:-60}" "$LOG" ;;
  status) status ;;
  *) echo "usage: $0 start|stop|restart|status|logs [n]" >&2; exit 2 ;;
esac
status
