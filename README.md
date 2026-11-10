# FastBar

A responsive, lightweight auto-hide taskbar for Windows 11.

Windows' built-in auto-hide waits before the taskbar appears and animates it at
about 30 fps, so the slide often stutters. FastBar replaces that behavior:

- **No delay**: the taskbar starts appearing about 10 ms after the cursor touches the
  bottom edge of the screen. The built-in auto-hide takes about 200–400 ms.
- **Animation at your refresh rate**: every frame is synced to the desktop compositor
  (`DwmFlush`), so on a 144 Hz monitor the slide runs at 144 fps.
- **Live content**: the slide shows the taskbar's real, current content, rendered by
  the GPU the same way Alt+Tab previews are.
- **Lightweight**: native C/Win32, ~75 KB executable (mostly the icon), ~2 MB of RAM, ~0% CPU when idle.
  It is event-driven (see below for the one exception).

## Usage

1. Keep `FastBar.exe`, `FastBarGuard.exe` and `FastBarTap.dll` in the same folder and run
   `FastBar.exe`. (`FastBarTap.dll` is only needed for **Transparent taskbar**.)
   Its icon appears in the system tray.
   On launch FastBar adds itself to the Start menu (`FastBar.lnk` in your user's
   Start menu *Programs* folder), so next time you can just type "FastBar" in
   Windows Search. If you move the folder, run the exe once from its new place and
   the shortcut is updated.
2. Move the cursor to the bottom edge of the screen and the taskbar slides up.
   Move away and it slides down.
3. Click the tray icon for the menu:
   - **Enabled**: turn FastBar on or off. When off, the taskbar behaves normally.
   - **Transparent taskbar**: see-through taskbar background, like TranslucentTB's
     "clear" mode. The icons stay solid. Works whether FastBar is enabled or not, and
     the original background comes back when you turn it off or exit FastBar.
   - **Start with Windows**
   - **Edit settings...** / **Reload settings**
   - **Exit**: restores the taskbar and your original auto-hide setting.
4. While FastBar runs, Windows' "Automatically hide the taskbar" setting is on
   (FastBar needs it). If you turn it **off** in Settings > Personalization >
   Taskbar, FastBar turns itself off and the taskbar stays visible as normal.
   Turning it back **on** turns FastBar on again. FastBar remembers this choice
   and restores it on Exit. Note: a Settings page that is already open does not
   update when FastBar changes the setting. Reopen the page to see the current
   value.

## Settings

Settings live in `FastBar.ini`, next to the executable. FastBar creates the file on
first run. After editing it, choose **Reload settings** from the tray menu.

| Key | Default | Meaning |
|---|---|---|
| `ShowDelay` | 0 | ms to wait after the cursor touches the edge (0 = instant). Counted from the first touch: moving the mouse around over the bottom of the screen (where the bar would be) does not restart it; leaving that area does |
| `HideDelay` | 300 | ms to wait after the cursor leaves the taskbar before hiding |
| `ShowAnimationMs` | 250 | slide-up duration in ms (0 = no animation) |
| `HideAnimationMs` | 180 | slide-down duration in ms (0 = no animation) |
| `Easing` | 1 | animation curve: 0 = linear, 1 = smooth, 2 = snappy (fast start), 3 = ease-in-out |
| `TriggerPixels` | 1 | how many pixels from the screen edge trigger the taskbar |
| `BlockInFullscreen` | 1 | 1 = don't pop up over full-screen games or videos |
| `ShowWithStartMenu` | 1 | 1 = show the taskbar while Start or Search is open |
| `TransparentTaskbar` | 0 | 1 = see-through taskbar background (same as the tray menu item) |
| `TaskbarColor` | 00000000 | background color while transparent, as `AARRGGBB` hex: `00000000` = fully clear, `66000000` = dark tint, `40FFFFFF` = light tint |

## How it works

- Windows' auto-hide setting is turned on, so maximized windows use the whole screen.
  The real taskbar window is then hidden, so Explorer's own slow auto-hide logic never
  runs.
- **Showing.** The real taskbar is shown immediately, standing still at its final
  position. Explorer processes hover on every frame when the bar moves under the
  cursor, which stalls the animation, so the bar itself never moves. On top of it,
  FastBar plays the slide in its own click-through window, using a live DWM thumbnail
  of the real taskbar. When the slide ends, that window goes away. Clicks during the
  slide already reach the real taskbar.
- In auto-hide mode, Explorer hides the taskbar's content by hiding a child window
  (`Windows.UI.Composition.DesktopWindowContentBridge`). Explorer would only show it
  again 40–90 ms after the cursor arrives, so FastBar shows it right away and the
  content is live from the first frame.
- Explorer also keeps its own "hidden / shown" state. FastBar sends the taskbar a
  `WM_ACTIVATE` message, which switches that state to "shown" without moving focus
  away from your app. Without it, the first click on the taskbar would make Explorer
  restart its own unhide animation.
- While Start or Quick Settings is open, Windows moves the taskbar to a higher z-band,
  above every normal window, and sometimes leaves it there afterwards. No window of
  FastBar's can cover it then. So during the slide, the real taskbar is made
  transparent (alpha 1/255, so it still takes clicks and keeps rendering for the live
  thumbnail), and it becomes opaque again when the slide ends.
- **Hiding.** A snapshot of the taskbar slides down in FastBar's own window. Explorer's
  ~30 fps hide animation is never used.
- **Programs running as administrator.** While a window of an elevated program (Task
  Manager, an admin console, ...) is in the foreground, Windows does not deliver mouse
  events to FastBar's low-level mouse hook. For as long as such a window is in front,
  FastBar reads the cursor position every ~16 ms instead.
- **Guard.** FastBar starts `FastBarGuard.exe` (~48 KB, ~300 KB of RAM, no CPU), which
  waits for FastBar to end. If FastBar is killed (End task, End process tree) or crashes
  without restoring the taskbar, the guard restores it and your auto-hide setting. The
  guard is started detached, so ending FastBar's process tree does not end it too.
  Without `FastBarGuard.exe`, FastBar uses a second copy of itself (`FastBar.exe --watch`).

- **Transparent taskbar.** Windows 11 draws the taskbar's background inside Explorer
  (a XAML `Rectangle` named `BackgroundFill`, plus a 1 px `BackgroundStroke` line). Like
  TranslucentTB, FastBar loads a small 64-bit DLL (`FastBarTap.dll`) into Explorer through
  Windows' XAML diagnostics interface (`InitializeXamlDiagnosticsEx`, started with 64-bit
  `rundll32`). The DLL only sets the `Fill` of those two rectangles, and puts Explorer's
  own brush back when transparency is turned off, FastBar exits, or FastBar is killed (the
  guard does it then). Once loaded, the DLL stays in Explorer until Explorer restarts.
- **Slides of a see-through bar.** A picture of the bar taken from the screen would also
  hold whatever is behind its clear background, and sliding it would drag that part of the
  screen along. So the slide-in uses the live thumbnail on a see-through window, and the
  picture used for hiding is taken twice, over a black and over a white backdrop placed
  right behind the bar, which gives a picture of the bar alone with per-pixel alpha. While
  that happens, an ordinary picture covers the bar on screen and is excluded from screen
  captures (`WDA_EXCLUDEFROMCAPTURE`), so you never see the backdrop.

- **Start closing with the cursor on the bar.** When Start or a flyout closes, Explorer
  hides the bar by itself even with the cursor on it, content included, and cannot be
  talked out of it. FastBar puts a picture in its place, makes the real bar invisible, and
  shows it again at its place once that hide is over, so on screen nothing happens.

## Limitations

- Only the main taskbar (primary monitor) is handled. Taskbars on other monitors are
  left alone.
- Tested on Windows 11 with the taskbar at the bottom. Other setups may not work as
  expected.
- While Quick Settings or the calendar is open, the taskbar behaves like Windows'
  own auto-hide: when the cursor moves away from it, Explorer hides it with its own
  animation. Touching the edge again brings it back with FastBar's slide.
- To remove FastBar completely, also delete its Start menu shortcut:
  `%APPDATA%\Microsoft\Windows\Start Menu\Programs\FastBar.lnk`).
- **Transparent taskbar:**
  - Screen recorders and screen sharing (OBS, Teams, ...) can show a black / white strip
    behind the taskbar for 2 frames when it starts hiding: they see the backdrop that the
    picture covers on your monitor (see "Slides of a see-through bar").
  - When the cursor goes in and out of the bar very quickly while Quick Settings or the
    calendar is open, Explorer's own hide animation can show for a few frames.
  - The DLL is locked while Explorer has it loaded. To replace it (new build), restart
    Explorer, or sign out and in again.
  - Building `FastBarTap.dll` needs a 64-bit gcc (mingw-w64); `build.bat` looks for it in
    `D:\mingw64\bin` or in the `GCC64` environment variable.
- If both FastBar and FastBarGuard are killed, the taskbar can stay hidden. Run FastBar
  again and choose **Exit**, or restart Windows Explorer.

## Building

You need MinGW gcc (with `windres`). Run `build.bat`, or:

```
windres fastbar.rc -O coff -o fastbar_res.o
gcc -Os -s -Wall -mwindows fastbar.c fastbar_res.o -o FastBar.exe -lshell32 -lgdi32 -lole32 -luuid
windres guard.rc -O coff -o guard_res.o
gcc -Os -s -Wall -mwindows guard.c guard_res.o -o FastBarGuard.exe -lshell32
```

The logo is `assets/FastBar.ico`, generated by `python assets/make_icon.py`
(needs Pillow).

`build.bat` cannot overwrite `FastBar.exe` while it is running. `rebuild.bat`
closes the running FastBar (the taskbar is restored), builds, and starts the
new build.

## Testing

`python tests\run_tests.py` drives the real mouse and keyboard against the
running FastBar (show the bar, open Quick Settings / the calendar, leave, come
back, go in and out quickly) and records every frame of what is on screen. It
fails when Explorer's own animation shows, when something shows through the
frozen bar, or when the slide stalls. Keep your hands off the mouse while it
runs. `--rebuild` runs `rebuild.bat` first; scenario names (`app`, `qs`, `cal`,
`qs-back`, `qs-rapid`, `cal-rapid`, `edge`, `edge-jitter`, `edge-sweep`,
`edge-wobble`, `edge-pull`, `edge-pull-sweep`) run only those. `edge` touches
the screen edge and leaves 20 times at random moments, with whatever `ShowDelay`
is set, and fails if the bar does not come up or a second taskbar shows.
`edge-jitter` rests on the edge with the cursor slipping a few pixels up and
back, like a hand on a mouse, and fails if that delays the bar. `edge-sweep`
and `edge-wobble` keep moving the mouse left and right along the bottom
(`edge-wobble` also drifting up to 15 px up and down) and check the same.
`edge-pull` and `edge-pull-sweep` move the mouse the way a real one does
(relative steps that push past the screen edge) instead of placing the cursor. It needs gcc and Python 3,
and assumes a bottom taskbar. Recordings go to `tests\out\`.

## License

MIT. See [LICENSE](LICENSE).
