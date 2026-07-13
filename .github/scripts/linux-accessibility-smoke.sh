#!/usr/bin/env bash
# External Linux accessibility smoke. The fixture must already be built; this
# runner deliberately does not guess a build directory or invoke Zig.
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
probe_source="$repo_root/tests/accessibility/linux_atspi_probe.c"
inside_session="${NATIVE_A11Y_INSIDE_SESSION:-0}"

usage() {
  cat >&2 <<'EOF'
usage: linux-accessibility-smoke.sh APP_BINARY [APP_ARGUMENT ...]

The application must already be built. It can instead be supplied through
NATIVE_A11Y_APP_BINARY. Useful overrides:

  NATIVE_A11Y_BACKEND=auto|xvfb|session  (default: auto)
  NATIVE_A11Y_APP_NAME=accessibility-smoke
  NATIVE_A11Y_TIMEOUT_MS=30000
  NATIVE_A11Y_SETTLE_MS=5000
  NATIVE_A11Y_OVERALL_TIMEOUT_SECONDS=90
  NATIVE_A11Y_EXPECTED_POSITION=42
  NATIVE_A11Y_EXPECTED_COUNT=1000
  NATIVE_A11Y_EXPECTED_ROWS=7
  NATIVE_A11Y_ORCA_SMOKE=0|1          (default: 0)
  NATIVE_A11Y_ARTIFACTS_DIR=/path/to/artifact-parent

The session backend is an explicit local-debug escape hatch and requires an
existing DISPLAY or WAYLAND_DISPLAY. Auto requires Xvfb. GTK's Broadway
backend is not used: GTK 4 does not register Broadway applications with the
AT-SPI registry, so a Broadway run would be a false smoke test.
EOF
}

diagnostics() {
  local temp_dir="${NATIVE_A11Y_TEMP_DIR:-}"
  echo "---- Linux accessibility diagnostics ----" >&2
  if [[ -z "$temp_dir" || ! -d "$temp_dir" ]]; then
    echo "temporary diagnostics directory is unavailable" >&2
  else
    local log
    for log in backend.log a11y-bus.log a11y-address.log app.log probe.log session.log \
      orca.version.log orca.stdout.log orca.stderr.log orca.log orca-driver.log orca-shutdown.log \
      speechd.stdout.log speechd.stderr.log speechd-shutdown.log; do
      echo "-- $log" >&2
      if [[ -f "$temp_dir/$log" ]]; then
        sed -n '1,400p' "$temp_dir/$log" >&2
      else
        echo "(missing)" >&2
      fi
    done
  fi
  echo "-----------------------------------------" >&2
}

fail() {
  echo "FAIL: $1" >&2
  [[ "$inside_session" == 1 ]] || diagnostics
  exit 1
}

find_a11y_launcher() {
  local candidate
  if command -v at-spi-bus-launcher >/dev/null 2>&1; then
    command -v at-spi-bus-launcher
    return 0
  fi
  for candidate in \
    /usr/libexec/at-spi-bus-launcher \
    /usr/lib/at-spi-bus-launcher \
    /usr/lib/at-spi2-core/at-spi-bus-launcher; do
    if [[ -x "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}

start_xvfb() {
  local candidate
  local attempt

  command -v Xvfb >/dev/null 2>&1 || return 1
  for attempt in $(seq 0 19); do
    candidate=$((90 + (RANDOM % 100) + attempt))
    Xvfb ":$candidate" -screen 0 1280x800x24 -ac -nolisten tcp -noreset \
      >"$NATIVE_A11Y_TEMP_DIR/backend.log" 2>&1 &
    backend_pid=$!
    sleep 0.2
    if kill -0 "$backend_pid" >/dev/null 2>&1; then
      export DISPLAY=":$candidate"
      export GDK_BACKEND=x11
      unset WAYLAND_DISPLAY BROADWAY_DISPLAY
      echo "backend=xvfb display=$DISPLAY" >>"$NATIVE_A11Y_TEMP_DIR/backend.log"
      return 0
    fi
    wait "$backend_pid" >/dev/null 2>&1 || true
    backend_pid=""
  done
  return 1
}

use_session_display() {
  if [[ -n "${WAYLAND_DISPLAY:-}" && -n "${XDG_RUNTIME_DIR:-}" && -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]]; then
    export GDK_BACKEND=wayland
    unset DISPLAY BROADWAY_DISPLAY
    echo "backend=session-wayland display=$WAYLAND_DISPLAY" >"$NATIVE_A11Y_TEMP_DIR/backend.log"
    return 0
  fi
  if [[ -n "${DISPLAY:-}" ]]; then
    export GDK_BACKEND=x11
    unset WAYLAND_DISPLAY BROADWAY_DISPLAY
    echo "backend=session-x11 display=$DISPLAY" >"$NATIVE_A11Y_TEMP_DIR/backend.log"
    return 0
  fi
  return 1
}

if [[ "$inside_session" != 1 ]]; then
  if [[ "${1:-}" == "--help" ]]; then
    usage
    exit 0
  fi
  app_binary="${NATIVE_A11Y_APP_BINARY:-}"
  if [[ -z "$app_binary" ]]; then
    if [[ $# -eq 0 ]]; then
      usage
      exit 2
    fi
    app_binary=$1
    shift
  fi
  if [[ ! -x "$app_binary" ]]; then
    echo "application binary is not executable: $app_binary" >&2
    exit 2
  fi
  app_binary="$(cd "$(dirname "$app_binary")" && pwd)/$(basename "$app_binary")"

  for dependency in cc dbus-run-session gdbus pkg-config setsid timeout; do
    command -v "$dependency" >/dev/null 2>&1 || {
      echo "missing required command: $dependency" >&2
      exit 2
    }
  done
  pkg-config --exists atspi-2 gobject-2.0 || {
    echo "missing development packages: atspi-2 and gobject-2.0" >&2
    exit 2
  }

  orca_smoke="${NATIVE_A11Y_ORCA_SMOKE:-0}"
  [[ "$orca_smoke" == 0 || "$orca_smoke" == 1 ]] || {
    echo "NATIVE_A11Y_ORCA_SMOKE must be 0 or 1" >&2
    exit 2
  }
  if [[ "$orca_smoke" == 1 ]]; then
    for dependency in orca pgrep speech-dispatcher xdotool; do
      command -v "$dependency" >/dev/null 2>&1 || {
        echo "missing required Orca smoke command: $dependency" >&2
        exit 2
      }
    done
  fi

  temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/native-linux-a11y.XXXXXX")"
  export NATIVE_A11Y_TEMP_DIR="$temp_dir"
  artifact_run_dir=""
  if [[ -n "${NATIVE_A11Y_ARTIFACTS_DIR:-}" ]]; then
    mkdir -p "$NATIVE_A11Y_ARTIFACTS_DIR"
    artifact_run_dir="$(mktemp -d "$NATIVE_A11Y_ARTIFACTS_DIR/native-linux-a11y.XXXXXX")"
  fi
  cleanup_outer() {
    if [[ -n "$artifact_run_dir" ]]; then
      cp -a "$temp_dir/." "$artifact_run_dir/"
      echo "Linux accessibility artifacts: $artifact_run_dir" >&2
    fi
    rm -rf "$temp_dir"
  }
  trap cleanup_outer EXIT

  cflags="$(pkg-config --cflags atspi-2 gobject-2.0)"
  libs="$(pkg-config --libs atspi-2 gobject-2.0)"
  # Intentional word splitting: pkg-config emits one compiler/linker token per
  # whitespace-delimited field.
  # shellcheck disable=SC2086
  cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror $cflags \
    "$probe_source" -o "$temp_dir/linux_atspi_probe" $libs \
    || fail "AT-SPI probe compilation failed"

  overall_timeout="${NATIVE_A11Y_OVERALL_TIMEOUT_SECONDS:-90}"
  [[ "$overall_timeout" =~ ^[1-9][0-9]*$ ]] || fail "NATIVE_A11Y_OVERALL_TIMEOUT_SECONDS must be a positive integer"

  export NATIVE_A11Y_INSIDE_SESSION=1
  # D-Bus-activated at-spi-bus-launcher inherits the environment captured by
  # dbus-run-session, not variables exported later by the inner runner.
  export ATSPI_DBUS_IMPLEMENTATION=dbus-daemon
  timeout --signal=TERM --kill-after=5s "${overall_timeout}s" \
    dbus-run-session -- "$0" "$app_binary" "$@" \
    >"$temp_dir/session.log" 2>&1
  status=$?
  if [[ $status -ne 0 ]]; then
    if [[ $status -eq 124 || $status -eq 137 ]]; then
      echo "FAIL: Linux accessibility smoke exceeded ${overall_timeout}s" >&2
    else
      echo "FAIL: Linux accessibility smoke exited with status $status" >&2
    fi
    diagnostics
  else
    cat "$temp_dir/probe.log"
    if [[ "$orca_smoke" == 1 ]]; then
      cat "$temp_dir/orca.version.log"
      grep -E 'SPEECH OUTPUT:.*(Lesson 42|Count action)' "$temp_dir/orca.log"
      cat "$temp_dir/orca-shutdown.log"
      echo "PASS: installed Orca announced Lesson 42 and Count action"
    fi
  fi
  exit "$status"
fi

app_binary=$1
shift
backend_pid=""
bus_pid=""
probe_pid=""
app_pid=""
orca_pid=""
orca_driver_pid=""
speechd_pid=""

cleanup_inner() {
  local attempt

  if [[ -n "$probe_pid" ]] && kill -0 "$probe_pid" >/dev/null 2>&1; then
    kill "$probe_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "$orca_driver_pid" ]] && kill -0 "$orca_driver_pid" >/dev/null 2>&1; then
    kill "$orca_driver_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "$orca_pid" ]] && kill -0 "$orca_pid" >/dev/null 2>&1; then
    kill -TERM -- "-$orca_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "$speechd_pid" ]] && kill -0 "$speechd_pid" >/dev/null 2>&1; then
    kill -TERM -- "-$speechd_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "$app_pid" ]] && kill -0 "$app_pid" >/dev/null 2>&1; then
    kill -TERM -- "-$app_pid" >/dev/null 2>&1 || true
    for attempt in 1 2 3 4 5; do
      kill -0 "$app_pid" >/dev/null 2>&1 || break
      sleep 0.1
    done
    kill -KILL -- "-$app_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "$backend_pid" ]] && kill -0 "$backend_pid" >/dev/null 2>&1; then
    kill "$backend_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "$bus_pid" ]] && kill -0 "$bus_pid" >/dev/null 2>&1; then
    kill "$bus_pid" >/dev/null 2>&1 || true
  fi
  [[ -n "$probe_pid" ]] && wait "$probe_pid" >/dev/null 2>&1 || true
  [[ -n "$orca_driver_pid" ]] && wait "$orca_driver_pid" >/dev/null 2>&1 || true
  [[ -n "$app_pid" ]] && wait "$app_pid" >/dev/null 2>&1 || true
  [[ -n "$orca_pid" ]] && wait "$orca_pid" >/dev/null 2>&1 || true
  [[ -n "$speechd_pid" ]] && wait "$speechd_pid" >/dev/null 2>&1 || true
  [[ -n "$backend_pid" ]] && wait "$backend_pid" >/dev/null 2>&1 || true
  [[ -n "$bus_pid" ]] && wait "$bus_pid" >/dev/null 2>&1 || true
}
trap cleanup_inner EXIT
trap 'exit 143' HUP INT TERM

mkdir -p "$NATIVE_A11Y_TEMP_DIR/cache" "$NATIVE_A11Y_TEMP_DIR/config" "$NATIVE_A11Y_TEMP_DIR/data" \
  "$NATIVE_A11Y_TEMP_DIR/speechd" "$NATIVE_A11Y_TEMP_DIR/speechd-config" \
  "$NATIVE_A11Y_TEMP_DIR/speechd-logs"
export XDG_CACHE_HOME="$NATIVE_A11Y_TEMP_DIR/cache"
export XDG_CONFIG_HOME="$NATIVE_A11Y_TEMP_DIR/config"
export XDG_DATA_HOME="$NATIVE_A11Y_TEMP_DIR/data"
export GSETTINGS_BACKEND=keyfile
# An inherited GTK_A11Y=none would invalidate this test. Let the AT-SPI status
# service select GTK's real backend; never disable accessibility here.
unset GTK_A11Y AT_SPI_BUS_ADDRESS NO_AT_BRIDGE
export ATSPI_DBUS_IMPLEMENTATION=dbus-daemon

case "${NATIVE_A11Y_BACKEND:-auto}" in
  auto)
    start_xvfb || fail "Xvfb is required for an isolated smoke; install it or explicitly use NATIVE_A11Y_BACKEND=session"
    ;;
  xvfb)
    start_xvfb || fail "NATIVE_A11Y_BACKEND=xvfb requested, but Xvfb could not start"
    ;;
  session)
    use_session_display || fail "NATIVE_A11Y_BACKEND=session requires a usable DISPLAY or WAYLAND_DISPLAY"
    ;;
  *)
    fail "NATIVE_A11Y_BACKEND must be auto, xvfb, or session"
    ;;
esac

a11y_launcher="$(find_a11y_launcher)" || fail "at-spi-bus-launcher is not installed"
"$a11y_launcher" --launch-immediately --a11y=1 --screen-reader=1 \
  >"$NATIVE_A11Y_TEMP_DIR/a11y-bus.log" 2>&1 &
bus_pid=$!

# Waiting for the name does not activate it. Calling GetAddress immediately
# can race the explicit launcher and D-Bus-activate a second launcher with the
# host's default implementation.
timeout 10s gdbus wait --session --timeout=10 org.a11y.Bus \
  >>"$NATIVE_A11Y_TEMP_DIR/a11y-bus.log" 2>&1 \
  || fail "the AT-SPI accessibility bus name did not become ready"
timeout 5s gdbus call --session \
  --dest org.a11y.Bus \
  --object-path /org/a11y/bus \
  --method org.a11y.Bus.GetAddress \
  >"$NATIVE_A11Y_TEMP_DIR/a11y-address.log" 2>>"$NATIVE_A11Y_TEMP_DIR/a11y-bus.log" \
  || fail "the AT-SPI accessibility bus did not return its address"

orca_smoke="${NATIVE_A11Y_ORCA_SMOKE:-0}"
if [[ "$orca_smoke" == 1 ]]; then
  [[ "${NATIVE_A11Y_BACKEND:-auto}" != session ]] \
    || fail "the Orca smoke requires the isolated Xvfb backend"
fi

probe_args=(
  --app-name "${NATIVE_A11Y_APP_NAME:-accessibility-smoke}"
  --root-name "${NATIVE_A11Y_ROOT_NAME:-Accessibility smoke}"
  --list-name "${NATIVE_A11Y_LIST_NAME:-Lesson list}"
  --item-name "${NATIVE_A11Y_ITEM_NAME:-Lesson 42}"
  --offscreen-item-name "${NATIVE_A11Y_OFFSCREEN_ITEM_NAME:-Lesson 40}"
  --full-item-name "${NATIVE_A11Y_FULL_ITEM_NAME:-Lesson 43}"
  --timeout-ms "${NATIVE_A11Y_TIMEOUT_MS:-30000}"
  --settle-ms "${NATIVE_A11Y_SETTLE_MS:-5000}"
  --expected-position "${NATIVE_A11Y_EXPECTED_POSITION:-42}"
  --expected-count "${NATIVE_A11Y_EXPECTED_COUNT:-1000}"
  --expected-rows "${NATIVE_A11Y_EXPECTED_ROWS:-7}"
)
"$NATIVE_A11Y_TEMP_DIR/linux_atspi_probe" "${probe_args[@]}" \
  >"$NATIVE_A11Y_TEMP_DIR/probe.log" 2>&1 &
probe_pid=$!

# The probe registers its object-event listener before the application starts,
# matching a real assistive technology and making dynamic GTK AT-SPI enablement
# deterministic.
sleep 0.2
setsid "$app_binary" "$@" >"$NATIVE_A11Y_TEMP_DIR/app.log" 2>&1 &
app_pid=$!

wait "$probe_pid"
probe_status=$?
probe_pid=""
if [[ $probe_status -ne 0 ]]; then
  fail "external AT-SPI probe failed"
fi
kill -0 "$app_pid" >/dev/null 2>&1 || fail "application exited before the probe completed"

if [[ "$orca_smoke" == 1 ]]; then
  export SPEECHD_SOCKET="$NATIVE_A11Y_TEMP_DIR/speechd/speechd.sock"
  speechd_config_source="${NATIVE_A11Y_SPEECHD_CONFIG_DIR:-/etc/speech-dispatcher}"
  [[ -f "$speechd_config_source/speechd.conf" ]] \
    || fail "Speech Dispatcher configuration is missing from $speechd_config_source"
  cp -a "$speechd_config_source/." "$NATIVE_A11Y_TEMP_DIR/speechd-config/" \
    || fail "Speech Dispatcher configuration could not be staged"
  printf '\nAudioOutputMethod "alsa"\nAudioALSADevice "null"\n' \
    >>"$NATIVE_A11Y_TEMP_DIR/speechd-config/speechd.conf"
  speechd_args=(
    --run-single
    --communication-method unix_socket
    --socket-path "$SPEECHD_SOCKET"
    --pid-file "$NATIVE_A11Y_TEMP_DIR/speechd.pid"
    --log-dir "$NATIVE_A11Y_TEMP_DIR/speechd-logs"
    --config-dir "$NATIVE_A11Y_TEMP_DIR/speechd-config"
    --timeout 0
  )
  if [[ -n "${NATIVE_A11Y_SPEECHD_MODULE_DIR:-}" ]]; then
    speechd_args+=(--module-dir "$NATIVE_A11Y_SPEECHD_MODULE_DIR")
  fi
  setsid speech-dispatcher "${speechd_args[@]}" \
    >"$NATIVE_A11Y_TEMP_DIR/speechd.stdout.log" 2>"$NATIVE_A11Y_TEMP_DIR/speechd.stderr.log" &
  speechd_pid=$!
  for attempt in $(seq 1 100); do
    [[ -S "$SPEECHD_SOCKET" ]] && break
    kill -0 "$speechd_pid" >/dev/null 2>&1 || fail "private Speech Dispatcher exited before creating its socket"
    sleep 0.1
  done
  [[ -S "$SPEECHD_SOCKET" ]] || fail "private Speech Dispatcher did not create its socket"

  orca --version >"$NATIVE_A11Y_TEMP_DIR/orca.version.log" 2>&1 \
    || fail "installed Orca did not report its version"
  setsid env LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 orca \
    --debug-file "$NATIVE_A11Y_TEMP_DIR/orca.log" \
    >"$NATIVE_A11Y_TEMP_DIR/orca.stdout.log" 2>"$NATIVE_A11Y_TEMP_DIR/orca.stderr.log" &
  orca_pid=$!
  orca_ready=""
  for attempt in $(seq 1 300); do
    if gdbus call --session \
      --dest org.freedesktop.DBus \
      --object-path /org/freedesktop/DBus \
      --method org.freedesktop.DBus.NameHasOwner org.gnome.Orca.Service \
      2>/dev/null | grep -Fq true; then
      orca_ready="dbus"
      break
    fi
    if grep -Fq 'ALSA: End of playback on ALSA' \
      "$NATIVE_A11Y_TEMP_DIR/speechd-logs/speech-dispatcher.log" 2>/dev/null; then
      orca_ready="speech"
      break
    fi
    kill -0 "$orca_pid" >/dev/null 2>&1 || fail "Orca exited before its AT-SPI event loop started"
    sleep 0.1
  done
  [[ -n "$orca_ready" ]] || fail "Orca did not finish screen-reader startup"
  sleep 0.5
  kill -0 "$orca_pid" >/dev/null 2>&1 || fail "Orca exited immediately after screen-reader startup"

  (
    window_id=""
    for attempt in $(seq 1 150); do
      window_id="$(xdotool search --onlyvisible --pid "$app_pid" 2>/dev/null | head -n 1)"
      [[ -n "$window_id" ]] && break
      sleep 0.1
    done
    [[ -n "$window_id" ]] || {
      echo "the accessibility fixture did not create a visible X11 window" >&2
      exit 1
    }
    xdotool windowfocus --sync "$window_id"
    focused_window="$(xdotool getwindowfocus)"
    [[ "$focused_window" == "$window_id" ]] || {
      echo "X11 focus is $focused_window, expected fixture window $window_id" >&2
      exit 1
    }
    for attempt in $(seq 1 32); do
      xdotool key --clearmodifiers Tab
      sleep 0.25
    done
    echo "Sent 32 XTEST Tab presses through fixture window $window_id"
  ) >"$NATIVE_A11Y_TEMP_DIR/orca-driver.log" 2>&1 &
  orca_driver_pid=$!
  wait "$orca_driver_pid"
  orca_driver_status=$?
  orca_driver_pid=""
  [[ $orca_driver_status -eq 0 ]] || fail "installed Orca focus traversal failed"

  kill -TERM -- "-$orca_pid" >/dev/null 2>&1 || fail "Orca could not be asked to stop"
  for attempt in $(seq 1 100); do
    kill -0 "$orca_pid" >/dev/null 2>&1 || break
    sleep 0.1
  done
  if kill -0 "$orca_pid" >/dev/null 2>&1; then
    kill -KILL -- "-$orca_pid" >/dev/null 2>&1 || true
    fail "Orca did not stop after SIGTERM"
  fi
  wait "$orca_pid"
  orca_status=$?
  [[ $orca_status -eq 0 || $orca_status -eq 143 ]] \
    || fail "Orca exited with unexpected status $orca_status"
  pgrep -g "$orca_pid" >/dev/null 2>&1 && fail "Orca left a child process in its process group"
  orca_pid=""
  grep -Fq 'ORCA: Starting Atspi main event loop' "$NATIVE_A11Y_TEMP_DIR/orca.log" \
    || fail "Orca never entered its AT-SPI event loop"

  first_tab_line="$(grep -n -m 1 -F "SPEECH OUTPUT: 'tab'" "$NATIVE_A11Y_TEMP_DIR/orca.log" | cut -d: -f1)"
  tab_count="$(grep -Fc "SPEECH OUTPUT: 'tab'" "$NATIVE_A11Y_TEMP_DIR/orca.log")"
  [[ -n "$first_tab_line" && $tab_count -ge 16 ]] \
    || fail "Orca logged only ${tab_count:-0} Tab speech records"
  tail -n "+$first_tab_line" "$NATIVE_A11Y_TEMP_DIR/orca.log" \
    | grep -Eq 'SPEECH OUTPUT:.*Lesson 42' \
    || fail "Orca did not announce Lesson 42"
  tail -n "+$first_tab_line" "$NATIVE_A11Y_TEMP_DIR/orca.log" \
    | grep -Eq 'SPEECH OUTPUT:.*Count action' \
    || fail "Orca did not announce Count action"

  kill -TERM -- "-$speechd_pid" >/dev/null 2>&1 || fail "private Speech Dispatcher could not be asked to stop"
  for attempt in $(seq 1 50); do
    kill -0 "$speechd_pid" >/dev/null 2>&1 || break
    sleep 0.1
  done
  if kill -0 "$speechd_pid" >/dev/null 2>&1; then
    kill -KILL -- "-$speechd_pid" >/dev/null 2>&1 || true
    fail "private Speech Dispatcher did not stop after SIGTERM"
  fi
  wait "$speechd_pid"
  speechd_status=$?
  [[ $speechd_status -eq 0 || $speechd_status -eq 143 ]] \
    || fail "private Speech Dispatcher exited with unexpected status $speechd_status"
  pgrep -g "$speechd_pid" >/dev/null 2>&1 && fail "private Speech Dispatcher left a module process running"
  speechd_pid=""
  echo "Orca stopped after its speech assertions" >"$NATIVE_A11Y_TEMP_DIR/orca-shutdown.log"
  echo "Private Speech Dispatcher stopped without child processes" >"$NATIVE_A11Y_TEMP_DIR/speechd-shutdown.log"
fi

exit 0
