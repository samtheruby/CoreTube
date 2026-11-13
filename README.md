# CoreTube

CoreTube is a YouTube TV app based on TizenTube, built as a Kodi add-on for CoreELEC boxes with Amlogic chips. It makes one add-on zip for each kind of CoreELEC:

| Box | CoreELEC | `--arch` |
|---|---|---|
| 32-bit, for example a Fire TV Cube (2nd gen) | 21, Amlogic-ng | `arm` |
| 64-bit, for example a Xiaomi stick | 22, Amlogic-no | `aarch64` |

Videos play on the box's own video hardware. Protected videos (for example bought or rented films) also need the official Widevine L3 libraries. They are not in this repository or in the add-on zip: they come separately as `coretube-widevine.zip` (see [Widevine](#widevine)). The libraries contain no keys: each box gets its own certificate from Google the first time it plays a protected video.

## What you need

- A Linux PC (x86-64) with Docker.
- A lot of disk space: the Cobalt source is about 25 GB, and a CoreELEC build folder is tens of GB.
- Two folders the build can reuse or fill:
  - a CoreELEC build folder (`--build-dir`). A new one may belong to root: the build then creates CoreELEC's folder inside it for the build user (uid 1000). A folder CoreELEC has already built in must not belong to root;
  - a Cobalt folder (`--cobalt-dir`) for the Cobalt source and its tools.

- `coretube-widevine.zip`, copied to `drm/runtime/` in this repository. The `helper` step links against it. Git ignores that folder.

A first build into empty folders takes several hours. Later builds reuse what is already built. Create the folders first if they do not exist yet.

## Build

```sh
./build-coretube.sh --docker --arch arm \
  --build-dir /path/to/coreelec-build --cobalt-dir /path/to/cobalt
```

Use `--arch aarch64` for the 64-bit add-on, with its own build folder. The zip lands in `out/arm/` or `out/aarch64/`, named `browser.tizentube-<version>.zip`.

The build runs five steps in order: `sources`, `packages`, `cobalt`, `helper` and `zip`. Run one of them with `--only STEP`. See every option with `./build-coretube.sh --help`.

The CoreELEC build folder may already hold other CoreELEC work. CoreTube adds its own `coretube-*` packages and reuses the rest. It keeps its CoreELEC source copy in a `coretube-coreelec` folder inside CoreELEC's own `build.CoreELEC-…` folder, and runs as the owner of that folder. If a package in the folder was built from different sources, the build stops and lists it instead of replacing it. Do not run another CoreELEC build in the same folder at the same time.

## Install

To install through Kodi and receive updates, add `https://samtheruby.github.io/CoreTube/` as a source in **Settings > File manager > Add source**. Open **Add-ons > Install from zip file**, choose that source, and install the repository ZIP. Enable **Unknown sources** in Kodi's add-on settings if prompted. Then choose **Install from repository > CoreTube Repository > Video add-ons > CoreTube**. Kodi selects the matching package for CoreELEC 21 (ARM) or 22 (AArch64); use only these supported systems.

If you already installed CoreTube from a ZIP, open its add-on information, choose **Versions**, and select CoreTube from **CoreTube Repository** to receive repository updates.

In Kodi, open **Add-ons**, choose **Install from zip file**, and pick the zip. Then open CoreTube from **Add-ons**. Kodi comes back when you exit CoreTube. The add-on keeps its `browser.tizentube` ID so existing installs retain their settings.

### Widevine

Without `coretube-widevine.zip`, CoreTube still starts and plays normal videos, but protected videos do not play. To add it, copy the zip unchanged (do not unpack it) to one of these folders on the box. CoreTube uses the first one it finds, in this order:

1. `/storage/.kodi/userdata/addon_data/browser.tizentube/widevine/` (CoreTube creates this folder the first time it starts)
2. `/storage/.config/`
3. `/flash/` (the boot partition; it is read-only while CoreELEC runs, so copy it there from a PC or remount it first)
4. `/storage/`

You can copy it with Kodi's file manager, over SMB (the `Userdata` and `Configfiles` shares), or with `scp`. CoreTube checks every file in the zip before it uses it. The add-on's `service.log` (in the same `addon_data/browser.tizentube` folder) shows which zip it used, or why it did not use one.

Auto frame rate changes the TV's display mode only for fullscreen videos. Preview cards do not trigger mode changes. Like Kodi, it switches to the video's resolution when Kodi's whitelist has a mode at that resolution and a refresh rate that fits the video. Otherwise it keeps the current resolution and changes only the refresh rate. The video's resolution is the best quality YouTube offers for it. YouTube can offer qualities up to the largest whitelisted resolution, so a 4K video on a TV set to 1080p can switch it to 2160p.

When you leave a video with back, the TV goes back to its own display mode at once, like Kodi when playback stops. The picture stays off until the switch is done, then the menu shows. When a video ends and the next one starts, the mode stays for 5 seconds, so a next video at the same rate needs no switch.

CoreTube tells YouTube it is a Fire TV Cube, because YouTube buffers much better for it. To send Cobalt's own user agent instead, put `export TIZENTUBE_USER_AGENT=''` in the add-on's `env` file (in Kodi's `userdata/addon_data/browser.tizentube` folder).

After a display mode switch, the video waits before it plays, as in Kodi. The wait is Kodi's **Delay after change of refresh rate** (**Settings** > **System** > **Display**), and at least 1 second, while the TV's picture is off for the switch. TizenTube's own **Auto frame rate pause duration** option has no effect.

Video previews on the home screen are always off: they can make the screen flash white for a moment as they start, and the cause is not known yet. TizenTube's **Video previews** option has no effect.

Update CoreTube through Kodi: install the newer zip the same way. TizenTube's own updater is off; its **Check for updates** button only says that Kodi handles updates.

## Tests

```sh
./build-coretube.sh test --docker
```

Without `--docker`, the tests need Python 3.10 or newer with pytest.

## Repository updates

GitHub Pages publishes the latest stable release automatically when a release is published or the repository setup changes on `main`. Upload both architecture ZIPs and `SHA256SUMS` before publishing each release. The **Publish Kodi repository** GitHub Actions workflow can also be run manually.

To check the repository locally, download those three release assets into a folder and run `python3 tools/build_repository.py ASSET_FOLDER out/pages`. The output folder must not already exist. The builder verifies both checksums and architectures before creating the site.

## License

CoreTube's own files are under the BSD-3-Clause license (`LICENSE`). Some files keep the license they came with, shown at their top. The Widevine libraries in `coretube-widevine.zip` belong to Google and Amazon and are not part of this repository.
