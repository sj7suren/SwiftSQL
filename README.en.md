# SwiftSQL

[简体中文](README.md) · **English**

[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2B%20x64-lightgrey.svg)](#building)
[![C++](https://img.shields.io/badge/C%2B%2B-17-brightgreen.svg)](CMakeLists.txt)

A lightweight multi-engine database client. Single self-contained executable, no runtime dependencies, with a built-in AI assistant and cross-engine schema/data synchronisation.

> On the name: *Swift* here means "quick" — small binary, fast startup. It has nothing to do with Apple's Swift language.

---

## Features

**Query and editing**
- SQL editor: syntax highlighting, completion, formatting, find and replace, multiple tabs (built on wxStyledTextCtrl)
- Visual query builder, no SQL required
- Result grid: in-place editing, filtering, sorting, column selection, cell detail viewer
- Script library and script-file execution (optional continue-on-error, live progress and failure reasons)

**Schema management**
- Table designer: columns / indexes / foreign keys / DDL. Edits generate dialect-aware ALTER statements, previewed before execution
- ER diagrams: self-drawn entity cards, PK/FK markers, relationship lines, zoom and auto-layout
- Object browser: tables, views, stored procedures, functions, users; dialogs for creating databases, tables and users

**Cross-engine synchronisation**
- Schema compare and sync: diff schemas across different engines, generate the delta SQL, preview, then apply
- Data sync: compare and synchronise table data across engines, with type mapping
- Stored procedure / function comparison, including normalised comparison

**AI assistant**
- Conversational SQL generation and explanation, with the current schema supplied as context
- Inline completion suggestions in the editor (ghost text)
- Mermaid diagram generation and rendering
- Works with Anthropic, OpenAI, DeepSeek, Gemini, Zhipu GLM and Ollama (local); API keys are encrypted at rest with Windows DPAPI

**Operations**
- SSH tunnelling (libssh2)
- Server process / session monitoring
- Scheduled automation jobs
- Data import / export / dump scripts
- Minidump written on a hard crash, with symbols kept so the dump is actually resolvable

**Other**
- Bilingual UI (English / Simplified Chinese); the language chosen during installation is the one the app starts in
- Persisted connection profiles, with passwords encrypted via DPAPI before they touch disk

---

## Supported databases

| Engine | Default port | How it connects | Notes |
|---|---|---|---|
| MySQL | 3306 | libmariadb (statically linked) | |
| MariaDB | 3306 | libmariadb (statically linked) | |
| PostgreSQL | 5432 | libpq (statically linked) | |
| OceanBase | 2881 | MySQL wire protocol, reuses libmariadb | |
| KingBase | 54321 | PostgreSQL wire protocol, reuses libpq | |
| SQLite | — | Statically linked, file-based, no server | |
| SQL Server | 1433 | Windows ODBC | Requires ODBC Driver 17/18 for SQL Server |
| DM (达梦) | 5236 | Windows ODBC | Requires the DM8 ODBC driver |
| Oracle | 1521 | OCI, `oci.dll` loaded at runtime | Instant Client is not bundled; drop your own next to the exe or set the path in Preferences |

Every driver except SQL Server, DM and Oracle — which go through system ODBC or a client you supply — is statically linked into the exe. No extra DLLs are needed.

---

## Building

### Prerequisites

| Item | Requirement |
|---|---|
| OS | Windows 10 x64 or later |
| Compiler | MSVC (Visual Studio 2022 / 2026), static CRT `/MT` |
| Build tools | CMake ≥ 3.24, Ninja |
| Packages | [vcpkg](https://github.com/microsoft/vcpkg), triplet `x64-windows-static` |

wxWidgets 3.2.9 is downloaded and built statically by CMake `FetchContent` — **no manual installation needed**. The first build takes roughly 15–30 minutes; incremental builds after that are fast.

### 1. Install the vcpkg dependencies

```bash
vcpkg install libmariadb:x64-windows-static libpq:x64-windows-static \
              sqlite3:x64-windows-static  libssh2:x64-windows-static
```

### 2. (Optional) Enable the native Oracle driver

**Skip this if you do not need Oracle — the project builds fine without it.**

Oracle's `<oci.h>` is a vendor header required at compile time, and its licence does not permit redistribution, so it is not in this repository. The build detects it: **if no SDK is found, the OCI driver is left out and every other engine works normally**, with Oracle reporting itself unavailable in the app. Configure prints the current state explicitly:

```
-- SwiftSQL: Oracle OCI driver ENABLED  (SDK: .../third_party/instantclient_23_5/sdk)
-- SwiftSQL: Oracle OCI driver DISABLED - no Instant Client SDK found. ...
```

To enable it, download the **Instant Client SDK** from Oracle (headers only, the runtime is not needed) and unpack it under `third_party/` where it is picked up automatically:

```
third_party/instantclient_<version>/sdk/include/oci.h
```

Or point at any location:

```bash
cmake -S . -B build -DSWIFTSQL_OCI_SDK=<directory containing include/oci.h>
```

> Note: SwiftSQL does **not** link `oci.lib`. Every OCI function is late-bound from `oci.dll` at runtime via `LoadLibrary`/`GetProcAddress` (see `src/db/OciLoader.cpp`), so a build with OCI enabled still runs on machines with no Oracle client installed — the user drops in `oci.dll` when they need it. The SDK only supplies compile-time types and constants.

### 3. Configure and build

```bash
cmake -S . -B build -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg root>/scripts/buildsystems/vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build -j
```

Output: `build/SwiftSQL.exe` — a statically linked single file that **needs no Visual C++ redistributable**.

### Running the tests

```bash
ctest --test-dir build --output-on-failure
```

55 suites covering pure-logic paths: SQL statement splitting, dialect profiles, schema diffing, cross-engine sync comparison, AI provider selection, connection cloning and more. Cases that need a real database read their connection details from `SWIFTSQL_MYTEST_*` / `SWIFTSQL_PGTEST_*` environment variables and skip themselves when those are unset.

---

## Architecture

Dependencies point one way only, no cycles:

```
swiftsql (exe)          main.cpp only — does not grow with features
    └── swiftsql::ui    wxWidgets presentation layer
            ├── swiftsql::ai    AI provider abstraction and conversation
            ├── swiftsql::net   SSH tunnelling
            └── swiftsql::db    IConnection + per-engine drivers (vendor libs PRIVATE, never leaked)
                    └── swiftsql::core   model and persistence; no GUI, no drivers
```

Each layer is its own static library. `swiftsql::core` depends on neither a GUI framework nor any database client library, so it can be tested on its own.

---

## Packaging (Windows installer)

```bat
cd /d <repo-root>\release\win
build_installer.bat
```

Requires [Inno Setup 6](https://jrsoftware.org/isdl.php). Produces `Output\SwiftSQL-Setup-<version>.exe`, about 7 MB.

Before invoking Inno, the script runs `dumpbin` to verify the exe has not picked up a dynamic CRT import. The installer bundles **no** VC++ redistributable, so a build that switched to `/MD` fails here rather than producing a package that dies on a clean machine. See [release/win/DEPENDENCIES.txt](release/win/DEPENDENCIES.txt).

---

## Scripted connections (CI / automation)

Connect and run a query at startup through environment variables, with no UI interaction:

```bash
SWIFTSQL_AUTOCONNECT="mysql|127.0.0.1|3306|user|password|dbname"
SWIFTSQL_AUTORUN="SELECT * FROM users LIMIT 100;"
```

The first field is the engine name: `mysql`, `postgresql`, `oceanbase`, and so on.

---

## Known limitations

- **Only built and verified on Windows.** The code keeps a cross-platform structure (`core::Secret` has a non-Windows `#else` branch, for instance), but the macOS / Linux builds are not wired up and there is no CI. On the non-Windows branch passwords are only base64-encoded, which is **not protection** — do not store real credentials there until a platform keychain is hooked up.
- **The native Oracle OCI driver needs Instant Client SDK headers** to be compiled in (see above). Without them it is simply excluded; every other engine still builds.
- Some source comments still reference a `docs/` directory that is not published with this repository.
- No CI, CONTRIBUTING guide or issue templates yet.

---

## Support the project ☕

The open-source build is entirely free — no crippled features, no time limit, no seat count. Everything is available to everyone. (A commercial licence buys different **licensing terms**, not extra functionality; the two are identical in features.)

If it saved you time, feel free to buy the author a coffee — open **Alipay** and use "Scan" on the QR code below:

<img src="src/win/assets/alipay_qr.png" width="200" alt="Alipay QR code — scan with Alipay">

<sub>Alipay payment code (WeChat cannot read it)</sub>

The same code is reachable in the app under **About → ☕ Donate**.

Donations are entirely voluntary: **they unlock nothing, and constitute neither a support commitment nor a commercial licence.** This project is licensed under the GPLv3 and a donation changes none of its terms.

---

## Licence

SwiftSQL is free software, licensed under the **GNU General Public License v3.0 or later**.

```
Copyright (C) 2026 SwiftSQL Contributors

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.
```

The full text is in [LICENSE](LICENSE), and every source file carries an `SPDX-License-Identifier: GPL-3.0-or-later` header.

**Use and resale:**

- **Free of charge for individuals and companies using it themselves** — production use and modification included, with no fee and no additional licence required.
- **To resell this project to other individuals or organisations**, please obtain the author's **written permission** in advance, or contact the author to purchase an **enterprise licence**: 📧 **sj7suren@hotmail.com**
- If you distribute this software or a modified version, the GPLv3 applies: include a copy of the licence and supply the recipient with the complete corresponding source.
- The software is provided "as is", without warranty of any kind, express or implied.

> See [TRADEMARK.md](TRADEMARK.md) for where resale, branding and trademarks stand exactly, and [Enterprise and commercial licensing](#enterprise-and-commercial-licensing) below for commercial terms.

### Third-party components

Each dependency keeps its own licence; all are GPLv3-compatible:

| Component | Licence |
|---|---|
| wxWidgets | wxWindows Library Licence 3.1 (LGPL with a binary-distribution exception) |
| OpenSSL 3.x | Apache-2.0 |
| MariaDB Connector/C | LGPL-2.1 |
| PostgreSQL libpq | PostgreSQL License |
| SQLite | Public Domain |
| libssh2 | BSD-3-Clause |
| zlib / lz4 | zlib / BSD-2-Clause |

> Note: OpenSSL 3.x is Apache-2.0, which is **incompatible with GPLv2** but compatible with GPLv3. That is one reason this project is GPLv3 rather than GPLv2.
>
> The Oracle Instant Client SDK is proprietary and is distributed neither with this repository nor with the installer. It is used only at compile time, on the machine of whoever chooses to build the optional Oracle driver.

The full dependency audit is in [release/win/DEPENDENCIES.txt](release/win/DEPENDENCIES.txt).

---

## Enterprise and commercial licensing

The open-source build (GPLv3) is feature-complete and holds nothing back. For the great majority of uses it is all you need.

The GPLv3's copyleft obligation does not suit every organisation, however — typically when you:

- need to embed SwiftSQL in a **closed-source** commercial product and distribute it
- work under an internal compliance policy that does not accept copyleft licences
- need custom development, private deployment support, or deeper support for a specific Chinese database engine
- need technical support, an SLA, or prioritised issue response

A **separate commercial licence** is available for these cases, free of the GPLv3 obligations.

### Contact

📧 **sj7suren@hotmail.com**

For commercial licensing, custom development or technical support, please email with a description of your use case and scale.

### Suggestions and feedback

The same address welcomes:

- feature suggestions and requests
- problems you hit while using it (reproduction steps are especially helpful)
- requests for better support of a particular database engine

> Ordinary bugs and feature requests are also welcome as [GitHub Issues](https://github.com/sj7suren/SwiftSQL/issues) — discussing them in the open lets other users find the same problem. Use email for commercial matters, private information, or anything you would rather not post publicly.
