#!/bin/bash
# No compiler needed. Needs: ffmpeg, pactl, and optionally zenity.
set -euo pipefail

PREFERRED="Sony_Interactive_Entertainment_Wireless_Headset"

die() { echo "$*" >&2; exit 1; }

command -v ffmpeg >/dev/null || die "ffmpeg not found"
command -v pactl >/dev/null || die "pactl not found"

mapfile -t LINES < <(pactl list short sources)
(( ${#LINES[@]} > 0 )) || die "No Pulse/PipeWire sources found"

SOURCES=()
LABELS=()
DEFAULT_IDX=0
FIRST_MON=-1
for i in "${!LINES[@]}"; do
  # index name driver format state
  read -r _ name _ _ state <<< "${LINES[$i]}"
  SOURCES+=("$name")
  LABELS+=("$name  [$state]")
  if [[ "$name" == *"$PREFERRED"* && "$name" == *.monitor ]]; then
    DEFAULT_IDX=$i
  elif [[ $FIRST_MON -lt 0 && "$name" == *.monitor && "$state" == RUNNING ]]; then
    FIRST_MON=$i
  fi
done
if [[ "${SOURCES[$DEFAULT_IDX]}" != *"$PREFERRED"* && $FIRST_MON -ge 0 ]]; then
  DEFAULT_IDX=$FIRST_MON
fi

pick_source() {
  if command -v zenity >/dev/null 2>&1; then
    local args=()
    for i in "${!LABELS[@]}"; do
      if [[ $i -eq $DEFAULT_IDX ]]; then
        args+=(TRUE "$i" "${LABELS[$i]}")
      else
        args+=(FALSE "$i" "${LABELS[$i]}")
      fi
    done
    local idx
    idx=$(zenity --list --title="Headset Recorder" \
      --text="Select the monitor of the device you are hearing" \
      --radiolist --hide-column=1 --print-column=1 \
      --column=idx --column=Source \
      --height=360 --width=720 \
      "${args[@]}") || return 1
    echo "${SOURCES[$idx]}"
  else
    echo "Sources:" >&2
    for i in "${!LABELS[@]}"; do
      mark=" "
      [[ $i -eq $DEFAULT_IDX ]] && mark="*"
      printf "  %s %2d  %s\n" "$mark" "$i" "${LABELS[$i]}" >&2
    done
    local idx=$DEFAULT_IDX
    read -r -p "Source number [$DEFAULT_IDX]: " idx || return 1
    idx=${idx:-$DEFAULT_IDX}
    echo "${SOURCES[$idx]}"
  fi
}

SRC=$(pick_source) || exit 1
[[ -n "$SRC" ]] || die "No source selected"

STAMP=$(date +%Y%m%d-%H%M%S)
SUGGEST="$HOME/recording-$STAMP.wav"

pick_file() {
  if command -v zenity >/dev/null 2>&1; then
    zenity --file-selection --save --confirm-overwrite \
      --title="Save WAV" --filename="$SUGGEST" || return 1
  else
    read -r -p "Save as [$SUGGEST]: " f || return 1
    echo "${f:-$SUGGEST}"
  fi
}

OUT=$(pick_file) || exit 1
[[ "$OUT" == *.wav || "$OUT" == *.WAV ]] || OUT="$OUT.wav"

info() {
  if command -v zenity >/dev/null 2>&1; then
    zenity --info --title="Headset Recorder" --text="$1" --width=420 || true
  else
    echo "$1"
  fi
}

# Record until the user presses Stop in the zenity window, or Ctrl+C in the terminal.
if command -v zenity >/dev/null 2>&1; then
  ffmpeg -hide_banner -loglevel info -nostdin -y \
    -f pulse -i "$SRC" \
    -ac 2 -ar 48000 -c:a pcm_f32le \
    "$OUT" &
  FFPID=$!
  zenity --info --title="Recording" \
    --text="Recording from:\n$SRC\n\nClick OK to stop and save." \
    --ok-label="Stop" --width=480 || true
  kill -INT "$FFPID" 2>/dev/null || true
  wait "$FFPID" 2>/dev/null || true
  info "Saved:\n$OUT"
else
  echo "Recording $SRC"
  echo "Press q then Enter to stop."
  ffmpeg -hide_banner -loglevel info -y \
    -f pulse -i "$SRC" \
    -ac 2 -ar 48000 -c:a pcm_f32le \
    "$OUT"
  echo "Saved $OUT"
fi
