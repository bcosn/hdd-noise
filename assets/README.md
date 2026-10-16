# App and tray icon

`app-icon.svg` is original vector artwork made for this project. It uses only
geometric shapes, with no external images, fonts, brand logos, or trademarks.

- `app-icon.ico`: 32-bit RGBA frames at 16, 20, 24, 32, 40, 48, 64, 128 and 256 px.
- `app-icon.png`: transparent 256 px PNG.
- `icon-preview.png`: light and dark taskbar previews, including actual small sizes.

To regenerate, run `python tools/generate_icon.py` from the project checkout.
Pillow is required for this development step only. The Windows app does not
require Python or Pillow. The generator reads the SVG geometry, strengthens
small-size outlines, writes the ICO, and verifies its directory and alpha data.
