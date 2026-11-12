# screen-translate

On-demand in-place screen translation orchestrator and layer-shell overlay for Wayland.

`screen-translate` captures the active screen (or selected region), runs OCR text recognition and translation, and renders translated text directly in place over the original screen contents using a hardware-accelerated Wayland layer-shell surface.

Components:
- `screen-translate.sh`: Main orchestrator script coordinating capture, translation, and UI display
- `screen-translate-overlay`: C layer-shell overlay client rendering bounding boxes and translations
- `screen-translate-render`: Python helper for OCR clustering, background blur, and layout
- `screen-translate-watcher`: Evdev keyboard watcher for dismiss and navigation hotkeys

## Requirements

- `wayland-client`
- `cairo`
- `xkbcommon`
- `grim` (for screen capture)
- Python 3 with Pillow
- C99 compiler (`cc`)
- GNU Make

## Installation

```sh
make
sudo make install
```

## Usage

Bind `screen-translate` to a compositor hotkey (e.g. `Super+Alt+t`). Press `Escape` to dismiss the overlay.

## License

MIT License.
