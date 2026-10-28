# Schatkamer Player for PS5

An experimental, personal web app for a jailbroken PS5 that browses, searches and
plays programmes from the [Schatkamer](https://schatkamer.beeldengeluid.nl) archive
of Beeld & Geluid on the console, with picture, sound and the site's PlayReady DRM.

> **Terms of use.** Schatkamer's published terms prohibit automated access and
> third-party interfaces, even for personal use. This project is a technical
> experiment; check with the service provider before relying on it.

## What it is

One payload, `schatkamer-app.elf`, and one home-screen app, **Schatkamer Player**.

- **The payload** installs the app, then runs a small server on `127.0.0.1:48731`. The
  server serves the UI, reads the site's search and browse data, signs a programme's
  stream when you pick it, and relays the CDN to the player with certificate-verified
  curl (`src/app.c`). It never decrypts media or touches licences: the player talks to
  the site's PlayReady service itself.
- **The app** is a web app in the console's media web-app context, the only place the
  console's DRM works for this site. Its page (`src/webui.html`) is the whole interface:
  featured rows, search with recent searches, lists with sorting and paging, details,
  and a player (Shaka Player 4) with a controls bar.

## Requirements

- A PS5 running a jailbreak with an ELF loader on port 9021 (the payload installs the
  app itself; nothing else is mounted).
- Windows with WSL (Ubuntu) or Linux to build. No hardware tools beyond that.

## Build

```bash
sudo apt install clang lld llvm llvm-dev make pkg-config git wget python3
bash build.sh
```

`build.sh` clones the pinned PS5 payload SDK, fetches the pinned PacBrew libraries
(about 350 MB, checksum verified), and builds `schatkamer-app.elf`. GitHub Actions does
the same on every push (`.github/workflows/build.yml`) and keeps the payload as an
artifact.

## Install and run

1. Send `schatkamer-app.elf` to the ELF loader, for example with `nc`:

   ```bash
   nc -w 5 <ps5-ip> 9021 < schatkamer-app.elf
   ```

   It installs **Schatkamer Player** on the home screen and starts the server.
2. Open **Schatkamer Player**.

The server stops when the console restarts or sleeps deeply: send the payload again
(or autoload it with your payload manager) before opening the app.

### The start URL

The console's web-app launcher only accepts an `https` start URL with a certificate
it trusts; local addresses, `file:` URLs and self-signed certificates were all
refused. `launcher/param.json` therefore points at a short link that redirects to
`http://localhost:48731/`. Make your own redirect that targets that address (a URL
shortener or any HTTPS page you control) and put it in `webAppUri`, then rebuild.

## Controls

| Button | Home | Search | List | Player |
| --- | --- | --- | --- | --- |
| D-pad | Move | Move | Move | Left/Right 10 s, Up/Down 1 min |
| Cross | Open | Open the keyboard | Open | Play / pause |
| Triangle | Search | Space | | |
| Square | | Delete / forget a recent search | Sort | |
| R2 | | Search | | |
| L1 / R1 | Jump along a shelf | | Jump in the list | |
| Circle | | Back | Back | Back |

The console's keyboard commits the word on the first R2; the page then searches.

## Layout

```
src/app.c          server: search, browse, stream signing, relay
src/webui.html     the whole UI and player
src/install.c      registers the web app on the console
src/console_curl.* libcurl support code for the payload environment
launcher/          the app's metadata, icon and splash (tools/launcher_art.py draws the art)
third_party/       Shaka Player (Apache-2.0), Inter font (OFL)
tools/             embed helper, PacBrew bootstrap, art script
PLAYBACK.MD        findings from the original playback experiments
```

## Licences and credits

GPL-3.0-or-later (see `LICENSE`). Includes code adapted from the PS5 payload SDK
samples (John Törnblom), `console_curl` and the PacBrew bootstrap from
ps5-native-app-boilerplate (BlackBearReloaded), Shaka Player (Apache-2.0) and the Inter
typeface (SIL Open Font License, `third_party/fonts/Inter-LICENSE.txt`).
