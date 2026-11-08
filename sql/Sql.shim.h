#ifndef SQL_SHIM_H
#define SQL_SHIM_H

/*
 * sql/Sql.shim.h - SQL builder 的 C 面（SELECT / INSERT / UPDATE / DELETE）。
 *
 * CT 侧只看到 `sql_select_*` / `sql_insert_*` / `sql_update_*` / `sql_delete_*`
 * 这些名字。带值的片段不再走变参：`where(cond)` / `set(expr)` / `row()` 只收
 * 结构片段，`?` 由随后紧邻的 `bindInt` / `bindDouble` / `bindText` / `bindNull`
 * 按出现顺序填充。这样 C 面完全是定参函数，不需要调用点类型分派、不需要
 * 实参计数宏，也不需要类型擦除后的还原。
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

struct sql_Select;
struct sql_Insert;
struct sql_Update;
struct sql_Delete;

/* ---------------------------------------------------------------- SELECT */

struct sql_Select *sql_select_new(char *table, char *fields);
struct sql_Select *sql_select_where(struct sql_Select *q, char *cond);
struct sql_Select *sql_select_where_raw(struct sql_Select *q, char *cond);
struct sql_Select *sql_select_bind_int(struct sql_Select *q, long v);
struct sql_Select *sql_select_bind_double(struct sql_Select *q, double v);
struct sql_Select *sql_select_bind_text(struct sql_Select *q, char *s);
struct sql_Select *sql_select_bind_null(struct sql_Select *q);
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
struct sql_Insert *sql_insert_row(struct sql_Insert *q);
struct sql_Insert *sql_insert_bind_int(struct sql_Insert *q, long v);
struct sql_Insert *sql_insert_bind_double(struct sql_Insert *q, double v);
struct sql_Insert *sql_insert_bind_text(struct sql_Insert *q, char *s);
struct sql_Insert *sql_insert_bind_null(struct sql_Insert *q);
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
struct sql_Update *sql_update_where(struct sql_Update *q, char *cond);
struct sql_Update *sql_update_where_raw(struct sql_Update *q, char *cond);
struct sql_Update *sql_update_bind_int(struct sql_Update *q, long v);
struct sql_Update *sql_update_bind_double(struct sql_Update *q, double v);
struct sql_Update *sql_update_bind_text(struct sql_Update *q, char *s);
struct sql_Update *sql_update_bind_null(struct sql_Update *q);
struct sql_Update *sql_update_set(struct sql_Update *q, char *expr);
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
struct sql_Delete *sql_delete_where(struct sql_Delete *q, char *cond);
struct sql_Delete *sql_delete_where_raw(struct sql_Delete *q, char *cond);
struct sql_Delete *sql_delete_bind_int(struct sql_Delete *q, long v);
struct sql_Delete *sql_delete_bind_double(struct sql_Delete *q, double v);
struct sql_Delete *sql_delete_bind_text(struct sql_Delete *q, char *s);
struct sql_Delete *sql_delete_bind_null(struct sql_Delete *q);
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

#endif /* SQL_SHIM_H */
