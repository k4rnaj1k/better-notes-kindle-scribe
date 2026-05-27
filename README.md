# BetterNotes

A note-taking app for jailbroken Kindle Scribe. Replaces the built-in notes
app with cross-note links, multi-page notes with templates, Markdown
rendering, OCR (Tesseract, toggleable), an on-screen Cairo keyboard, and PDF
export. Notes live on disk as JSON + PNG and can be backed up by `scp`.

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
