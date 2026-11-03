#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "resources" / "lib"))
from android_drm import run

if __name__ == "__main__":
    try:
        raise SystemExit(run(os.environ["ADDON_DIR"], os.environ["HOME"], sys.argv[1:]))
    except (OSError, ValueError, RuntimeError) as error:
        print(f"android-drm: {error}", file=sys.stderr)
        raise SystemExit(1)
