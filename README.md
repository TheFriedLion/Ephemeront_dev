# Ephemeront

Instant replay for your ears. Ephemeront sits in the Windows system tray, keeps a rolling recording of your computer's audio, and saves the last few seconds the moment you press a hotkey. Every app is recorded as its own track, so you can keep the line your friend said in Discord without the game noise underneath.

## Features

- **Quick Save** (`Ctrl+Alt+S`): saves the last 30 seconds instantly. No window, no interruption.
- **Clip Picker** (`Ctrl+Alt+C`): a small window showing a waveform for each track. Drag to choose the exact moment, untick the tracks you don't want, preview with Space, save with Enter.
- **One track per app**: Discord, your browser, Spotify, a game, and so on are each saved to their own WAV file. A full system mix track and your microphone are also available.
- **Live view**: the Clip Picker's waveforms update in real time while it's open.
- **Lightweight**: a single ~310 KB executable with no installer or runtime. Silent tracks use no buffer memory.
- Hotkeys, lengths, save folder, and tracks are all configurable from the tray icon's **Settings**.

Clips are saved as 32-bit float stereo WAV files at your output device's sample rate (exactly what Windows plays, with no conversion; Settings can switch to 48 kHz for film and video work) to `Music\Ephemeront` by default, named like `2026-10-10 21-45-03 Discord.wav`.

## Requirements

Windows 10 version 2004 or later, or Windows 11. Per-app capture relies on the Windows process loopback API introduced in that release.

## Usage

Run `Ephemeront.exe`. A gold level-meter icon appears in the tray (it may be inside the `^` hidden icons fold-out; drag it onto the taskbar to keep it visible).

| Action | How |
| --- | --- |
| Quick Save | Hotkey, or tray menu |
| Clip Picker | Hotkey, left-click the tray icon, or tray menu |
| Settings | Right-click the tray icon |
| Open clips folder | Tray menu, or click the "Clip saved" notification |

Inside the Clip Picker:

| Key | Action |
| --- | --- |
| Drag on the timeline | Select a range (drag the gold edges to adjust) |
| Click a track name | Include or exclude that track |
| `1` to `5` | Last 5 s, 10 s, 30 s, 60 s, or everything |
| `A` | Toggle all tracks |
| Space | Preview the selection |
| Enter / Esc | Save / Cancel |

### Command line

Running `Ephemeront.exe` again while it's running forwards a command to the running copy. This is handy for macro pads and stream decks.

```
Ephemeront.exe --save       Quick Save
Ephemeront.exe --pick       Open the Clip Picker
Ephemeront.exe --settings   Open Settings
Ephemeront.exe --exit       Quit
```

## Limits

- Voice chat apps mix every speaker into one stream, so Discord is one track, not one per person.
- All tabs of a browser share one track.
- The System track is the full mix, so it also contains every app that has its own track.

## Building

Requires the Visual Studio 2019 (or newer) C++ build tools and a Windows 10 SDK.

```
build.bat
```

The result is `build\Ephemeront.exe`. `tools\glitch_check.py` makes a test tone and checks saved clips for dropouts and clicks. Settings live in `%APPDATA%\Ephemeront\settings.ini`, alongside a `log.txt` that's useful when troubleshooting capture.
