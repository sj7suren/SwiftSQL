# SwiftSQL

轻量、跨平台的 SQL / 数据库客户端工具。

## 技术栈

- **语言**：C++（C++17 及以上）
- **GUI 框架**：[wxWidgets](https://www.wxwidgets.org/) —— 使用各平台原生控件，外观贴合系统
- **构建系统**：CMake
- **目标平台**：Windows、macOS、Ubuntu/Linux

## 设计目标

- **小体积、快启动**：Windows/macOS 静态链接为单可执行文件；Linux 通过 AppImage 达成"单文件双击即跑"
- **原生外观**：Windows 用 Win32 控件、macOS 用 Cocoa、Linux 用 GTK3
- **清晰分层**：GUI 层 / 业务逻辑层 / 数据访问层解耦，业务层不依赖具体 GUI 框架

> 命名说明：*Swift* 取"轻快"之意（呼应小体积、快启动），并非指 Apple 的 Swift 语言。

## 目录结构（规划）

```
SwiftSQL/
├── CMakeLists.txt        # 顶层构建配置
├── src/
│   ├── ui/               # wxWidgets 界面层
│   ├── core/             # 业务逻辑（不依赖 GUI 框架）
│   └── db/               # 数据库连接与访问层
├── assets/               # 图标、资源
└── packaging/            # 各平台打包脚本（Win exe / Mac .app / Linux AppImage）
```

## 支持的数据库

| 数据库 | 默认端口 | 驱动(规划) |
|---|---|---|
| MySQL | 3306 | libmysqlclient |
| PostgreSQL | 5432 | libpq |
| OceanBase | 2881 | MySQL 协议兼容(libmysqlclient) |

每种数据库在界面中有专属品牌图标(纯 C++ `wxGraphicsContext` 矢量绘制,无外部图片依赖)。

## 构建

依赖通过 CMake FetchContent 自动拉取(wxWidgets 3.2.9 静态构建),无需手动安装。

```bash
# Windows(VS 2022/2026 开发者环境)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 产物:build/SwiftSQL.exe(静态链接单文件)

# macOS / Linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

> 首次构建会从源码编译 wxWidgets,耗时较长(15–30 分钟);之后增量编译很快。

## 脚本化连接(CI / 自动化)

支持通过环境变量在启动时自动连接并执行查询,无需 UI 操作:

```bash
SWIFTSQL_AUTOCONNECT="mysql|127.0.0.1|3306|user|password|dbname"
SWIFTSQL_AUTORUN="SELECT * FROM users LIMIT 100;"
```

`type` 取 `mysql` / `postgresql` / `oceanbase`。

## 开发状态

✅ **M1 界面**:主窗口按 `docs/UI` 设计稿实现(侧栏树 / SQL 语法高亮编辑器 / 结果网格 / 新建连接对话框)。
✅ **M2 真实驱动**:MySQL / PostgreSQL 已对真实数据库(MySQL 8.0、PostgreSQL 15)端到端验证——真连接、真库表树、后台线程真查询、真错误提示。OceanBase 复用 MySQL 协议驱动(与 MySQL 同一代码路径,已随 MySQL 实测间接验证协议;OceanBase 实例真机验证待补)。
✅ **M3 结构视图**:表设计视图(字段/索引/外键/DDL 页签,真实 schema 内省)+ ER 图视图(自绘实体卡片、PK/FK 标记、外键连线、缩放/自动布局),三视图切换(SQL 编辑器/表设计/ER 图)。已对 MySQL(表设计+ER)与 PostgreSQL(表设计,内省 SQL 独立实现)真机验证渲染。
✅ **M4 持久化 + 结构编辑**:连接配置持久化(存到用户目录 ini,密码经 **Windows DPAPI 加密**,连接对话框带已保存列表);表设计可编辑(增删改字段 → 生成方言感知的 ALTER DDL → 预览确认 → 执行 → 重载)。已对 MySQL 与 PostgreSQL 真机验证:加列 DDL 生成正确、DB 实际变更、UI 重载反映。
⏭️ **M5**:密码在非 Windows 平台接 OS keychain(macOS Keychain / libsecret);可配置 SSL 模式(CA 验证);OceanBase 真机验证;macOS/Ubuntu 构建。

## 许可证

SwiftSQL 是自由开源软件,依据 **Apache License 2.0** 授权发布。

```
Copyright (c) 2026 SwiftSQL Contributors
```

许可证全文见 [LICENSE](LICENSE)。你可以自由使用、修改、分发本软件(含商业用途),
并获得贡献者的专利许可;再分发时需随附许可证副本、保留原有版权与归属声明,
并在被修改的文件中标注你所做的更改。完整条款以 LICENSE 中的英文原文为准。

本软件按"原样"提供,不附带任何明示或暗示的担保。

第三方组件(wxWidgets、各数据库客户端库等)遵循各自的原始许可证,不受本许可影响,
详见 [release/win/DEPENDENCIES.txt](release/win/DEPENDENCIES.txt)。
