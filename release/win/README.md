# SwiftSQL — Windows 发布与打包 (release/win)

用 **Inno Setup 6** 打包 SwiftSQL 的 Windows 安装程序。

## 目录结构

```
release/win/
├─ SwiftSQL.iss          Inno Setup 打包脚本（双语、语言联动、许可页、环境检查、卸载）
├─ eula_zh.txt           中文开源许可说明（UTF-8 BOM；英文页直接引用根目录 LICENSE）
├─ build_installer.bat   一键调用 ISCC.exe 生成安装包
├─ DEPENDENCIES.txt      依赖审计
├─ SwiftSQL.ico          应用/安装器图标（= 官方 src/win/assets/swiftsql.ico）
├─ dist/                 打包载荷（安装器的 [Files] 源）
│  ├─ SwiftSQL.exe       主程序（x64、静态 CRT、单文件）
│  └─ lang/              可选的外置语言覆盖包目录
├─ redist/               VC_redist.x64.exe（VC++ 2015-2022，缺失时自动装）
└─ Output/               ISCC 生成的 SwiftSQL-Setup-1.1.21.exe 落地于此
```

## 前置条件

1. **安装 Inno Setup 6**：<https://jrsoftware.org/isdl.php>（本机当前未安装）。
2. **ChineseSimplified.isl**：简体中文安装界面需要它，位于 Inno 的 `Languages\` 目录。
   Inno Setup 6 的翻译包通常已含；若缺失，从
   <https://github.com/jrsoftware/issrc/tree/main/Files/Languages> 下载放入。
3. **dist\SwiftSQL.exe** 为最新的 Release 构建（见下"刷新 exe"）。

## 生成安装包

```bat
cd /d <repo-root>\release\win
build_installer.bat
```
产物：`Output\SwiftSQL-Setup-1.1.21.exe`。

## 关键设计

### 语言联动（装时选什么，装后就是什么）
安装器首屏让用户选语言（**默认英文**，可选简体中文）。选定后，`[INI]` 段会把该语言写入
SwiftSQL 自己的配置文件：

```
%APPDATA%\SwiftSQL\settings.ini   →   [general] language = en | zh
```

app 启动时正是从这个键读取界面语言，因此**安装选英文 → SwiftSQL 英文启动；选中文 → 中文启动**，
无需改动任何程序代码。只写入 `language` 这一个键，其余既有设置保持不变。

> 采用 **per-user 安装**（`PrivilegesRequired=lowest`，装到 `%LocalAppData%\Programs\SwiftSQL`）。
> 这样 `{userappdata}` 一定指向真实用户，语言写入不会因管理员提权而落错账户；且 SwiftSQL 是
> 零系统足迹的静态单 exe，本就无需管理员权限。

### EULA（软件使用协议）
安装向导会显示最终用户许可协议，用户须选择"我接受"才能继续。`[Languages]` 为中英各挂一份：
选英文显示 `eula_en.txt`，选中文显示 `eula_zh.txt`。

### 环境 / 前置检查
- **架构**：`ArchitecturesAllowed=x64compatible` —— 仅 64 位 Windows（含 ARM64 x64 模拟）可装，
  32 位系统会被拦下并给出提示。（SwiftSQL 是 x64-only 产品。）
- **系统版本**：`MinVersion=6.1sp1`（Windows 7 SP1 起）。如需只允许 Win10+，改为 `10.0`。

### 卸载
Inno 自动生成卸载器，并注册到"应用和功能 / 控制面板"；开始菜单也放了"卸载 SwiftSQL"快捷方式。
卸载时：
- 自动删除安装的程序文件（exe、lang 覆盖目录）。
- **弹出双语询问**（默认"否"）：是否同时删除用户数据目录 `%APPDATA%\SwiftSQL\`
  ——设置、**已保存的连接与密码**、脚本、收藏、自动化作业、崩溃日志。默认保留，方便重装续用。

### 关于 C++ 运行库
- **SwiftSQL.exe 本身不需要 VC++ 运行库**：用**静态 CRT（`/MT`）**构建，`vcruntime140.dll` /
  `msvcp140.dll` 已焊进 exe（`dumpbin /dependents` 只列 Windows 系统 DLL），双击即跑。
- **安装包仍随包分发 VC++ 2015–2022 x64 运行库**（`redist\VC_redist.x64.exe`）作为**防御性保险**：
  安装时 `[Code] VCRedistNeeded` 检测系统是否已装（先查注册表
  `HKLM\...\VC\Runtimes\x64\Installed`，回退查 `vcruntime140.dll`）——**已装则完全跳过、静默无感；
  仅当缺失时**才 `/install /quiet /norestart` 静默安装。绝大多数现代 Windows 已自带，通常不触发。

> 说明：本安装采用 per-user（无需管理员）。当目标机器**确实缺失**运行库、需要安装时，redist 会
> 自提权弹一次 UAC（`/quiet` 静默完成）；系统已有运行库时不弹、不装。若你不想要这份保险，删掉
> `redist\VC_redist.x64.exe` 与 `[Run]` 的 vcredist 行即可（本静态 exe 本就不依赖它）。

> Oracle 未随包分发（否则会拖入 `MSVCR80.dll` = VC++ 2005 依赖）。app 仍支持 Oracle / 达梦：
> 需要时用户把 **64 位 `oci.dll`** 放到 SwiftSQL.exe 同目录，或在应用内设置 OCI 路径即可。

## 刷新 exe（发布前）
`dist\SwiftSQL.exe` 应来自干净的 Release 构建。构建后：

```bat
copy /Y ..\..\build\SwiftSQL.exe dist\SwiftSQL.exe
```
