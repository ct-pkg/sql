#include "Sql.shim.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * builder 内核，四种语句共用一份 `sql_Query` 状态；四个句柄类型只用于
 * 类型宿主区分（SELECT / INSERT / UPDATE / DELETE 的方法集合互不混用）。
 *
 * 所有输入字符串都复制一份，builder 自包含：调用方可以在链式调用后立刻
 * 复用或释放自己的 buffer。文本参数由 builder 持有，free() 时统一释放。
 *
 * 绑定模型：`where(cond)` / `set(expr)` / `row()` 打开一个"待绑定片段"，
 * 记下片段里 `?` 的个数；随后紧邻的 `bindInt` / `bindDouble` / `bindText` /
 * `bindNull` 依次填进去。片段被下一次结构调用（再一个 where/set/row、
 * groupBy、limit、toSql……）冻结：绑定数不等于占位符数即 SQL_E_PARAM。
 *
 * 参数顺序 = 渲染后的 SQL 文本顺序：SELECT/INSERT/DELETE 是调用顺序，
 * UPDATE 固定为 SET 先、WHERE 后（与调用先后无关）。
 */

enum {
    SQL_STMT_SELECT = 1,
    SQL_STMT_INSERT,
    SQL_STMT_UPDATE,
    SQL_STMT_DELETE
};

/* 当前开放的待绑定片段所属的数组。 */
enum {
    PEND_NONE = 0,
    PEND_WHERE,
    PEND_SET,
    PEND_VALUES
};

/* 绑定值的 kind：0 null、1 int、2 double、3 text。 */
typedef struct sql_value {
    int kind;
    long long i;
    double d;
    char *s; /* 由 builder 拥有，free() 时释放 */
} sql_value;

struct sql_buf {
    char *p;
    size_t len;
    size_t cap;
};

struct sql_Query {
    int stmt;
    char *table;
    char *fields; /* SELECT 列表 / INSERT 列名 */
    int cols;     /* INSERT 列数 */
    struct sql_buf where; /* 已渲染的条件片段："(a = ?) AND (b IS NULL)" */
    sql_value *where_params;
    int nwhere;
    int capwhere;
    struct sql_buf set; /* 已渲染的 SET 片段："name = ?, age = ?" */
    sql_value *set_params;
    int nset;
    int capset;
    sql_value *values; /* INSERT 值，按行展平 */
    int nvalues;
    int capvalues;
    int pend_target;
    int pend_need;
    int pend_got;
    char *group;
    char *order;
    int limit;
    int offset;
    int has_limit;
    int has_offset;
    int allow_all;
    char *rendered;
    int status;
};

struct sql_Select {
    struct sql_Query q;
};

struct sql_Insert {
    struct sql_Query q;
};

struct sql_Update {
    struct sql_Query q;
};

struct sql_Delete {
    struct sql_Query q;
};

/* ---------------------------------------------------------------- 基础工具 */

static int is_hard(int status)
{
    return status != SQL_E_OK && status != SQL_E_SPACE;
}

static void q_fail(struct sql_Query *q, int code)
{
    if (q && !is_hard(q->status))
        q->status = code;
}

static char *dup_str(const char *s)
{
    size_t n;
    char *p;

    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    return p;
}

static int buf_reserve(struct sql_buf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    size_t cap;
    char *p;

    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 64;
    while (cap < need)
        cap *= 2;
    p = (char *)realloc(b->p, cap);
    if (!p)
        return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

static int buf_add(struct sql_buf *b, const char *s)
{
    size_t n;

    if (!s)
        return 0;
    n = strlen(s);
    if (buf_reserve(b, n) != 0)
        return -1;
    memcpy(b->p + b->len, s, n + 1);
    b->len += n;
    return 0;
}

static int buf_addf(struct sql_buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if (buf_reserve(b, (size_t)n) != 0)
        return -1;
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

/* 追加一个以 ", " 分隔的片段；空片段被忽略。 */
static int append_list(struct sql_buf *b, const char *piece)
{
    if (!piece || !piece[0])
        return 0;
    if (b->len && buf_add(b, ", ") != 0)
        return -1;
    return buf_add(b, piece);
}

static int append_csv(char **dst, const char *piece)
{
    size_t a = *dst ? strlen(*dst) : 0;
    size_t b = piece ? strlen(piece) : 0;
    char *p;

    if (b == 0)
        return 0;
    p = (char *)malloc(a + (a ? 2 : 0) + b + 1);
    if (!p)
        return -1;
    if (a) {
        memcpy(p, *dst, a);
        memcpy(p + a, ", ", 2);
        memcpy(p + a + 2, piece, b + 1);
    } else {
        memcpy(p, piece, b + 1);
    }
    free(*dst);
    *dst = p;
    return 0;
}

/*
 * 尽力数占位符：跳过单引号字符串（含 '' 转义）。
 * 不解析双引号标识符、注释和美元引用。
 */
static int count_placeholders(const char *s)
{
    int n = 0;
    int in_quote = 0;

    if (!s)
        return 0;
    while (*s) {
        if (in_quote) {
            if (*s == '\'') {
                if (s[1] == '\'') {
                    s += 2;
                    continue;
                }
                in_quote = 0;
            }
        } else if (*s == '\'') {
            in_quote = 1;
        } else if (*s == '?') {
            n++;
        }
        s++;
    }
    return n;
}

/*
 * 按顶层逗号数 INSERT 列数；括号内和单引号内的逗号不算。
 * 任一顶层列名为空（只含空白）时返回 -1，由调用方报 ARG。
 */
static int count_columns(const char *s)
{
    int n = 1;
    int depth = 0;
    int in_quote = 0;
    int seg = 0; /* 当前列名是否已出现非空白字符 */

    if (!s || !s[0])
        return 0;
    while (*s) {
        if (in_quote) {
            if (*s == '\'') {
                if (s[1] == '\'') {
                    s += 2;
                    continue;
                }
                in_quote = 0;
            }
        } else if (*s == '\'') {
            in_quote = 1;
            seg = 1;
        } else if (*s == '(') {
            depth++;
            seg = 1;
        } else if (*s == ')' && depth > 0) {
            depth--;
        } else if (*s == ',' && depth == 0) {
            if (!seg)
                return -1;
            n++;
            seg = 0;
        } else if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
            seg = 1;
        }
        s++;
    }
    return seg ? n : -1;
}

static void free_values(sql_value *vals, int n)
{
    int i;

    if (!vals)
        return;
    for (i = 0; i < n; i++)
        free(vals[i].s);
}

static void invalidate(struct sql_Query *q)
{
    free(q->rendered);
    q->rendered = NULL;
}

static int reserve_where(struct sql_Query *q, int extra)
{
    sql_value *p;
    int need = q->nwhere + extra;
    int cap;

    if (need <= q->capwhere)
        return 0;
    cap = q->capwhere ? q->capwhere : 4;
    while (cap < need)
        cap *= 2;
    p = (sql_value *)realloc(q->where_params, (size_t)cap * sizeof *p);
    if (!p)
        return -1;
    q->where_params = p;
    q->capwhere = cap;
    return 0;
}

static int reserve_set(struct sql_Query *q, int extra)
{
    sql_value *p;
    int need = q->nset + extra;
    int cap;

    if (need <= q->capset)
        return 0;
    cap = q->capset ? q->capset : 4;
    while (cap < need)
        cap *= 2;
    p = (sql_value *)realloc(q->set_params, (size_t)cap * sizeof *p);
    if (!p)
        return -1;
    q->set_params = p;
    q->capset = cap;
    return 0;
}

static int reserve_values(struct sql_Query *q, int extra)
{
    sql_value *p;
    int need = q->nvalues + extra;
    int cap;

    if (need <= q->capvalues)
        return 0;
    cap = q->capvalues ? q->capvalues : 4;
    while (cap < need)
        cap *= 2;
    p = (sql_value *)realloc(q->values, (size_t)cap * sizeof *p);
    if (!p)
        return -1;
    q->values = p;
    q->capvalues = cap;
    return 0;
}

static int append_condition(struct sql_Query *q, const char *cond)
{
    struct sql_buf *b = &q->where;

    if (b->len && buf_add(b, " AND ") != 0)
        return -1;
    if (buf_add(b, "(") != 0)
        return -1;
    if (buf_add(b, cond) != 0)
        return -1;
    return buf_add(b, ")");
}

/* ------------------------------------------------------------ 待绑定片段 */

static int pend_ok(const struct sql_Query *q)
{
    return q->pend_need == q->pend_got;
}

static void pend_begin(struct sql_Query *q, int target, int need)
{
    q->pend_target = target;
    q->pend_need = need;
    q->pend_got = 0;
}

static void pend_clear(struct sql_Query *q)
{
    q->pend_target = PEND_NONE;
    q->pend_need = 0;
    q->pend_got = 0;
}

/* 冻结当前片段：绑定数必须已齐，否则记 SQL_E_PARAM。 */
static int pend_freeze(struct sql_Query *q)
{
    if (!pend_ok(q)) {
        q_fail(q, SQL_E_PARAM);
        return -1;
    }
    pend_clear(q);
    return 0;
}

/* ------------------------------------------------------------ 值构造函数 */

static sql_value val_int(long v)
{
    sql_value x;

    x.kind = 1;
    x.i = (long long)v;
    x.d = 0.0;
    x.s = NULL;
    return x;
}

static sql_value val_double(double v)
{
    sql_value x;

    x.kind = 2;
    x.i = 0;
    x.d = v;
    x.s = NULL;
    return x;
}

static sql_value val_text(const char *s)
{
    sql_value x;

    x.kind = 3;
    x.i = 0;
    x.d = 0.0;
    x.s = s ? dup_str(s) : NULL;
    return x;
}

static sql_value val_null(void)
{
    sql_value x;

    x.kind = 0;
    x.i = 0;
    x.d = 0.0;
    x.s = NULL;
    return x;
}

/*
 * 把一个值压进当前开放片段。没有开放片段、或片段已经填满，都视为
 * "绑定与占位符数量不一致"（SQL_E_PARAM）。失败时释放值的文本所有权。
 */
static int pend_push(struct sql_Query *q, sql_value v)
{
    if (!q) {
        free(v.s);
        return -1;
    }
    if (is_hard(q->status)) {
        free(v.s);
        return -1;
    }
    if (q->pend_target == PEND_NONE || q->pend_got >= q->pend_need) {
        free(v.s);
        q_fail(q, SQL_E_PARAM);
        return -1;
    }
    switch (q->pend_target) {
    case PEND_WHERE:
        if (reserve_where(q, 1) != 0) {
            free(v.s);
            q_fail(q, SQL_E_OOM);
            return -1;
        }
        q->where_params[q->nwhere++] = v;
        break;
    case PEND_SET:
        if (reserve_set(q, 1) != 0) {
            free(v.s);
            q_fail(q, SQL_E_OOM);
            return -1;
        }
        q->set_params[q->nset++] = v;
        break;
    case PEND_VALUES:
        if (reserve_values(q, 1) != 0) {
            free(v.s);
            q_fail(q, SQL_E_OOM);
            return -1;
        }
        q->values[q->nvalues++] = v;
        break;
    default:
        free(v.s);
        q_fail(q, SQL_E_PARAM);
        return -1;
    }
    q->pend_got++;
    invalidate(q);
    return 0;
}

/* ---------------------------------------------------------------- 共享操作 */

static struct sql_Query *where_begin(struct sql_Query *q, char *cond)
{
    if (!q)
        return NULL;
    if (is_hard(q->status))
        return q;
    if (pend_freeze(q) != 0)
        return q;
    if (!cond || !cond[0]) {
        q_fail(q, SQL_E_ARG);
        return q;
    }
    if (append_condition(q, cond) != 0) {
        q_fail(q, SQL_E_OOM);
        return q;
    }
    pend_begin(q, PEND_WHERE, count_placeholders(cond));
    invalidate(q);
    return q;
}

static struct sql_Query *where_raw(struct sql_Query *q, char *cond)
{
    if (!q)
        return NULL;
    if (is_hard(q->status))
        return q;
    if (pend_freeze(q) != 0)
        return q;
    if (!cond || !cond[0]) {
        q_fail(q, SQL_E_ARG);
        return q;
    }
    if (append_condition(q, cond) != 0) {
        q_fail(q, SQL_E_OOM);
        return q;
    }
    invalidate(q);
    return q;
}

static struct sql_Query *set_begin(struct sql_Query *q, char *expr)
{
    if (!q)
        return NULL;
    if (is_hard(q->status))
        return q;
    if (pend_freeze(q) != 0)
        return q;
    if (!expr || !expr[0]) {
        q_fail(q, SQL_E_ARG);
        return q;
    }
    if (append_list(&q->set, expr) != 0) {
        q_fail(q, SQL_E_OOM);
        return q;
    }
    pend_begin(q, PEND_SET, count_placeholders(expr));
    invalidate(q);
    return q;
}

static struct sql_Query *set_raw(struct sql_Query *q, char *expr)
{
    if (!q)
        return NULL;
    if (is_hard(q->status))
        return q;
    if (pend_freeze(q) != 0)
        return q;
    if (!expr || !expr[0]) {
        q_fail(q, SQL_E_ARG);
        return q;
    }
    if (append_list(&q->set, expr) != 0) {
        q_fail(q, SQL_E_OOM);
        return q;
    }
    invalidate(q);
    return q;
}

static struct sql_Query *allow_all(struct sql_Query *q)
{
    if (!q)
        return NULL;
    if (is_hard(q->status))
        return q;
    if (pend_freeze(q) != 0)
        return q;
    q->allow_all = 1;
    invalidate(q);
    return q;
}

/* ------------------------------------------------------------------ 渲染 */

static int render(struct sql_Query *q)
{
    struct sql_buf b;
    int rows;
    int r;
    int c;
    int n;

    b.p = NULL;
    b.len = 0;
    b.cap = 0;
    if (!q || is_hard(q->status))
        return -1;
    if (pend_freeze(q) != 0)
        return -1;
    free(q->rendered);
    q->rendered = NULL;

    switch (q->stmt) {
    case SQL_STMT_SELECT:
        if (q->has_offset && !q->has_limit) {
            q_fail(q, SQL_E_STATE);
            return -1;
        }
        if (buf_add(&b, "SELECT ") != 0 || buf_add(&b, q->fields) != 0 ||
            buf_add(&b, " FROM ") != 0 || buf_add(&b, q->table) != 0)
            goto oom;
        if (q->where.len) {
            if (buf_add(&b, " WHERE ") != 0 || buf_add(&b, q->where.p) != 0)
                goto oom;
        }
        if (q->group) {
            if (buf_add(&b, " GROUP BY ") != 0 || buf_add(&b, q->group) != 0)
                goto oom;
        }
        if (q->order) {
            if (buf_add(&b, " ORDER BY ") != 0 || buf_add(&b, q->order) != 0)
                goto oom;
        }
        if (q->has_limit && buf_addf(&b, " LIMIT %d", q->limit) != 0)
            goto oom;
        if (q->has_offset && buf_addf(&b, " OFFSET %d", q->offset) != 0)
            goto oom;
        break;

    case SQL_STMT_INSERT:
        if (q->cols <= 0 || q->nvalues == 0 || q->nvalues % q->cols != 0) {
            q_fail(q, SQL_E_ARG);
            return -1;
        }
        if (buf_add(&b, "INSERT INTO ") != 0 || buf_add(&b, q->table) != 0 ||
            buf_add(&b, " (") != 0 || buf_add(&b, q->fields) != 0 ||
            buf_add(&b, ") VALUES ") != 0)
            goto oom;
        rows = q->nvalues / q->cols;
        for (r = 0; r < rows; r++) {
            if (r && buf_add(&b, ", ") != 0)
                goto oom;
            if (buf_add(&b, "(") != 0)
                goto oom;
            for (c = 0; c < q->cols; c++) {
                if (c && buf_add(&b, ", ") != 0)
                    goto oom;
                if (buf_add(&b, "?") != 0)
                    goto oom;
            }
            if (buf_add(&b, ")") != 0)
                goto oom;
        }
        break;

    case SQL_STMT_UPDATE:
        if (q->set.len == 0) {
            q_fail(q, SQL_E_ARG);
            return -1;
        }
        if (q->where.len == 0 && !q->allow_all) {
            q_fail(q, SQL_E_STATE);
            return -1;
        }
        if (buf_add(&b, "UPDATE ") != 0 || buf_add(&b, q->table) != 0 ||
            buf_add(&b, " SET ") != 0 || buf_add(&b, q->set.p) != 0)
            goto oom;
        if (q->where.len) {
            if (buf_add(&b, " WHERE ") != 0 || buf_add(&b, q->where.p) != 0)
                goto oom;
        }
        break;

    case SQL_STMT_DELETE:
        if (q->where.len == 0 && !q->allow_all) {
            q_fail(q, SQL_E_STATE);
            return -1;
        }
        if (buf_add(&b, "DELETE FROM ") != 0 || buf_add(&b, q->table) != 0)
            goto oom;
        if (q->where.len) {
            if (buf_add(&b, " WHERE ") != 0 || buf_add(&b, q->where.p) != 0)
                goto oom;
        }
        break;

    default:
        q_fail(q, SQL_E_STATE);
        return -1;
    }

    n = (int)b.len;
    q->rendered = b.p;
    return n;

oom:
    free(b.p);
    q_fail(q, SQL_E_OOM);
    return -1;
}

static int to_sql(struct sql_Query *q, char *out, int cap)
{
    int n;

    if (!q || !out || cap <= 0)
        return -1;
    n = render(q);
    if (n < 0)
        return -1;
    if (cap <= n) {
        q_fail(q, SQL_E_SPACE);
        return -1;
    }
    memcpy(out, q->rendered, (size_t)n + 1);
    if (q->status == SQL_E_SPACE)
        q->status = SQL_E_OK;
    return n;
}

static char *sql_text(struct sql_Query *q)
{
    if (!q || render(q) < 0)
        return "";
    return q->rendered;
}

/* 对外参数数组按渲染顺序：INSERT 只有 VALUES，UPDATE 是 SET + WHERE。 */
static int param_total(struct sql_Query *q)
{
    if (!q)
        return 0;
    if (pend_freeze(q) != 0)
        return 0;
    switch (q->stmt) {
    case SQL_STMT_INSERT:
        return q->nvalues;
    case SQL_STMT_UPDATE:
        return q->nset + q->nwhere;
    default:
        return q->nwhere;
    }
}

static sql_value *param_at(struct sql_Query *q, int i)
{
    if (q->stmt == SQL_STMT_INSERT)
        return &q->values[i];
    if (q->stmt == SQL_STMT_UPDATE) {
        if (i < q->nset)
            return &q->set_params[i];
        return &q->where_params[i - q->nset];
    }
    return &q->where_params[i];
}

static int param_kind(struct sql_Query *q, int i)
{
    if (!q || i < 0 || i >= param_total(q))
        return -1;
    return param_at(q, i)->kind;
}

static int param_int(struct sql_Query *q, int i, long *out)
{
    sql_value *v;

    if (!q || !out || i < 0 || i >= param_total(q))
        return -1;
    v = param_at(q, i);
    if (v->kind != 1)
        return -1;
    *out = (long)v->i;
    return 0;
}

static int param_double(struct sql_Query *q, int i, double *out)
{
    sql_value *v;

    if (!q || !out || i < 0 || i >= param_total(q))
        return -1;
    v = param_at(q, i);
    if (v->kind != 2)
        return -1;
    *out = v->d;
    return 0;
}

static char *param_text(struct sql_Query *q, int i)
{
    sql_value *v;

    if (!q || i < 0 || i >= param_total(q))
        return NULL;
    v = param_at(q, i);
    if (v->kind != 3)
        return NULL;
    return v->s;
}

/*
 * status() 是只读查询：把"片段还没绑满"也报成 SQL_E_PARAM，
 * 但不改动 builder，免得一次 status() 让后续 bind 失效。
 */
static int q_status(struct sql_Query *q)
{
    if (!q)
        return SQL_E_ARG;
    if (is_hard(q->status))
        return q->status;
    if (!pend_ok(q))
        return SQL_E_PARAM;
    return q->status;
}

static char *q_message(struct sql_Query *q)
{
    switch (q_status(q)) {
    case SQL_E_OK:
        return (char *)"ok";
    case SQL_E_OOM:
        return (char *)"out of memory";
    case SQL_E_STATE:
        return (char *)"invalid builder state";
    case SQL_E_ARG:
        return (char *)"invalid argument";
    case SQL_E_SPACE:
        return (char *)"output buffer too small";
    case SQL_E_PARAM:
        return (char *)"parameter count or type mismatch";
    default:
        return (char *)"unknown error";
    }
}

static void query_free(struct sql_Query *q)
{
    if (!q)
        return;
    free(q->table);
    free(q->fields);
    free(q->where.p);
    free_values(q->where_params, q->nwhere);
    free(q->where_params);
    free(q->set.p);
    free_values(q->set_params, q->nset);
    free(q->set_params);
    free_values(q->values, q->nvalues);
    free(q->values);
    free(q->group);
    free(q->order);
    free(q->rendered);
    free(q);
}

/* ---------------------------------------------------------------- SELECT */

struct sql_Select *sql_select_new(char *table, char *fields)
{
    struct sql_Select *s = (struct sql_Select *)calloc(1, sizeof *s);

    if (!s)
        return NULL;
    s->q.stmt = SQL_STMT_SELECT;
    s->q.table = dup_str(table);
    s->q.fields = dup_str(fields);
    if (!table || !table[0] || !fields || !fields[0])
        q_fail(&s->q, SQL_E_ARG);
    else if (!s->q.table || !s->q.fields)
        q_fail(&s->q, SQL_E_OOM);
    return s;
}

struct sql_Select *sql_select_where(struct sql_Select *q, char *cond)
{
    return (struct sql_Select *)where_begin(q ? &q->q : NULL, cond);
}

struct sql_Select *sql_select_where_raw(struct sql_Select *q, char *cond)
{
    return (struct sql_Select *)where_raw(q ? &q->q : NULL, cond);
}

struct sql_Select *sql_select_bind_int(struct sql_Select *q, long v)
{
    if (q)
        pend_push(&q->q, val_int(v));
    return q;
}

struct sql_Select *sql_select_bind_double(struct sql_Select *q, double v)
{
    if (q)
        pend_push(&q->q, val_double(v));
    return q;
}

struct sql_Select *sql_select_bind_text(struct sql_Select *q, char *s)
{
    if (q)
        pend_push(&q->q, val_text(s));
    return q;
}

struct sql_Select *sql_select_bind_null(struct sql_Select *q)
{
    if (q)
        pend_push(&q->q, val_null());
    return q;
}

struct sql_Select *sql_select_group_by(struct sql_Select *q, char *expr)
{
    struct sql_Query *x = q ? &q->q : NULL;

    if (!x)
        return NULL;
    if (is_hard(x->status))
        return q;
    if (pend_freeze(x) != 0)
        return q;
    if (!expr || !expr[0]) {
        q_fail(x, SQL_E_ARG);
        return q;
    }
    if (append_csv(&x->group, expr) != 0) {
        q_fail(x, SQL_E_OOM);
        return q;
    }
    invalidate(x);
    return q;
}

struct sql_Select *sql_select_order_by(struct sql_Select *q, char *expr)
{
    struct sql_Query *x = q ? &q->q : NULL;

    if (!x)
        return NULL;
    if (is_hard(x->status))
        return q;
    if (pend_freeze(x) != 0)
        return q;
    if (!expr || !expr[0]) {
        q_fail(x, SQL_E_ARG);
        return q;
    }
    if (append_csv(&x->order, expr) != 0) {
        q_fail(x, SQL_E_OOM);
        return q;
    }
    invalidate(x);
    return q;
}

struct sql_Select *sql_select_limit(struct sql_Select *q, int n)
{
    if (!q)
        return NULL;
    if (is_hard(q->q.status))
        return q;
    if (pend_freeze(&q->q) != 0)
        return q;
    if (n < 0) {
        q_fail(&q->q, SQL_E_ARG);
        return q;
    }
    q->q.limit = n;
    q->q.has_limit = 1;
    invalidate(&q->q);
    return q;
}

struct sql_Select *sql_select_offset(struct sql_Select *q, int n)
{
    if (!q)
        return NULL;
    if (is_hard(q->q.status))
        return q;
    if (pend_freeze(&q->q) != 0)
        return q;
    if (n < 0) {
        q_fail(&q->q, SQL_E_ARG);
        return q;
    }
    q->q.offset = n;
    q->q.has_offset = 1;
    invalidate(&q->q);
    return q;
}

int sql_select_to_sql(struct sql_Select *q, char *out, int cap)
{
    return to_sql(q ? &q->q : NULL, out, cap);
}

char *sql_select_sql(struct sql_Select *q)
{
    return sql_text(q ? &q->q : NULL);
}

int sql_select_param_count(struct sql_Select *q)
{
    return param_total(q ? &q->q : NULL);
}

int sql_select_param_kind(struct sql_Select *q, int i)
{
    return param_kind(q ? &q->q : NULL, i);
}

int sql_select_param_int(struct sql_Select *q, int i, long *out)
{
    return param_int(q ? &q->q : NULL, i, out);
}

int sql_select_param_double(struct sql_Select *q, int i, double *out)
{
    return param_double(q ? &q->q : NULL, i, out);
}

char *sql_select_param_text(struct sql_Select *q, int i)
{
    return param_text(q ? &q->q : NULL, i);
}

int sql_select_status(struct sql_Select *q)
{
    return q_status(q ? &q->q : NULL);
}

char *sql_select_message(struct sql_Select *q)
{
    return q_message(q ? &q->q : NULL);
}

void sql_select_free(struct sql_Select *q)
{
    query_free(q ? &q->q : NULL);
}

/* ---------------------------------------------------------------- INSERT */

struct sql_Insert *sql_insert_new(char *table, char *columns)
{
    struct sql_Insert *s = (struct sql_Insert *)calloc(1, sizeof *s);

    if (!s)
        return NULL;
    s->q.stmt = SQL_STMT_INSERT;
    s->q.table = dup_str(table);
    s->q.fields = dup_str(columns);
    s->q.cols = count_columns(columns);
    if (!table || !table[0] || !columns || !columns[0] || s->q.cols <= 0)
        q_fail(&s->q, SQL_E_ARG);
    else if (!s->q.table || !s->q.fields)
        q_fail(&s->q, SQL_E_OOM);
    return s;
}

struct sql_Insert *sql_insert_row(struct sql_Insert *q)
{
    struct sql_Query *x = q ? &q->q : NULL;

    if (!x)
        return NULL;
    if (is_hard(x->status))
        return q;
    if (pend_freeze(x) != 0)
        return q;
    pend_begin(x, PEND_VALUES, x->cols);
    return q;
}

struct sql_Insert *sql_insert_bind_int(struct sql_Insert *q, long v)
{
    if (q)
        pend_push(&q->q, val_int(v));
    return q;
}

struct sql_Insert *sql_insert_bind_double(struct sql_Insert *q, double v)
{
    if (q)
        pend_push(&q->q, val_double(v));
    return q;
}

struct sql_Insert *sql_insert_bind_text(struct sql_Insert *q, char *s)
{
    if (q)
        pend_push(&q->q, val_text(s));
    return q;
}

struct sql_Insert *sql_insert_bind_null(struct sql_Insert *q)
{
    if (q)
        pend_push(&q->q, val_null());
    return q;
}

int sql_insert_to_sql(struct sql_Insert *q, char *out, int cap)
{
    return to_sql(q ? &q->q : NULL, out, cap);
}

char *sql_insert_sql(struct sql_Insert *q)
{
    return sql_text(q ? &q->q : NULL);
}

int sql_insert_param_count(struct sql_Insert *q)
{
    return param_total(q ? &q->q : NULL);
}

int sql_insert_param_kind(struct sql_Insert *q, int i)
{
    return param_kind(q ? &q->q : NULL, i);
}

int sql_insert_param_int(struct sql_Insert *q, int i, long *out)
{
    return param_int(q ? &q->q : NULL, i, out);
}

int sql_insert_param_double(struct sql_Insert *q, int i, double *out)
{
    return param_double(q ? &q->q : NULL, i, out);
}

char *sql_insert_param_text(struct sql_Insert *q, int i)
{
    return param_text(q ? &q->q : NULL, i);
}

int sql_insert_status(struct sql_Insert *q)
{
    return q_status(q ? &q->q : NULL);
}

char *sql_insert_message(struct sql_Insert *q)
{
    return q_message(q ? &q->q : NULL);
}

void sql_insert_free(struct sql_Insert *q)
{
    query_free(q ? &q->q : NULL);
}

/* ---------------------------------------------------------------- UPDATE */

struct sql_Update *sql_update_new(char *table)
{
    struct sql_Update *s = (struct sql_Update *)calloc(1, sizeof *s);

    if (!s)
        return NULL;
    s->q.stmt = SQL_STMT_UPDATE;
    s->q.table = dup_str(table);
    if (!table || !table[0])
        q_fail(&s->q, SQL_E_ARG);
    else if (!s->q.table)
        q_fail(&s->q, SQL_E_OOM);
    return s;
}

struct sql_Update *sql_update_where(struct sql_Update *q, char *cond)
{
    return (struct sql_Update *)where_begin(q ? &q->q : NULL, cond);
}

struct sql_Update *sql_update_where_raw(struct sql_Update *q, char *cond)
{
    return (struct sql_Update *)where_raw(q ? &q->q : NULL, cond);
}

struct sql_Update *sql_update_bind_int(struct sql_Update *q, long v)
{
    if (q)
        pend_push(&q->q, val_int(v));
    return q;
}

struct sql_Update *sql_update_bind_double(struct sql_Update *q, double v)
{
    if (q)
        pend_push(&q->q, val_double(v));
    return q;
}

struct sql_Update *sql_update_bind_text(struct sql_Update *q, char *s)
{
    if (q)
        pend_push(&q->q, val_text(s));
    return q;
}

struct sql_Update *sql_update_bind_null(struct sql_Update *q)
{
    if (q)
        pend_push(&q->q, val_null());
    return q;
}

struct sql_Update *sql_update_set(struct sql_Update *q, char *expr)
{
    return (struct sql_Update *)set_begin(q ? &q->q : NULL, expr);
}

struct sql_Update *sql_update_set_raw(struct sql_Update *q, char *expr)
{
    return (struct sql_Update *)set_raw(q ? &q->q : NULL, expr);
}

struct sql_Update *sql_update_allow_all(struct sql_Update *q)
{
    return (struct sql_Update *)allow_all(q ? &q->q : NULL);
}

int sql_update_to_sql(struct sql_Update *q, char *out, int cap)
{
    return to_sql(q ? &q->q : NULL, out, cap);
}

char *sql_update_sql(struct sql_Update *q)
{
    return sql_text(q ? &q->q : NULL);
}

int sql_update_param_count(struct sql_Update *q)
{
    return param_total(q ? &q->q : NULL);
}

int sql_update_param_kind(struct sql_Update *q, int i)
{
    return param_kind(q ? &q->q : NULL, i);
}

int sql_update_param_int(struct sql_Update *q, int i, long *out)
{
    return param_int(q ? &q->q : NULL, i, out);
}

int sql_update_param_double(struct sql_Update *q, int i, double *out)
{
    return param_double(q ? &q->q : NULL, i, out);
}

char *sql_update_param_text(struct sql_Update *q, int i)
{
    return param_text(q ? &q->q : NULL, i);
}

int sql_update_status(struct sql_Update *q)
{
    return q_status(q ? &q->q : NULL);
}

char *sql_update_message(struct sql_Update *q)
{
    return q_message(q ? &q->q : NULL);
}

void sql_update_free(struct sql_Update *q)
{
    query_free(q ? &q->q : NULL);
}

/* ---------------------------------------------------------------- DELETE */

struct sql_Delete *sql_delete_new(char *table)
{
    struct sql_Delete *s = (struct sql_Delete *)calloc(1, sizeof *s);

    if (!s)
        return NULL;
    s->q.stmt = SQL_STMT_DELETE;
    s->q.table = dup_str(table);
    if (!table || !table[0])
        q_fail(&s->q, SQL_E_ARG);
    else if (!s->q.table)
        q_fail(&s->q, SQL_E_OOM);
    return s;
}

struct sql_Delete *sql_delete_where(struct sql_Delete *q, char *cond)
{
    return (struct sql_Delete *)where_begin(q ? &q->q : NULL, cond);
}

struct sql_Delete *sql_delete_where_raw(struct sql_Delete *q, char *cond)
{
    return (struct sql_Delete *)where_raw(q ? &q->q : NULL, cond);
}

struct sql_Delete *sql_delete_bind_int(struct sql_Delete *q, long v)
{
    if (q)
        pend_push(&q->q, val_int(v));
    return q;
}

struct sql_Delete *sql_delete_bind_double(struct sql_Delete *q, double v)
{
    if (q)
        pend_push(&q->q, val_double(v));
    return q;
}

struct sql_Delete *sql_delete_bind_text(struct sql_Delete *q, char *s)
{
    if (q)
        pend_push(&q->q, val_text(s));
    return q;
}

struct sql_Delete *sql_delete_bind_null(struct sql_Delete *q)
{
    if (q)
        pend_push(&q->q, val_null());
    return q;
}

struct sql_Delete *sql_delete_allow_all(struct sql_Delete *q)
{
    return (struct sql_Delete *)allow_all(q ? &q->q : NULL);
}

int sql_delete_to_sql(struct sql_Delete *q, char *out, int cap)
{
    return to_sql(q ? &q->q : NULL, out, cap);
}

char *sql_delete_sql(struct sql_Delete *q)
{
    return sql_text(q ? &q->q : NULL);
}

int sql_delete_param_count(struct sql_Delete *q)
{
    return param_total(q ? &q->q : NULL);
}

int sql_delete_param_kind(struct sql_Delete *q, int i)
{
    return param_kind(q ? &q->q : NULL, i);
}

int sql_delete_param_int(struct sql_Delete *q, int i, long *out)
{
    return param_int(q ? &q->q : NULL, i, out);
}

int sql_delete_param_double(struct sql_Delete *q, int i, double *out)
{
    return param_double(q ? &q->q : NULL, i, out);
}

char *sql_delete_param_text(struct sql_Delete *q, int i)
{
    return param_text(q ? &q->q : NULL, i);
}

int sql_delete_status(struct sql_Delete *q)
{
    return q_status(q ? &q->q : NULL);
}

char *sql_delete_message(struct sql_Delete *q)
{
    return q_message(q ? &q->q : NULL);
}

void sql_delete_free(struct sql_Delete *q)
{
    query_free(q ? &q->q : NULL);
}
