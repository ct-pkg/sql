#ifndef SQL_SHIM_H
#define SQL_SHIM_H

/*
 * sql/Sql.shim.h - SQL builder 的 C 面（SELECT / INSERT / UPDATE / DELETE）。
 *
 * CT 侧只看到 `sql_select_*` / `sql_insert_*` / `sql_update_*` / `sql_delete_*`
 * 这些名字；`where(q, cond, ...)` / `set(q, expr, ...)` / `values(q, ...)` 的
 * recipe 会把变参原样转发成一次 C 调用，这里再用 `_Generic` 在调用点给每个
 * 值打类型标签，收进有序的 `sql_value` 数组。SQL 文本里永远只有 `?`。
 *
 * 四个句柄在 shim.c 里定义，对消费者保持不透明；头文件只做前置声明。
 */

/* 状态码（与设计文档 §4.4 一致）。 */
#define SQL_E_OK 0
#define SQL_E_OOM (-1)
#define SQL_E_STATE (-2)
#define SQL_E_ARG (-3)
#define SQL_E_SPACE (-4)
#define SQL_E_PARAM (-5)

/* 绑定值的 kind：0 null、1 int、2 double、3 text。第一期不产生 0。 */
typedef struct sql_value {
    int kind;
    long long i;
    double d;
    char *s; /* 由 builder 拥有，free() 时释放 */
} sql_value;

struct sql_Select;
struct sql_Insert;
struct sql_Update;
struct sql_Delete;

/* ---------------------------------------------------------------- SELECT */

struct sql_Select *sql_select_new(char *table, char *fields);
struct sql_Select *sql_select_where_impl(struct sql_Select *q, char *cond,
                                         int n, const sql_value *vals);
struct sql_Select *sql_select_where_raw(struct sql_Select *q, char *cond);
struct sql_Select *sql_select_group_by(struct sql_Select *q, char *expr);
struct sql_Select *sql_select_order_by(struct sql_Select *q, char *expr);
struct sql_Select *sql_select_limit(struct sql_Select *q, int n);
struct sql_Select *sql_select_offset(struct sql_Select *q, int n);
int sql_select_to_sql(struct sql_Select *q, char *out, int cap);
char *sql_select_sql(struct sql_Select *q);
int sql_select_param_count(struct sql_Select *q);
int sql_select_param_kind(struct sql_Select *q, int i);
int sql_select_param_int(struct sql_Select *q, int i, long *out);
int sql_select_param_double(struct sql_Select *q, int i, double *out);
char *sql_select_param_text(struct sql_Select *q, int i);
int sql_select_status(struct sql_Select *q);
char *sql_select_message(struct sql_Select *q);
void sql_select_free(struct sql_Select *q);

/* ---------------------------------------------------------------- INSERT */

struct sql_Insert *sql_insert_new(char *table, char *columns);
struct sql_Insert *sql_insert_values_impl(struct sql_Insert *q, int n,
                                          const sql_value *vals);
int sql_insert_to_sql(struct sql_Insert *q, char *out, int cap);
char *sql_insert_sql(struct sql_Insert *q);
int sql_insert_param_count(struct sql_Insert *q);
int sql_insert_param_kind(struct sql_Insert *q, int i);
int sql_insert_param_int(struct sql_Insert *q, int i, long *out);
int sql_insert_param_double(struct sql_Insert *q, int i, double *out);
char *sql_insert_param_text(struct sql_Insert *q, int i);
int sql_insert_status(struct sql_Insert *q);
char *sql_insert_message(struct sql_Insert *q);
void sql_insert_free(struct sql_Insert *q);

/* ---------------------------------------------------------------- UPDATE */

struct sql_Update *sql_update_new(char *table);
struct sql_Update *sql_update_where_impl(struct sql_Update *q, char *cond,
                                         int n, const sql_value *vals);
struct sql_Update *sql_update_where_raw(struct sql_Update *q, char *cond);
struct sql_Update *sql_update_set_impl(struct sql_Update *q, char *expr,
                                       int n, const sql_value *vals);
struct sql_Update *sql_update_set_raw(struct sql_Update *q, char *expr);
struct sql_Update *sql_update_allow_all(struct sql_Update *q);
int sql_update_to_sql(struct sql_Update *q, char *out, int cap);
char *sql_update_sql(struct sql_Update *q);
int sql_update_param_count(struct sql_Update *q);
int sql_update_param_kind(struct sql_Update *q, int i);
int sql_update_param_int(struct sql_Update *q, int i, long *out);
int sql_update_param_double(struct sql_Update *q, int i, double *out);
char *sql_update_param_text(struct sql_Update *q, int i);
int sql_update_status(struct sql_Update *q);
char *sql_update_message(struct sql_Update *q);
void sql_update_free(struct sql_Update *q);

/* ---------------------------------------------------------------- DELETE */

struct sql_Delete *sql_delete_new(char *table);
struct sql_Delete *sql_delete_where_impl(struct sql_Delete *q, char *cond,
                                         int n, const sql_value *vals);
struct sql_Delete *sql_delete_where_raw(struct sql_Delete *q, char *cond);
struct sql_Delete *sql_delete_allow_all(struct sql_Delete *q);
int sql_delete_to_sql(struct sql_Delete *q, char *out, int cap);
char *sql_delete_sql(struct sql_Delete *q);
int sql_delete_param_count(struct sql_Delete *q);
int sql_delete_param_kind(struct sql_Delete *q, int i);
int sql_delete_param_int(struct sql_Delete *q, int i, long *out);
int sql_delete_param_double(struct sql_Delete *q, int i, double *out);
char *sql_delete_param_text(struct sql_Delete *q, int i);
int sql_delete_status(struct sql_Delete *q);
char *sql_delete_message(struct sql_Delete *q);
void sql_delete_free(struct sql_Delete *q);

/* 值构造函数：由 SQL_VAL 选中，返回一个带 kind 的 sql_value。 */
sql_value sql_v_int(long long v);
sql_value sql_v_dbl(double v);
sql_value sql_v_str(const char *s);

/*
 * SQL_VAL(x) 用 `((x) + 0)` 作为控制表达式：字符串字面量是 `char[N]`，
 * 加 0 后退化成 `char *`，否则 `_Generic` 匹配不到指针分支。
 */
#define SQL_VAL(x)                                                \
    _Generic(((x) + 0),                                           \
        char: sql_v_int,                                          \
        signed char: sql_v_int,                                   \
        unsigned char: sql_v_int,                                 \
        short: sql_v_int,                                         \
        unsigned short: sql_v_int,                                \
        int: sql_v_int,                                            \
        unsigned int: sql_v_int,                                   \
        long: sql_v_int,                                           \
        unsigned long: sql_v_int,                                  \
        long long: sql_v_int,                                      \
        unsigned long long: sql_v_int,                              \
        float: sql_v_dbl,                                          \
        double: sql_v_dbl,                                         \
        char *: sql_v_str,                                         \
        const char *: sql_v_str,                                   \
        default: sql_v_int)((x) + 0)

#define SQL_NARG_(a, b, c, d, e, f, g, h, n, ...) n
#define SQL_NARG(...) SQL_NARG_(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#define SQL_CAT_(a, b) a##b
#define SQL_CAT(a, b) SQL_CAT_(a, b)

/* 展开成 "n, (const sql_value[]){...}"，供 *_impl 调用。 */
#define SQL_ARGS_1(a) 1, (const sql_value[]){ SQL_VAL(a) }
#define SQL_ARGS_2(a, b) 2, (const sql_value[]){ SQL_VAL(a), SQL_VAL(b) }
#define SQL_ARGS_3(a, b, c) 3, (const sql_value[]){ SQL_VAL(a), SQL_VAL(b), SQL_VAL(c) }
#define SQL_ARGS_4(a, b, c, d) 4, (const sql_value[]){ SQL_VAL(a), SQL_VAL(b), SQL_VAL(c), SQL_VAL(d) }
#define SQL_ARGS_5(a, b, c, d, e) 5, (const sql_value[]){ SQL_VAL(a), SQL_VAL(b), SQL_VAL(c), SQL_VAL(d), SQL_VAL(e) }
#define SQL_ARGS_6(a, b, c, d, e, f) 6, (const sql_value[]){ SQL_VAL(a), SQL_VAL(b), SQL_VAL(c), SQL_VAL(d), SQL_VAL(e), SQL_VAL(f) }
#define SQL_ARGS_7(a, b, c, d, e, f, g) 7, (const sql_value[]){ SQL_VAL(a), SQL_VAL(b), SQL_VAL(c), SQL_VAL(d), SQL_VAL(e), SQL_VAL(f), SQL_VAL(g) }
#define SQL_ARGS_8(a, b, c, d, e, f, g, h) 8, (const sql_value[]){ SQL_VAL(a), SQL_VAL(b), SQL_VAL(c), SQL_VAL(d), SQL_VAL(e), SQL_VAL(f), SQL_VAL(g), SQL_VAL(h) }

#define SQL_ARGS(...) SQL_CAT(SQL_ARGS_, SQL_NARG(__VA_ARGS__))(__VA_ARGS__)

/*
 * 一次调用支持 1..8 个值。0 个值走 whereRaw / setRaw：
 * 经典 SQL_NARG 对 0 个实参返回 1，无法和 1 个值区分（设计 §3.12 实测）。
 */
#define sql_select_where(s, cond, ...) \
    sql_select_where_impl((s), (cond), SQL_ARGS(__VA_ARGS__))
#define sql_update_where(s, cond, ...) \
    sql_update_where_impl((s), (cond), SQL_ARGS(__VA_ARGS__))
#define sql_delete_where(s, cond, ...) \
    sql_delete_where_impl((s), (cond), SQL_ARGS(__VA_ARGS__))
#define sql_update_set(s, expr, ...) \
    sql_update_set_impl((s), (expr), SQL_ARGS(__VA_ARGS__))
#define sql_insert_values(s, ...) \
    sql_insert_values_impl((s), SQL_ARGS(__VA_ARGS__))

#endif /* SQL_SHIM_H */
