# sql 模块设计（SQL builder）

- 日期：2026-11-07
- 状态：设计待评审，本轮不实现、不提交、不 bump 版本
- 目标包：`sql`（ct-pkg 新包）
- 参考实现：<https://github.com/samxxu/migears-sql>

---

## 0. 结论摘要

1. **目标写法在 CT 侧可行**：`"my_table".select("id, username").where("id = ?", 1)` 已在从当前 ct 工作树构建的编译器上跑通。但它依赖**尚未发布**的两项编译器能力：字符串字面量作接收者、trait 方法尾部变参；发布版 `ct v0.19.1` 会直接报错。
2. **裸 `...` 只能转发、不能捕获类型**：`where("id = ?", 1, 2, 3)` 可以在语法上落到一个 C 变参函数，但 C 变参函数拿不到每个实参的静态类型，无法安全地把 `1`、`"bob"`、`2.5` 分开存。设计用 **shim 里的 `_Generic` 宏**在调用点给每个值打类型标签，再写入有序的 `sql_value` 数组；SQL 文本里只保留 `?`，值永远不拼进字符串。补测确认：`_Generic` 在 `-std=c99 -Wall -Wextra` 下**既不报错也不告警**（Apple clang 21、Homebrew clang 23、GCC 16 均实测），只有 `-pedantic` 才告警；`cflags = -std=c11` 仍是标准符合性上的正确选择，但不是当前门禁通过的前提。
3. **不能用 `void *` 作 builder 宿主**：宿主键是类型拼写，`void *` 是全仓共享的一个全局键；任何包映射它都会落进同一个命名空间，任何无类型指针（文件句柄、分配器指针、opaque 库句柄）都能调用这些方法。当前 `png::Handle` 已改挂 `PngReader *`，`void *` 暂时没有包占用，但这只说明“暂时没有冲突”，不改变 `void *` 会污染全局类型空间的事实。因此 builder 使用命名结构体指针宿主：`struct sql_Select *`、`struct sql_Insert *`、`struct sql_Update *`、`struct sql_Delete *`。
4. **CT 不支持方法重载**：同一宿主类型上同名方法只能有一个映射；不同实参形态不能共存。用**分语句类型的句柄**区分 `select/insert/update/delete`，用**不同方法名**区分有参/无参条件（`where` / `whereRaw`）。补测确认经典 `SQL_NARG` 对 0 个实参返回 1，无法区分 0/1；`__VA_OPT__` 写法可以返回 0，但它属于 C23 预处理器特性（`-pedantic` 会告警）。因此阶段一保留 `whereRaw` 作为可移植的零值入口，`where(cond)` 的零值写法留作可选项。
5. **求值时机是延迟渲染**：入口返回 heap 上的 builder 句柄，链式方法只记录子句和参数；`toSql(out, cap)` / `sql()` 才渲染。字符串所有权遵循 std 现有约定：builder 持有内部缓冲，`sql()` 借用，`toSql` 写入调用方 buffer，显式 `free()`，没有 Drop。
6. **落点是 `sql/sql/` 下的一个映射单元 + shim 对**：`Sql.m.ct` + `Sql.shim.h` + `Sql.shim.c`，**不用 `.h.ct`**。原因是 `.m.ct` 不能 import `.h.ct`；用 `.shim.c` 可以随 `.m.ct` 自动编译，消费者只 import 一次，结构体 tag 也是手写的稳定 C 名，不依赖 CT 的 mangling。`std::str::String` 已经采用同样的“只有 `.m.ct` + shim”形态。
7. **第一期只做 SELECT**：把入口、链式、参数捕获、渲染、所有权、错误模型、安全边界跑通；INSERT/UPDATE/DELETE、方言、标识符引用、join/having 留到第二期。
8. **安全边界要写清**：值（`1`、`"bob"`）只绑定、不拼接；表名、列名、条件片段、`ORDER BY` 表达式是“可信 SQL 片段”，第一期不做引用/白名单校验，文档必须明确禁止把不可信输入传给 `whereRaw`、`orderBy`、表名/列名。

---

## 0.1 修订（2026-11-08）：去掉变参，改为链式 bind

**触发原因**：`ct v0.20.3` 的 trait 方法不支持变参，`struct sql_Select *where(char *cond, ...);`
在解析阶段报 `error: expected a type (tok 4 '...')`。原方案里 `_Generic` + `SQL_ARGS_1..8`
整套分派因此全部失效。

**最终接口**：结构片段与值绑定分离，绑定紧邻它所属的片段。

```ct
"users".select("id, name").where("age > ?").bindInt(18).limit(10)
"users".insert("name, age").row().bindText("alice").bindInt(30)
"users".update().set("name = ?").bindText("carol").where("id = ?").bindInt(7)
"users".delete().where("id = ?").bindInt(7)
```

- `where(cond)` / `set(expr)` / `row()` 只收字符串，片段里 `?` 的个数记进
  `pend_need`；随后紧邻的 `bindInt` / `bindDouble` / `bindText` / `bindNull` 依次填入
  对应数组并累加 `pend_got`。
- 片段遇到下一次结构调用或渲染即冻结，`pend_got != pend_need` 报 `SQL_E_PARAM`（`-5`）。
  `status()` 用只读方式做同样核对，不会改动 builder。
- `whereRaw` / `setRaw` 不参与绑定计数，语义与阶段一一致。
- UPDATE 的参数顺序仍固定 SET 先、WHERE 后；绑定跟着片段走，与调用先后无关。
- 去掉了 8 个值上限；`SQL_E_ARG`（-3）不再承担「值数不等于占位符数」，改由 `SQL_E_PARAM` 承担。
- 新增 `bindNull`，`kind == 0`，三种类型化读取接口对它一律返回失败。
- C 面全部变成定参函数：`Sql.shim.h` 不再有 `_Generic` / `SQL_NARG` / `SQL_ARGS_*`，
  `ct.pkg` 不再需要 `cflags = -std=c11`，`requires` 提升到 `ct >= 0.20.3`。

**已实测**：字面量接收者 + 多段长链、方法调用结果再次作接收者、零参方法（`row()`）出现在链中间、
链先落变量再续接，均在 `ct v0.20.3`（`/Users/samxx/.local/bin/ct`）上编译运行通过；
`-Wall -Wextra` 零警告。原先「链式只能起一段」的担心不成立，但文档仍建议多段链落变量，便于阅读。

**本文其余章节保留 2026-11-07 的探针记录与推演过程，其中提到 `...`、`_Generic`、`SQL_ARGS*`、
8 个值上限、`whereRaw` 只为零值而存在的结论均为历史内容，已被本节取代。**


---

## 1. 目标与非目标

### 1.1 目标

提供纯 SQL 文本构建能力，不依赖数据库驱动：

```ct
"my_table".select("id, username").where("id = ?", 1)
```

第一期希望覆盖：

- 字符串字面量直接作接收者，表名就是接收者；
- 链式追加条件、分组、排序、分页；
- 参数与 SQL 文本分离，值只进入参数数组；
- 延迟渲染，同一个 builder 可以多次取 SQL；
- 显式所有权，零第三方依赖，零全局状态；
- 与 `std::str`、`std::vec`、`std::io` 在调用方自然协作；
- 不使用 `void *` 宿主，避免把 SQL 方法暴露给所有无类型指针、污染全局宿主键（详见 §3.7）。

### 1.2 非目标

- 不建立数据库连接、不执行 SQL、不管理事务、不做连接池；
- 不做 ORM、实体映射、迁移；
- 不做完整 SQL 解析/校验（只做占位符数量的尽力检查）；
- 不在第一期做多方言（MySQL/PostgreSQL/SQLite 的占位符/引用差异）；
- 不做命名参数（`:name`）；
- 不做日志、指标、审计；这些由调用方或未来的驱动适配层处理。

---

## 2. 参考实现 migears-sql 的 API 与设计意图

### 2.1 参考实现的形态

`migears-sql` 是 PHP 的 PDO 查询构建器，核心文件：

- `SqlBuilder`：工厂，提供 `select($table)`、`insert($table)`、`update($table)`、`delete($table)`；
- `SqlSelect`：`fields()` / `from()` / `where()` / `filter()` / `bind()` / `orderBy()` / `groupBy()` / `limit()` / `offset()` / `toSql()` / `execute()` / `single()` / `count()` / `paginate()`；
- `HasWhereClause`：`where($condition, array $bindings = [])`、`filter(array)`、`bind(array)`、`buildWhere()`；
- `SqlInsert`：`values(array)`、`toSql()`、`execute()`、`lastInsertId()`；
- `SqlUpdate`：`set(array|string)`、`toSql()`、`execute()`；
- `SqlDelete`：`toSql()`、`execute()`。

典型用法：

```php
$sql->select('users')
    ->fields(['id', 'name'])
    ->where('status = :status', ['status' => 1])
    ->orderBy('id', 'DESC')
    ->limit(10)
    ->execute();
```

### 2.2 设计意图

参考实现真正值得继承的是下面几点，而不是 PHP 的具体语法：

1. **构建与执行分离**：`toSql()` 只生成 SQL，`execute()` 才碰 PDO。
2. **参数绑定**：SQL 文本里是命名占位符，值走 `bindValue`，不拼字符串。
3. **延迟生成**：链式方法只记录状态，`toSql()` 时按固定顺序拼装。
4. **安全护栏**：
   - UPDATE/DELETE 没有 WHERE 时拒绝执行，避免误改/误删全表；
   - `offset` 没有 `limit` 时 `SqlSelect::toSql()` 直接抛异常；
   - 表名/列名做 `assertValidIdentifier`，并用反引号包起来。
5. **链式返回 `self`**：每一段都能继续挂下一段。
6. **语句类型分开**：SELECT/INSERT/UPDATE/DELETE 各自持有自己的方法，避免在 SELECT 上出现 `values()`。

### 2.3 CT 侧要继承/改写的东西

| 参考实现意图 | CT 侧处理 |
| --- | --- |
| 构建与执行分离 | 完整保留；`sql` 包只做构建，不引入数据库依赖 |
| 参数绑定 | 保留；改成位置 `?` + `sql_value` 数组 |
| 延迟生成 | 保留；`toSql()`/`sql()` 才渲染 |
| UPDATE/DELETE 无 WHERE 护栏 | 保留，但提前到 `toSql()`，因为 CT 没有 `execute()` |
| offset 无 limit 护栏 | 保留，第一期在 `toSql()` 返回错误码 |
| 表名/列名校验 | 第一期不做；改为文档声明“可信片段”，第二期再做引用/白名单 |
| 语句类型分开 | 保留；用不同 C 句柄类型，而不是 PHP class |
| 链式返回 `self` | 保留；C 函数返回同一个句柄指针 |

---

## 3. CT 可行性探针（实测证据）

> 探针全部放在 `/tmp/sqlprobe/`，没有修改仓库。  
> “发布版”指本机安装的 `ct v0.19.1`（2026-11-03 构建）。  
> “工作树版”指从 `/Users/samxx/vscspace/ct` 当前工作树 bootstrap 到 `/tmp/ctprobe/ct` 的编译器；它自报 `v0.19.1`，但包含 HEAD `915c12c`（字面量接收者）和未提交的变参映射改动，**不是发布版**。

### 3.1 字符串字面量作接收者

探针：`/tmp/sqlprobe/p1b`

```ct
namespace probe;
pub trait Len { int probeLen(); }
map trait Len for type char * { probeLen(s) -> probe_len(s); }
```

```ct
#include <stdio.h>
import probe::entry;
int main(void) { printf("%d\n", "abc".probeLen()); return 0; }
```

结果：

- 发布版：`main.ct:3:1: error: method receiver must be a variable, an element, a call or a field chain`
- 工作树版：编译通过，生成的 C 是 `printf("%d\n", probe_len(("abc")));`

**结论：字面量接收者可用，但依赖未发布能力。**

#### 3.1.1 `char *` 宿主的全局命名约束（补测）

`char *` 是共享宿主：`std::str::String`、`std::io::Path`、`std::fs::FsOps`、`std::utf8::Utf8Ops` 都挂在它上面。补测让两个包分别在 `char *` 上定义 `len()` 和 `len(int)`：

```ct
/* inc/a/a.m.ct */
namespace a;
pub trait A { int len(); }
map trait A for type char * { len(s) -> a_len(s); }

/* inc/b/b.m.ct */
namespace b;
pub trait B { int len(int n); }
map trait B for type char * { len(s, n) -> b_len(s, n); }
```

```text
$ /tmp/ctprobe/ct build -I inc import_only.ct -o build/import_only
'cc' -std=c99 -I 'inc/a' -I 'inc/b' -I '.ctbuild' '.ctbuild/import_only.c' '-o' 'build/import_only'
build_exit=0

$ /tmp/ctprobe/ct build -I inc call_len.ct -o build/call_len
call_len.ct:7:1: error: ambiguous type-host map for method 'len' on type 'char *': 'inc/a/a.m.ct' and 'inc/b/b.m.ct'
build_exit=1
```

**两条硬约束：**

1. **`char *` 上的方法名必须全仓唯一**。签名、参数个数、返回类型不同都不能消歧；`len()` 和 `len(int)` 一样会 `ambiguous`。目标编译器会丢弃类型拼写里**所有位置**的 `const` / `volatile` token，所以 `char *` 与 `const char *` 是同一个宿主，不能靠加 `const` 规避。
2. **解析作用域是当前编译单元 import 的可达闭包**。import 了重名包但不调用，不会报错；重名是“调用时才炸”的懒失败。因此不能靠“这个包里没用到 std::str”来规避冲突。

#### `const char *` 是否独立宿主：按编译器版本分

补测 `/tmp/sqlprobe2/consthost`，四种场景分别用发布版 `v0.19.1` 和目标工作树版跑：

| 场景 | 发布版 `v0.19.1` | 工作树（目标 0.20.0） |
| --- | --- | --- |
| 只声明 `for type char *`，用 `const char *s` 调用 | 命中，输出 `1` | 命中，输出 `1` |
| 只声明 `for type const char *`，用 `char *s` 调用 | 命中，输出 `2` | 命中，输出 `2` |
| 两份同名 `dup()`，用 `char *s` 调用 | 命中 `char *` 那份，输出 `10` | `ambiguous type-host map ... on type 'char *'` |
| 两份同名 `dup()`，用 `const char *s` 调用 | 命中 `const char *` 那份，输出 `20` | `ambiguous type-host map ... on type 'const char *'` |

关键命令与输出：

```text
$ /tmp/ctprobe/ct build -I inc both_char_recv.ct -o build/both_char_recv
both_char_recv.ct:7:1: error: ambiguous type-host map for method 'dup' on type 'char *':
  'inc/both/both.m.ct' and 'inc/both/both.m.ct'
build_exit=1

$ /tmp/ctprobe/ct build -I inc both_const_recv.ct -o build/both_const_recv
both_const_recv.ct:7:1: error: ambiguous type-host map for method 'dup' on type 'const char *':
  'inc/both/both.m.ct' and 'inc/both/both.m.ct'
build_exit=1

$ ct build -I inc both_char_recv.ct -o build/rel_both_char_recv
build_exit=0
$ ./build/rel_both_char_recv
10

$ ct build -I inc both_const_recv.ct -o build/rel_both_const_recv
build_exit=0
$ ./build/rel_both_const_recv
20
```

**结论：**

- **发布版 `v0.19.1`：两个独立宿主键**，`char *` 和 `const char *` 各命中各的映射；但上表第 1 行也证明，只写一份 `for type char *` 仍能命中 `const char *` 接收者，所以“只写一份不够”并不成立——真正的差异是**能不能再写第二份 `const` 映射**：0.19.1 可以共存并优先精确匹配。
- **目标工作树 / 0.20.0：同一个宿主键**，只写一份 `for type char *` 即可同时覆盖 `char *` 和 `const char *` 接收者；写第二份 `const` 映射会直接歧义。
- 因此 `sql` **只写一份 `for type char *`**，不要再写 `for type const char *`；否则在目标编译器上会直接触发歧义。
- 当前 ct-pkg 的 [String.m.ct](file:///Users/samxx/vscspace/ct-pkg/std/std/str/String.m.ct#L63) 只有一处 `map trait String for type char *`，没有 `for type const char *`；所以在目标编译器上不会因 std 产生 `len` 的第二个 `char *` 宿主映射。若其它分支/旧版确实有两份，升级到 0.20.0 时必须删掉 `const` 那份。

**`map_type_key` 的归一化范围（以实际代码为准）：**

- [TypeCheck.h.ct](file:///Users/samxx/vscspace/ct/selfhost/ct-ast/frontend/type/TypeCheck.h.ct#L74-L97)：`type_key` 先丢掉所有空白，再逐个标识符扫描；只要 token 等于 `const` 或 `volatile` 就 `continue` 跳过。**不是只丢顶层 const**，而是类型拼写里任何位置的 `const` / `volatile` 都丢；随后折叠 `unsigned` / `long` / `short` 等整数别名。
- [Emitter.h.ct](file:///Users/samxx/vscspace/ct/selfhost/ct-ast/backend/emit/Emitter.h.ct#L417-L440)：`map_type_key` 用同一张表、同一段逻辑；注释也明确写了 “`const char *` and `char *` are one host”。两处必须同步修改。
- 归一化只用于**比较键**，不改变生成的 C 类型拼写；所以 `const char *` 的语义仍由 C 编译器检查，但方法解析时它与 `char *` 不可区分。

**`char *` 名字盘点（ct-pkg 现有代码）：**

| 模块 | 已占用的 `char *` 方法名 |
| --- | --- |
| `std::str::String` | `len`、`find`、`findChar`、`startsWith`、`endsWith`、`copy`、`slice`、`concat`、`toLower`、`toUpper`、`compare`、`copyTo`、`toInt`、`toDouble` |
| `std::io::Path` | `open` |
| `std::fs::FsOps` | `exists`、`readFile`、`writeFile`、`list` |
| `std::utf8::Utf8Ops` | `decode`、`codepointCount`、`encode` |

**`sql` 计划挂在 `char *` 上的名字核对：**

| 计划名 | 与上表冲突 | 结论 |
| --- | --- | --- |
| `select` | 否 | 保留 |
| `insert` | 否 | 保留（第二期） |
| `update` | 否 | 保留（第二期） |
| `delete` | 否 | 保留（第二期） |

设计不再新增 `len`：需要渲染 SQL 的长度时，直接复用 `std::str::String` 的 `q->sql().len()`（同时 import `std::str::String`），不要自己再定义。本文档里的探针也把 `len` 改成了 `probeLen`，避免示例本身诱导出冲突名。

> 注意：上表只统计了 ct-pkg 里实际存在的 `map trait … for type char *`；目标编译器下 `const char *` 与它同键，所以不另列。如果未来别的包新增 `char *` 方法，必须先查这张表；反过来，`sql` 新增 `char *` 方法也要同步更新这张表。

### 3.2 链式调用与“类型宿主返回结构体”

探针：`/tmp/sqlprobe/p2`、`/tmp/sqlprobe/p14`

`p2` 先验证了概念：`.m.ct` 里的类型宿主方法返回 `struct probe_Builder *`，调用点可以继续 `.where(...)`。`p14` 是干净的包级验证：

```ct
struct sql_Select *q = "users".select("id, name")
    .where("status = ? AND age > ?", 1, 18)
    .whereRaw("deleted_at IS NULL")
    .groupBy("role")
    .orderBy("id DESC")
    .limit(10)
    .offset(20);
```

工作树版编译、链接、运行通过，输出：

```text
toSql=132 status=0 message=ok
sql=SELECT id, name FROM users WHERE (status = ? AND age > ?) AND (deleted_at IS NULL) GROUP BY role ORDER BY id DESC LIMIT 10 OFFSET 20
params=2 kind0=1 kind1=1
```

**结论：链式成立；类型宿主方法可以返回 `.shim.c` 里定义的结构体指针；返回值可以继续作接收者。**

### 3.3 变参：类型宿主上的 `...`

探针：`/tmp/sqlprobe/p1`、`/tmp/sqlprobe/p3`

`p1` 验证语法：

```ct
pub trait Fmt { int probePrintf(...); }
map trait Fmt for type char * { probePrintf(s, ...) -> printf(s, ...); }
```

- 发布版：`inc/probe/entry.m.ct:3:28: error: expected a type (tok 4 '...')`
- 工作树版：编译通过，`"lit %d\n".probePrintf(7);` 运行输出 `lit 7`。

`p3` 把变参接到 builder 链上：

```ct
pub trait Chain { int where(char *cond, ...); }
map trait Chain for type struct probe_Builder * {
    where(b, cond, ...) -> probe_builder_where(b, cond, ...);
}
```

调用：

```ct
b->where("id = ?", 1, 2, 3);
```

生成的 C：

```c
probe_builder_where((b), ("id = ?"), (1), (2), (3));
```

**关键结论：变参在语法上可用，但转发是“原样透传”，没有类型标签、没有实参个数。** 如果被调函数签名是 `(b, cond, int n, ...)`，那么 `1` 会被当成 `n`，`2/3` 才是 varargs；调用方无法在不额外传类型/个数的情况下安全地区分 `1`、`"bob"`、`2.5`。

### 3.4 多态/重载：同名方法接不同实参形态

探针：`/tmp/sqlprobe/p4`

```ct
pub trait A { int where(char *c); }
map trait A for type struct probe_Builder * { where(b, c) -> probe_where1(b, c); }

pub trait B { int where(char *c, int x); }
map trait B for type struct probe_Builder * { where(b, c, x) -> probe_where2(b, c, x); }
```

结果：

```text
main.ct:7:1: error: ambiguous type-host map for method 'where' on type 'struct probe_Builder *': 'inc/probe/entry.m.ct' and 'inc/probe/entry.m.ct'
```

**结论：CT 不支持按实参形态重载。** 同一宿主类型上，同名方法只能有一个 map；不同 arity 也会被判歧义。可行的做法：

- 不同语句类型用不同宿主（`sql_Select *` / `sql_Update *`），这样同名 `where` 可以分别存在；
- 同一宿主上，用不同方法名（`where` / `whereRaw` / `orderBy` / `orderByDesc`）；
- 变参方法本身可以接受 0..N 个额外实参，但 C 侧仍要靠宏恢复类型/个数，不能靠 CT 重载。

### 3.5 求值时机与字符串归属

探针：`/tmp/sqlprobe/p2`、`/tmp/sqlprobe/p14`

`p2` 的链式生成 C 是嵌套调用，说明 `select(...)` 返回的是**句柄**而不是最终字符串：

```c
probe_builder_select(("my_table"), ("id, username"))
```

如果 `select()` 返回最终字符串，`.where()` 就只能去解析这段字符串，既脆弱又无法处理参数顺序。因此设计选择：

- 入口返回 heap 上的 `struct sql_Select *`；
- 链式方法只改状态；
- `toSql(out, cap)` / `sql()` 延迟渲染；
- builder 持有内部缓冲，`sql()` 返回借用指针，`toSql` 写入调用方 buffer，`free()` 显式释放。

这与 std 的现有约定一致：`std::vec` 的 `free()`、`std::str` 的“输出 buffer + capacity”都是显式生命周期，没有 Drop。

### 3.6 与 `std::io`、`std::str`、`std::vec` 的复用

探针：`/tmp/sqlprobe/p11`

在同一程序里 import `sql::entry`、`std::str::String`、`std::vec::Vec`、`std::io::Io`，运行输出：

```text
sql=SELECT id, name FROM users WHERE id = ? AND name = ? len=52 params=2
vec len=1 first=SELECT id, name FROM users WHERE id = ? AND name = ?
```

编译命令包含 `std/std/str`、`Vec.c`、`Io.c`，与 `sql.shim.c` 一起链接成功。

**结论：**

- `std::str::String` 的 `len/copy/compare/...` 可以直接作用在 `q->sql()` 返回的 `char *` 上（复用 std 的方法，`sql` 不再新增同名 `len`）；
- `std::vec::VecStr` 可以收集多条 SQL（它只存借用指针，不拥有字符串）；
- `std::io::printf` 可以用来调试/打印，但 `sql` 包自身不依赖 std；
- `std::vec` 没有 `VecDouble`，参数又是异构的，所以 builder 内部不复用 `VecStr/VecInt`，而是自己维护 `sql_value` 数组。

### 3.7 为什么不用 `void *` 宿主

**前提已变**：`png` 的 `Handle` 曾经挂在 `void *` 上，这是早期设计“避让 png”的直接理由；当前代码已改为 [map trait Handle for type PngReader *](file:///Users/samxx/vscspace/ct-pkg/png/png/Png.m.ct#L46)，`void *` 在 ct-pkg 的 `.m.ct` 里暂时没有包占用。因此不能再把“避开 png 的具体占用”当成理由。

选命名 struct 宿主这个结论仍然成立，理由换成下面五条：

1. **`void *` 是全局共享的文本宿主键**。类型宿主解析只看类型拼写，不按包作用域隔离；任何包写 `for type void *` 都进入同一个键。任何 `void *` 值（文件句柄、分配器指针、opaque 库句柄）都能调用这些方法，SQL builder 也会被别的包的方法污染。即使今天没有占用者，这也是把方法暴露给一个无限集合的类型。
2. **命名 struct 保留语句类型区分**。`struct sql_Select *`、`struct sql_Insert *`、`struct sql_Update *`、`struct sql_Delete *` 是四个不同的键：`select` 链上不会出现 `set`/`values`，`insert` 链上不会出现 `groupBy`。用 `void *` 则所有语句类型共享一套方法，编译期完全失去这层约束。
3. **可读性与诊断**。接收者类型直接出现在 trait 签名、生成的 C 和错误信息里；`void *` 只剩“某个指针”，调用点看不出是 Select 还是 Insert。
4. **不依赖具体包的占用情况**。今天 `void *` 空闲，明天任何包都可以占用；命名 struct 让 sql 与 png 或未来包的宿主选择解耦。
5. **结构体 tag 稳定**。结构体定义在 `Sql.shim.c`（纯 C），tag 是手写名字，不依赖 CT 的 mangling；消费者只看到 shim.h 里的前置声明。

早期探针 `/tmp/sqlprobe/p5`、`p9` 用旧版 png 的 `void *` 映射做对照：同名 `free()` 会 `ambiguous`，不撞名时同一个 `void * h` 可以同时调用 `png_width` 和 `sql_where`。它们证明的是共享 `void *` 的后果，不是 png 现状；当前 png 已迁到 `PngReader *`。保留这两条记录，是因为它们说明了第 1 条理由的实际形态。

代价是四类句柄和内部共享 `sql_Query` 的样板；这个代价换的是编译期语句类型隔离和全局命名空间卫生，值得。

### 3.8 `.shim.c` 随 `.m.ct` 自动编译

探针：`/tmp/sqlprobe/p7`、`/tmp/sqlprobe/p14`

`p7` 只 import `sql::entry`，`ct build` 的 cc 行自动带上 `entry.shim.c`：

```text
'cc' -std=c99 -I 'inc/sql' -I 'out' 'out/main.c' 'inc/sql/entry.shim.c' '-o' 'a.out'
```

`p14` 的真实包布局（`pkg/sql/sql.m.ct` + `sql.shim.h` + `sql.shim.c`）同样自动编译：

```text
'cc' -std=c99 -std=c11 -Wall -Wextra -I './sql' -I 'build' 'build/main.c' './sql/sql.shim.c' '-o' 'a.out'
```

**结论：一个映射单元 + 一个 `.shim.h` + 一个 `.shim.c`，消费者只 import 一次，shim.c 自动参与编译/链接。**

补测（本地 `ct get` / `ct install` / 索引路径，2026-11-07）——探针 `/tmp/sqlprobe2/sqlpkg`、`/tmp/sqlprobe2/consumer`、`/tmp/sqlprobe2/index-consumer`：

```text
$ ct get file:///tmp/sqlprobe2/sqlpkg
vendored 1 package(s)
$ ct install
ct build -I vendor/sql --cflags '-std=c11' <unit>.ct -o build
$ /tmp/ctprobe/ct build -I vendor/sql --cflags '-std=c11' src/main.ct -o build/vendored
'cc' -std=c99 -std=c11 -I 'vendor/sql/sql' -I '.ctbuild' '.ctbuild/main.c' 'vendor/sql/sql/Sql.shim.c' '-o' 'build/vendored'
$ ./build/vendored
n=62 sql=SELECT id, name FROM users WHERE id = ? AND deleted_at IS NULL params=0
$ nm -g build/vendored | grep sql_select_
000000010000054c T _sql_select_new
0000000100000644 T _sql_select_where
0000000100000830 T _sql_select_where_raw
000000010000088c T _sql_select_to_sql
0000000100000cd4 T _sql_select_sql
0000000100000d24 T _sql_select_param_count
0000000100000d38 T _sql_select_free
```

索引路径用本地索引文件 + 本地 git tag 复现：

```text
$ cat /tmp/sqlprobe2/index
sql 0.1.0 file:///tmp/sqlprobe2/indexpkg v0.1.0 sha256:7ddf3bcbd8ce72a6f25d35b44056c1b559ce1f14a94db0a84bca125d9b000073
$ ct get --index /tmp/sqlprobe2/index sql@^0.1.0
vendored 1 package(s)
$ cat ct.lock
sql 0.1.0 git 8518fa164f6a1640e94a18b757befb4eeae421af sha256:7ddf3bcbd8ce72a6f25d35b44056c1b559ce1f14a94db0a84bca125d9b000073
$ ct install
ct build -I vendor/sql --cflags '-std=c11' <unit>.ct -o build
$ /tmp/ctprobe/ct build -I vendor/sql --cflags '-std=c11' src/main.ct -o build/indexed
'cc' -std=c99 -std=c11 -I 'vendor/sql/sql' -I '.ctbuild' '.ctbuild/main.c' 'vendor/sql/sql/Sql.shim.c' '-o' 'build/indexed'
$ ./build/indexed
n=62 sql=SELECT id, name FROM users WHERE id = ? AND deleted_at IS NULL params=0
```

两条路径都确认：`ct install` 只打印 `-I vendor/sql`（包根），`ct build` 自动补 `-I vendor/sql/sql`，并把 `Sql.shim.c` 编进命令；`nm` 能看到 shim 的全部符号。**注意**：`ct get` / `ct install` 由发布版 `v0.19.1` 执行即可，但 `ct build` 必须用工作树版（含变参 trait）；发布版在 `Sql.m.ct` 的 `...` 处报错。真实远程索引/网络下载未测，本地 `--index` + git tag 已覆盖索引解析、vendor 与摘要校验。

### 3.9 `_Generic` 宏在调用点捕获类型

探针：`/tmp/sqlprobe/p6`、`/tmp/sqlprobe/p8`、`/tmp/sqlprobe/p12`

`p8` 的调用：

```ct
struct sql_Builder *b = "users".select("id, name")
    .where("id = ? AND name = ?", 7, "bob");
```

运行输出：

```text
SELECT id, name FROM users WHERE id = ? AND name = ?
params=2
```

`p12` 验证 1..4 个参数，混合 `int` / `double` / `char *`，全部捕获成功。`p8` 用 `--cflags '-std=c11 -Wall -Wextra'` 构建零警告：

```text
'cc' -std=c99 -std=c11 -Wall -Wextra -I 'inc/sql' -I 'out2' 'out2/main.c' 'inc/sql/entry.shim.c' '-o' 'a.out'
```

补测（2026-11-07，`/tmp/sqlprobe2/generic/t.c`，真实编译器逐条跑）：

```text
$ cc -std=c99 -Wall -Wextra t.c -o t_c99
cc_exit=0
（无任何输出）
$ ./t_c99
int kind=1 i=7 | text kind=3 i=1
run_exit=0

$ cc -std=c11 -Wall -Wextra t.c -o t_c11
cc_exit=0
（无任何输出）
$ ./t_c11
int kind=1 i=7 | text kind=3 i=1
run_exit=0

$ cc -std=c99 -Wall -Wextra -pedantic t.c -o t_pedantic
t.c:16:19: warning: '_Generic' is a C11 extension [-Wc11-extensions]
t.c:17:19: warning: '_Generic' is a C11 extension [-Wc11-extensions]
2 warnings generated.
cc_exit=0

$ cc -std=c99 -Wall -Wextra -pedantic-errors t.c -o t_pedantic_err
t.c:16:19: error: '_Generic' is a C11 extension [-Werror,-Wc11-extensions]
t.c:17:19: error: '_Generic' is a C11 extension [-Werror,-Wc11-extensions]
2 errors generated.
cc_exit=1
```

同一份源码换编译器复核：

```text
$ /opt/homebrew/opt/llvm/bin/clang -std=c99 -Wall -Wextra t.c -o t_hb_c99
cc_exit=0
$ ./t_hb_c99
int kind=1 i=7 | text kind=3 i=1

$ /opt/homebrew/bin/gcc-16 -std=c99 -Wall -Wextra t.c -o t_gcc_c99
cc_exit=0
$ ./t_gcc_c99
int kind=1 i=7 | text kind=3 i=1

$ /opt/homebrew/bin/gcc-16 -std=c99 -Wall -Wextra -pedantic t.c -o t_gcc_ped
t.c:8:20: warning: ISO C99 does not support '_Generic' [-Wpedantic]
t.c:8:20: warning: ISO C99 does not support '_Generic' [-Wpedantic]
cc_exit=0
```

再用设计里完整的 `SQL_VAL` 宏（含全部整数、`float`/`double`、`char *`/`const char *`、`default`）跑 `int`/`double`/字符串/`char` 四种值：

```text
$ cc -std=c99 -Wall -Wextra sql_val.c -o sql_val_c99
cc_exit=0
$ ./sql_val_c99
1 2 3 1
$ cc -std=c11 -Wall -Wextra sql_val.c -o sql_val_c11
cc_exit=0
$ ./sql_val_c11
1 2 3 1
$ /opt/homebrew/bin/gcc-16 -std=c99 -Wall -Wextra sql_val.c -o sql_val_gcc
cc_exit=0
$ ./sql_val_gcc
1 2 3 1
```

**结论：**

- 用 `_Generic` 在宏展开时给每个实参选 `sql_v_int` / `sql_v_dbl` / `sql_v_str`，可以恢复静态类型；
- 字符串字面量需要 `((x) + 0)` 强制数组到指针退化，否则 `_Generic` 看到的是 `char[N]`；
- **`-std=c99 -Wall -Wextra` 下 `_Generic` 既不报错也不告警**（Apple clang 21、Homebrew clang 23、GCC 16 均实测，`cc_exit=0`）；只有 `-pedantic` 才出现 `C11 extension` / `ISO C99 does not support '_Generic'` 告警，`-pedantic-errors` 才失败（`cc_exit=1`）。所以当前门禁不会因 c99 失败；
- `cflags = -std=c11` 仍然保留：它是 `_Generic` 的标准归属，且只影响 `sql` 包的消费者，不改变任何现有包；如果消费者自己再传 `-std=c99`，在 clang/GCC 上仍可编译，但属于扩展用法，文档要说明；
- `ct build --cflags` 追加在默认 `-std=c99` 之后，实测 cc 行是 `-std=c99 -std=c11 ...`，后者生效。

### 3.10 方法名与关键字

探针：`/tmp/sqlprobe/p10`、`/tmp/sqlprobe/p13`

已验证可用的方法名：

`select`、`insert`、`update`、`delete`、`where`、`whereRaw`、`groupBy`、`orderBy`、`limit`、`offset`、`set`、`values`、`toSql`、`paramInt`、`paramDouble`、`paramText`、`paramCount`、`paramKind`、`status`、`message`、`sql`、`free`。

注意 `delete` 在 CT 里可以作为方法名（映射到 `sql_delete_new`，不生成 C 的 `delete` 符号）。

另一个实测细节：**类型宿主的接收者不计入 trait 方法签名**。写 `update(char *t)` 会被判“binding 有 1 个参数但方法 `update` 也取 1 个参数”；正确写法是 `update()` + recipe `update(t) -> sql_update_new(t)`。

### 3.11 探针汇总

| # | 问题 | 探针 | 工作树版结果 | 发布版结果 |
| --- | --- | --- | --- | --- |
| 1 | 字面量接收者 | `p1b` | 通过 | `method receiver must be...` |
| 2 | 链式 + 返回结构体 | `p2`、`p14` | 通过 | 受 1 限制 |
| 3 | 类型宿主变参 | `p1`、`p3` | 语法通过，类型被擦除 | `expected a type (tok 4 '...')` |
| 4 | 同名方法不同实参 | `p4` | `ambiguous type-host map` | 同 |
| 5 | 求值/所有权 | `p2`、`p14` | 句柄 + 延迟渲染可行 | 受 1 限制 |
| 6 | std 复用 | `p11` | `len/VecStr/Io` 均通过 | 受 1 限制 |
| 7 | 为什么不用 `void *` | `p5`、`p9` | 同名歧义；不撞名则类型混淆；当前 png 已迁到 `PngReader *` | 结论不变，理由改为全局宿主污染 |
| 8 | `.shim.c` 自动编译 | `p7`、`p14` | 通过 | 同 |
| 9 | `_Generic` 捕获 | `p6`、`p8`、`p12` | 1..4 参数混合类型通过 | 受 1/3 限制 |
| 10 | 方法名/关键字 | `p10`、`p13` | 通过 | 受 1/3 限制 |
| 11 | `_Generic` 在 c99 门禁下 | `/tmp/sqlprobe2/generic` | c99/c11 均零告警 | 同 |
| 12 | `SQL_NARG` 0/1/2/3 | `/tmp/sqlprobe2/narg` | 经典 0→1；`__VA_OPT__` 0→0 | 同 |
| 13 | 0 实参变参方法 | `/tmp/sqlprobe2/ctnarg` | 生成 `probe_where((b), (cond))` | 受 1/3 限制 |
| 14 | `where0` 与变参共存 | `/tmp/sqlprobe2/ctnarg` | 不同方法名通过 | 受 1/3 限制 |
| 15 | `where(char*)` + `where(char*, ...)` | `/tmp/sqlprobe2/ctover` | `ambiguous type-host map` | 同 |
| 16 | `ct get` / `ct install` / 索引 + shim | `/tmp/sqlprobe2/consumer`、`index-consumer` | 通过；shim.c 入链接 | 管理命令可用，编译受 1/3 限制 |

### 3.12 `SQL_NARG` 的 0/1/2/3 实参复核

探针：`/tmp/sqlprobe2/narg/narg.c`。

经典写法：

```c
#define SQL_NARG_(a,b,c,d,e,f,g,h,N,...) N
#define SQL_NARG(...) SQL_NARG_(__VA_ARGS__, 8,7,6,5,4,3,2,1,0)
```

`cc -std=c99 -E -P narg.c` 的关键展开（其余是 stdio 头）：

```text
printf("classic: 0=%d 1=%d 2=%d 3=%d\n",
       1, 1, 2, 3);
printf("va_opt : 0=%d 1=%d 2=%d 3=%d\n",
       0, 1, 2, 3);
```

编译运行：

```text
$ cc -std=c99 -Wall -Wextra narg.c -o narg_c99
cc_exit=0
$ ./narg_c99
classic: 0=1 1=1 2=2 3=3
va_opt : 0=0 1=1 2=2 3=3
```

`-std=c11`、`-std=c23` 以及 GCC 16 的 c99/c11/c23 结果一致：

```text
classic: 0=1 1=1 2=2 3=3
va_opt : 0=0 1=1 2=2 3=3
```

第二组宏是 C23 的 `__VA_OPT__` 写法：

```c
#define SQL_NARG2(...) SQL_NARG2_(__VA_OPT__(,) __VA_ARGS__)
#define SQL_NARG2_(...) SQL_NARG2_I(__VA_ARGS__, 8,7,6,5,4,3,2,1,0)
#define SQL_NARG2_I(_0,_1,_2,_3,_4,_5,_6,_7,_8,N,...) N
```

它在 `-Wall -Wextra` 下零告警，但 `-pedantic` 会明确告警：

```text
$ cc -std=c11 -Wall -Wextra -pedantic narg.c -o narg_ped
narg.c:17:12: warning: passing no argument for the '...' parameter of a variadic macro is a C23 extension [-Wvariadic-macro-arguments-omitted]
cc_exit=0

$ gcc-16 -std=c11 -Wall -Wextra -pedantic narg.c -o narg_gcc_ped
warning: '__VA_OPT__' is not available until C23 [-Wpedantic]
warning: ISO C99 requires at least one argument for the '...' in a variadic macro [-Wpedantic]
cc_exit=0
```

CT 侧补测（工作树版）：类型宿主上 0 个额外实参可以调用变参方法，生成的是普通 C 调用：

```ct
b->where("y IS NULL");   /* 0 个值 */
b->where("x = ?", 1);    /* 1 个值 */
```

```text
$ /tmp/ctprobe/ct build -I inc main.ct -o build/narg
'cc' -std=c99 -I 'inc/probe' -I '.ctbuild' '.ctbuild/main.c' '-o' 'build/narg'
where0=0 where1=1 where0args=1
```

生成 C：

```c
int a = probe_where0((b), ("x IS NULL"));
int c = probe_where((b), ("x = ?"), (1));
int d = probe_where((b), ("y IS NULL"));
```

同一个宿主上 `where0(char *)` 与 `where(char *, ...)` 用**不同方法名**可以共存；但把 `where(char *)` 和 `where(char *, ...)` 用**同一个名字**挂到同一宿主，仍然失败：

```text
main.ct:7:1: error: ambiguous type-host map for method 'where' on type 'struct probe_Builder *': 'inc/probe/entry.m.ct' and 'inc/probe/entry.m.ct'
```

把 `__VA_OPT__` 计数宏接到 CT 生成的调用上，0/1/2/3 个值全部正确：

```text
$ /tmp/ctprobe/ct build -I inc --cflags '-Wall -Wextra' main.ct -o build/narg2
'cc' -std=c99 -Wall -Wextra -I 'inc/probe' -I '.ctbuild' '.ctbuild/main.c' '-o' 'build/narg2'
0=0 1=1 2=2 3=3
```

**结论：**

- 经典 `SQL_NARG` 对 0 个实参确实返回 1，不能区分 0/1；`whereRaw` 因此仍有存在价值。
- `__VA_OPT__` 写法可以返回 0，且 clang/GCC 在 c99/c11 下只在 `-pedantic` 时才告警；如果团队接受 C23 预处理器扩展，就可以让 `where(cond)` 支持 0..8 个值，并去掉 `whereRaw`。
- 阶段一建议**保留 `whereRaw`**：它不依赖 C23 宏，语义也更明确（“这一段没有绑定值”）；`where(cond)` 的零值形式列为可选/二期项。
- “不同宿主”在技术上可以避开同名歧义，但零值条件仍要回到同一个 Select 链上，需要额外的转换方法，链式体验更差；不同方法名（`where` / `whereRaw`）是更简单的解。

---

## 4. 目标 API 全貌

### 4.1 第一期：SELECT

```ct
struct sql_Select *q = "users".select("id, name")
    .where("status = ? AND age > ?", 1, 18)
    .whereRaw("deleted_at IS NULL")
    .groupBy("role")
    .orderBy("id DESC")
    .limit(10)
    .offset(20);
```

方法表：

| 宿主 | 方法 | 返回 | 说明 |
| --- | --- | --- | --- |
| `char *` | `select(char *fields)` | `struct sql_Select *` | 表名是接收者；`fields` 是 SELECT 列表片段 |
| `struct sql_Select *` | `where(char *cond, ...)` | `struct sql_Select *` | 追加一个带绑定值的条件；条件是 `?` 占位符 |
| `struct sql_Select *` | `whereRaw(char *cond)` | `struct sql_Select *` | 追加一个无绑定值的条件；用于 `IS NULL`、`1 = 1` 等 |
| `struct sql_Select *` | `groupBy(char *expr)` | `struct sql_Select *` | 追加 GROUP BY 表达式 |
| `struct sql_Select *` | `orderBy(char *expr)` | `struct sql_Select *` | 追加 ORDER BY 表达式（可含 `DESC`） |
| `struct sql_Select *` | `limit(int n)` | `struct sql_Select *` | 单个值，覆盖前一次；`n >= 0` |
| `struct sql_Select *` | `offset(int n)` | `struct sql_Select *` | 单个值，覆盖前一次；`n >= 0`；依赖 `limit` |
| `struct sql_Select *` | `toSql(char *out, int cap)` | `int` | 渲染并复制到调用方 buffer；返回字节数或 -1 |
| `struct sql_Select *` | `sql()` | `char *` | 返回内部渲染缓冲的借用指针；错误时返回 `""` |
| `struct sql_Select *` | `paramCount()` | `int` | 绑定参数个数 |
| `struct sql_Select *` | `paramKind(int i)` | `int` | `-1` 越界；`0` null；`1` int；`2` double；`3` text |
| `struct sql_Select *` | `paramInt(int i, long *out)` | `int` | 0 成功，-1 失败 |
| `struct sql_Select *` | `paramDouble(int i, double *out)` | `int` | 0 成功，-1 失败 |
| `struct sql_Select *` | `paramText(int i)` | `char *` | 借用；类型/下标不符返回 NULL |
| `struct sql_Select *` | `status()` | `int` | 0 成功，负值错误码 |
| `struct sql_Select *` | `message()` | `char *` | 静态错误文本，不需要 free |
| `struct sql_Select *` | `free()` | `void` | 释放 builder 和全部内部资源 |

> 零值条件在阶段一统一写 `whereRaw("deleted_at IS NULL")`。如果不想要 `whereRaw`，可把计数宏换成 `__VA_OPT__` 版本，让 `where("deleted_at IS NULL")` 也工作；该写法已实测可用，但因属 C23 预处理器扩展，列为评审项（§11 第 2 项）。

完整示例：

```ct
#include <stdio.h>
import sql::Sql;

int main(void)
{
    struct sql_Select *q = "users".select("id, name")
        .where("status = ? AND age > ?", 1, 18)
        .whereRaw("deleted_at IS NULL")
        .orderBy("id DESC")
        .limit(10)
        .offset(20);

    char buf[512];
    int n = q->toSql(buf, 512);
    if (n < 0) {
        printf("error: %s\n", q->message());
    } else {
        printf("sql: %s\n", buf);
    }

    long status = 0;
    if (q->paramInt(0, &status) == 0) {
        printf("status=%ld\n", status);
    }

    q->free();
    return 0;
}
```

### 4.2 第二期：INSERT / UPDATE / DELETE

```ct
"users".insert("name, age").values("Bob", 30);
"users".update().set("name = ?", "Bob").where("id = ?", 1);
"users".delete().where("id = ?", 1);
```

方法表（第二期）：

| 宿主 | 方法 | 返回 | 说明 |
| --- | --- | --- | --- |
| `char *` | `insert(char *columns)` | `struct sql_Insert *` | 列名片段 |
| `struct sql_Insert *` | `values(...)` | `struct sql_Insert *` | 追加一行值；参数用 `_Generic` 捕获 |
| `char *` | `update()` | `struct sql_Update *` | 进入 UPDATE 构建 |
| `struct sql_Update *` | `set(char *expr, ...)` | `struct sql_Update *` | 追加 SET 片段和值 |
| `struct sql_Update *` | `setRaw(char *expr)` | `struct sql_Update *` | 追加无绑定值的 SET 片段 |
| `char *` | `delete()` | `struct sql_Delete *` | 进入 DELETE 构建 |
| `struct sql_Update *` / `struct sql_Delete *` | `where(char *cond, ...)` | 同宿主 | 追加条件 |
| `struct sql_Update *` / `struct sql_Delete *` | `whereRaw(char *cond)` | 同宿主 | 追加无值条件 |
| `struct sql_Update *` / `struct sql_Delete *` | `allowAll()` | 同宿主 | 显式允许无 WHERE 的 UPDATE/DELETE |

第二期的安全护栏：

- UPDATE 没有 SET → `SQL_E_ARG`；
- UPDATE/DELETE 没有 WHERE 且没有 `allowAll()` → `SQL_E_STATE`；
- INSERT 的列数（按逗号粗分）与值的个数不一致 → `SQL_E_ARG`；
- UPDATE 的参数顺序固定为 **SET 先、WHERE 后**（见 §7.3）。

### 4.3 求值时机与所有权

- **入口返回句柄，不返回字符串。** 返回字符串会让 `.where()` 无法继续，也会让参数顺序失控。
- **链式方法只记录状态。** 每次修改都让渲染缓存失效。
- **`toSql(out, cap)`**：渲染到内部缓存，再复制到调用方 buffer；返回写入的字节数（不含 NUL），失败返回 -1。
- **`sql()`**：返回内部缓存的借用指针，生命周期到下一次修改或 `free()`；错误时返回 `""`，用 `status()` 取原因。
- **`free()`**：释放表名、字段、条件、SET、参数数组、文本参数副本、渲染缓存和句柄本身；`free(NULL)` 是 no-op。
- **没有 Drop。** 与 `std::vec` 的 `free()`、`std::str` 的调用方 buffer 一致。

输出 buffer 太小的两步用法：

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

### 4.4 错误模型

`status()` 返回：

| 码 | 名称 | 含义 |
| ---: | --- | --- |
| 0 | `SQL_E_OK` | 成功 |
| -1 | `SQL_E_OOM` | 内存分配失败 |
| -2 | `SQL_E_STATE` | 状态非法：offset 无 limit、UPDATE/DELETE 无 WHERE 且未 `allowAll` |
| -3 | `SQL_E_ARG` | 参数非法：负 limit/offset、空表名/字段、INSERT 列数与值数不一致 |
| -4 | `SQL_E_SPACE` | 输出 buffer 太小 |
| -5 | `SQL_E_PARAM` | 参数下标/类型不符，或占位符数量与绑定值数量不一致 |

- 链式方法返回 `self`；只有句柄本身分配失败时 `select/insert/update/delete` 可能返回 NULL。后续方法对 NULL 做保护并返回 NULL，不会崩溃。
- `toSql` 失败返回 -1；`sql()` 失败返回 `""`；`message()` 返回静态文本。
- 不使用异常；CT 没有异常。

---

## 5. 与 migears-sql 的差异及原因

| 维度 | migears-sql | CT `sql`（本设计） | 原因 |
| --- | --- | --- | --- |
| 入口 | `$sql->select('users')->fields([...])` | `"users".select("id, name")` | CT 支持字面量接收者；表名直接作接收者，省掉 `from()` |
| 参数形态 | 命名占位符 `:name` + 关联数组 | 位置 `?` + 变参值 | CT 没有 PHP 关联数组；位置参数更适合变参捕获 |
| WHERE 语义 | `where()` 替换上一次条件 | `where()` 追加，以 ` AND ` 连接 | 链式 builder 更直观；没有数组需要合并；避免“最后一次悄悄覆盖” |
| 无值条件 | `where('a IS NULL', [])` | `whereRaw('a IS NULL')` | 经典 `SQL_NARG` 无法区分 0 个和 1 个实参；`__VA_OPT__` 可以但属 C23，阶段一拆成独立方法更稳 |
| 语句类型 | 4 个 PHP class | 4 个 C 句柄类型，共享内部 `sql_Query` | C 没有继承；不同句柄类型保留方法隔离 |
| 执行 | `execute/single/count/paginate` | 无 | 构建与执行分离；数据库执行留给驱动适配层 |
| 错误 | 异常 | 状态码 + `message()` | CT 没有异常 |
| 标识符 | 反引号 + `^\w+$` 校验 | 第一期不引用、不校验 | 引用是方言相关的；第一期保持方言中立，文档声明“可信片段” |
| UPDATE/DELETE 护栏 | `execute()` 里拒绝无 WHERE | `toSql()` 返回 `SQL_E_STATE` | CT 没有 `execute()`，所以护栏要提前到渲染 |
| 方言 | PDO 抽象 | 第一期统一 `?` | builder 更底层；方言留到第二期 |
| 日志 | PSR-3 logger | 无 | 不引入依赖；调用方用 `std::io` |
| `filter/bind` | 数组合并 | 多次 `where` / `whereRaw` | CT 没有关联数组；链式追加已够用 |
| `orderBy` | `orderBy($column, $direction)` | `orderBy(char *expr)` | 没有重载；原始表达式可一次写多列/方向 |
| `toSql` 返回值 | PHP string（GC 管理） | `toSql(out, cap)` + `sql()` 借用 | 显式所有权；无隐藏分配；与 std 一致 |
| 分页 | `paginate()` 返回页对象 | 无 | CT 没有泛型/行集合；调用方自行组合 |

---

## 6. 目录、单元与 shim 落点

### 6.1 包布局

```text
sql/
├── ct.pkg
├── LICENSE
├── README.md                 # 后补：一屏简版
├── docs/
│   ├── design.md             # 本文
│   └── tutorial.md           # 后补：详版
├── sql/                      # namespace sql 的根
│   ├── Sql.m.ct              # 映射单元：traits + maps
│   ├── Sql.shim.h            # C API、sql_value、宏
│   └── Sql.shim.c            # C 实现：结构体、缓冲、参数、渲染、free
└── demo/
    └── main.ct               # 后补：目标写法实跑
```

- 目录 `sql/sql/` 对应 `namespace sql;`，满足“目录 = 命名空间”。
- 消费者写 `import sql::Sql;`。
- `Sql.shim.h` / `Sql.shim.c` 与 `Sql.m.ct` 同基名同目录；shim 不受 namespace 规则约束。
- 第一期一个单元承载 SELECT；第二期如果文件变大，再按 `Select.m.ct` / `Insert.m.ct` / ... 拆分，并评估共享 C core 的重复符号问题。

### 6.2 每个文件的职责

| 文件 | 职责 | 不做什么 |
| --- | --- | --- |
| `sql/sql/Sql.m.ct` | `namespace sql;`；`Entry`/`Select`（第二期加 `Insert`/`Update`/`Delete`）trait 与 map；所有 recipe 都调用 shim 里的 C 名/宏 | 不定义结构体、不写 C 实现、不 import `.h.ct` |
| `sql/sql/Sql.shim.h` | 前置声明句柄；定义 `sql_value`、状态码、C 函数原型、`_Generic`/参数个数宏 | 不定义 builder 结构体；不写可变状态 |
| `sql/sql/Sql.shim.c` | 定义 `struct sql_Query` 与四个句柄；动态 buffer、参数数组、渲染、护栏、`free` | 不写 `main`；不引入全局状态 |
| `sql/ct.pkg` | 包清单：name/version/licence/requires/cflags | 不写实现 |
| `sql/docs/tutorial.md` | 后补；按仓库 8 段骨架 | 本轮不写 |
| `sql/README.md` | 后补；一屏简版 | 本轮不写 |
| `sql/demo/main.ct` | 后补；目标写法实跑 | 本轮不写 |

### 6.3 为什么不用 `.h.ct`

这是本设计最重要的形态取舍，理由有三条：

1. **`.m.ct` 不能 import `.h.ct`。** 如果 builder 核心放在 `Builder.h.ct`，消费者只 import `sql::Sql`（`.m.ct`）时，`.h.ct` 不会被编译/链接；必须再 import `sql::Builder`，变成两个 import。
2. **mangling 耦合。** `.m.ct` 里要把返回类型写成 C tag。若结构体是 `namespace sql; pub struct Builder`，生成的 tag 是 `struct sql_Builder`（p2 观察到的 `probe_Builder` 同理）。这是编译器实现细节，写死在 `.m.ct` 里很脆。
3. **`.shim.c` 可以随 `.m.ct` 自动编译，且 tag 是手写的。** 消费者只 import 一次；结构体在 shim.h 里只前置声明，在 shim.c 里定义，对外保持不透明。`std::str::String` 已经采用“只有 `.m.ct` + shim、没有 `.h.ct`”的形态，它的注释明确说：类型宿主只能在 `.m.ct` 里映射，所以 C 面放在 shim 对里，不放在 `.h.ct`。

**代价**：builder 是自研状态对象，按仓库 OO 规范更像“原生 OO（`.h.ct` 内 trait + struct + impl）”；本设计把它实现成 `.shim.c` 里的 C 句柄，属于有理由的偏离。评审如果认为 OO 规范是硬门槛，备选方案是：

- **方案 B**：`Builder.h.ct` 做原生 OO core + `Sql.m.ct` 做字面量入口 facade；消费者 import 两个单元；facade 的返回类型写 `struct sql_Builder *`；veriadic 仍需 shim 宏。复杂度更高，用户体验更差，但 OO 形态更正统。

本设计推荐方案 A（`.m.ct` + `.shim.c`），把 OO 规范偏离作为评审点显式列出。

### 6.4 shim 要求

- 命名：`Sql.m.ct` ↔ `Sql.shim.h` ↔ `Sql.shim.c`，同目录同基名。
- `Sql.shim.h` 会被编译器自动包含到消费者的生成 `.c` 里；不要在 `.m.ct` 里重复 `#include`。
- `Sql.shim.c` 在消费者 import `sql::Sql` 时自动参与编译/链接；它 `#include "Sql.shim.h"`（同目录，编译器的 `-I` 指向单元目录）。
- shim 里只放 C 声明/宏/实现；`sql_value` 和状态码放 shim.h，结构体定义放 shim.c。
- 不使用全局可变状态；每个 builder 自带状态。
- `cflags = -std=c11`，保证 `_Generic`。

### 6.5 `ct.pkg`

```ini
name = sql
version = 0.1.0
licence = MIT
requires = ct >= 0.20.0
cflags = -std=c11
```

说明：

- `requires` 是计划值。当前发布版 `v0.19.1` 不支持字面量接收者/变参 trait，实际会在解析阶段失败；`requires` 只告警，不阻止安装，因此 README/tutorial 必须明确“需要 0.20.0 或包含该提交的编译器”。
- `cflags = -std=c11` 由 `ct install` 折进构建命令；直接 `ct build` 时需要显式 `--cflags '-std=c11'`。
- 无 `dep`、无 `ldflags`：只依赖 libc。
- 本轮不 bump 版本、不提交、不打 tag、不改 `index/`。

---

## 7. 内部结构（C core）

### 7.1 值类型

```c
typedef struct {
    int kind;          /* 0 null, 1 int, 2 double, 3 text */
    long long i;
    double d;
    char *s;           /* owned copy */
} sql_value;
```

- 整数统一存 `long long`，避免 `int/long/long long` 截断；
- 文本值在写入 builder 时 `strdup`，由 builder 释放；
- `kind` 供 `paramKind/paramInt/paramDouble/paramText` 分派；
- 第一期不支持 SQL NULL 作为绑定值；`NULL`/`0` 会按整数 0 绑定，`IS NULL` 用 `whereRaw`。

### 7.2 共享查询状态与四个句柄

```c
struct sql_Query {
    int stmt;                  /* SELECT / INSERT / UPDATE / DELETE */
    char *table;
    char *fields;              /* SELECT 列表 / INSERT 列名 */
    struct sql_buf where;      /* WHERE 片段文本 */
    sql_value *where_params;
    int nwhere, capwhere;
    struct sql_buf set;        /* UPDATE SET 片段文本 */
    sql_value *set_params;
    int nset, capset;
    sql_value *values;         /* INSERT 值 */
    int nvalues, capvalues;
    char *group;
    char *order;
    int limit, offset;
    int has_limit, has_offset;
    int allow_all;
    char *rendered;
    int status;
};

struct sql_Select { struct sql_Query q; };
struct sql_Insert { struct sql_Query q; };
struct sql_Update { struct sql_Query q; };
struct sql_Delete { struct sql_Query q; };
```

- 四个句柄类型只用于类型宿主区分；真正的状态在 `sql_Query`。
- `sql_Query` 是首成员，函数可以直接 `&s->q`；也可以让四个句柄 `typedef` 成不同 tag 的同一布局，但首成员更直观。
- 每个句柄的构造函数 `malloc` 包装结构，初始化 `sql_Query`；`free` 先释放内部资源再释放句柄。

### 7.3 参数顺序

这是位置参数最容易出错的地方：

```ct
"users".update()
    .where("id = ?", 1)       /* WHERE 参数：[1] */
    .set("name = ?", "Bob")   /* SET 参数：["Bob"] */
```

渲染后的 SQL 是：

```sql
UPDATE users SET name = ? WHERE (id = ?)
```

最终参数数组必须是 `["Bob", 1]`，即 **SET 先、WHERE 后**，而不是调用顺序。因此：

- UPDATE 维护 `set_params` 和 `where_params` 两个数组；
- 渲染时先输出 SET 片段，再输出 WHERE 片段；
- 最终对外暴露的参数数组按 `set_params + where_params` 拼接。

SELECT/DELETE 只有 WHERE 参数；INSERT 只有 VALUES 参数。

### 7.4 渲染顺序

- SELECT：`SELECT <fields> FROM <table> [WHERE ...] [GROUP BY ...] [ORDER BY ...] [LIMIT n] [OFFSET m]`
- INSERT：`INSERT INTO <table> (<columns>) VALUES (?, ...)`
- UPDATE：`UPDATE <table> SET ... [WHERE ...]`
- DELETE：`DELETE FROM <table> [WHERE ...]`

调用顺序不影响子句顺序；只有多个 `where`/`set`/`orderBy` 之间的追加顺序保留。

### 7.5 动态 buffer

`sql_buf`：

```c
struct sql_buf {
    char *p;
    size_t len;
    size_t cap;
};
```

- `sql_buf_add` / `sql_buf_addf` 按需 `realloc`，`vsnprintf` 两遍（先算长度，再写入）；
- 渲染缓存 `rendered` 是 `char *`，每次修改 builder 都 `free` 并置 0；
- 所有输入字符串（表名、字段、条件、SET、GROUP、ORDER、文本参数）都复制，builder 自包含；调用方可以立刻复用/释放自己的 buffer。

### 7.6 生命周期

```
"t".select(...)  ->  malloc 句柄 + 复制 table/fields
.where(...)      ->  追加条件/参数；rendered 失效
toSql/sql        ->  按需渲染到 rendered；toSql 再复制到调用方 buffer
free()           ->  释放所有 owned 内存和句柄
```

- 多次 `sql()` 返回同一个缓存指针；修改后指针失效。
- `toSql` 不转移所有权。
- 没有引用计数、没有 GC、没有 Drop。

---

## 8. 参数绑定的安全边界

### 8.1 值：只绑定，不拼接

```ct
q->where("name = ? AND age > ?", user_name, 18);
```

渲染结果永远是：

```sql
WHERE (name = ? AND age > ?)
```

`user_name` 只进入 `sql_value` 数组，不会出现在 SQL 文本里。即使 `user_name` 是 `"x' OR '1'='1"`，SQL 文本也不变；是否安全由调用方的数据库驱动绑定决定。

### 8.2 类型捕获：`_Generic` + 变参宏

shim.h 的核心宏（示意）：

```c
#define SQL_VAL(x) _Generic(((x) + 0), \
    char: sql_v_int, signed char: sql_v_int, unsigned char: sql_v_int, \
    short: sql_v_int, unsigned short: sql_v_int, \
    int: sql_v_int, unsigned int: sql_v_int, \
    long: sql_v_int, unsigned long: sql_v_int, \
    long long: sql_v_int, unsigned long long: sql_v_int, \
    float: sql_v_dbl, double: sql_v_dbl, \
    char *: sql_v_str, const char *: sql_v_str, \
    default: sql_v_int)((x) + 0)

#define SQL_NARG_(a,b,c,d,e,f,g,h,N,...) N
#define SQL_NARG(...) SQL_NARG_(__VA_ARGS__, 8,7,6,5,4,3,2,1,0)
#define SQL_CAT_(a,b) a##b
#define SQL_CAT(a,b) SQL_CAT_(a,b)
#define SQL_WHERE_1(s,cond,a,...) sql_select_where_impl((s), (cond), 1, (sql_value[]){ SQL_VAL(a) })
#define SQL_WHERE_2(s,cond,a,b,...) sql_select_where_impl((s), (cond), 2, (sql_value[]){ SQL_VAL(a), SQL_VAL(b) })
/* ... 3..8 ... */
#define sql_select_where(s,cond,...) SQL_CAT(SQL_WHERE_, SQL_NARG(__VA_ARGS__))(s, cond, __VA_ARGS__)
```

- CT 侧只看到 `sql_select_where(b, cond, ...)`；C 预处理器展开成带类型标签的数组。
- `((x) + 0)` 让字符串字面量从 `char[N]` 退化为 `char *`，否则 `_Generic` 匹配不到。
- 支持的类型：各种整数、`float`、`double`、`char *`、`const char *`；其他类型（结构体、函数指针、`long double`、`void *`）不在第一期支持范围。
- `where` 一次最多 8 个值（宏按 1..8 分派）；更多值拆成多次 `.where()`，每次追加 AND。
- 0 个值用 `whereRaw`；经典 `SQL_NARG` 无法区分 0 和 1（实测 `SQL_NARG()` 对 0 参数返回 1，见 §3.12）。如果改用 `__VA_OPT__` 计数宏，0 个值可以正确返回 0，`where(cond)` 就能走同一个方法；但 `__VA_OPT__` 是 C23 预处理器特性，`-pedantic` 会告警，因此阶段一仍把 `whereRaw` 作为可移植入口。
- 备选宏（阶段二可切换）：

  ```c
  #define SQL_NARG(...) SQL_NARG_(__VA_OPT__(,) __VA_ARGS__)
  #define SQL_NARG_(...) SQL_NARG_I(__VA_ARGS__, 8,7,6,5,4,3,2,1,0)
  #define SQL_NARG_I(_0,_1,_2,_3,_4,_5,_6,_7,_8,N,...) N
  #define SQL_WHERE_0(s,cond) sql_select_where_impl((s), (cond), 0, NULL)
  #define sql_select_where(s,cond,...) \
      SQL_CAT(SQL_WHERE_, SQL_NARG(__VA_ARGS__))(s, cond __VA_OPT__(,) __VA_ARGS__)
  ```

  该写法已在 `/tmp/sqlprobe2/ctnarg2` 中用 CT 生成代码实测 0/1/2/3 个值全部正确；是否采用由评审决定，见 §11 第 2 项。
- 文本值在 `sql_v_str` 里复制；builder 负责释放。

### 8.3 结构：可信 SQL 片段

第一期明确**不做**以下保护：

- 表名、列名、`GROUP BY` / `ORDER BY` 表达式、`whereRaw` 的条件文本，都是“可信 SQL 片段”；
- 不自动加反引号/双引号；
- 不校验 `^\w+$`；
- 不解析 SQL，不检查表达式语法。

原因：

- 引用符是方言相关的（MySQL 反引号、PostgreSQL/SQLite 双引号），第一期保持方言中立；
- 表名/列名通常来自代码常量或白名单，不属于“值”；
- 真正的标识符校验/引用放到第二期（`quoteIdent` / schema 白名单）。

安全契约：

- **值安全**：`where(cond, value...)` / `values(...)` / `set(expr, value...)` 的值只绑定，不拼接；
- **结构不安全**：`whereRaw`、`orderBy`、`groupBy`、表名、列名必须由开发者控制；禁止直接传用户输入；
- `limit/offset` 只接受 `int`，校验非负，渲染为十进制字面量，不接受字符串。

错误示例（文档中要显式标注为不安全）：

```ct
/* 不安全：用户输入进入结构片段 */
q->whereRaw(user_condition);
q->orderBy(user_column);
"users".select(user_fields);
```

正确示例：

```ct
/* 安全：用户输入只作为值 */
q->where("name = ?", user_name);
q->where("status = ?", user_status);
```

### 8.4 占位符数量检查

`where_impl` 会对条件文本做一次**尽力**扫描，统计 `?` 的个数并与绑定值个数比较：

- 跳过单引号字符串（含 `''` 转义）；
- 不解析双引号标识符、注释、美元引用；
- 不一致 → `SQL_E_PARAM`，`toSql` 返回 -1。

这是“没有数据库驱动时的早期报警”，不是 SQL 解析器。文档要说明：

- `where("note = '?'")` 这种条件里的 `?` 会被跳过（单引号内），不会误报；
- `whereRaw` 不做数量检查；
- 如果条件里有复杂的 SQL 片段，建议用 `whereRaw` 或拆成更小的 `where`。

---

## 9. 分期范围

### 9.1 第一期（本轮设计通过后实现）

- 包骨架：`ct.pkg`、`sql/sql/Sql.m.ct`、`Sql.shim.h`、`Sql.shim.c`；
- SELECT builder：
  - `"table".select(fields)`
  - `where(cond, ...)` / `whereRaw(cond)`
  - `groupBy` / `orderBy` / `limit` / `offset`
  - `toSql` / `sql` / `paramCount` / `paramKind` / `paramInt` / `paramDouble` / `paramText` / `status` / `message` / `free`
- 参数捕获：整数、浮点、文本；一次 `where` 最多 8 个值；文本复制；
- 护栏：offset 无 limit、空表名/字段、占位符数量不符、输出 buffer 不足；
- demo：目标写法 + 参数读取 + free；
- 文档：README 简版 + `docs/tutorial.md` 八段骨架；
- 测试：见 §12。

### 9.2 第二期

- INSERT / UPDATE / DELETE 的 typed handles；
- `values` / `set` / `setRaw` / `allowAll`；
- UPDATE/DELETE 无 WHERE 护栏；INSERT 列数/值数检查；
- `orWhere` / `orWhereRaw` / `having` / `join` / `orderByAsc` / `orderByDesc`；
- 方言：`generic` / `sqlite` / `mysql` / `postgres`，占位符 `?` ↔ `$n`，引用符；
- 标识符引用/白名单（`quoteIdent`）；
- 超过 8 个参数的宏扩展；
- SQL NULL 绑定；
- 如果 OO 规范要求，补 `.h.ct` 原生 OO facade（方案 B）。

### 9.3 明确不做

- 数据库连接/执行/事务/连接池；
- ORM、实体映射、迁移；
- 完整 SQL 解析/语法校验；
- 命名参数 `:name`（位置参数 + 变参已经是本设计的核心）；
- 日志/指标/审计；
- 自动分页对象（`paginate()`）。

---

## 10. 尚未验证或依赖未发布编译器能力的事项

1. **字面量接收者**：发布版 `ct v0.19.1` 直接拒绝；工作树版（HEAD `915c12c`）通过。`requires = ct >= 0.20.0` 是计划值，0.20.0 尚未发布。
2. **trait 方法尾部变参**：发布版报 `expected a type (tok 4 '...')`；工作树版（含未提交的 MapBuild/Parser 改动）通过。这个能力目前不是发布版的一部分。
3. **结构体返回 + 链式**：工作树版通过；发布版受 1 限制，未单独验证“变量接收者 + 结构体返回”在发布版上的行为。
4. **`_Generic`**：
   - `-std=c99 -Wall -Wextra` 实测零告警（Apple clang 21、Homebrew clang 23、GCC 16）；`-pedantic` 才告警（`C11 extension` / `ISO C99 does not support '_Generic'`）；
   - 建议 `cflags = -std=c11`；直接 `ct build` 需显式 `--cflags`；它不是当前门禁通过的前提，而是标准符合性选择；
   - Windows/MSVC 的 C11 支持不完整，未验证；如果 Windows 是目标，需要换类型标签方案或限制平台。
5. **0 个绑定值的 `where`**：经典 `SQL_NARG` 对 0 个实参返回 1，无法区分 0/1；`__VA_OPT__` 写法实测可以返回 0（`/tmp/sqlprobe2/narg`、`/tmp/sqlprobe2/ctnarg2`），但它是 C23 预处理器特性，`-pedantic` 会告警。第一期用 `whereRaw`；是否切换成单一 `where(cond, ...)` 见 §11 第 2 项。
6. **超过 8 个绑定值**：当前宏只展开 1..8；更多值需要拆分调用或扩展宏，未实现。
7. **SQL NULL / `long double` / 结构体值**：不在 `_Generic` 支持列表；未验证。
8. **`.shim.c` 的打包链路**：已验三条路径——`ct build -I <包根>`、`ct get file://` + `ct install` + 打印的 `ct build`、本地 `--index` + git tag + `ct install` + `ct build`；三条都确认 `Sql.shim.c` 进入 cc 命令、`nm` 能看到全部 shim 符号。仍未验证：真实远程索引/网络下载；发布版 `v0.19.1` 能执行 `ct get` / `ct install`，但编译含变参 trait 的 `Sql.m.ct` 会失败，需要 >= 0.20.0 的编译器。
9. **`ct check` / gate**：探针只用 `ct build`；包级 gate（语义 warning、C 编译器 warning、demo 输出、layout）尚未在 `sql` 包上跑过。
10. **线程安全**：无全局状态，理论上是每句柄独立；未做并发测试。
11. **性能**：未做基准；每次 builder 有若干 `malloc/realloc`，按“查询构建”场景可接受，但未量化。
12. **标识符校验/方言**：第一期不做；方言和引用符是第二期设计，未实现。

---

## 11. 评审需要确认的取舍

| # | 取舍 | 推荐 | 备选/代价 |
| ---: | --- | --- | --- |
| 1 | `where()` 追加还是替换 | **追加（AND）**，链式语义直观 | 替换（对齐 migears-sql）；代价是“最后一次悄悄覆盖”，且多条件要合并 |
| 2 | 0 个绑定值怎么写 | **`whereRaw`**（阶段一，可移植） | 单一 `where("cond", ...)` 用 `__VA_OPT__` 已实测 0/1/2/3 全部正确，但属 C23 预处理器扩展；接受扩展即可去掉 `whereRaw` |
| 3 | 单 builder 还是四类句柄 | **四类句柄**（Select/Insert/Update/Delete），编译期隔离方法 | 单 builder + kind 字段；代码少，但 SELECT 上也能 `.values()`，只能运行期报错 |
| 4 | 文本参数复制还是借用 | **复制**，builder 自包含 | 借用，零拷贝；代价是调用方必须保证字符串活到 `free()` |
| 5 | `.m.ct + .shim.c` 还是 `.h.ct + facade` | **`.m.ct + .shim.c`**，一个 import、稳定 tag | `.h.ct` 更符合原生 OO 规范；代价是两个 import、mangled tag 耦合、实现更绕 |
| 6 | `_Generic` 还是 GNU C99 类型内建 | **C11 + `cflags = -std=c11`** | 实测 `-std=c99 -Wall -Wextra` 也不告警，可留在 c99；但 `-pedantic` 会告警，标准符合性差。GNU `__builtin_types_compatible_p` 可留在 C99，但可移植性/可读性更差 |
| 7 | 第一期是否只做 SELECT | **只做 SELECT**，先把模型跑通 | 一次做四个语句；代价是本期评审面变大、边界更多 |
| 8 | 第一期是否做方言 | **不做**，统一 `?` | 做 dialect 枚举；代价是第一期复杂度和测试矩阵膨胀 |
| 9 | 第一期是否做标识符引用/校验 | **不做**，文档声明“可信片段” | 做 `quoteIdent`/白名单；代价是方言耦合、表名/列名规则要重新讨论 |
| 10 | 单元命名 `Sql.m.ct` 还是 `Select.m.ct` | **`Sql.m.ct`** 承载语句家族 | `Select.m.ct` 更符合“一单元一主类型”；第二期拆分会遇到共享 C core 的重复符号问题 |

---

## 12. 验收计划（第一期）

### 12.1 出口条件

- `ct build`（工作树版，含字面量接收者/变参）无语义 warning；
- C 编译器 `-Wall -Wextra` 零警告（含 `Sql.shim.c`），并分别在 `-std=c99` 与 `-std=c11` 下各跑一次；
- demo 实跑输出符合预期；
- `tools/check-layout.sh` 通过（`sql/sql/Sql.m.ct` 的 `namespace sql;` 与目录一致）；
- README/tutorial 的示例代码是实际 gate 编译过的文件，不把未验证的示例写给读者；
- 本轮不 commit、不 bump 版本、不打 tag、不改 `index/`。

### 12.2 测试矩阵

| # | 用例 | 期望 |
| ---: | --- | --- |
| 1 | 字面量接收者 + 链式 | `"t".select("a").where("a = ?", 1)` 编译并渲染 |
| 2 | 1..8 个混合参数 | `int` / `double` / `char *` 的 kind/值正确 |
| 3 | `whereRaw` 无参数 | 不产生参数，条件追加 |
| 4 | 多次 `where` | 以 ` AND ` 连接，参数顺序保持调用顺序 |
| 5 | `groupBy` / `orderBy` / `limit` / `offset` | 渲染顺序固定；覆盖语义正确 |
| 6 | offset 无 limit | `SQL_E_STATE`，`toSql` 返回 -1 |
| 7 | 输出 buffer 太小 | `SQL_E_SPACE`，`sql()` 可拿长度后重试 |
| 8 | 参数访问器 | 正确索引/类型；越界/错类型返回 -1/NULL |
| 9 | 文本复制 | `where` 后修改调用方 buffer，`paramText` 仍是旧值 |
| 10 | `free(NULL)` / 正常 free | 不崩溃；无泄漏（可用 leak/valgrind 辅助） |
| 11 | std 协作 | `q->sql().len()`、`VecStr.push(q->sql())`、`std::io::printf` 编译运行 |
| 12 | 与 png 共存 | sql 不映射 `void *`；png `Handle` 挂在 `PngReader *`；同时 import 不产生歧义 |
| 13 | 占位符数量不符 | `SQL_E_PARAM`，`toSql` 返回 -1 |
| 14 | 空表名/空字段 | `SQL_E_ARG`，`toSql` 返回 -1 |
| 15 | `-std=c99` / `-std=c11` 门禁 | 两者均 `-Wall -Wextra` 零告警；`_Generic` 在 c99 下不报错 |
| 16 | 包根 `-I` / `ct install` / 索引 | cc 命令包含 `Sql.shim.c`；运行不缺符号 |
| 17 | `char *` 名字唯一性 | 同时 import `std::str` 与 `sql` 后调用 `select/insert/update/delete` 不报歧义；`const char *` 接收者命中同一映射；`sql` 不新增 `len` |

### 12.3 文档

- `README.md`：一屏简版，包含包定位、依赖、引入方式、最小可运行示例；
- `docs/tutorial.md`：按仓库固定八段骨架；
- 所有示例放在包内并被 gate 编译；未验证的示例明确标注原因；
- 本轮只交付 `docs/design.md`，README/tutorial 等实现阶段再写。

---

## 附录 A：探针索引

> 以下路径都在 `/tmp/sqlprobe/`，不属于仓库；编译器在 `/tmp/ctprobe/ct`。

| 探针 | 内容 | 关键结果 |
| --- | --- | --- |
| `p1b` | 字面量接收者，最小 `probeLen()` | 发布版拒绝；工作树版生成 `probe_len(("abc"))` |
| `p1` | 类型宿主变参 `probePrintf(...)` | 发布版拒绝 `...`；工作树版运行输出 `lit 7` |
| `p2` | 类型宿主返回结构体 + 链式 | 内联 `"second".select("a").where("a = 2")` 编译通过 |
| `p3` | builder 上的变参 | 生成 `probe_builder_where((b), ("id = ?"), (1), (2), (3));`，无类型/个数 |
| `p4` | 同名方法不同 arity | `ambiguous type-host map for method 'where'` |
| `p5` | 两个映射都占用 `void *`（合成探针；旧版 png 当时仍占用 `void *`），方法名不撞 | 同一个 `void * h` 同时调用 `png_width` 和 `sql_where` |
| `p6` | `_Generic` 宏捕获 | `-std=c11` 下 int/string 正确 |
| `p7` | `.shim.c` 自动编译 | cc 行包含 `entry.shim.c`，一个 import |
| `p8` | 完整目标链 + 宏 | `params=2`；`-Wall -Wextra` 零警告 |
| `p9` | 两个映射都占用 `void *`（合成探针；旧版 png 当时仍占用 `void *`），都映射 `free` | `ambiguous type-host map for method 'free' on type 'void *'` |
| `p10` | SQL 方法名/关键字 | `select/insert/update/delete/...` 均可解析 |
| `p11` | std 协作 | `len=52`、`VecStr` 收集成功 |
| `p12` | 1..4 参数混合类型 | 全部捕获；0 参数宏不可靠 |
| `p13` | `set/values/toSql/paramInt/...` 名称与输出指针 | 通过；接收者不计入 trait 参数 |
| `p14` | 真实包布局 `pkg/sql/` + shim.c | 编译链接运行；SQL/GROUP BY/ORDER BY/LIMIT/OFFSET 正确 |
| `p15` | `/tmp/sqlprobe2/generic` | `_Generic` 在 c99/c11 `-Wall -Wextra` 零告警；`-pedantic` 才告警 |
| `p16` | `/tmp/sqlprobe2/narg` | 经典 `SQL_NARG` 0=1；`__VA_OPT__` 0=0；1/2/3 均正确 |
| `p17` | `/tmp/sqlprobe2/ctnarg`、`ctnarg2` | 0 实参变参方法可用；`where0` 与 `where` 不同名可共存；`__VA_OPT__` 宏 0/1/2/3 全通 |
| `p18` | `/tmp/sqlprobe2/ctover` | 同名 `where(char*)` 与 `where(char*, ...)` 仍 `ambiguous` |
| `p19` | `/tmp/sqlprobe2/consumer`、`index-consumer` | `ct get` / `ct install` / 本地索引 + git tag；shim.c 入链接，符号齐全 |
| `p20` | `/tmp/sqlprobe2/charhost` | 两个包都映射 `char *` 的 `len`：只 import 不调用通过；调用报 `ambiguous` |
| `p21` | `/tmp/sqlprobe2/consthost` | 0.19.1：`char *` / `const char *` 两键可共存；工作树：同键，两份映射 `ambiguous` |

## 附录 B：关键生成 C 快照

字面量接收者（`p1b`，工作树版）：

```c
printf("%d\n", probe_len(("abc")));
```

裸变参转发（`p3`）：

```c
probe_builder_where((b), ("id = ?"), (1), (2), (3));
```

`void *` 类型混淆（`p5`；合成探针，旧版 png 仍占用 `void *`，当前 png 已迁到 `PngReader *`）：

```c
printf("%d %d\n", png_width((h)), sql_where((h), ("x")));
```

真实包构建命令（`p14`）：

```text
'cc' -std=c99 -std=c11 -Wall -Wextra -I './sql' -I 'build' 'build/main.c' './sql/sql.shim.c' '-o' 'a.out'
```

真实包运行输出（`p14`）：

```text
toSql=132 status=0 message=ok
sql=SELECT id, name FROM users WHERE (status = ? AND age > ?) AND (deleted_at IS NULL) GROUP BY role ORDER BY id DESC LIMIT 10 OFFSET 20
params=2 kind0=1 kind1=1
```

---

## 附录 C：一句话结论

**语法可行，但“值捕获”不能靠裸变参，必须靠 shim 的 `_Generic` 宏；宿主不能是 `void *`，必须是命名句柄；第一期的正确形态是 `sql/sql/Sql.m.ct` + `Sql.shim.h` + `Sql.shim.c`，延迟渲染、显式 free、值只绑定不拼接。补测确认 `_Generic` 在 `-std=c99 -Wall -Wextra` 下不报错也不告警（`-pedantic` 才告警），`.shim.c` 在包根 `-I`、`ct get`/`ct install` 和本地索引三条路径都能自动编译链接；经典 `SQL_NARG` 区分不了 0/1，阶段一保留 `whereRaw`，`__VA_OPT__` 的单 `where` 写法作为可选。最终仍需等待 0.20.0 编译器发布后再对外承诺可用。**
