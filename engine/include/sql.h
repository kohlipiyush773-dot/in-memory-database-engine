// sql.h - tiny SQL parser (DBMS query layer).  Supported:
//   CREATE TABLE t (c INT PRIMARY KEY, d TEXT, ...)     DROP TABLE t
//   INSERT INTO t VALUES (..),(..)
//   SELECT * | COUNT(*) | c1,c2 FROM t [WHERE a op v [AND ...]] [ORDER BY c [ASC|DESC]] [LIMIT n]
//   UPDATE t SET c = v [, ...] [WHERE ...]              DELETE FROM t [WHERE ...]
//   BEGIN | COMMIT | ROLLBACK | CHECKPOINT | SHOW TABLES | DESCRIBE t | EXPLAIN <select/update/delete>
#pragma once
#include <string>
#include <vector>
#include <utility>

enum StmtType { ST_CREATE, ST_DROP, ST_INSERT, ST_SELECT, ST_UPDATE, ST_DELETE, ST_BEGIN, ST_COMMIT,
                ST_ROLLBACK, ST_SHOW_TABLES, ST_CHECKPOINT, ST_DESCRIBE };
enum CmpOp { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };

struct Literal { bool is_int; long long i; std::string s; Literal() : is_int(true), i(0) {} };
struct ColDef { std::string name; bool is_int; bool pk; };
struct CondAst { std::string col; CmpOp op; Literal lit; };

struct Stmt {
    StmtType type;
    bool explain;
    std::string table;
    std::vector<ColDef> coldefs;
    std::vector<std::vector<Literal> > rows;
    bool star, count_star;
    std::vector<std::string> cols;
    std::vector<CondAst> where;
    std::string order_col;
    bool order_desc;
    long long limit;
    std::vector<std::pair<std::string, Literal> > assigns;
    Stmt() : type(ST_SELECT), explain(false), star(false), count_star(false), order_desc(false), limit(-1) {}
};

Stmt ParseSql(const std::string& sql);     // throws std::runtime_error with a readable message
const char* OpText(CmpOp op);
