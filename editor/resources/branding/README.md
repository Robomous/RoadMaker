# Branding / application icon

First-party Robomous artwork (MIT). All raster artifacts here are **generated**
from the source logo `assets/logo/robomous-original.png`.

Treatment: solid coral (`#EA5A47`) rounded square with the robot glyph knocked
out in white.

- `icon_{16,24,32,48,64,128,256,512}.png` — runtime `QIcon` (embedded via
  `resources.qrc` under `:/branding/`) and the Linux hicolor theme.
- `roadmaker.icns` — macOS bundle icon (`MACOSX_BUNDLE_ICON_FILE`).
- `roadmaker.ico` + `roadmaker.rc` — Windows executable icon.
- `roadmaker.desktop` — Linux desktop entry.

## Regenerating

The generator is a one-time manual tool (Pillow is intentionally **not** a repo
dependency; the CI asset scripts stay stdlib-only). To regenerate after the
source logo changes:

```sh
python -m venv .venv && . .venv/bin/activate && pip install pillow
python gen_branding_icons.py \
    --src assets/logo/robomous-original.png \
    --out editor/resources/branding
```

(The `gen_branding_icons.py` helper is kept out of the tree by design; recreate
it from this PR's description if needed.)
