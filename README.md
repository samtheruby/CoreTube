# CoreTube

CoreTube builds TizenTube, the YouTube TV app, as a Kodi add-on for CoreELEC boxes with Amlogic chips. It makes one add-on zip for each kind of CoreELEC:

| Box | CoreELEC | `--arch` |
|---|---|---|
| 32-bit, for example a Fire TV Cube (2nd gen) | 21, Amlogic-ng | `arm` |
| 64-bit, for example a Xiaomi stick | 22, Amlogic-no | `aarch64` |

Videos play on the box's own video hardware. Protected videos use the official Widevine L3 libraries in `drm/runtime/`. These contain no keys: each box gets its own certificate from Google the first time it plays a protected video.

## What you need

- A Linux PC (x86-64) with Docker.
- A lot of disk space: the Cobalt source is about 25 GB, and a CoreELEC build folder is tens of GB.
- Two folders the build can reuse or fill:
  - a CoreELEC build folder (`--build-dir`). A new one may belong to root: the build then creates CoreELEC's folder inside it for the build user (uid 1000). A folder CoreELEC has already built in must not belong to root;
  - a Cobalt folder (`--cobalt-dir`) for the Cobalt source and its tools.

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

In Kodi, open **Add-ons**, choose **Install from zip file**, and pick the zip. Then open TizenTube from **Add-ons**. Kodi comes back when you exit TizenTube.

Auto frame rate changes the TV's display mode only for fullscreen videos. Preview cards do not trigger mode changes.

TizenTube tells YouTube it is a Fire TV Cube, because YouTube buffers much better for it. To send Cobalt's own user agent instead, put `export TIZENTUBE_USER_AGENT=''` in the add-on's `env` file (in Kodi's `userdata/addon_data/browser.tizentube` folder).

Video previews on the home screen are off. To turn them on, open the add-on's settings (**Add-ons** > TizenTube > **Configure**) and switch on **Video previews on the home screen**. Previews can make the screen flash white for a moment as they start; the cause is not known yet.

## Tests

```sh
./build-coretube.sh test --docker
```

Without `--docker`, the tests need Python 3.10 or newer with pytest.

## License

CoreTube's own files are under the BSD-3-Clause license (`LICENSE`). Some files keep the license they came with, shown at their top. The Widevine libraries in `drm/runtime/` belong to Google and Amazon.
