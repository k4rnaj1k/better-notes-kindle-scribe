# BetterNotes

A note-taking app for jailbroken Kindle Scribe. Replaces the built-in notes
app with cross-note links, multi-page notes with templates, Markdown
rendering, OCR (Tesseract, toggleable), an on-screen Cairo keyboard,
Bluetooth/USB keyboard support, and PDF export. Notes live on disk as JSON + PNG and can be backed up by `scp`.

## Hardware keyboard

Bluetooth keyboards work through
[kindle-hid-passthrough](https://github.com/zampierilucas/kindle-hid-passthrough),
which connects them and exposes each one as a `/dev/input/eventN` node.
BetterNotes reads that node itself, the same way it reads the pen, so the
keyboard works even though the Kindle's X server ignores it. USB OTG keyboards
work the same way.

1. Install kindle-hid-passthrough (`kpm install kindle-hid-passthrough`, or its
   manual installer).
2. Launch BetterNotes from KUAL. `launch.sh` starts the passthrough daemon if
   it isn't running and stops it again on exit. Set `BN_HID_AUTOSTART=0` to
   turn that off.
3. Open **Settings → Bluetooth keyboards → Manage**. Put the keyboard in
   pairing mode, tap **Scan**, then **Pair** next to it. If the keyboard asks
   for a passkey, the page shows it: type it on the keyboard and press Enter.
4. From then on the keyboard reconnects by itself (press a key to wake it).
   It's picked up whenever it connects, including mid-session, and the status
   line shows `keyboard: <name>`.

The Bluetooth page talks to the daemon's local API on port 8321, the same one
BTManager uses, so pairing with BTManager or `kindle-hid-passthrough --pair`
works too. On that page you can also connect, disconnect and forget paired
keyboards (tap the bin twice) and turn Bluetooth off. Scanning and pairing
briefly disconnect any keyboard that's already connected. If the daemon isn't
running, the page offers to start it. Set `BN_HID_DIR` if it's installed
somewhere other than `/mnt/us/kindle_hid_passthrough`.

While a keyboard is connected, text dialogs skip the on-screen keyboard. The
browser and pickers show a focus ring you move with the arrow keys.

Two layouts are built in: English (US QWERTY) and Ukrainian (ЙЦУКЕН, the
standard Windows Ukrainian layout). `Ctrl+Space` or `Alt+Shift` switches
between them, from anywhere, and the choice applies to the on-screen keyboard
too (its globe key does the same). It's also under Settings → Keyboard layout
and is remembered. While typing, the toolbar shows `EN` or `UA`. In Ukrainian,
`ґ` is on the `\` key or `AltGr+г`, and shortcuts like `Ctrl+S` stay on their
usual keys. To use the layout configured in X instead, set `BN_KBD_EVDEV=0` in
`launch.sh`.

| Where | Keys |
| --- | --- |
| Anywhere | `Ctrl+S` save · `Ctrl+Q` quit · `Esc` close the open dialog · `Ctrl+Space` / `Alt+Shift` switch EN/UA |
| Browser | arrows / `Home` / `End` / `PgUp` / `PgDn` move · `Enter` open · `Backspace` parent folder · `Ctrl+N` new note · `Ctrl+M` new markdown · `Ctrl+Shift+N` new folder · `F2` rename · `Del` delete · `m` move (`Ctrl+Enter` drop it here, `Esc` cancel) |
| Notebook | `←` `→` / `PgUp` `PgDn` page · `Home` / `End` first / last page · `p` pen · `e` eraser · `l` lasso · `1`–`5` pen width · `t` tags · `h` show/hide toolbar · `Ctrl+N` add page · `Ctrl+T` template · `Ctrl+E` export PDF · `Esc` back |
| Markdown | type to edit · `Ctrl+←` `→` by word · `Ctrl+Home` / `End` start / end · `PgUp` `PgDn` · `Ctrl+Z` undo · `Ctrl+B` bold · `Ctrl+I` italic · `Ctrl+K` link · `Ctrl+E` source/preview · `Tab` indent · `Enter` continues lists · `Esc` back |
| Bluetooth page | `↑` `↓` choose · `Enter` connect / disconnect / pair · `Del` forget (press twice) · `s` scan · `b` Bluetooth on/off · `Esc` back to Settings |
| Dialogs | `Enter` confirm · `Esc` cancel · `Tab` page/notebook scope (tags) · `↑` `↓` `Enter` pick (link and template pickers) · `y` / `n` (delete) |

## Layout

```
src/                  C++17 sources (GTK2 + Cairo + Pango)
kual/betternotes/     KUAL extension (config.xml, menu.json, launch.sh)
deps/                 Cross-build scripts for tesseract + leptonica
.github/workflows/    CI: cross-compile for kindlehf, package KUAL tarball
Dockerfile            Reproduces the koxtoolchain + kindle-sdk locally
meson.build           Build definition (cross-compiles to ARM kindlehf)
```

## Build

CI handles the cross-compile end-to-end (see `.github/workflows/build.yml`).
The artifact is a `betternotes-<sha>.tar.gz` that unpacks into
`/mnt/us/extensions/` on a jailbroken Scribe.

Local build:

```
make docker-image          # one-time, ~30 min, builds koxtoolchain + kindle-sdk
make build                 # cross-compile betternotes inside the container
```

## On-device install

1. Unpack the artifact into `/mnt/us/extensions/`.
2. Open KUAL on the Scribe; the "BetterNotes" extension appears.
3. Notes are stored under `/mnt/us/documents/betternotes/`. Backup:

```
scp -r kindle:/mnt/us/documents/betternotes ./backup
```
