EPHEMERONT - Devbuild 1.01
Instant replay for your ears.
==========================================================================

Ephemeront sits quietly in your system tray and keeps a rolling recording
of the last couple of minutes of your computer's audio. When something
good happens (a funny line in voice chat, a sound worth sampling), press a
hotkey and it's saved. Every app is recorded as its own track, so you can
keep your friend's voice from Discord without the game noise underneath.


GETTING STARTED
--------------------------------------------------------------------------
1. Put Ephemeront.exe anywhere you like (Desktop, Documents, a tools
   folder). There's nothing to install.
2. Double-click it.
   - Windows may show a blue "Windows protected your PC" screen. That's
     normal for small independent programs. Click "More info", then
     "Run anyway". It only asks once.
   - Some antivirus programs are cautious about new programs that record
     audio. If yours complains, allow Ephemeront.
3. A small gold icon appears in your system tray. It may be hidden under
   the ^ arrow next to the clock; you can drag it onto the taskbar to
   keep it visible.

That's it. Ephemeront is now listening.


SAVING CLIPS
--------------------------------------------------------------------------
Quick Save  (Ctrl+Alt+S)
    Instantly saves the last 30 seconds. No window, no interruption.

Clip Picker  (Ctrl+Alt+C, or left-click the tray icon)
    Opens a window showing a live waveform for each track, so you can
    choose exactly what to keep:

    Drag on the timeline ......... select a stretch of audio
    Drag the gold edges .......... fine-tune the selection
    Click a track's name ......... include or leave out that track
    5s / 10s / 30s / 60s / All ... quick selections (or keys 1 to 5)
    A ............................ tick or untick all tracks
    Space ........................ preview the selection
    Enter ........................ save
    Esc .......................... cancel

Clips are saved to your Music\Ephemeront folder, one WAV file per track,
named by date, time and app, for example:
    2026-11-11 21-45-03 Discord.wav
Click the "Clip saved" notification to open the folder.


TRACKS
--------------------------------------------------------------------------
- Each app       Every program that plays sound gets its own track.
- System         The full mix of everything you hear.
- Microphone     Your mic. Off by default; turn it on from the tray menu.

Tracks that were silent are skipped, so you won't get piles of empty
files.


SETTINGS  (right-click the tray icon > Settings)
--------------------------------------------------------------------------
- Hotkeys: click a hotkey box and press the combination you want.
- Quick Save length and buffer length (the longest clip you can save).
- Sample rate: matches your speakers or headphones by default. Choose
  48000 Hz if your clips are going into a video editor.
- Microphone input: if your mic only shows up on the left side (common
  with studio mics on an audio interface), choose "Mono, input 1". If
  it's plugged into your interface's second input, choose "Mono, input 2".
- Save folder, which tracks to record, notifications, and start with
  Windows.


GOOD TO KNOW
--------------------------------------------------------------------------
- Recordings are full quality: 32-bit WAV, exactly what Windows plays.
  Any audio editor or DAW can open them.
- Voice chat apps mix everyone into one stream, so Discord is one track,
  not one per person. All browser tabs also share one track.
- The System track contains everything, including apps that also have
  their own track.
- Ephemeront only keeps audio in memory. Nothing is written to disk
  until you save a clip.
- Memory use grows with buffer length: about 23 MB per minute of buffer
  for each track that's making sound.


REQUIREMENTS
--------------------------------------------------------------------------
Windows 10 (version 2004 or newer) or Windows 11, 64-bit.


REMOVING EPHEMERONT
--------------------------------------------------------------------------
1. If you turned on "Start Ephemeront with Windows", turn it off first
   in Settings.
2. Right-click the tray icon and choose Exit.
3. Delete Ephemeront.exe.
4. Optional: delete its settings folder. Press Win+R, type
   %APPDATA%\Ephemeront and press Enter, then delete that folder.
Your saved clips in Music\Ephemeront are yours to keep.


--------------------------------------------------------------------------
Ephemeront by FriedLion
