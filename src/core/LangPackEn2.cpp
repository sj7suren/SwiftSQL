// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// LangPackEn2.cpp — English pack, part 2 (spill-over data table). Kept separate
// from LangPackEn.cpp purely to keep each translation-unit under the 1000-line
// file limit; Lang.cpp merges kLangPackEn + kLangPackEn2 into the single "en"
// catalog (part 2 wins on any duplicate key). Add new English strings here.
//
// Current contents: multi-dialect user-management strings (the model-driven
// UserEditDialog for PostgreSQL / SQL Server / Oracle・达梦).
#include <cwchar>

namespace core {

extern const wchar_t* const kLangPackEn2[] = {
    // ---- user management: shared columns / prompts ----
    L"用户 (User@Host)", L"User (User@Host)",
    L"用户", L"User",
    L"认证插件", L"Auth Plugin",
    L"角色属性", L"Role Attributes",
    L"系统权限", L"System Privileges",
    L"服务器角色", L"Server Roles",
    L"类型", L"Type",
    L"账户状态", L"Account Status",
    L"新用户名:", L"New user name:",

    // ---- PostgreSQL ----
    L"提示: PostgreSQL 用户即\"角色\"(ROLE);勾选 LOGIN 才可作为登录账户。编辑已有角色时不可改名(重命名为独立操作)。",
        L"Tip: a PostgreSQL user IS a ROLE; tick LOGIN to make it a login account. Name is read-only when editing (rename is a separate op).",
    L"角色属性 — 作用于整个数据库集群:",
        L"Role attributes — apply across the whole database cluster:",
    L"该库权限 (ON DATABASE)", L"Privileges on this database (ON DATABASE)",
    L"隶属成员 — 勾选授予本角色的其他角色 (GRANT role TO …):",
        L"Member Of — check roles granted to this role (GRANT role TO …):",

    // ---- SQL Server ----
    L"提示: 此处管理服务器登录 (LOGIN) 及其固定服务器角色;数据库级用户/角色映射暂不在此维护。编辑已有登录时不可改名。",
        L"Tip: this manages server logins and their fixed server roles; database-level user/role mapping is not maintained here. Name is read-only when editing.",
    L"固定服务器角色 — 勾选本登录所属的角色:",
        L"Fixed server roles — check the roles this login belongs to:",

    // ---- Oracle / 达梦 ----
    L"提示: 需 DBA 权限;标识符默认折叠为大写。删除拥有对象的用户需手动 DROP USER … CASCADE。表空间配额/PROFILE/对象权限暂不在此维护。",
        L"Tip: DBA privilege required; identifiers fold to uppercase by default. A user owning objects needs a manual DROP USER … CASCADE. Tablespace quota / PROFILE / object privileges are not maintained here.",
    L"系统权限 (System Privileges) — 勾选授予本用户的系统权限(目录外已有系统权限保持不变):",
        L"System Privileges — check the system privileges granted to this user (privileges outside this list are left unchanged):",
    L"角色 — 勾选授予本用户的角色 (GRANT role TO …):",
        L"Roles — check the roles granted to this user (GRANT role TO …):",

    // ---- automation jobs ----
    L"新建自动化作业", L"New Automation Job",
    L"修改自动化作业", L"Edit Automation Job",
    L"自动化作业", L"Automation Job",
    L"作业名称", L"Job Name",
    L"作业类型", L"Job Type",
    L"同步表结构", L"Sync Schema",
    L"同步数据", L"Sync Data",
    L"同步表结构 + 数据", L"Sync Schema + Data",
    L"源连接", L"Source Connection",
    L"源数据库", L"Source Database",
    L"目标连接", L"Target Connection",
    L"目标数据库", L"Target Database",
    L"在事务中应用变更（失败回滚）", L"Apply changes in a transaction (rollback on failure)",
    L"删除目标端多余的表", L"Drop tables that exist only on the target",
    L"导出内容包含数据（否则仅结构）", L"Include data in export (otherwise schema only)",
    L"计划（周期/定时）在列表中通过「设定自动计划」配置。",
        L"Scheduling (periodic / daily) is configured via “Set Schedule” in the list.",
    L"请填写作业名称。", L"Please enter a job name.",
    L"请选择源连接与源数据库。", L"Please choose a source connection and database.",
    L"同步作业需要选择目标连接与目标数据库。",
        L"A sync job requires a target connection and database.",
    L"源与目标不能是同一个数据库。", L"Source and target cannot be the same database.",
    // ---- schedule dialog ----
    L"设定自动计划", L"Set Schedule",
    L"自动计划", L"Schedule",
    L"应用运行时按此计划自动执行本作业（关闭应用则不运行）。",
        L"While the app is running the job runs on this schedule (it does not run when the app is closed).",
    L"周期性 — 每", L"Periodic — every",
    L"定时 — 每天", L"Daily — at",
    L"分钟", L"Minutes",
    L"小时", L"Hours",
    // ---- automation list / runner ----
    L"上次运行", L"Last Run",
    L"计划（下次/周期）", L"Schedule (next / period)",
    L"从未运行", L"Never run",
    L"✓ 成功", L"✓ Success",
    L"✗ 失败", L"✗ Failed",
    L"未找到连接: %s", L"Connection not found: %s",
    L"源连接无法连接: %s", L"Cannot connect the source: %s",
    L"目标连接无法连接: %s", L"Cannot connect the target: %s",
    L"未设定", L"Not scheduled",
    L"下次 ", L"Next ",
    L"每 %d 小时", L"Every %d h",
    L"每 %d 分钟", L"Every %d min",
    L"每天 %02d:%02d", L"Daily %02d:%02d",
    L"开始", L"Start",
    L"删除自动计划", L"Remove Schedule",
    L"作业已保存（更改即时生效）", L"Jobs saved (changes take effect immediately)",
    L"作业「%s」已存在，覆盖它吗?", L"Job “%s” already exists — overwrite it?",
    L"已创建作业: %s", L"Job created: %s",
    L"已保存作业: %s", L"Job saved: %s",
    L"确定删除作业「%s」吗?此操作不可撤销。",
        L"Delete job “%s”? This cannot be undone.",
    L"删除作业", L"Delete Job",
    L"已删除作业: %s", L"Job deleted: %s",
    L"作业正在运行: %s", L"Job already running: %s",
    L"正在运行作业: %s…", L"Running job: %s…",
    L"作业完成: ", L"Job finished: ",
    L"作业失败: ", L"Job failed: ",
    L"自动化作业 — ", L"Automation Job — ",
    L"已设定自动计划: %s", L"Schedule set: %s",
    L"已删除自动计划: %s", L"Schedule removed: %s",
    L"源连接未连接: %s", L"Source connection not connected: %s",
    L"目标连接未连接: %s", L"Target connection not connected: %s",
    L"已导出至 ", L"Exported to ",
    L"比对失败: ", L"Comparison failed: ",
    L"目标已与源一致，无需同步", L"Target already matches source — nothing to sync",
    L"执行失败: ", L"Execution failed: ",
    L"同步完成，共 %zu 张表", L"Sync complete — %zu tables",
    L"无法写入文件: ", L"Cannot write file: ",
    L"写入文件失败: ", L"Failed to write file: ",

    // ---- AI (placeholder tab + generate button) ----
    L"AI", L"AI",
    L"AI 生成 SQL", L"AI Gen SQL",
    L"AI 功能建设中", L"AI Features Coming Soon",
    L"敬请期待——AI 生成/优化 SQL、智能诊断即将上线",
        L"Stay tuned — AI-powered SQL generation / optimization and smart diagnostics are on the way",

    // ---- top menu bindings (open/save SQL file, run, import/export/backup) ----
    L"运行\tF5", L"Run\tF5",
    L"打开 SQL 文件", L"Open SQL File",
    L"保存 SQL 文件", L"Save SQL File",
    L"无法读取文件: ", L"Cannot read file: ",
    L"已打开: ", L"Opened: ",
    L"已保存: ", L"Saved: ",
    L"没有可保存的 SQL 编辑器", L"No SQL editor to save",
    L"请先连接并打开数据库", L"Connect to a database first",

    // ---- splash / launch screen ----
    L"正在初始化连接池…", L"Initializing connection pool…",

    // ---- About / Donate dialog ----
    L"关于 SwiftSQL", L"About SwiftSQL",
    L"确定", L"OK",
    L"☕ 捐赠支持", L"☕ Donate",
    L"捐赠支持", L"Donate",
    L"收款码加载失败", L"Failed to load QR code",
    L"打开支付宝「扫一扫」，支持开发者 ☕",
        L"Open Alipay \"Scan\" to support the developer ☕",
    L"随心", L"Any amount",
    L"感谢每一位支持者 ❤️ —— ", L"Thank you to every supporter ❤️ — ",
    L"关闭", L"Close",
    L"QQ 交流群：", L"QQ Group: ",
    L"源代码：", L"Source code: ",

    // ---- AI knowledge base ----
    L"无法为知识库建立连接：", L"Cannot open a connection for the knowledge base: ",
    L"读取表结构…", L"Reading schema…",
    L"正在读取表结构…", L"Reading schema…",
    L"读取表结构失败", L"Failed to read schema",
    L"AI 正在校验表关系…", L"AI is verifying table relationships…",
    L"AI 校验失败，已降级为原始外键关系", L"AI verification failed — falling back to raw foreign keys",
    L"知识库已就绪", L"Knowledge base ready",
    L"知识库已就绪 ✓", L"Knowledge base ready ✓",

    // ---- AI chat panel: input bar ----
    L"输入消息，Enter 发送，Ctrl+Enter 换行",
        L"Type a message — Enter to send, Ctrl+Enter for newline",
    L"添加上下文", L"Add context",

    // ---- AI chat panel: model selector + KB refresh (persisted knowledge) ----
    L"选择模型", L"Select model",
    L"更新知识库", L"Update knowledge base",
    L"数据库连接不可用", L"Database connection unavailable",

    // ---- AI chat panel: conversations + history drawer ----
    L"你", L"You",
    L"新对话", L"New chat",
    L"对话历史", L"Chat history",
    L"历史", L"History",
    L"复制", L"Copy",
    L"复制全部对话", L"Copy entire conversation",
    L"无标题对话", L"Untitled chat",
    L"刚刚", L"just now",
    L"分钟前", L"min ago",
    L"小时前", L"h ago",
    L"天前", L"d ago",
    L"批量删除", L"Batch delete",
    L"收起历史", L"Collapse history",
    L"全选", L"Select all",
    L"取消", L"Cancel",
    L"删除", L"Delete",
    L"确定删除选中的 %d 个对话？", L"Delete the %d selected conversation(s)?",

    // ---- server monitor (工具 ▸ 服务器监控) ----
    L"服务器监控", L"Server Monitor",
    L"结束进程", L"End Process",
    L"请先连接数据库", L"Connect to a database first",
    L"当前数据库暂不支持会话监控", L"Session monitoring is not supported for this database",
    L"当前数据库暂不支持结束会话", L"Ending a session is not supported for this database",
    L"请先选择要结束的会话", L"Select a session to end first",
    L"确定结束会话 %s (%s) 吗？", L"End session %s (%s)?",
    L"无效的进程号", L"Invalid process id",
    L"无效的会话号", L"Invalid session id",
    L"无效的会话标识", L"Invalid session identifier",

    // ---- data browser: primary-key gate refusals (ResultGridPanel) ----
    L"无法保存修改", L"Cannot Save Changes",
    L"该表没有主键，数据浏览器无法确定要修改哪一行，因此不会生成任何语句。",
        L"This table has no primary key, so the data browser cannot tell which row "
        L"to change. No statements were generated.",
    L"当前结果集缺少主键列 %s，无法安全地定位要修改的行，因此不会生成任何语句。\n\n"
    L"请重新查询并包含该表的全部主键列后再编辑。",
        L"The result set is missing the primary-key column(s) %s, so the rows you "
        L"edited cannot be targeted safely. No statements were generated.\n\n"
        L"Re-run the query including every primary-key column, then edit.",
    L"当前结果集缺少主键列 %s，无法生成有主键条件的语句。\n\n"
    L"请重新查询并包含该表的全部主键列。",
        L"The result set is missing the primary-key column(s) %s, so no statement "
        L"with a primary-key condition can be generated.\n\n"
        L"Re-run the query including every primary-key column.",

    nullptr
};

} // namespace core
