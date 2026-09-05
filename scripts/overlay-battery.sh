#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Steam overlay on Linux — cross-environment compatibility battery
#
# Produces a single comparable report per machine. Self-contained: clones and
# builds steamworks-ffi-node itself, writes its own Electron harness, runs the
# battery, and prints a report you can paste back.
#
#   ./overlay-battery.sh                 # full run
#   ./overlay-battery.sh --no-build      # reuse an existing ./ffi-work build
#   ./overlay-battery.sh --branch NAME   # test a different branch
#
# Grab it on a fresh machine:
#   curl -fsSLO https://raw.githubusercontent.com/openfrontio/steamworks-ffi-node/josh/linux-overlay-integration/scripts/overlay-battery.sh
#   chmod +x overlay-battery.sh && ./overlay-battery.sh
#
# Requires: Steam running and logged in, a graphical session, and NO Steam game
# running anywhere on the account (the script refuses otherwise).
# ---------------------------------------------------------------------------
set -uo pipefail

BRANCH="josh/linux-overlay-integration"
REPO="https://github.com/openfrontio/steamworks-ffi-node.git"
DO_BUILD=1
WORK="${WORK:-$HOME/overlay-battery}"
REPORT="$WORK/report-$(hostname)-$(date +%Y%m%d-%H%M%S).txt"

while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) DO_BUILD=0 ;;
    --branch)   BRANCH="$2"; shift ;;
    --work)     WORK="$2"; shift ;;
    -h|--help)  sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac
  shift
done

mkdir -p "$WORK"
FFI="$WORK/ffi-work"
APP="$WORK/harness"

say()  { printf '%s\n' "$*" | tee -a "$REPORT"; }
hdr()  { printf '\n=== %s ===\n' "$*" | tee -a "$REPORT"; }
warn() { printf '  !! %s\n' "$*" | tee -a "$REPORT"; }

# Kill helper. NEVER use `pkill -f <pattern>` here: the pattern matches this
# script's own argv and kills the shell mid-run, silently, which makes the whole
# rig look flaky. Match on process NAME instead.
kill_by_name_and_arg() {
  local name="$1" needle="$2"
  ps -eo pid,comm,args | awk -v n="$name" -v s="$needle" \
    '$2==n && index($0,s) {print $1}' | while read -r p; do kill -9 "$p" 2>/dev/null; done
}

have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
hdr "PRECONDITIONS"

MISSING=""
for t in node npm gcc make python3 git; do have "$t" || MISSING="$MISSING $t"; done
if [ -n "$MISSING" ]; then
  warn "missing required tools:$MISSING"
  say  "  install (Debian/Ubuntu): sudo apt-get install -y nodejs npm build-essential python3 git"
  say  "  install (Arch):          sudo pacman -S --needed nodejs npm base-devel python git"
  exit 1
fi

OPTIONAL_MISSING=""
for t in glxinfo vulkaninfo xdotool import xwininfo xprop identify lspci; do
  have "$t" || OPTIONAL_MISSING="$OPTIONAL_MISSING $t"
done
[ -n "$OPTIONAL_MISSING" ] && {
  warn "missing optional tools (some rows will read 'unavailable'):$OPTIONAL_MISSING"
  say  "  Debian/Ubuntu: sudo apt-get install -y mesa-utils vulkan-tools xdotool imagemagick x11-utils pciutils"
  say  "  Arch:          sudo pacman -S --needed mesa-utils vulkan-tools xdotool imagemagick xorg-xwininfo xorg-xprop pciutils"
}

# Build headers. binding.gyp links these but nothing in the repo documents them.
HDRS_MISSING=""
for h in X11/Xlib.h X11/extensions/shape.h X11/extensions/Xfixes.h X11/extensions/Xcomposite.h GL/glx.h; do
  [ -f "/usr/include/$h" ] || HDRS_MISSING="$HDRS_MISSING $h"
done
if [ -n "$HDRS_MISSING" ] && [ "$DO_BUILD" = 1 ]; then
  warn "missing build headers:$HDRS_MISSING"
  say  "  Debian/Ubuntu: sudo apt-get install -y libx11-dev libxext-dev libxfixes-dev libxcomposite-dev libgl1-mesa-dev"
  say  "  Arch:          sudo pacman -S --needed libx11 libxext libxfixes libxcomposite mesa"
  exit 1
fi

if ! pgrep -x steam >/dev/null 2>&1; then
  warn "Steam is not running. Start it and sign in, then re-run."
  exit 1
fi
say "  Steam: running"

# One account cannot be in-game twice, and an already-attached game takes over
# the single gameoverlayui slot, which silently breaks the hotkey for our test.
# An attached overlay takes the single gameoverlayui slot. A LIVE one means a
# real game is running and we must not proceed. A stale one whose target pid is
# already dead is our own leftover, and is cleared rather than refused --
# otherwise an aborted run poisons every re-run.
BLOCKED=0
while read -r uipid target; do
  [ -z "${uipid:-}" ] && continue
  if kill -0 "$target" 2>/dev/null; then
    warn "a live process already has the Steam overlay attached (pid $target):"
    ps -p "$target" -o args= 2>/dev/null | sed 's/^/     /' | tee -a "$REPORT"
    BLOCKED=1
  else
    say "  clearing stale gameoverlayui (target pid $target is gone)"
    kill -9 "$uipid" 2>/dev/null
  fi
done < <(ps -eo pid,args | grep '[g]ameoverlayui -pid' | sed -E 's/^ *([0-9]+).*-pid ([0-9]+).*/\1 \2/')
if [ "$BLOCKED" = 1 ]; then
  warn "close it (and any game on another machine on this account) and re-run."
  exit 1
fi
say "  no live Steam game currently attached"

reset_overlay_state() {
  kill_by_name_and_arg electron "$APP"
  kill_by_name_and_arg OpenFront "$APP"
  ps -eo pid,comm,args | awk '$2=="glxgears"{print $1}' | while read -r p; do kill -9 "$p" 2>/dev/null; done
  sleep 2
  # A stale socket for a dead pid silently disables the hotkey for the live
  # process — for REAL keypresses as well as synthetic ones.
  rm -f /tmp/steam_chrome_overlay_uid"$(id -u)"_spid* 2>/dev/null
  rm -f /tmp/gameoverlay_ui.txt /tmp/steam-overlay-host-* 2>/dev/null
}

# ---------------------------------------------------------------------------
hdr "IDENTIFICATION"
say "  host            : $(hostname)"
say "  date            : $(date -Is)"
say "  kernel          : $(uname -r)"
say "  distro          : $( . /etc/os-release 2>/dev/null && echo "$PRETTY_NAME" )"
say "  session type    : ${XDG_SESSION_TYPE:-unset}"
say "  desktop         : ${XDG_CURRENT_DESKTOP:-unset}"
say "  DISPLAY         : ${DISPLAY:-unset}"
say "  WAYLAND_DISPLAY : ${WAYLAND_DISPLAY:-unset}"
say "  node            : $(node --version 2>/dev/null)"
say "  __GLX_VENDOR_LIBRARY_NAME : ${__GLX_VENDOR_LIBRARY_NAME:-unset}"

hdr "GPU / DRIVER"
if have lspci; then
  lspci -k 2>/dev/null | grep -A3 -iE 'vga|3d controller|display controller' \
    | grep -iE 'vga|3d|display|kernel driver in use|kernel modules' | sed 's/^/  /' | tee -a "$REPORT"
else
  say "  lspci unavailable"
fi
if have glxinfo; then
  glxinfo -B 2>/dev/null | grep -iE 'vendor|renderer|opengl core profile version|opengl version|direct rendering' \
    | sed 's/^/  /' | tee -a "$REPORT"
else
  say "  glxinfo unavailable"
fi
if have vulkaninfo; then
  vulkaninfo --summary 2>/dev/null | sed -n '/^Devices:/,$p' \
    | grep -E 'GPU[0-9]|deviceName|driverName|driverInfo' | sed 's/^/  /' | tee -a "$REPORT"
else
  say "  vulkaninfo unavailable"
fi

hdr "STEAM ROOT RESOLUTION"
# Mirrors findOverlayLibrary()'s search order, including the Flatpak path, which
# has never been exercised.
ARCHDIR="ubuntu12_64"
FOUND_LIB=""
for root in ".local/share/Steam" ".steam/steam" ".steam/root" ".steam/debian-installation" \
            ".var/app/com.valvesoftware.Steam/data/Steam"; do
  cand="$HOME/$root/$ARCHDIR/gameoverlayrenderer.so"
  if [ -r "$cand" ]; then
    say "  FOUND    $cand"
    [ -z "$FOUND_LIB" ] && FOUND_LIB="$cand"
  else
    say "  absent   $HOME/$root/$ARCHDIR/"
  fi
done
if [ -z "$FOUND_LIB" ]; then
  warn "findOverlayLibrary() would return nothing here — the overlay cannot attach."
  warn "If this is Flatpak Steam, that is the finding; record it and stop."
  exit 1
fi
say "  flatpak steam   : $( [ -d "$HOME/.var/app/com.valvesoftware.Steam" ] && echo yes || echo no )"

# ---------------------------------------------------------------------------
if [ "$DO_BUILD" = 1 ]; then
  hdr "BUILD"
  if [ -d "$FFI/.git" ]; then
    ( cd "$FFI" && git fetch --quiet origin && git checkout --quiet -B batt "origin/$BRANCH" \
      && git reset --hard --quiet "origin/$BRANCH" ) || { warn "git update failed"; exit 1; }
  else
    git clone --quiet "$REPO" "$FFI" || { warn "clone failed"; exit 1; }
    ( cd "$FFI" && git checkout --quiet -B batt "origin/$BRANCH" ) || { warn "branch checkout failed"; exit 1; }
  fi
  say "  branch : $BRANCH"
  say "  commit : $( cd "$FFI" && git log --oneline -1 )"
  ( cd "$FFI" && npm install --silent ) >"$WORK/npm.log" 2>&1 || { warn "npm install failed — see $WORK/npm.log"; exit 1; }
  ( cd "$FFI" && npm run build ) >"$WORK/build.log" 2>&1 || {
    warn "build failed — see $WORK/build.log"; tail -20 "$WORK/build.log" | sed 's/^/    /' | tee -a "$REPORT"; exit 1; }
  say "  build  : ok"
fi

# Electron: explicit override, then the fork's own, then anything else on the
# box, then install one. A fresh machine will have none.
if [ -n "${ELECTRON:-}" ] && [ -x "$ELECTRON" ]; then
  :
elif [ -x "$FFI/node_modules/electron/dist/electron" ]; then
  ELECTRON="$FFI/node_modules/electron/dist/electron"
else
  ELECTRON="$(find "$HOME" -maxdepth 6 -path '*/node_modules/electron/dist/electron' -type f 2>/dev/null | head -1)"
fi
if [ ! -x "${ELECTRON:-}" ]; then
  say "  electron: not found, installing into $FFI (one-off, ~100MB)"
  ( cd "$FFI" && npm i -D --silent electron@33 ) >>"$WORK/npm.log" 2>&1
  ELECTRON="$FFI/node_modules/electron/dist/electron"
fi
if [ ! -x "${ELECTRON:-}" ]; then
  warn "no Electron binary and install failed — see $WORK/npm.log"
  warn "set one explicitly:  ELECTRON=/path/to/electron $0 --no-build"
  exit 1
fi
say "  electron: $("$ELECTRON" --version 2>/dev/null | head -1)  ($ELECTRON)"

# ---------------------------------------------------------------------------
# Harness app. Logs synchronously so nothing is lost if the process is killed.
mkdir -p "$APP"
cat > "$APP/package.json" <<'PKG'
{ "name": "overlay-battery-harness", "version": "1.0.0", "main": "main.js" }
PKG

cat > "$APP/main.js" <<'MAIN'
const { app, BrowserWindow } = require("electron");
const fs = require("fs");
const path = require("path");
const LOG = path.join(__dirname, "harness.log");
const log = (...a) => {
  const s = a.map(x => (typeof x === "string" ? x : JSON.stringify(x))).join(" ");
  fs.appendFileSync(LOG, s + "\n");   // sync: survives SIGTERM/SIGKILL
};
const FFI = process.env.BATTERY_FFI;
const NO_OVERLAY = process.env.BATTERY_NO_OVERLAY === "1";

// Print rather than pop a modal. Electron's default handler shows an OS dialog
// that never reaches a log file, so a crash can happen in full view of a human
// and leave no trace in the captured output.
process.on("uncaughtException", (e) => {
  const m = "UNCAUGHT: " + (e && e.code) + " | " + (e && e.message) + "\n" + ((e && e.stack) || "");
  log(m); console.error(m);
});
process.on("unhandledRejection", (e) => {
  const m = "Unhandled rejection: " + ((e && e.message) || String(e));
  log(m); console.error(m);
});

app.whenReady().then(() => {
  const win = new BrowserWindow({ width: 1280, height: 720, show: true, title: "overlay-battery" });
  // A solid colour makes the Steam dim layer obvious to a human observer.
  win.loadURL("data:text/html,<body style='background:%23204080;margin:0'></body>");
  log("electron pid:", process.pid);

  if (NO_OVERLAY) { log("=== NO-OVERLAY CONTROL (no attach) ==="); return; }

  let SteamOverlay, SteamLogger;
  try { ({ SteamOverlay, SteamLogger } = require(path.join(FFI, "dist", "index.js"))); }
  catch (e) { log("FATAL: cannot load addon:", e.message); return; }

  // Belt and braces: enable BOTH debug switches. They are separate, and which
  // one reaches the host process has changed between branches. Setting both
  // means the battery captures host diagnostics whatever branch it runs on.
  try { SteamLogger.setDebug(true); } catch (e) { log("SteamLogger.setDebug unavailable:", e.message); }

  const overlay = new SteamOverlay();
  // Called BEFORE attach the flag is lost: the host process does not exist yet,
  // so the "debug" command has nowhere to go and the host runs with its
  // diagnostics silent. Call it again after attach so the host actually gets it.
  try { overlay.setDebugMode(true); } catch {}

  const t0 = process.hrtime.bigint();
  let ret;
  try { ret = overlay.addElectronSteamOverlay(win, { transparent: true }); }
  catch (e) { log("addElectronSteamOverlay THREW:", e.message); }
  const ms = Number(process.hrtime.bigint() - t0) / 1e6;
  try { overlay.setDebugMode(true); } catch {}

  log("=== HANDSHAKE ===");
  log("addElectronSteamOverlay ->", String(ret));
  log("BLOCKING_MS:", ms.toFixed(2));
  try { log("isTransparent ->", String(overlay.isTransparent())); }
  catch (e) { log("isTransparent unavailable:", e.message); }
}).catch((e) => {
  // A throw inside this callback is near-silent: it surfaces only as an
  // UnhandledPromiseRejectionWarning, leaving an app that started, attached
  // nothing, and printed no error — indistinguishable from several real
  // failures. Log it loudly instead.
  log("FATAL: whenReady callback threw:", (e && e.message) || String(e));
  log((e && e.stack) || "");
});
MAIN

# ---------------------------------------------------------------------------
# run_case <label> <extra-env...> -- <extra-electron-args...>
run_case() {
  local label="$1"; shift
  local -a envs=() eargs=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
  [ "${1:-}" = "--" ] && shift
  while [ $# -gt 0 ]; do eargs+=("$1"); shift; done

  reset_overlay_state
  rm -f "$APP/harness.log"
  local lddir="$WORK/ld-$label"; rm -rf "$lddir"; mkdir -p "$lddir"

  # Run in the background so the per-process table can be sampled WHILE the
  # overlay is live. Sampled after the process exits it is all zeros and reads
  # like a pass, which is exactly the vacuous evidence to avoid.
  env "${envs[@]}" BATTERY_FFI="$FFI" \
      LD_DEBUG=bindings LD_DEBUG_OUTPUT="$lddir/ld" \
      timeout 40 "$ELECTRON" "$APP" "${eargs[@]}" >"$WORK/out-$label.log" 2>&1 &
  local runner=$!
  sleep 22
  CASE_EPID=$(grep -oE 'electron pid: [0-9]+' "$APP/harness.log" 2>/dev/null | grep -oE '[0-9]+' | head -1)
  CASE_TABLE=""
  if [ -n "${CASE_EPID:-}" ] && [ -d "/proc/$CASE_EPID" ]; then
    CASE_TABLE=$(per_process_table "$CASE_EPID")
  fi
  CASE_OVWIN_DEPTH=""
  if [ -n "${CASE_EPID:-}" ] && have xwininfo && have xprop; then
    local hp; hp=$(ps -eo pid,ppid,args | awk -v p="$CASE_EPID" '$2==p' \
                   | grep 'steam-overlay-host-.*\.js' | awk '{print $1}' | head -1)
    [ -z "$hp" ] && hp="$CASE_EPID"
    local w; w=$(xwininfo -root -children 2>/dev/null | grep -oE '0x[0-9a-f]+' | while read -r x; do
      q=$(xprop -id "$x" _NET_WM_PID 2>/dev/null | grep -oE '[0-9]+$')
      [ "$q" = "$hp" ] && echo "$x"; done | head -1)
    [ -n "$w" ] && CASE_OVWIN_DEPTH=$(xwininfo -id "$w" 2>/dev/null | grep -E 'Depth|Width|Height' | tr '\n' ' ')
  fi
  wait "$runner"
  CASE_EXIT=$?
  CASE_LDDIR="$lddir"
}

per_process_table() {
  local epid="$1"
  # Walk the tree from the Electron main pid. Never match on argv: that is how
  # a cleanup step ends up killing the shell, or a grep matches itself.
  local kids; kids=$(
    walk() { for c in $(ps -eo pid,ppid | awk -v p="$1" '$2==p{print $1}'); do echo "$c"; walk "$c"; done; }
    walk "$epid"
  )
  printf '  %-9s %-16s %-16s %s\n' pid type overlay_in_maps has_LD_PRELOAD | tee -a "$REPORT"
  for pid in $epid $kids; do
    [ -r "/proc/$pid/maps" ] || continue
    local cmd typ ov ldp
    cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
    typ=$(printf '%s' "$cmd" | grep -oE '\-\-type=[a-z-]+' | head -1 | sed 's/--type=//')
    printf '%s' "$cmd" | grep -q 'steam-overlay-host-.*\.js' && typ="HOST"
    [ -z "$typ" ] && typ="browser(main)"
    ov=$(grep -c gameoverlayrenderer "/proc/$pid/maps" 2>/dev/null)
    ldp=$(tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | grep -c '^LD_PRELOAD=')
    printf '  %-9s %-16s %-16s %s\n' "$pid" "$typ" "$ov" "$ldp" | tee -a "$REPORT"
  done
}

# Applied to EVERY case, not just A. An uncaught exception in the induced-failure
# case is exactly where a dead-host EPIPE would surface, and that case does not
# go through report_case().
assert_case() {
  local label="$1" out="$WORK/out-$1.log"
  local loaded uncaught
  loaded=$(grep -c 'electron pid:' "$APP/harness.log" 2>/dev/null)
  if [ "${loaded:-0}" = 0 ]; then
    warn "app never reached whenReady — results for $label are VOID, not a pass"
    head -5 "$out" 2>/dev/null | sed 's/^/    /' | tee -a "$REPORT"
  else
    say "  app loaded      : yes"
  fi
  uncaught=$(grep -cE 'UNCAUGHT|uncaughtException|write EPIPE|Unhandled' "$out" 2>/dev/null)
  if [ "${uncaught:-0}" != 0 ]; then
    warn "UNCAUGHT EXCEPTION in $label ($uncaught line(s)) — a defect, not noise:"
    grep -E 'UNCAUGHT|write EPIPE|Unhandled' "$out" 2>/dev/null | head -3 | sed 's/^/    /' | tee -a "$REPORT"
    grep -A6 'UNCAUGHT' "$out" 2>/dev/null | head -8 | sed 's/^/    /' | tee -a "$REPORT"
  else
    say "  uncaught errors : 0"
  fi
}

report_case() {
  local label="$1"
  local out="$WORK/out-$label.log"
  local epid; epid=$(grep -oE 'electron pid: [0-9]+' "$APP/harness.log" 2>/dev/null | grep -oE '[0-9]+' | head -1)

  assert_case "$label"
  say "  exit code       : $CASE_EXIT $( [ "$CASE_EXIT" = 124 ] && echo '(survived to timeout)'; [ "$CASE_EXIT" = 139 ] && echo '(SIGSEGV)' )"
  say "  core dumped     : $(grep -c 'dumped core' "$out" 2>/dev/null)"
  say "  GPU fatal       : $(grep -c "GPU process isn't usable" "$out" 2>/dev/null)"
  say "  handshake ms    : $(grep -oE 'BLOCKING_MS: [0-9.]+' "$APP/harness.log" 2>/dev/null | awk '{print $2}' | head -1)"
  say "  attach returned : $(grep -oE 'addElectronSteamOverlay -> \w+' "$APP/harness.log" 2>/dev/null | awk '{print $3}' | head -1)"
  say "  isTransparent   : $(grep -oE 'isTransparent -> \w+' "$APP/harness.log" 2>/dev/null | awk '{print $3}' | head -1)"
  say "  stamp           : $(cat /tmp/steam-overlay-host-*.json 2>/dev/null | head -c 200)"
  say "  visual (log)    : $(grep -oE 'Chose visual depth [0-9]+ \(ARGB: \w+\)' "$out" 2>/dev/null | head -1)"
  say "  visual (window) : ${CASE_OVWIN_DEPTH:-not sampled}"
  say "  hook line       : $(grep -oE 'Steam overlay hook ACTIVE.*|gameoverlayrenderer64\.so NOT in.*' "$out" 2>/dev/null | head -1)"
  say "  gameoverlayui   : $(ps -eo args | grep '[g]ameoverlayui -pid' | grep -oE '\-pid [0-9]+ .*-gameid [0-9]+' | head -1)"
  say "  enable_requested: $(grep -c 'Overlay enable requested' /tmp/gameoverlay_ui.txt 2>/dev/null)"

  say ""
  say "  glXSwapBuffers bindings (VERBATIM — the mechanism check):"
  local n=0
  for f in "$CASE_LDDIR"/ld.*; do
    [ -f "$f" ] || continue
    while IFS= read -r line; do say "    $line"; n=$((n+1)); done < <(grep -h 'glXSwapBuffers' "$f" 2>/dev/null | sort -u)
  done
  [ "$n" = 0 ] && say "    (none — the addon never bound glXSwapBuffers in any process)"
  local into out_of
  into=$(grep -h 'steam-overlay.node' "$CASE_LDDIR"/ld.* 2>/dev/null | grep -c 'gameoverlayrenderer.*glXSwapBuffers' || true)
  out_of=$(grep -h 'gameoverlayrenderer' "$CASE_LDDIR"/ld.* 2>/dev/null | grep -cE 'to /.*libGL[^ ]*\.so[^ ]*.*glXSwapBuffers' || true)
  say ""
  say "  MECHANISM VERDICT:"
  say "    addon -> gameoverlayrenderer.so : ${into:-0}  (want >=1; the call enters Steam's interposer)"
  say "    gameoverlayrenderer.so -> libGL : ${out_of:-0}  (want >=1; the interposer's RTLD_NEXT resolves)"
  # Name the object the interposer actually forwards INTO. On Mesa this is
  # libGL.so.1; under GLVND with the proprietary driver the vendor library is
  # libGLX_nvidia, and whether Steam's RTLD_NEXT still lands on libGL.so.1 or
  # somewhere else is the single most important thing this battery can report
  # off-Mesa. A count cannot answer that -- the target has to be named.
  # Lines where gameoverlayrenderer.so is the SOURCE, minus the self-binding.
  # The interposer legitimately binds glXSwapBuffers to its OWN definition as
  # well -- ordinary resolution for a library referencing its own export -- and
  # that is not what it forwards into, so it is excluded rather than reported.
  local target
  target=$(grep -h 'glXSwapBuffers' "$CASE_LDDIR"/ld.* 2>/dev/null \
           | grep -oE 'binding file [^ ]*gameoverlayrenderer[^ ]*\.so \[[0-9]+\] to [^ ]+' \
           | sed -E 's/.* to //' | grep -v 'gameoverlayrenderer' | sort -u | tr '\n' ' ')
  say "    interposer forwards INTO : ${target:-<nothing — RTLD_NEXT resolved to no object>}"
  say "    (on Mesa expect libGL.so.1; under GLVND/NVIDIA this is the value to report)"
  say "    NOTE the counts above are heuristics — read the verbatim lines, not them."

  say ""
  if [ -n "${CASE_TABLE:-}" ]; then
    say "  per-process table (sampled while live — only evidence when HOST is non-zero):"
    printf '%s\n' "$CASE_TABLE" | tee -a "$REPORT"
  else
    say "  per-process table: could not sample (process exited early, or no pid logged)"
  fi
}

# ---------------------------------------------------------------------------
hdr "CASE A — default session (the working path)"
run_case A SteamGameId=480 SteamAppId=480 --
report_case A

# Case A leaves the app dead (timeout). For the human dim check we need it live.
hdr "CASE A2 — live instance for the HUMAN check"
reset_overlay_state
rm -f "$APP/harness.log"
env SteamGameId=480 SteamAppId=480 BATTERY_FFI="$FFI" setsid "$ELECTRON" "$APP" \
    >"$WORK/out-A2.log" 2>&1 &
sleep 20
A2_EPID=$(grep -oE 'electron pid: [0-9]+' "$APP/harness.log" 2>/dev/null | grep -oE '[0-9]+' | head -1)
A2_HOST=$(ps -eo pid,ppid,args | awk -v p="${A2_EPID:-0}" '$2==p' | grep 'steam-overlay-host-.*\.js' | awk '{print $1}' | head -1)
say "  electron pid : ${A2_EPID:-none}   host pid : ${A2_HOST:-none}"
say "  gameoverlayui: $(ps -eo args | grep '[g]ameoverlayui -pid' | grep -oE '\-pid [0-9]+ .*-gameid [0-9]+' | head -1)"

say ""
say "  ------------------------------------------------------------------"
say "  HUMAN CHECK — this cannot be automated."
say ""
say "  A capture of a 32-bit ARGB window records transparent pixels as BLACK,"
say "  so a screenshot cannot tell 'Steam dimmed the surface' from 'nothing"
say "  was drawn'. Relying on one produced a confident FALSE PASS before."
say ""
say "  1. Click the blue 'overlay-battery' window to focus it."
say "  2. Press Shift+Tab ONCE on a real keyboard (not a remote/synthetic tool)."
say "  ------------------------------------------------------------------"
say ""
# Ask on the terminal if there is one, else fall back to stdin, else record
# "unanswered" — never abort, and never let a missing answer read as a "no".
ask() {
  local prompt="$1" __var="$2" reply=""
  printf '  %s ' "$prompt"
  if { : </dev/tty; } 2>/dev/null; then
    read -r reply </dev/tty 2>/dev/null || reply=""
  else
    read -r reply || reply=""
  fi
  [ -z "$reply" ] && { reply="UNANSWERED"; printf '\n'; }
  printf -v "$__var" '%s' "$reply"
}
ask 'Did the FULL overlay appear (Game Overview + toolbar + Back to Game)? [y/n/partial]' ANS_FULL
ask 'Did the blue page DIM behind the overlay? [y/n]' ANS_DIM
ask 'Did it work on the FIRST press? [y/n]' ANS_FIRST
ask 'Did the overlay float above UNRELATED windows (z-index defect, OPE-322)? [y/n]' ANS_ZIDX
say ""
say "  HUMAN: full_overlay=$ANS_FULL  dim=$ANS_DIM  first_press=$ANS_FIRST  floats_above_others=$ANS_ZIDX"
say "  enable_requested after human press: $(grep -c 'Overlay enable requested' /tmp/gameoverlay_ui.txt 2>/dev/null)"
[ -n "${A2_EPID:-}" ] && kill -TERM "$A2_EPID" 2>/dev/null
sleep 3
say "  after SIGTERM — host still alive: $( [ -n "${A2_HOST:-}" ] && kill -0 "$A2_HOST" 2>/dev/null && echo YES || echo no )"
if have xwininfo; then
  say "  stranded overlay window: $(xwininfo -root -children 2>/dev/null | grep -ci 'Electron Steam App' || true)"
else
  say "  stranded overlay window: unknown (xwininfo unavailable)"
fi

# ---------------------------------------------------------------------------
hdr "CASE B — induced host failure (fast-fail timing)"
HOSTMAIN="$FFI/dist/internal/linuxOverlayHostMain.js"
if [ -f "$HOSTMAIN" ]; then
  # Refuse to sabotage an already-sabotaged file. If a previous run was
  # interrupted here (this box hard-powers-off), the entry is still broken and
  # backing it up would make the corruption permanent -- the backup becomes a
  # copy of the broken file and "restoring" it restores the sabotage.
  if head -1 "$HOSTMAIN" | grep -q 'process.exit(3)'; then
    warn "host entry is already sabotaged from an interrupted run — rebuilding before Case B"
    ( cd "$FFI" && npm run build ) >"$WORK/rebuild.log" 2>&1 || { warn "rebuild failed — see $WORK/rebuild.log"; exit 1; }
  fi
  cp "$HOSTMAIN" "$WORK/hostmain.bak"
  # Restore on ANY exit, including interrupt or kill, so a broken build is never
  # left behind for the next run to inherit.
  trap 'cp -f "$WORK/hostmain.bak" "$HOSTMAIN" 2>/dev/null' EXIT INT TERM
  printf 'process.exit(3);\n%s' "$(cat "$WORK/hostmain.bak")" > "$HOSTMAIN"
  run_case B SteamGameId=480 SteamAppId=480 --
  assert_case B
  say "  exit code       : $CASE_EXIT"
  say "  handshake ms    : $(grep -oE 'BLOCKING_MS: [0-9.]+' "$APP/harness.log" 2>/dev/null | awk '{print $2}' | head -1)"
  say "  (compare against CASE A: a working fast-fail is far below the deadline)"
  cp -f "$WORK/hostmain.bak" "$HOSTMAIN"
  trap - EXIT INT TERM
  if head -1 "$HOSTMAIN" | grep -q 'process.exit(3)'; then
    warn "host entry STILL sabotaged after restore — rebuild before trusting any later run"
  else
    say "  host entry restored (verified clean)"
  fi
else
  say "  skipped — no host entry on this branch"
fi

# ---------------------------------------------------------------------------
hdr "CASE C — no-overlay control (for the SIGTERM core dump, OPE-324)"
run_case C BATTERY_NO_OVERLAY=1 --
assert_case C
say "  exit code   : $CASE_EXIT"
say "  core dumped : $(grep -c 'dumped core' "$WORK/out-C.log" 2>/dev/null)"
say "  GPU fatal   : $(grep -c "GPU process isn't usable" "$WORK/out-C.log" 2>/dev/null)"
say "  (CASE A with a core dump and CASE C without it = the overlay triggers it)"

# ---------------------------------------------------------------------------
if [ "${XDG_SESSION_TYPE:-}" = "wayland" ]; then
  hdr "CASE D — native Wayland (must NOT crash; overlay simply absent)"
  run_case D SteamGameId=480 SteamAppId=480 -- --ozone-platform=wayland
  # The app dying here is NOT always a signal: Xlib's default error handler
  # calls exit(1), so an unfixed build exits 1 rather than segfaulting. Anything
  # other than 124 (ran to the timeout) means it did not survive.
  assert_case D
  if [ "$CASE_EXIT" = 124 ]; then
    say "  exit code   : 124 (SURVIVED — correct: overlay absent, app alive)"
  else
    say "  exit code   : $CASE_EXIT  <-- APP DIED (expected 124)"
  fi
  say "  core dumped : $(grep -c 'dumped core' "$WORK/out-D.log" 2>/dev/null)"
  say "  X error HANDLED   : $(grep -c 'X error suppressed' "$WORK/out-D.log" 2>/dev/null)  (want >=1 on a fixed build)"
  say "  X error UNHANDLED : $(grep -c 'X Error of failed request' "$WORK/out-D.log" 2>/dev/null)  (want 0; >=1 means Xlib killed the app)"
  say "  guard message     : $(grep -oE 'not a valid X window[^\"]*' "$WORK/out-D.log" 2>/dev/null | head -1)"
  say "  attach ret  : $(grep -oE 'addElectronSteamOverlay -> \w+' "$APP/harness.log" 2>/dev/null | awk '{print $3}' | head -1)"
else
  hdr "CASE D — native Wayland"
  say "  skipped: not a Wayland session (XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-unset})"
fi

# ---------------------------------------------------------------------------
reset_overlay_state
hdr "DONE"
say "  report: $REPORT"
say ""
say "  Send back this whole file. Raw logs are in $WORK/ if anything needs chasing."
