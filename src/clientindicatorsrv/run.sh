#!/usr/bin/env bash
set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SELF_DIR/../.." && pwd)"

if [[ -f "$SELF_DIR/.env" ]]; then
	set -a
	# shellcheck disable=SC1091
	source "$SELF_DIR/.env"
	set +a
fi

BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
STATE_DIR="${STATE_DIR:-$ROOT_DIR/run/clientindicatorsrv}"
BIN_PATH="${BIN_PATH:-$BUILD_DIR/clientindicatorsrv/release/bestclient-indicator-server}"

TOKEN_PATH="${TOKEN_PATH:-$STATE_DIR/shared-token.txt}"
JSON_PATH="${JSON_PATH:-$STATE_DIR/users.json}"
TLS_DIR="${TLS_DIR:-$STATE_DIR/tls}"
TLS_CERT_FILE="${TLS_CERT_FILE:-$TLS_DIR/cert.pem}"
TLS_KEY_FILE="${TLS_KEY_FILE:-$TLS_DIR/key.pem}"
PUBLIC_HOST="${PUBLIC_HOST:-150.241.70.188}"

PID_PATH="${PID_PATH:-$STATE_DIR/bestclient-indicator-server.pid}"
LOG_PATH="${LOG_PATH:-$STATE_DIR/bestclient-indicator-server.log}"
MODE="${1:-run}"

mkdir -p "$STATE_DIR" "$TLS_DIR"

pid_is_running() {
	[[ -s "$PID_PATH" ]] || return 1
	local pid
	pid="$(cat "$PID_PATH")"
	[[ -n "$pid" ]] && kill -0 "$pid" 2> /dev/null
}

cleanup_pidfile_if_stale() {
	if ! pid_is_running; then
		rm -f "$PID_PATH"
	fi
}

ensure_setup() {
	cleanup_pidfile_if_stale
	mkdir -p "$STATE_DIR" "$TLS_DIR"

	if [[ ! -x "$BIN_PATH" ]]; then
		echo "bestclient-indicator-server not built, building it..."
		cmake --build "$BUILD_DIR" --target bestclient-indicator-server -j"$(nproc)"
	fi

	if [[ ! -s "$TOKEN_PATH" && -z "${BC_CLIENT_INDICATOR_SHARED_TOKEN:-}" ]]; then
		echo "Generating shared token: $TOKEN_PATH"
		openssl rand -hex 32 > "$TOKEN_PATH"
		chmod 600 "$TOKEN_PATH"
	fi

	if [[ ! -s "$TLS_CERT_FILE" || ! -s "$TLS_KEY_FILE" ]]; then
		echo "Generating self-signed TLS certificate for $PUBLIC_HOST"
		openssl req -x509 -newkey rsa:4096 -sha256 -days 3650 -nodes \
			-keyout "$TLS_KEY_FILE" \
			-out "$TLS_CERT_FILE" \
			-subj "/CN=$PUBLIC_HOST" \
			-addext "subjectAltName = IP:$PUBLIC_HOST"
		chmod 600 "$TLS_KEY_FILE"
		chmod 644 "$TLS_CERT_FILE"
	fi
}

print_config() {
	echo "BestClient indicator server"
	echo "  UDP:      ${UDP_BIND:-0.0.0.0:8778}"
	echo "  Web:      https://${WEB_HOST:-0.0.0.0}:${WEB_PORT:-8779}/users.json"
	echo "  JSON:     $JSON_PATH"
	echo "  Token:    $TOKEN_PATH"
	echo "  TLS cert: $TLS_CERT_FILE"
	echo "  TLS key:  $TLS_KEY_FILE"
	echo "  PID:      $PID_PATH"
	echo "  LOG:      $LOG_PATH"
}

run_server() {
	ensure_setup
	if pid_is_running; then
		echo "bestclient-indicator-server is already running" >&2
		exit 1
	fi
	print_config
	echo $$ > "$PID_PATH"
	trap 'rm -f "$PID_PATH"' EXIT
	STATE_DIR="$STATE_DIR" TOKEN_PATH="$TOKEN_PATH" JSON_PATH="$JSON_PATH" TLS_CERT_FILE="$TLS_CERT_FILE" TLS_KEY_FILE="$TLS_KEY_FILE" exec "$BIN_PATH"
}

start_server() {
	ensure_setup
	if pid_is_running; then
		echo "bestclient-indicator-server is already running"
		exit 0
	fi
	: > "$LOG_PATH"
	nohup "$0" run >> "$LOG_PATH" 2>&1 &
	echo $! > "$PID_PATH"
	sleep 1
	if pid_is_running; then
		echo "Started bestclient-indicator-server"
		print_config
	else
		echo "Failed to start bestclient-indicator-server" >&2
		exit 1
	fi
}

stop_server() {
	if pid_is_running; then
		local pid
		pid="$(cat "$PID_PATH")"
		kill -TERM "$pid" 2> /dev/null || true
		for _ in $(seq 1 20); do
			if ! kill -0 "$pid" 2> /dev/null; then
				break
			fi
			sleep 1
		done
		kill -KILL "$pid" 2> /dev/null || true
	fi
	rm -f "$PID_PATH"
	echo "Stopped bestclient-indicator-server"
}

status_server() {
	cleanup_pidfile_if_stale
	if pid_is_running; then
		echo "Server: running (PID $(cat "$PID_PATH"))"
		exit 0
	fi
	echo "Server: stopped"
	exit 1
}

case "$MODE" in
run) run_server ;;
start) start_server ;;
stop) stop_server ;;
restart)
	stop_server
	start_server
	;;
status) status_server ;;
*)
	echo "Usage: $0 [run|start|stop|restart|status]" >&2
	exit 1
	;;
esac
