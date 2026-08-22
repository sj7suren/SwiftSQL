# SwiftSQL

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
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

### 2. 准备 Oracle OCI 头文件 ⚠️

**当前这一步是必需的，即使你不打算使用 Oracle。** `src/db/OciLoader.h` 无条件 `#include <oci.h>`，缺少它会导致编译失败。

从 Oracle 官网下载 **Instant Client SDK**（仅需头文件，不需要运行库），解压到：

```
third_party/instantclient_23_5/sdk/include/oci.h
```

该目录被 `.gitignore` 排除——Oracle 的许可条款不允许随仓库再分发其 SDK。

> 说明：SwiftSQL **不链接** `oci.lib`，所有 OCI 函数都在运行时经 `LoadLibrary`/`GetProcAddress` late-bind（见 `src/db/OciLoader.cpp`），所以构建产物在没有 Oracle 客户端的机器上照样能跑。这里需要的仅仅是编译期的类型与常量定义。
>
> 让 Oracle 变成可选的 CMake 开关是明确的改进方向，见[已知限制](#已知限制)。

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

共 54 个测试套件，覆盖 SQL 语句切分、方言 profile、schema 差异、跨引擎同步比对、AI provider 选择、连接克隆等纯逻辑路径。涉及真实数据库的用例通过 `SWIFTSQL_MYTEST_*` / `SWIFTSQL_PGTEST_*` 环境变量提供连接信息，未设置时自动跳过。

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
- **Oracle SDK 头文件目前是硬性构建依赖**，即使不用 Oracle 也必须准备（见上）。应改为可选的 CMake 开关。
- **源码尚未逐文件添加 Apache 许可证头。** 许可证以仓库根目录的 [LICENSE](LICENSE) 为准。
- 源码注释中残留部分指向 `docs/` 的引用，该目录未随本仓库发布。
- 尚无 CI、CONTRIBUTING 与 issue 模板。

---

## 许可证

SwiftSQL 是自由开源软件，依据 **Apache License 2.0** 授权发布。

```
Copyright 2026 SwiftSQL Contributors
```

许可证全文见 [LICENSE](LICENSE)。你可以自由使用、修改、分发本软件（含商业用途），并获得贡献者的专利许可；再分发时需随附许可证副本、保留原有版权与归属声明，并在被修改的文件中标注你所做的更改。完整条款以 LICENSE 的英文原文为准。

本软件按"原样"提供，不附带任何明示或暗示的担保。

第三方组件（wxWidgets、各数据库客户端库、libssh2、OpenSSL 等）遵循各自的原始许可证，不受本许可影响，详见 [release/win/DEPENDENCIES.txt](release/win/DEPENDENCIES.txt)。
