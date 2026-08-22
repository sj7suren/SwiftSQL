# SwiftSQL

**简体中文** · [English](README.en.md)

[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2B%20x64-lightgrey.svg)](#构建)
[![C++](https://img.shields.io/badge/C%2B%2B-17-brightgreen.svg)](CMakeLists.txt)

轻量的多引擎数据库客户端。单个可执行文件，无运行时依赖，内置 AI 助手与跨引擎结构/数据同步。

> 命名说明：*Swift* 取"轻快"之意（呼应小体积、快启动），与 Apple 的 Swift 语言无关。

---

## 特性

**查询与编辑**
- SQL 编辑器：语法高亮、自动补全、格式化、查找替换、多标签页（基于 wxStyledTextCtrl）
- 可视化查询构建器，无需手写 SQL
- 结果网格：就地编辑、筛选、排序、列选择、单元格详情查看
- 脚本库与脚本文件执行（可选忽略错误、实时进度与失败原因）

**结构管理**
- 表设计器：字段 / 索引 / 外键 / DDL，改动生成方言感知的 ALTER 语句，预览确认后执行
- ER 图：自绘实体卡片、主外键标记、外键连线、缩放与自动布局
- 对象浏览：表、视图、存储过程、函数、用户；建库 / 建表 / 用户编辑对话框

**跨引擎同步**
- 结构对比与同步：跨不同引擎比对 schema，生成差异 SQL，预览后执行
- 数据同步：跨引擎表数据比对与同步，含类型映射
- 存储过程 / 函数比对（含规范化后比较）

**AI 助手**
- 对话式 SQL 生成与解释，携带当前库表结构作为上下文
- 编辑器内联补全建议（ghost text）
- Mermaid 图表生成与渲染
- 支持 Anthropic、OpenAI、DeepSeek、Gemini、智谱 GLM、Ollama（本地）；API Key 经 Windows DPAPI 加密存储

**运维**
- SSH 隧道（libssh2）
- 服务器进程 / 会话监控
- 定时自动化作业
- 数据导入 / 导出 / 转储脚本
- 崩溃时写 minidump，带符号可回溯定位

**其他**
- 中英双语界面，安装时选择的语言即应用启动语言
- 连接配置持久化，密码经 DPAPI 加密后落盘

---

## 支持的数据库

| 引擎 | 默认端口 | 接入方式 | 备注 |
|---|---|---|---|
| MySQL | 3306 | libmariadb（静态链接） | |
| MariaDB | 3306 | libmariadb（静态链接） | |
| PostgreSQL | 5432 | libpq（静态链接） | |
| OceanBase | 2881 | MySQL 协议，复用 libmariadb | |
| KingBase 人大金仓 | 54321 | PostgreSQL 协议，复用 libpq | |
| SQLite | — | 静态链接，文件型，无需服务端 | |
| SQL Server | 1433 | Windows ODBC | 需另装 ODBC Driver 17/18 for SQL Server |
| 达梦 DM | 5236 | Windows ODBC | 需另装 DM8 ODBC 驱动 |
| Oracle | 1521 | OCI，运行时动态加载 `oci.dll` | 不捆绑 Instant Client；用户自备后放到 exe 旁或在设置中指定路径 |

除 SQL Server / 达梦 / Oracle 走系统 ODBC 或用户自备客户端外，其余驱动均已静态链接进 exe，无需额外 DLL。

---

## 构建

### 前置条件

| 项 | 要求 |
|---|---|
| 操作系统 | Windows 10 x64 或更高 |
| 编译器 | MSVC（Visual Studio 2022 / 2026），静态 CRT `/MT` |
| 构建工具 | CMake ≥ 3.24、Ninja |
| 包管理 | [vcpkg](https://github.com/microsoft/vcpkg)，triplet `x64-windows-static` |

wxWidgets 3.2.9 由 CMake `FetchContent` 自动下载并静态编译，**无需手动安装**；首次构建耗时约 15–30 分钟，之后增量构建很快。

### 1. 安装 vcpkg 依赖

```bash
vcpkg install libmariadb:x64-windows-static libpq:x64-windows-static \
              sqlite3:x64-windows-static  libssh2:x64-windows-static
```

### 2.（可选）启用 Oracle 原生驱动

**不需要 Oracle 的话跳过这一步即可，项目正常构建。**

Oracle 的 `<oci.h>` 是编译期必需的厂商头文件，而其许可条款不允许随仓库再分发，因此仓库里没有它。构建系统会自动探测：**找不到 SDK 就跳过 OCI 驱动，其余引擎全部照常工作**，Oracle 在应用内报告为不可用。配置阶段会明确打印当前状态：

```
-- SwiftSQL: Oracle OCI driver ENABLED  (SDK: .../third_party/instantclient_23_5/sdk)
-- SwiftSQL: Oracle OCI driver DISABLED - no Instant Client SDK found. ...
```

要启用，从 Oracle 官网下载 **Instant Client SDK**（只需头文件，不需要运行库），解压到 `third_party/` 下让其自动识别：

```
third_party/instantclient_<版本>/sdk/include/oci.h
```

或指定任意路径：

```bash
cmake -S . -B build -DSWIFTSQL_OCI_SDK=<含 include/oci.h 的目录>
```

> 说明：SwiftSQL **不链接** `oci.lib`，所有 OCI 函数都在运行时经 `LoadLibrary`/`GetProcAddress` late-bind（见 `src/db/OciLoader.cpp`），所以启用 OCI 构建出的产物，在没有安装 Oracle 客户端的机器上照样能跑——用户届时自行放入 `oci.dll` 即可。SDK 仅提供编译期的类型与常量。

### 3. 配置并构建

```bash
cmake -S . -B build -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg根目录>/scripts/buildsystems/vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build -j
```

产物：`build/SwiftSQL.exe` —— 静态链接单文件，**不依赖任何 Visual C++ 运行库**。

### 运行测试

```bash
ctest --test-dir build --output-on-failure
```

共 55 个测试套件，覆盖 SQL 语句切分、方言 profile、schema 差异、跨引擎同步比对、AI provider 选择、连接克隆等纯逻辑路径。涉及真实数据库的用例通过 `SWIFTSQL_MYTEST_*` / `SWIFTSQL_PGTEST_*` 环境变量提供连接信息，未设置时自动跳过。

---

## 架构

依赖单向，无环：

```
swiftsql (exe)          仅 main.cpp，不随功能增长
    └── swiftsql::ui    wxWidgets 表现层
            ├── swiftsql::ai    AI provider 抽象与对话
            ├── swiftsql::net   SSH 隧道
            └── swiftsql::db    IConnection 接口 + 各引擎驱动（厂商库 PRIVATE，不外泄）
                    └── swiftsql::core   模型与持久化，不含 GUI、不含驱动
```

每层是独立静态库。`swiftsql::core` 不依赖任何 GUI 框架与数据库客户端库，可单独用于测试。

---

## 打包（Windows 安装器）

```bat
cd /d <repo-root>\release\win
build_installer.bat
```

需要 [Inno Setup 6](https://jrsoftware.org/isdl.php)。产物 `Output\SwiftSQL-Setup-<版本>.exe`，约 7 MB。

打包脚本在调用 Inno 之前会用 `dumpbin` 校验 exe 未引入动态 CRT——安装器**不捆绑** VC++ 运行库，若构建改用 `/MD` 会在此直接失败，而不是产出一个在干净机器上启动即崩的安装包。详见 [release/win/DEPENDENCIES.txt](release/win/DEPENDENCIES.txt)。

---

## 脚本化连接（CI / 自动化）

通过环境变量在启动时自动连接并执行查询，无需 UI 操作：

```bash
SWIFTSQL_AUTOCONNECT="mysql|127.0.0.1|3306|user|password|dbname"
SWIFTSQL_AUTORUN="SELECT * FROM users LIMIT 100;"
```

首字段取 `mysql` / `postgresql` / `oceanbase` 等引擎名。

---

## 已知限制

- **仅在 Windows 上构建与验证过。** 代码保留了跨平台结构（如 `core::Secret` 对非 Windows 平台有 `#else` 分支），但 macOS / Linux 构建尚未打通，也没有 CI 验证。非 Windows 分支下密码仅做 base64 编码，**不构成保护**，接入系统钥匙串前不要在这些平台存放真实凭据。
- **Oracle 原生 OCI 驱动需自备 Instant Client SDK 头文件**才会编入（见上）。未提供时该驱动自动排除，不影响其余引擎构建。
- 源码注释中残留部分指向 `docs/` 的引用，该目录未随本仓库发布。
- 尚无 CI、CONTRIBUTING 与 issue 模板。

---

## 支持项目 ☕

SwiftSQL 开源版完全免费，没有功能阉割、使用期限或数量限制——所有能力对所有人开放。（商业授权买的是**许可条款**，不是额外功能；两者功能完全一致。）

如果它为你省下了时间，欢迎请作者喝杯咖啡 —— 请打开**支付宝**，用「扫一扫」扫描下方二维码：

<img src="src/win/assets/alipay_qr.png" width="200" alt="支付宝收款码 — 请使用支付宝扫一扫">

<sub>支付宝收款码（微信扫码无法识别）</sub>

应用内「关于 → ☕ 捐赠支持」可以扫到同一个码。

捐赠完全自愿：**它不解锁任何功能，也不构成服务承诺或商业授权**。本项目依 GPLv3 授权，捐赠不改变任何许可条款。

---

## 许可证

SwiftSQL 是自由软件，依据 **GNU General Public License v3.0 或更新版本**授权发布。

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

许可证全文见 [LICENSE](LICENSE)，每个源文件头部带有 `SPDX-License-Identifier: GPL-3.0-or-later` 标识。

**这对你意味着什么：**

- 你可以自由地运行、研究、修改和分发本软件，**包括商业用途**。
- 但只要你**分发**（含分发修改版或包含本代码的衍生作品），就必须以同样的 GPLv3 条款开放对应的完整源代码，并保留版权与许可声明。
- 这一要求对**内部自用**不适用——你自己或公司内部使用、修改而不对外分发，无需开源。
- 本软件按"原样"提供，不附带任何明示或暗示的担保。

> 若你需要在专有软件中集成本项目、而不希望受 GPL 的开源传染性约束，可另行获取商业授权 —— 见下方[企业版与商业授权](#企业版与商业授权)。

### 第三方组件

各依赖遵循各自的原始许可证，均与 GPLv3 兼容：

| 组件 | 许可证 |
|---|---|
| wxWidgets | wxWindows Library Licence 3.1（LGPL 加二进制分发例外） |
| OpenSSL 3.x | Apache-2.0 |
| MariaDB Connector/C | LGPL-2.1 |
| PostgreSQL libpq | PostgreSQL License |
| SQLite | Public Domain |
| libssh2 | BSD-3-Clause |
| zlib / lz4 | zlib / BSD-2-Clause |

> 注意：OpenSSL 3.x 采用 Apache-2.0，它与 **GPLv2 不兼容**、与 GPLv3 兼容。这是本项目选择 GPLv3 而非 GPLv2 的原因之一。
>
> Oracle Instant Client SDK 为专有组件，不随本仓库或安装包分发，仅在用户自备时于本地编译期使用。

完整依赖审计见 [release/win/DEPENDENCIES.txt](release/win/DEPENDENCIES.txt)。

---

## 企业版与商业授权

开源版本（GPLv3）功能完整、无任何阉割，绝大多数使用场景直接用它即可。

但 GPLv3 的开源传染性对部分企业不适用 —— 典型情形是：

- 需要把 SwiftSQL 集成进**闭源**的商业产品并对外分发
- 内部合规政策不接受 copyleft 许可证
- 需要定制开发、私有部署适配或国产数据库深度支持
- 需要技术支持、SLA 或问题优先响应

以上情形可**单独获取商业授权**，不受 GPLv3 条款约束。

### 联系方式

📧 **sj7suren@hotmail.com**

商业授权咨询、定制开发、技术支持，请发送邮件说明你的使用场景与规模。

### 建议与反馈

同一邮箱也欢迎：

- 功能建议与需求反馈
- 使用中遇到的问题（附复现步骤更佳）
- 对某个数据库引擎的适配诉求

> 一般性 bug 与功能请求也可以直接提 [GitHub Issue](https://github.com/sj7suren/SwiftSQL/issues)，公开讨论便于其他用户搜索到相同问题。涉及商业合作、私有信息或不便公开的内容再走邮件。
