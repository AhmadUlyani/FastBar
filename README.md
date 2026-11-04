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
- **Lightweight**: native C/Win32, ~45 KB executable, ~2 MB of RAM, ~0% CPU when idle.
  It is event-driven (see below for the one exception).

## Usage

1. Keep `FastBar.exe` and `FastBarGuard.exe` in the same folder and run `FastBar.exe`.
   Its icon appears in the system tray.
2. Move the cursor to the bottom edge of the screen and the taskbar slides up.
   Move away and it slides down.
3. Click the tray icon for the menu:
   - **Enabled**: turn FastBar on or off. When off, the taskbar behaves normally.
   - **Start with Windows**
   - **Edit settings...** / **Reload settings**
   - **Exit**: restores the taskbar and your original auto-hide setting.

## Settings

Settings live in `FastBar.ini`, next to the executable. FastBar creates the file on
first run. After editing it, choose **Reload settings** from the tray menu.

| Key | Default | Meaning |
|---|---|---|
| `ShowDelay` | 0 | ms to wait after the cursor touches the edge (0 = instant) |
| `HideDelay` | 300 | ms to wait after the cursor leaves the taskbar before hiding |
| `ShowAnimationMs` | 250 | slide-up duration in ms (0 = no animation) |
| `HideAnimationMs` | 180 | slide-down duration in ms (0 = no animation) |
| `Easing` | 1 | animation curve: 0 = linear, 1 = smooth, 2 = snappy (fast start), 3 = ease-in-out |
| `TriggerPixels` | 1 | how many pixels from the screen edge trigger the taskbar |
| `BlockInFullscreen` | 1 | 1 = don't pop up over full-screen games or videos |
| `ShowWithStartMenu` | 1 | 1 = show the taskbar while Start or Search is open |

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
- **Guard.** FastBar starts `FastBarGuard.exe` (~22 KB, ~300 KB of RAM, no CPU), which
  waits for FastBar to end. If FastBar is killed (End task, End process tree) or crashes
  without restoring the taskbar, the guard restores it and your auto-hide setting. The
  guard is started detached, so ending FastBar's process tree does not end it too.
  Without `FastBarGuard.exe`, FastBar uses a second copy of itself (`FastBar.exe --watch`).

## Limitations

- Only the main taskbar (primary monitor) is handled. Taskbars on other monitors are
  left alone.
- Tested on Windows 11 with the taskbar at the bottom. Other setups may not work as
  expected.
- While Quick Settings or the calendar is open, the taskbar behaves like Windows'
  own auto-hide: when the cursor moves away from it, Explorer hides it with its own
  animation. Touching the edge again brings it back with FastBar's slide.
- If both FastBar and FastBarGuard are killed, the taskbar can stay hidden. Run FastBar
  again and choose **Exit**, or restart Windows Explorer.

## Building

You need MinGW gcc. Run `build.bat`, or:

```
gcc -Os -s -Wall -mwindows fastbar.c -o FastBar.exe -lshell32 -lgdi32
gcc -Os -s -Wall -mwindows guard.c -o FastBarGuard.exe -lshell32
```

## License

MIT. See [LICENSE](LICENSE).
