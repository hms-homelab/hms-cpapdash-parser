# hms-cpapdash-parser

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-%23FFDD00.svg?logo=buy-me-a-coffee)](https://www.buymeacoffee.com/aamat09)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

Shared C++ library for parsing CPAP therapy data into a unified, standards-based session model. It is the single source of truth for parsing across the CpapDash stack — consumed by both [hms-cpap](https://github.com/hms-homelab/hms-cpap) (Home Assistant data collection) and the [CpapDash](https://www.cpapdash.com/opensource) apps, so a fix in one place benefits both.

## Features

- One interface (`ISessionParser`) over multiple manufacturers
- Parse from disk **or** from in-memory buffers (no temp files needed)
- Auto-detect the manufacturer from a session directory
- Unified output model: events, vitals, breathing summaries, settings, and derived metrics (AHI, etc.)
- Minimal dependencies — header + static lib, C++17, filesystem only

## Supported Formats

| Manufacturer | Devices | Files | Build flag |
|--------------|---------|-------|------------|
| ResMed | AirSense 10 / 11 | EDF+ (`BRP`, `PLD`, `SAD`, `EVE`, `STR`) | on by default |
| Lowenstein | Prisma | WMEDF + event XML (`VLD`) | `CPAPDASH_PARSER_WITH_LOWENSTEIN` |

## Quick Start

```cpp
#include <cpapdash/parser/ISessionParser.h>
#include <iostream>

using namespace cpapdash::parser;

int main() {
    // Auto-detects ResMed / Lowenstein from the directory contents
    auto parser = createParser("/path/to/session_dir");
    if (!parser) return 1;

    auto session = parser->parseSession(
        "/path/to/session_dir",
        "cpap_resmed_23243570851",   // device id (MQTT/DB key)
        "Bedroom CPAP");             // friendly name
    if (!session) return 1;

    std::cout << "Events: "  << session->events.size()
              << "  Vitals: " << session->vitals.size() << "\n";
    return 0;
}
```

Parsing from memory buffers (e.g. files streamed off a WiFi SD card) uses
`parser->parseSessionFromBuffers(buffers, device_id, device_name)`.

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CPAPDASH_PARSER_WITH_LOWENSTEIN` | OFF | Build the Lowenstein Prisma parser |
| `CPAPDASH_PARSER_BUILD_TESTS` | OFF | Build unit tests (requires GTest) |

### Dependencies

- C++17 compiler (uses `<filesystem>`)
- GTest (libgtest-dev) — tests only

## Use as a Shared Library

This library is meant to be consumed, not vendored by copy-paste. Two ways:

### 1. Import — CMake FetchContent (pin a tag)

Pulls the library straight from GitHub at build time. Best for a project that
doesn't keep a local checkout.

```cmake
include(FetchContent)
FetchContent_Declare(cpapdash_parser
    GIT_REPOSITORY https://github.com/hms-homelab/hms-cpapdash-parser.git
    GIT_TAG v2026.1.2
)
FetchContent_MakeAvailable(cpapdash_parser)

# Optional manufacturer parsers:
# set(CPAPDASH_PARSER_WITH_LOWENSTEIN ON CACHE BOOL "" FORCE)

target_link_libraries(your_target PRIVATE cpapdash_parser)
```

### 2. Direct reference — local checkout with FetchContent fallback

How hms-cpap and CpapDash consume it: use a side-by-side checkout when present
(so local edits are picked up immediately), otherwise fall back to GitHub.

```cmake
set(PARSER_LOCAL "${CMAKE_CURRENT_SOURCE_DIR}/../hms-cpapdash-parser")
if(EXISTS "${PARSER_LOCAL}/CMakeLists.txt")
    add_subdirectory(${PARSER_LOCAL} cpapdash_parser)
else()
    include(FetchContent)
    FetchContent_Declare(cpapdash_parser
        GIT_REPOSITORY https://github.com/hms-homelab/hms-cpapdash-parser.git
        GIT_TAG v2026.1.2
    )
    FetchContent_MakeAvailable(cpapdash_parser)
endif()

target_link_libraries(your_target PRIVATE cpapdash_parser)
```

Headers are then available under the `cpapdash/parser/` prefix, e.g.
`#include <cpapdash/parser/ISessionParser.h>`.

## API Overview

| Type | Purpose |
|------|---------|
| `createParser(dir)` / `createParser(manufacturer)` | Factory — returns the right `ISessionParser` |
| `ISessionParser` | `parseSession(...)` / `parseSessionFromBuffers(...)` → `ParsedSession` |
| `ParsedSession` | Device info + `events`, `vitals`, breathing summaries, settings |
| `SessionMetrics` | Derived per-session metrics (AHI, pressures, durations) |
| `EventType`, `DeviceManufacturer` | Enums for events and source device |

## Related Projects

- [hms-cpap](https://github.com/hms-homelab/hms-cpap) — Home Assistant CPAP data collection service (consumer)
- [hms-cpapdash-charts](https://github.com/hms-homelab/hms-cpapdash-charts) — Angular charts for the parsed data
- [CpapDash](https://www.cpapdash.com/opensource) — CPAP therapy companion (web + app)

## Versioning

Calendar scheme `yyyy.n.patch`. See [CHANGELOG.md](CHANGELOG.md).
