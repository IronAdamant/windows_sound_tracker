# Testing guide

How to check Sound Tracker before a release, and how to collect logs when
something goes wrong on someone else's PC.

## 1. Turn on debug logging first

**Options → Debug Logging**:

| Level | What it records | When to use |
|---|---|---|
| Normal (default) | Startup info, devices, audio sessions added/removed, every alert fired, alerts that were blocked by cooldown or input, warnings, errors, crashes | Always on. Small. |
| Debug | Plus every sound start/end, every alert decision with the reason, focus changes | Reproducing a problem |
| Verbose | Plus level envelopes for every sound (loudness per 100 ms) | Tuning thresholds, deep debugging |

- **Help → Show Current Debug Log** opens the folder with the current file selected.
- Logs are in the `logs` folder next to `SoundTracker.exe` (or
  `%LOCALAPPDATA%\WindowsSoundTracker\logs` if that folder isn't writable).
  The last 10 debug logs are kept; each rolls over at 20 MB.
- A crash writes a `FATAL` line and a `crash_*.dmp` minidump to the same folder.
  `SoundTracker.exe --test-crash` checks that this works.
- One-off runs: `SoundTracker.exe --debug` or `--verbose` (not saved).

Useful things to search for in a debug log:

| Search for | Meaning |
|---|---|
| `ALERT` | An alert fired: which process, PID, peak, how long it was quiet before |
| `No alert:` | A loud moment that did **not** alert, and why (cooldown, input, not quiet long enough, too short) |
| `Border shown` / `Border skipped` | Which window got the border, its position and DPI, or why not (minimized, other virtual desktop) |
| `Session added` / `Session removed` | Which programs have audio, on which output device, and which window they map to |
| `WARN` / `ERROR` / `FATAL` | Problems |
| `stalled` | The PC was too busy for the meter loop; sounds in that gap can be missed |
| `Heartbeat` | Every 5 min: session count, memory, handles (should stay flat) |

> Logs contain window titles (e.g. browser tab titles). Turn off
> **Options → Record Window Titles** before sending logs if that matters.

## 2. Multi-window test without a game: SoundBeacon

`SoundBeacon.exe` (built next to `SoundTracker.exe`) is a fake game client.
Each copy is a separate process with its own window and audio session, just
like one multiboxed WoW client. Press **Space** or click to "cast": it plays a
quiet cast sound, then a loud splash 3-12 seconds later, and turns orange so you
can see which window splashed.

```powershell
.\tools\launch_beacons.ps1 -Count 5          # 5 windows, cast manually
.\tools\launch_beacons.ps1 -Count 5 -Auto    # 5 windows fishing on their own
.\tools\launch_beacons.ps1 -Count 3 -LoudCast  # cast as loud as the splash
```

Then in Sound Tracker: **Start Tracking → Window Alerts... → Add**, process name
`SoundBeacon.exe`, trigger level `15`, **Save rule**.

- [ ] Every orange (splashing) beacon gets a red border with its own PID in the label.
- [ ] No border appears for casts (with the default quiet cast).
- [ ] With `-LoudCast`, casts **you** trigger by clicking are ignored when
      "Ignore after input" is 1200 ms; auto casts (no input) do alert.
- [ ] The **Alerts** column in the main list shows `1` on splash rows only.
- [ ] The live list in Window Alerts shows each beacon separately, with
      "Recent max" around 30-35 % for splashes and about 5 % for casts.

The beacons can log exact splash times for automated comparison:
`SoundBeacon.exe --log=beacons.csv` (all copies can share one file).

## 3. Browser checklist

Browsers are a good way to test real-world audio, but one thing to know first:

> **All windows and tabs of one browser instance share one audio session.**
> Chrome, Edge, Brave, Opera and Vivaldi play every tab's sound from a single
> audio helper process, so Windows (and Sound Tracker) can tell *which browser*
> made a sound but not *which window*. A border then goes on every window of
> that browser, labelled "N windows, can't tell which". To get separately
> identifiable windows, start **separate instances** with their own profile
> folder (section C). That is also the closest match to multiboxed game clients.

`tools/browser_beacon.html` is a browser version of SoundBeacon (cast then
splash, page turns orange on splash). Open it with `?name=Window%201` etc. so
each window shows its own name. Click once in each window first, because browsers
block sound until you interact with the page.

### A. Basics (one browser window)

- [ ] Play a YouTube video: a row appears for the browser (e.g. `brave.exe`)
      with **Window** = that window's title, the duration counting up with ▶.
- [ ] Pause: the row stops (▶ disappears) within about half a second.
- [ ] Short sounds are caught: the beacon page's 0.45 s cast and 0.8 s splash each
      give their own row with sensible durations.
- [ ] Mute the tab (right-click tab → Mute site): no new rows while muted.
- [ ] Windows volume mixer: set the browser to 0 %, then 50 %. Note whether
      rows still appear and what **Peak** shows, and tell me. (Peak
      meters can behave differently per driver, so I want to see real results.)
- [ ] A notification sound (e.g. a web chat ping) shows as a short row.

### B. Several windows of the same browser

- [ ] Two windows of the same browser each playing audio: **one** row/PID, Window
      column says `... (+1 more windows)`. *Expected limitation (see above).*
- [ ] Alert rule for that browser: both windows get a border and the label
      says "2 windows, can't tell which".

### C. Separate browser instances (the multibox equivalent)

Start each instance with its own profile folder (close nothing, these run side by side):

```powershell
# Brave (same idea for chrome.exe / msedge.exe with their own paths)
$brave = "C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe"
$page  = "file:///D:/Coding%20Projects/windows_sound_tracker/tools/browser_beacon.html"
1..3 | % { Start-Process $brave "--user-data-dir=$env:TEMP\st-test-$_ --no-first-run --new-window `"$($page)?name=Window%20$_`"" }

# Firefox: each profile folder must exist first
1..2 | % { md "$env:TEMP\st-ff-$_" -Force | Out-Null; Start-Process firefox "-no-remote -profile `"$env:TEMP\st-ff-$_`" `"$($page)?name=Firefox%20$_`"" }
```

- [ ] Window Alerts live list shows one row per instance, each with a different PID.
- [ ] **Identify all windows** puts a blue labelled border on every instance, each
      with the right PID (the PID is also in Task Manager → Details).
- [ ] Rule for `brave.exe` (or `chrome.exe`/`msedge.exe`/`firefox.exe`), trigger
      level 15: turn on **Auto** in each page. Every time a page turns orange,
      **that** window (and only that one) gets the red border.
- [ ] The taskbar button of a splashing background window flashes orange and
      stops when you click into that window.

### D. Different browsers at the same time

- [ ] Brave + Edge + Firefox each playing: three separate rows, correct names and
      window titles.

### E. Window states

- [ ] **Minimized** window splashes: no border (log: `Border skipped ... minimized`),
      taskbar button still flashes.
- [ ] Drag the window while its border is showing: the border follows it.
- [ ] **Maximized** and **F11 fullscreen**: border is drawn on top along the screen edges.
- [ ] **Second monitor**, and a monitor with different scaling (e.g. 100 % vs 150 %):
      border lines up exactly with the window edges and the thickness looks the same.
- [ ] Window on **another virtual desktop** (Win+Tab): no border (log says why).
- [ ] Clicks still go to the window under the border (it must never block input).

### F. Output devices

- [ ] Change the default output device while audio plays: log shows
      `Default output device changed`, rows continue on the new device.
- [ ] Unplug a USB/Bluetooth headset while it's playing: session removed in the
      log, no crash, nothing stuck "playing".
- [ ] Plug in a USB stick: a **System Sounds** row whose description says
      `Connected: <device name>`.

### G. Long run

- [ ] Leave it tracking for an hour with music playing in a browser. In the debug
      log, `Heartbeat` memory and handle counts stay roughly flat and there are
      no `stalled` warnings.

## 4. What to send when reporting a problem

1. Set **Options → Debug Logging → Debug**.
2. Reproduce the problem and note the time it happened.
3. **Help → Show Current Debug Log**, then send the `debug_*.log` file(s), plus any
   `crash_*.dmp` and the `sound_log_*.csv` from the same folder.
4. Say what you expected to happen and roughly when (clock time) it went wrong.
