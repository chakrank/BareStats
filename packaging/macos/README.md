# BareStats macOS Packaging & Distribution

BareStats for macOS is distributed as a standalone **Application Bundle (`BareStats.app`)** and a **Drag-and-Drop Disk Image (`.dmg`)**, supporting both Apple Silicon (`arm64`) and Intel (`x86_64`) via Universal Binaries.

## Requirements
- macOS 10.15 (Catalina) or newer (macOS 11 Big Sur, 12 Monterey, 13 Ventura, 14 Sonoma, 15 Sequoia supported).
- Xcode Command Line Tools (`xcode-select --install`).
- CMake 3.20+.

## Building & Packaging
Run the packaging script from any terminal:
```bash
./packaging/macos/build_macos.sh
```

Or configure manually via CMake:
```bash
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DBARESTATS_BUILD_TESTS=ON

cmake --build build --config Release
cpack --config build/CPackConfig.cmake -G DragNDrop
```

The resulting Drag-and-Drop `.dmg` will be placed in `packages/macos/`.

## Running & CLI Options
Launch the application:
```bash
open BareStats.app
```

Or run directly from the command line:
```bash
./BareStats.app/Contents/MacOS/BareStats [OPTIONS]
```

### CLI Commands:
- `--test`, `--cli`: Run instant hardware telemetry verification in terminal and exit.
- `--toggle`: Toggle visibility of running overlay.
- `--move`: Toggle interactive reposition mode.
- `--preset <name>`: Switch position preset (`BottomCenter`, `TopRight`, `TopLeft`, etc.).
- `--reload`: Hot-reload configuration.
- `--quit`: Exit the running BareStats instance.
