# sql

## 1. 定位与边界

`sql` 是一个**方言中立**的 SQL 文本构建包：只生成 SQL、不连接数据库、不执行、不管理事务。
覆盖完整 CRUD：

```ct
"users".select("id, name").where("status = ?", 1).limit(10);
"users".insert("name, age").values("alice", 30).values("bob", 25);
"users".update().set("name = ?", "carol").where("id = ?", 7);
"users".delete().where("id = ?", 7);
```

它解决的是「拼接 SQL 字符串」这件事里最容易出错的部分：

- **值只绑定、不拼接**：`where` / `set` / `values` 的值进入有序的 `sql_value` 数组，SQL 文本里只有 `?`；
- **延迟渲染**：链式方法只记录状态，`toSql()` / `sql()` 才生成文本；
- **显式所有权**：builder 持有全部内部内存，调用方最后 `free()`，与 `std::vec` 一致；
- **语句类型隔离**：四个句柄类型各自拥有方法集合，`SELECT` 上不会出现 `values`，`INSERT` 上不会出现 `where`；
- **默认拒绝危险写操作**：`UPDATE` / `DELETE` 没有 `WHERE` 时渲染失败，必须显式 `allowAll()`。

实现层只有三个文件：

| 文件 | 作用 |
| --- | --- |
| `sql/Sql.m.ct` | 映射层。`Entry` 挂 `char *`（四个入口）；`Select` / `Insert` / `Update` / `Delete` 各挂自己的命名 struct 句柄；调用点零成本展开成 C 调用 |
| `sql/Sql.shim.h` | C 面声明：状态码、`sql_value`、函数原型、`_Generic` 值捕获与 1..8 值分派宏 |
| `sql/Sql.shim.c` | C 实现：builder 状态、动态 buffer、参数数组、四类渲染、护栏、`free` |

明确不做：数据库连接/执行/事务、ORM、迁移、完整 SQL 解析、命名参数、日志与指标。

## 2. 依赖与安装

| 项 | 值 |
| --- | --- |
| 包名 / 版本 | `sql` / `0.1.0` |
| 许可 | MIT，见 [LICENSE](LICENSE) |
| 链接 | 无 `ldflags`，只依赖 libc |
| `cflags` | `-std=c11`（`_Generic` 的标准归属） |
| 编译器 | `requires = ct >= 0.20.0` |
| 包依赖 | 无 |

`requires` 是硬前提而不是建议：四个入口都依赖**字面量接收者**，带值方法依赖**类型宿主上的
变参透传**；`ct v0.19.1` 会在解析阶段直接报错。`ct install` 对 `requires` 只告警、不阻止安装，
所以请显式确认编译器版本。

本地安装：

```sh
ct get ./sql            # 或 ct get file:///path/to/ct-pkg/sql
ct install
```

工程 `ct.pkg` 加一行：

```ini
dep = sql@^0.1.0
```

`ct install` 会把 `cflags` 折进它打印的构建命令：

```text
ct build -I vendor/sql --cflags '-std=c11' <unit>.ct -o build
```

`ct build` 本身不读 `ct.pkg`。直接在包内构建 demo 时要显式传：

```sh
mkdir -p build
ct build -I . demo/main.ct -o build/demo --cflags '-std=c11 -Wall -Wextra'
```

第一条命令里 `-I .` 指向包根（`import sql::Sql` → `sql/Sql.m.ct`），`ct build` 会自动补
`-I ./sql`、编译 `sql/Sql.shim.c` 并链接。

## 3. 快速开始

完整可运行文件是 [demo/main.ct](demo/main.ct)，覆盖四种语句与主要错误路径。最小片段：

```ct
#include <stdio.h>

import sql::Sql;

int main(void)
{
    struct sql_Select *q = "users".select("id, name")
        .where("status = ? AND age > ?", 1, 18)
        .orderBy("id DESC")
        .limit(10);

    char buf[256];
    int n = q->toSql(buf, 256);
    if (n < 0)
        printf("error: %s\n", q->message());
    else
        printf("%s\n", buf);

    q->free();
    return 0;
}
```

输出：

```text
SELECT id, name FROM users WHERE (status = ? AND age > ?) ORDER BY id DESC LIMIT 10
```

demo 的期望输出在 [demo/expected.out](demo/expected.out)，可用 `diff` 做回归。

## 4. API 全表

### 4.1 入口（宿主 `char *`）

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `select(char *fields)` | `struct sql_Select *` | 表名是接收者；`fields` 是 SELECT 列表片段 |
| `insert(char *columns)` | `struct sql_Insert *` | `columns` 是列名片段，如 `"name, age"` |
| `update()` | `struct sql_Update *` | 表名是接收者 |
| `delete()` | `struct sql_Delete *` | 表名是接收者 |

`char *` 是全仓公共宿主，本包占用 `select` / `insert` / `update` / `delete` 四个名字。
不要写第二份 `map trait ... for type const char *`：类型键会丢弃任意位置的
`const` / `volatile`，在 0.20.0 上会直接变成同一宿主的重复映射。

### 4.2 SELECT 句柄（宿主 `struct sql_Select *`）

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `where(char *cond, ...)` | 同句柄 | 追加一个带绑定值的条件，1..8 个值；条件用 `?` 占位 |
| `whereRaw(char *cond)` | 同句柄 | 追加一个无绑定值的条件（`IS NULL`、`1 = 1`） |
| `groupBy(char *expr)` | 同句柄 | 追加 GROUP BY 表达式；多次调用以 `, ` 连接 |
| `orderBy(char *expr)` | 同句柄 | 追加 ORDER BY 表达式；可含 `DESC` |
| `limit(int n)` | 同句柄 | 覆盖上一次；`n >= 0` |
| `offset(int n)` | 同句柄 | 覆盖上一次；`n >= 0`；必须有 `limit` |
| `toSql(char *out, int cap)` | `int` | 渲染并复制到调用方 buffer；返回字节数（不含 NUL）或 `-1` |
| `sql()` | `char *` | 内部渲染缓冲的借用指针；失败返回 `""` |
| `paramCount()` | `int` | 绑定参数个数 |
| `paramKind(int i)` | `int` | `-1` 越界；`1` int、`2` double、`3` text |
| `paramInt(int i, long *out)` | `int` | `0` 成功，`-1` 失败（下标/类型不符） |
| `paramDouble(int i, double *out)` | `int` | `0` 成功，`-1` 失败 |
| `paramText(int i)` | `char *` | 借用指针；下标/类型不符返回 `NULL` |
| `status()` | `int` | 当前状态码 |
| `message()` | `char *` | 静态错误文本，不需要 `free` |
| `free()` | `void` | 释放 builder 与全部内部资源；`free(NULL)` 是 no-op |

### 4.3 INSERT 句柄（宿主 `struct sql_Insert *`）

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `values(...)` | 同句柄 | 追加一行，1..8 个值；值数必须等于 `insert(columns)` 的列数 |
| `toSql` / `sql` / `paramCount` / `paramKind` / `paramInt` / `paramDouble` / `paramText` / `status` / `message` / `free` | 同 4.2 | 接口形态与 SELECT 完全一致 |

多行就是多次 `values(...)`；参数顺序等于 `values` 的调用顺序。没有 `where`，因为
`INSERT` 不支持条件。

### 4.4 UPDATE 句柄（宿主 `struct sql_Update *`）

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `set(char *expr, ...)` | 同句柄 | 追加一个赋值片段，1..8 个值；多次调用以 `, ` 连接 |
| `setRaw(char *expr)` | 同句柄 | 追加一个无绑定值的赋值（`updated_at = CURRENT_TIMESTAMP`） |
| `where(char *cond, ...)` | 同句柄 | 同 SELECT |
| `whereRaw(char *cond)` | 同句柄 | 同 SELECT |
| `allowAll()` | 同句柄 | 显式允许没有 `WHERE` 的 UPDATE |
| 其余方法 | — | `toSql` / `sql` / 参数访问 / `status` / `message` / `free` 与 SELECT 一致 |

**参数顺序固定为 SET 先、WHERE 后**，与 `set` / `where` 的调用先后无关。

### 4.5 DELETE 句柄（宿主 `struct sql_Delete *`）

| 方法 | 返回 | 说明 |
| --- | --- | --- |
| `where(char *cond, ...)` | 同句柄 | 同 SELECT |
| `whereRaw(char *cond)` | 同句柄 | 同 SELECT |
| `allowAll()` | 同句柄 | 显式允许没有 `WHERE` 的 DELETE |
| 其余方法 | — | `toSql` / `sql` / 参数访问 / `status` / `message` / `free` 与 SELECT 一致 |

### 4.6 状态码

| 码 | 名称 | 含义 |
| ---: | --- | --- |
| `0` | `SQL_E_OK` | 成功 |
| `-1` | `SQL_E_OOM` | 内存分配失败 |
| `-2` | `SQL_E_STATE` | 状态非法：SELECT 的 `offset` 没有 `limit`；UPDATE/DELETE 没有 `WHERE` 且未 `allowAll()` |
| `-3` | `SQL_E_ARG` | 参数非法：空表名/空字段/空列名、负 `limit`/`offset`、空条件、INSERT 值数不等于列数、UPDATE 没有 SET |
| `-4` | `SQL_E_SPACE` | 输出 buffer 太小 |
| `-5` | `SQL_E_PARAM` | `?` 数量与绑定值数量不一致 |

所有链式方法返回同一个句柄；句柄为 `NULL` 时方法返回 `NULL` 或失败码，不会崩溃。

## 5. 教程正文

### 5.1 从字符串拼接到 builder

手写 SQL 最容易出两类错：值里的引号破坏语句（注入），以及条件一多就漏空格/漏 `AND`。
builder 把这两件事分开：

```ct
/* 值：只绑定 */
q = "users".select("id, name")
    .where("name = ? AND age > ?", user_name, 18);

/* 结构：可信 SQL 片段 */
q = q->orderBy("id DESC").limit(20);
```

渲染结果里 `user_name` 不会出现：

```sql
SELECT id, name FROM users WHERE (name = ? AND age > ?) ORDER BY id DESC LIMIT 20
```

### 5.2 条件追加与括号

每次 `where` / `whereRaw` 追加一个条件，条件整体加一层括号，之间用 ` AND ` 连接：

```ct
"t".select("a")
    .where("x = ?", 1)
    .whereRaw("y IS NULL")
    .where("z > ?", 2.5);
```

```sql
SELECT a FROM t WHERE (x = ?) AND (y IS NULL) AND (z > ?)
```

这是**追加**语义，不是覆盖；参数顺序就是调用顺序。

### 5.3 取值：`_Generic` 在调用点打标签

`where` / `set` / `values` 的变参在 C 里会被擦除类型，所以 shim 用宏在调用点展开：

```c
sql_select_where(q, "status = ? AND age > ?", 1, 18)
/* 展开为带 kind 的 sql_value 数组 */
```

支持的类型：`char`、各种有符号/无符号整数、`float`、`double`、`char *`、`const char *`。
字符串字面量通过 `((x) + 0)` 退化成指针后匹配。一次调用最多 8 个值；`where` / `set`
可以把更多条件拆到多次调用里。

0 个值用 `whereRaw` / `setRaw`，因为经典 `SQL_NARG` 对 0 个实参返回 1，无法和 1 个值区分。

### 5.4 INSERT：列名与多行

```ct
struct sql_Insert *ins = "users".insert("name, age")
    .values("alice", 30)
    .values("bob", 25);

ins->toSql(buf, sizeof buf);
/* INSERT INTO users (name, age) VALUES (?, ?), (?, ?) */
```

列数按顶层逗号数（括号与单引号内的逗号不算）。每行值数必须等于列数，否则 `status()`
为 `SQL_E_ARG`，`toSql` 返回 `-1`。没有调用过 `values` 也在渲染期报 `SQL_E_ARG`。

### 5.5 UPDATE：SET 与 WHERE

```ct
struct sql_Update *up = "users".update()
    .where("id = ?", 7)
    .set("name = ?", "carol")
    .setRaw("updated_at = CURRENT_TIMESTAMP");
```

```sql
UPDATE users SET name = ?, updated_at = CURRENT_TIMESTAMP WHERE (id = ?)
```

参数是 `carol`、`7`：**SET 先、WHERE 后**，即使 `where` 写在 `set` 前面。没有 `set` 任何
片段时渲染失败（`SQL_E_ARG`）。没有 `WHERE` 时渲染失败（`SQL_E_STATE`），除非显式：

```ct
"users".update().set("active = ?", 1).allowAll();
/* UPDATE users SET active = ? */
```

### 5.6 DELETE：同一套条件护栏

```ct
struct sql_Delete *del = "users".delete().where("id = ?", 7);
/* DELETE FROM users WHERE (id = ?) */
```

没有 `WHERE` 时同样是 `SQL_E_STATE`；确实要清表必须写 `allowAll()`：

```ct
"users".delete().allowAll();
/* DELETE FROM users */
```

### 5.7 读取参数

builder 不执行 SQL，但把参数按渲染顺序保存，便于调用方交给驱动：

```ct
long id = 0;
double score = 0.0;

if (q->paramInt(0, &id) == 0)
    printf("id=%ld\n", id);
if (q->paramDouble(2, &score) == 0)
    printf("score=%.1f\n", score);
printf("name=%s\n", q->paramText(3));
```

`paramKind(i)` 用于分派；`paramText` 返回 builder 内部的借用指针，生命周期到 `free()`。
文本参数在写入时已复制，调用方可以立刻复用或释放自己的 buffer。

### 5.8 渲染与 buffer

`toSql(out, cap)` 返回写入字节数（不含结尾 NUL），`cap` 不足返回 `-1` 并把状态置为
`SQL_E_SPACE`。两步用法：

```ct
char small[64];
int n = q->toSql(small, sizeof small);
if (n < 0 && q->status() == SQL_E_SPACE) {
    int need = (int)strlen(q->sql()) + 1;
    char *big = malloc((size_t)need);
    if (big) {
        q->toSql(big, need);
        /* use big */
        free(big);
    }
}
```

`sql()` 返回内部缓存，下一次修改或 `free()` 后失效；不要再 free 它。

### 5.9 生命周期

```ct
struct sql_Select *q = "t".select("a");  /* 分配句柄，复制 table/fields */
q = q->where("a = ?", 1);                /* 追加条件与参数，渲染缓存失效 */
char *s = q->sql();                      /* 按需渲染，返回借用指针 */
q->free();                               /* 释放全部内存 */
```

没有引用计数、没有 GC、没有 Drop。多次 `sql()` 返回同一个指针；`toSql` 不转移所有权。

## 6. 常见配方

### 6.1 分页查询

```ct
struct sql_Select *q = "orders".select("id, total")
    .where("user_id = ?", uid)
    .where("created_at >= ?", since)
    .orderBy("id DESC")
    .limit(page_size)
    .offset(page * page_size);
```

`offset` 必须和 `limit` 一起用；只写 `offset` 会在渲染时得到 `SQL_E_STATE`。

### 6.2 批量插入

```ct
struct sql_Insert *ins = "users".insert("name, age")
    .values("alice", 30)
    .values("bob", 25)
    .values("carol", 41);
```

实际输出（两行版本取自 demo）：

```sql
INSERT INTO users (name, age) VALUES (?, ?), (?, ?)
```

### 6.3 安全清零

```ct
"users".delete().allowAll();       /* DELETE FROM users */
"users".update().set("active = ?", 0).allowAll();
```

`allowAll()` 只表示「我确认没有 WHERE」，不会改变 SQL 的其它部分。

### 6.4 条件里的字面量 `?`

占位符检查会跳过单引号字符串：

```ct
q = q->where("note = '?' AND id = ?", 1);  /* 只有最后一个是占位符 */
```

它不解析双引号标识符、注释和美元引用；复杂片段请改用 `whereRaw`。

### 6.5 与 std 协作

`sql` 不依赖 `std`，但结果可以直接交给它：

```ct
import sql::Sql;
import std::str::String;

int len = q->sql().len();   /* 复用 std 的 char * len，不要自己再定义 len */
```

## 7. 排错

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `requires ct >= 0.20.0` 告警 | 编译器太旧 | 换 0.20.0+；0.19.1 不支持字面量接收者与变参 trait |
| `expected a type (tok 4 '...')` | 旧编译器解析变参 trait 失败 | 同上 |
| C 编译报 `sql_*_where` / `sql_*_set` / `sql_insert_values` 参数不足 | 对 0 个值调用了带值方法 | 改用 `whereRaw` / `setRaw`；INSERT 至少一个值 |
| `toSql` 返回 `-1`、`status() == -2` | SELECT 只有 `offset`；UPDATE/DELETE 没有 `WHERE` | 补 `limit`；补 `where`，或确认后 `allowAll()` |
| `toSql` 返回 `-1`、`status() == -5` | `?` 数量与值数量不一致 | 数一遍片段里的 `?`，或把复杂片段改成 `*Raw` |
| `toSql` 返回 `-1`、`status() == -4` | 输出 buffer 太小 | 用 `strlen(q->sql()) + 1` 重试 |
| `toSql` 返回 `-1`、`status() == -3` | 空表名/空字段/空列名、负 limit、INSERT 值数不对、UPDATE 没有 SET | 对照 §4.6 |
| `ambiguous type-host map for method 'select'` | 别的包也在 `char *` 上定义了同名入口 | 查 `docs/char-host-methods.md`，名字必须全仓唯一 |
| 链接缺 `sql_*` | 没走 `ct build`，或 `Sql.shim.c` 没进命令 | 用 `ct build -I <包根>`，确认 cc 行包含 `Sql.shim.c` |

`ct build` 不读 `ct.pkg` 里的 `cflags`。直接构建时漏传 `--cflags '-std=c11'` 在
clang/GCC 上仍能编译（`_Generic` 在 c99 下只是扩展），但 `-pedantic` 会告警。

## 8. 术语表（Glossary）

术语按三类收录：本包特有概念、全仓易混概念、正文出现过但未解释的英文术语。
类型名、方法名、文件名与源码逐字一致。

| 术语 | 含义 | 本包中的所指 |
| --- | --- | --- |
| `Entry` | 挂在表名字符串上的入口 trait，负责按语句类型创建 builder。 | `sql/Sql.m.ct` 中 `map trait Entry for type char *`，四个方法 `select` / `insert` / `update` / `delete`，接收者就是表名。 |
| builder | 逐步拼接 SQL 文本与绑定参数的可变对象。 | `Select` / `Insert` / `Update` / `Delete` 四个句柄的统称；每个句柄对应一个 `struct sql_*` 与一组链式方法。 |
| `Select` | SELECT builder 的方法集合。 | 宿主 `struct sql_Select *`：`where` / `whereRaw` / `groupBy` / `orderBy` / `limit` / `offset`，外加公共的渲染、参数读取与 `free`。 |
| `Insert` | INSERT builder 的方法集合。 | 宿主 `struct sql_Insert *`：只有 `values(...)` 和一个共享尾组；列在入口 `Entry.insert(columns)` 时给定，没有 `where`。 |
| `Update` | UPDATE builder 的方法集合。 | 宿主 `struct sql_Update *`：`set` / `setRaw` / `where` / `whereRaw` / `allowAll`。 |
| `Delete` | DELETE builder 的方法集合。 | 宿主 `struct sql_Delete *`：`where` / `whereRaw` / `allowAll`。 |
| `where` | 追加一个带绑定值的条件。 | `where(cond, ...)`：条件整体加括号、多个条件用 ` AND ` 连接；`?` 个数必须等于值个数，否则 `SQL_E_PARAM`。 |
| `whereRaw` | 追加一个不带绑定值的条件。 | `whereRaw(cond)`：存在理由是 `where` 传不了 0 个值（`SQL_NARG` 在 0 实参上不可用），而 `IS NULL`、`1 = 1` 这类片段本来就没有值。 |
| `setRaw` | 追加一个不带绑定值的赋值片段。 | UPDATE 的 `setRaw(expr)`，如 `updated_at = CURRENT_TIMESTAMP`；有值用 `set`。 |
| `allowAll()` | 显式声明「这条 UPDATE / DELETE 确实没有 `WHERE`」。 | `Update` / `Delete` 上的方法；不调用且没有条件时渲染失败并置 `SQL_E_STATE`，防止漏条件变成全表写。 |
| `values(...)` | 追加一行待插入的值。 | `Insert` 上的方法，1..8 个值；值数必须等于 `insert(columns)` 的列数，多行就是多次调用。 |
| 占位符（`?`） | SQL 文本中代表一个绑定值的标记，由驱动按顺序替换。 | 值方法生成的文本里只有 `?`，真实值在 `sql_value` 数组里；`where` / `set` 调用时就核对 `?` 数与值数，不一致立刻置 `SQL_E_PARAM`。 |
| `sql_value` | 带类型标签的绑定值，记录 kind 及其数据。 | `sql/Sql.shim.h` 的 `typedef struct sql_value`，`kind` 为 `0` null（第一期不产生）、`1` int、`2` double、`3` text；`paramKind` 返回同一套编号。 |
| `_Generic` | C11 的调用点类型分派机制，按实参的静态类型选函数。 | `SQL_VAL(x)` 用 `_Generic(((x) + 0), ...)` 把每个变参包装成 `sql_value`；这是变参能保留类型的原因。 |
| `SQL_NARG` / `SQL_ARGS_n` | 数变参个数的宏，以及按个数展开的 1..8 元宏。 | `sql/Sql.shim.h` 里的 `SQL_NARG` 与 `SQL_ARGS_1`..`SQL_ARGS_8`；一次带值调用最多 8 个值。 |
| 标识符与值（identifier / value） | 标识符指表名、列名、`GROUP BY` / `ORDER BY` 表达式等 SQL 结构部分；值指要绑到 `?` 的数据。 | 标识符不转义也不校验，原样拼进 SQL，必须是可信片段；值从不进文本，只以 `sql_value` 形式排队。 |
| `toSql` / `sql()` | 两个渲染入口：前者复制进调用方缓冲区，后者返回内部缓冲。 | `toSql(out, cap)` 返回字节数或 `-1`；`sql()` 返回内部缓冲的借用指针，下一次修改或 `free()` 后失效。 |
| `SQL_E_*` | 本包自己的状态码前缀，由 `status()` 读取。 | `SQL_E_OK`(0) / `SQL_E_OOM`(-1) / `SQL_E_STATE`(-2) / `SQL_E_ARG`(-3) / `SQL_E_SPACE`(-4) / `SQL_E_PARAM`(-5)，定义在 `sql/Sql.shim.h`。 |
| 句柄（handle） | 一个不透明指针，调用方只通过方法操作它，不直接碰内部结构。 | 四个 builder 句柄 `struct sql_Select *` / `struct sql_Insert *` / `struct sql_Update *` / `struct sql_Delete *`，内部结构不出包。 |
| 宿主（host） | trait 方法挂靠的类型。 | 五个宿主：`char *`（`Entry`）与四个 `struct sql_* *`（`Select` / `Insert` / `Update` / `Delete`）。 |
| 映射（map trait） | 把 trait 方法绑定到宿主并转调 C 函数的声明。 | `sql/Sql.m.ct` 的五段 `map trait … for …`，每行转调一个 `sql_*` 函数。 |
| 链式返回自身（return receiver） | 方法返回接收者本身，使下一次调用可以接在后面。 | 全部结构方法（`where` / `whereRaw` / `set` / `setRaw` / `values` / `groupBy` / `orderBy` / `limit` / `offset` / `allowAll`）都返回同一个句柄；`toSql` / `sql` / `param*` / `status` / `message` / `free` 是终结或查询方法。 |
| 所有权（ownership） | 谁负责释放一块内存的约定。 | builder 句柄归调用方，由 `free()` 释放；文本值在 `where` / `set` / `values` 时被复制进 builder，`free()` 一并释放。 |
| 借用（borrow） | 只取得指针的使用权，不承担释放责任。 | `sql()` 与 `paramText()` 返回借用指针，有效期到下一次修改 builder 或 `free()`。 |

## 9. 限制与未支持

- **一次带值调用最多 8 个值**；`where` / `set` 可以拆成多次调用，INSERT 每行最多 8 列。
- **0 个绑定值用 `whereRaw` / `setRaw`**；`where(cond)` 的零值写法要等 `__VA_OPT__` 方案定稿。
- **不做标识符引用/校验**：表名、列名、`GROUP BY` / `ORDER BY`、`setRaw`、`whereRaw` 条件都是
  “可信 SQL 片段”，禁止直接传用户输入。值才走绑定。
- **不做方言**：占位符统一 `?`，引用符/`$n` 留给后续。
- **不支持 SQL NULL 作为绑定值**；`IS NULL` 用 `whereRaw`。
- **占位符检查是尽力而为**：只跳过单引号字符串，不解析注释/双引号/美元引用。
- **INSERT 的多行语法是通用 `VALUES (...), (...)`**；不检测目标数据库是否支持。
- **未在 Windows/MSVC 验证**；`requires` 只告警，不阻止安装。
- 线程安全：builder 无全局状态，每个句柄独立；未做并发测试。

## 10. 版本与变更记录

- `0.1.0`（2026-11-07，未打 tag、未写入 `index/`）：完整 CRUD。
  - SELECT：`where` / `whereRaw` / `groupBy` / `orderBy` / `limit` / `offset`；
  - INSERT：列名 + 多行 `values`，列数/值数校验；
  - UPDATE：`set` / `setRaw` + `where`，无 SET 拒绝，无 WHERE 需 `allowAll()`；
  - DELETE：`where`，无 WHERE 需 `allowAll()`；
  - 四种语句共用 `toSql` / `sql` / 参数访问 / `status` / `message` / `free`；
  - 1..8 个混合类型参数的 `_Generic` 捕获；demo 与回归输出覆盖四种语句和主要错误路径。
- 后续计划：方言与标识符引用、`orWhere` / `having` / `join`、超过 8 个参数、SQL NULL 绑定。
