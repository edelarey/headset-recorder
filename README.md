# Headset Recorder

Save anything you're listening to.

Linux GTK 3 app that records a PipeWire/Pulse **monitor** source (what the headset is playing) to a 48 kHz stereo 32-bit float WAV via ffmpeg. It captures the incoming playback stream, not the microphone — pick a `.monitor` source.

## Dependencies

```bash
sudo apt install build-essential pkg-config libgtk-3-dev ffmpeg
```

`pactl` (PipeWire/Pulse) must be available at runtime.

## Build

```bash
./build.sh
# or
make
./headset-recorder
```

## Install (GNOME / Ubuntu dock)

```bash
./install.sh
```

Installs a launcher under `~/.local` and pins it to the dock. Folder choice is remembered in `~/.config/headset-recorder/config.ini`.

```bash
./install.sh --uninstall
```

## Usage

1. Select the headset **`.monitor`** source (incoming playback stream).
2. Set the save folder.
3. Name the take.
4. Record / Stop.

There is also a no-compiler fallback: `./headset-recorder.sh` (needs ffmpeg, pactl, optional zenity).
