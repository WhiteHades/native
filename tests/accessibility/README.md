# Accessibility target probes

## Linux AT-SPI

`linux_atspi_probe.c` is an external AT-SPI2 client. It does not link to the
Native SDK and therefore checks the accessibility tree that a screen reader
actually receives rather than an in-process semantic snapshot.

The probe's default contract is the `accessibility-smoke` fixture:

- `Accessibility smoke` grouping -> `Lesson list` list -> seven materialized
  `Lesson 40` through `Lesson 46` listitems;
- `Lesson 42` has `posinset=42`, `setsize=1000`, and clipped visible bounds;
- `Lesson 40` has bounds fully outside the viewport (GTK's stock AT-SPI
  backend reports `SHOWING` for every non-hidden accessible);
- `Count action`, `Study mode`, `Reading mode`, `Search lessons`, `Volume`, and
  `Details` round-trip actions into observable result semantics;
- `Completion` exposes the read-only value `0.42`;
- removing `Transient target` removes it from the tree and makes a retained
  AT-SPI reference defunct.

Names, timeouts, and list expectations can be overridden with probe options.
Run `linux_atspi_probe --help` for those options.

The repository runner compiles the probe and launches an already-built fixture
inside an isolated D-Bus/AT-SPI session:

```sh
.github/scripts/linux-accessibility-smoke.sh \
  /absolute/path/to/accessibility-smoke
```

The default backend is Xvfb. For local debugging on an existing graphical
session, opt in explicitly:

```sh
NATIVE_A11Y_BACKEND=session \
  .github/scripts/linux-accessibility-smoke.sh \
  /absolute/path/to/accessibility-smoke
```

Broadway is intentionally not a fallback: GTK 4 does not register Broadway
applications in the AT-SPI registry, so it cannot provide evidence for this
smoke test. The runner unsets an inherited `GTK_A11Y` value and never disables
GTK accessibility.

For installed-Orca acceptance, install Orca, Xvfb, and `xdotool`, then enable
the screen-reader mode inside the same isolated D-Bus session:

```sh
NATIVE_A11Y_ORCA_SMOKE=1 \
  .github/scripts/linux-accessibility-smoke.sh \
  /absolute/path/to/accessibility-smoke
```

That mode first requires the raw external AT-SPI probe to pass, then starts
tracked Orca and Speech Dispatcher process groups with a private speech socket,
drives the fixture's real X11 focus order with XTEST input, and requires
Orca-owned speech records for `Lesson 42` and `Count action`. It refuses the
session backend so it cannot attach to a user's existing desktop screen reader.

## Windows UI Automation

`windows_uia_probe.cpp` is an external UI Automation client for the same
installed fixture. It verifies raw-view hierarchy, stable virtual-list runtime
IDs and position/count metadata, clipped bounds, exact control patterns,
focus/actions, Unicode Text/TextEdit ranges, precise property, automation, and
structure events. It also removes and restores a keyed text field to prove that
semantic AutomationId reuse creates a new RuntimeId while retained old
providers, ranges, and actions stay unavailable, then verifies provider
disconnection after host teardown.

Run it from a Visual Studio Developer PowerShell so the runner can compile the
probe with `cl.exe`:

```powershell
.\tests\accessibility\windows-accessibility-smoke.ps1 `
  -HostExe .\examples\accessibility-smoke\zig-out\bin\accessibility-smoke.exe
```

The default requires an interactive, non-session-0 desktop. CI may pass
`-AllowNonInteractive` for the external UIA protocol check, but that does not
count as NVDA evidence; an NVDA smoke must run in an interactive Windows user
session.

The installed-NVDA acceptance runner performs a strict interactive-desktop
preflight, verifies the official NVDA 2026.1.1 installer hash and signature,
installs it silently, starts it with isolated configuration and input/output
logging, runs the full UIA probe plus focused virtual-list and Toggle checks,
and requires NVDA-owned speech records for `Lesson 42`, `list item`,
`42 of 1000`, and the checked `Study mode` state:

```powershell
.\tests\accessibility\windows-nvda-smoke.ps1 `
  -HostExe .\examples\accessibility-smoke\zig-out\bin\accessibility-smoke.exe
```

Use a clean dedicated desktop with no pre-existing NVDA process or installation.
The runner measures and rejects non-interactive, session-0, locked, or
non-elevated desktops, and removes the installed NVDA copy after the run. The
hosted x64 runner is tried first; an interactive self-hosted runner is a fallback
only if that measured preflight fails.
