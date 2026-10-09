#!/bin/bash
# Build and install Headset Recorder for this user, then pin it to the GNOME dock.
set -euo pipefail
cd "$(dirname "$0")"

PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"
APPDIR="$PREFIX/share/applications"
ICONDIR="$PREFIX/share/icons/hicolor/scalable/apps"
DESKTOP_ID="headset-recorder.desktop"
PIN=1
UNINSTALL=0

usage() {
  cat <<EOF
Usage: $0 [--prefix DIR] [--no-pin] [--uninstall]
  --prefix DIR   Install prefix (default: \$HOME/.local)
  --no-pin       Do not add the app to the GNOME dock
  --uninstall    Remove a previous user install
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="$2"; BINDIR="$PREFIX/bin"; APPDIR="$PREFIX/share/applications"
              ICONDIR="$PREFIX/share/icons/hicolor/scalable/apps"; shift 2 ;;
    --no-pin) PIN=0; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

unpin() {
  command -v gsettings >/dev/null 2>&1 || return 0
  python3 - "$DESKTOP_ID" <<'PY'
import ast, subprocess, sys
desktop_id = sys.argv[1]
raw = subprocess.check_output(["gsettings", "get", "org.gnome.shell", "favorite-apps"], text=True)
favs = ast.literal_eval(raw)
if desktop_id not in favs:
    raise SystemExit(0)
favs = [x for x in favs if x != desktop_id]
out = "[" + ", ".join("'" + x + "'" for x in favs) + "]"
subprocess.check_call(["gsettings", "set", "org.gnome.shell", "favorite-apps", out])
PY
}

if [[ $UNINSTALL -eq 1 ]]; then
  unpin
  rm -f "$BINDIR/headset-recorder" \
        "$BINDIR/headset-recorder.bin" \
        "$APPDIR/$DESKTOP_ID" \
        "$ICONDIR/headset-recorder.svg"
  update-desktop-database "$APPDIR" 2>/dev/null || true
  gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
  echo "Removed Headset Recorder from $PREFIX"
  exit 0
fi

if ! command -v gcc >/dev/null 2>&1 || ! pkg-config --exists gtk+-3.0; then
  echo "Need gcc and GTK 3 headers. Install with:"
  echo "  sudo apt install build-essential pkg-config libgtk-3-dev"
  exit 1
fi
command -v ffmpeg >/dev/null 2>&1 || echo "Warning: ffmpeg is not on PATH; recording will not work until it is installed."

make -s

install -d "$BINDIR" "$APPDIR" "$ICONDIR"
install -m 755 headset-recorder "$BINDIR/headset-recorder.bin"
install -m 644 headset-recorder.svg "$ICONDIR/headset-recorder.svg"

# Wrapper starts the GTK binary in a clean session env. VS Code (snap)
# injects GTK/pixbuf module paths that load snap libc and crash at startup.
cat > "$BINDIR/headset-recorder" <<EOF
#!/bin/bash
exec env -i \
  HOME="\$HOME" \
  USER="\${USER:-}" \
  LANG="\${LANG:-C.UTF-8}" \
  DISPLAY="\${DISPLAY:-}" \
  WAYLAND_DISPLAY="\${WAYLAND_DISPLAY:-}" \
  XAUTHORITY="\${XAUTHORITY:-}" \
  XDG_RUNTIME_DIR="\${XDG_RUNTIME_DIR:-}" \
  XDG_CURRENT_DESKTOP="\${XDG_CURRENT_DESKTOP:-}" \
  DBUS_SESSION_BUS_ADDRESS="\${DBUS_SESSION_BUS_ADDRESS:-}" \
  XDG_ACTIVATION_TOKEN="\${XDG_ACTIVATION_TOKEN:-}" \
  DESKTOP_STARTUP_ID="\${DESKTOP_STARTUP_ID:-}" \
  GIO_LAUNCHED_DESKTOP_FILE="\${GIO_LAUNCHED_DESKTOP_FILE:-}" \
  GIO_LAUNCHED_DESKTOP_FILE_PID="\${GIO_LAUNCHED_DESKTOP_FILE_PID:-}" \
  PATH="/usr/local/bin:/usr/bin:/bin:\$HOME/.local/bin" \
  "$BINDIR/headset-recorder.bin" "\$@"
EOF
chmod 755 "$BINDIR/headset-recorder"

cat > "$APPDIR/$DESKTOP_ID" <<EOF
[Desktop Entry]
Type=Application
Name=Headset Recorder
Comment=Record a PipeWire/Pulse monitor source to 32-bit float WAV
Exec=$BINDIR/headset-recorder
TryExec=$BINDIR/headset-recorder
Icon=$ICONDIR/headset-recorder.svg
Terminal=false
Categories=AudioVideo;Audio;Recorder;
Keywords=record;audio;headset;wav;pulse;pipewire;ffmpeg;
StartupNotify=true
StartupWMClass=za.delarey.headsetrecorder
EOF
chmod 644 "$APPDIR/$DESKTOP_ID"

if command -v desktop-file-validate >/dev/null 2>&1; then
  desktop-file-validate "$APPDIR/$DESKTOP_ID"
fi

update-desktop-database "$APPDIR" 2>/dev/null || true
gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true

if [[ $PIN -eq 1 ]] && command -v gsettings >/dev/null 2>&1; then
  python3 - "$DESKTOP_ID" <<'PY'
import ast, subprocess, sys
desktop_id = sys.argv[1]
raw = subprocess.check_output(["gsettings", "get", "org.gnome.shell", "favorite-apps"], text=True)
favs = ast.literal_eval(raw)
if desktop_id not in favs:
    favs.append(desktop_id)
    out = "[" + ", ".join("'" + x + "'" for x in favs) + "]"
    subprocess.check_call(["gsettings", "set", "org.gnome.shell", "favorite-apps", out])
print("pinned")
PY
fi

echo "Installed $BINDIR/headset-recorder"
echo "Launcher: $APPDIR/$DESKTOP_ID"
if [[ $PIN -eq 1 ]]; then
  echo "Pinned to the Ubuntu dock. Click the headset icon to launch."
else
  echo "Search 'Headset Recorder' in Activities, then right-click → Pin to Dash."
fi
