#include "sql.h"
#include "common.h"

namespace {
enum TokKind { T_ID, T_NUM, T_STR, T_SYM, T_END };
struct Tok { TokKind kind; std::string text; };

std::vector<Tok> Lex(const std::string& s) {
    std::vector<Tok> t;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (std::isspace(c)) { ++i; continue; }
        if (std::isalpha(c) || c == '_') {
            size_t j = i; while (j < n && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
            Tok k; k.kind = T_ID; k.text = s.substr(i, j - i); t.push_back(k); i = j; continue;
        }
        if (std::isdigit(c)) {
            size_t j = i; while (j < n && std::isdigit((unsigned char)s[j])) ++j;
            Tok k; k.kind = T_NUM; k.text = s.substr(i, j - i); t.push_back(k); i = j; continue;
        }
        if (c == '\'') {
            std::string v; size_t j = i + 1; bool closed = false;
            while (j < n) {
                if (s[j] == '\'') { if (j + 1 < n && s[j + 1] == '\'') { v += '\''; j += 2; continue; } closed = true; ++j; break; }
                v += s[j++];
            }
            if (!closed) throw std::runtime_error("unterminated string literal");
            Tok k; k.kind = T_STR; k.text = v; t.push_back(k); i = j; continue;
        }
        Tok k; k.kind = T_SYM;
        if ((c == '<' || c == '>' || c == '!') && i + 1 < n && (s[i + 1] == '=' || (c == '<' && s[i + 1] == '>'))) {
            k.text = s.substr(i, 2); i += 2;
        } else if (std::strchr("(),*=<>;-", c)) { k.text = std::string(1, (char)c); ++i; }
        else throw std::runtime_error(std::string("unexpected character '") + (char)c + "'");
        t.push_back(k);
    }
    Tok e; e.kind = T_END; t.push_back(e);
    return t;
}

class Parser {
public:
    explicit Parser(const std::string& sql) : t_(Lex(sql)), p_(0) {}
    Stmt Parse() {
        Stmt st;
        if (Kw("EXPLAIN")) { Next(); st.explain = true; }
        if (Kw("CREATE")) { Next(); ExpectKw("TABLE"); st.type = ST_CREATE; st.table = Ident(); Create(st); }
        else if (Kw("DROP")) { Next(); ExpectKw("TABLE"); st.type = ST_DROP; st.table = Ident(); }
        else if (Kw("INSERT")) { Next(); ExpectKw("INTO"); st.type = ST_INSERT; st.table = Ident(); Insert(st); }
        else if (Kw("SELECT")) { Next(); st.type = ST_SELECT; Select(st); }
        else if (Kw("UPDATE")) { Next(); st.type = ST_UPDATE; st.table = Ident(); Update(st); }
        else if (Kw("DELETE")) { Next(); ExpectKw("FROM"); st.type = ST_DELETE; st.table = Ident(); Where(st); }
        else if (Kw("BEGIN")) { Next(); if (Kw("TRANSACTION")) Next(); st.type = ST_BEGIN; }
        else if (Kw("COMMIT")) { Next(); st.type = ST_COMMIT; }
        else if (Kw("ROLLBACK")) { Next(); st.type = ST_ROLLBACK; }
        else if (Kw("CHECKPOINT")) { Next(); st.type = ST_CHECKPOINT; }
        else if (Kw("SHOW")) { Next(); ExpectKw("TABLES"); st.type = ST_SHOW_TABLES; }
        else if (Kw("DESCRIBE")) { Next(); st.type = ST_DESCRIBE; st.table = Ident(); }
        else throw std::runtime_error("syntax error: unknown statement" + Near());
        if (Sym(";")) Next();
        if (Cur().kind != T_END) throw std::runtime_error("syntax error: unexpected" + Near());
        if (st.explain && st.type != ST_SELECT && st.type != ST_UPDATE && st.type != ST_DELETE)
            throw std::runtime_error("EXPLAIN works on SELECT, UPDATE and DELETE");
        return st;
    }
private:
    const Tok& Cur() const { return t_[p_]; }
    void Next() { if (t_[p_].kind != T_END) ++p_; }
    std::string Near() const {
        if (Cur().kind == T_END) return " end of statement";
        return " near '" + Cur().text + "'";
    }
    bool Kw(const char* k) const { return Cur().kind == T_ID && Upper(Cur().text) == k; }
    bool Sym(const char* s) const { return Cur().kind == T_SYM && Cur().text == s; }
    void ExpectKw(const char* k) { if (!Kw(k)) throw std::runtime_error(std::string("syntax error: expected ") + k + Near()); Next(); }
    void ExpectSym(const char* s) { if (!Sym(s)) throw std::runtime_error(std::string("syntax error: expected '") + s + "'" + Near()); Next(); }
    std::string Ident() {
        if (Cur().kind != T_ID) throw std::runtime_error("syntax error: expected a name" + Near());
        std::string r = Lower(Cur().text); Next(); return r;
    }
    Literal Lit() {
        Literal l;
        bool neg = false;
        if (Sym("-")) { neg = true; Next(); }
        if (Cur().kind == T_NUM) {
            if (Cur().text.size() > 10) throw std::runtime_error("number too large");
            long long v = 0; ParseLL(Cur().text, &v); l.is_int = true; l.i = neg ? -v : v; Next(); return l;
        }
        if (!neg && Cur().kind == T_STR) { l.is_int = false; l.s = Cur().text; Next(); return l; }
        throw std::runtime_error("syntax error: expected a value" + Near());
    }
    void Create(Stmt& st) {
        ExpectSym("(");
        for (;;) {
            ColDef c; c.name = Ident(); c.pk = false;
            if (Kw("INT") || Kw("INTEGER")) { c.is_int = true; Next(); }
            else if (Kw("TEXT")) { c.is_int = false; Next(); }
            else if (Kw("VARCHAR")) { c.is_int = false; Next(); if (Sym("(")) { Next(); if (Cur().kind != T_NUM) throw std::runtime_error("expected length"); Next(); ExpectSym(")"); } }
            else throw std::runtime_error("syntax error: column type must be INT, TEXT or VARCHAR" + Near());
            if (Kw("PRIMARY")) { Next(); ExpectKw("KEY"); c.pk = true; }
            st.coldefs.push_back(c);
            if (Sym(",")) { Next(); continue; }
            break;
        }
        ExpectSym(")");
        int pks = 0;
        for (size_t i = 0; i < st.coldefs.size(); ++i) {
            if (st.coldefs[i].pk) { ++pks; if (!st.coldefs[i].is_int) throw std::runtime_error("PRIMARY KEY must be an INT column"); }
            for (size_t j = i + 1; j < st.coldefs.size(); ++j) if (st.coldefs[i].name == st.coldefs[j].name) throw std::runtime_error("duplicate column name: " + st.coldefs[i].name);
        }
        if (pks > 1) throw std::runtime_error("only one PRIMARY KEY column is supported");
    }
    void Insert(Stmt& st) {
        ExpectKw("VALUES");
        for (;;) {
            ExpectSym("(");
            std::vector<Literal> row;
            for (;;) { row.push_back(Lit()); if (Sym(",")) { Next(); continue; } break; }
            ExpectSym(")");
            st.rows.push_back(row);
            if (Sym(",")) { Next(); continue; }
            break;
        }
    }
    void Where(Stmt& st) {
        if (!Kw("WHERE")) return;
        Next();
        for (;;) {
            CondAst c; c.col = Ident();
            if (Sym("=")) c.op = OP_EQ; else if (Sym("!=") || Sym("<>")) c.op = OP_NE;
            else if (Sym("<=")) c.op = OP_LE; else if (Sym(">=")) c.op = OP_GE;
            else if (Sym("<")) c.op = OP_LT; else if (Sym(">")) c.op = OP_GT;
            else throw std::runtime_error("syntax error: expected a comparison operator" + Near());
            Next();
            c.lit = Lit();
            st.where.push_back(c);
            if (Kw("AND")) { Next(); continue; }
            break;
        }
    }
    void Select(Stmt& st) {
        if (Sym("*")) { st.star = true; Next(); }
        else if (Kw("COUNT")) { Next(); ExpectSym("("); ExpectSym("*"); ExpectSym(")"); st.count_star = true; }
        else for (;;) { st.cols.push_back(Ident()); if (Sym(",")) { Next(); continue; } break; }
        ExpectKw("FROM");
        st.table = Ident();
        Where(st);
        if (Kw("ORDER")) {
            Next(); ExpectKw("BY"); st.order_col = Ident();
            if (Kw("DESC")) { st.order_desc = true; Next(); } else if (Kw("ASC")) Next();
        }
        if (Kw("LIMIT")) {
            Next(); if (Cur().kind != T_NUM) throw std::runtime_error("LIMIT needs a number");
            ParseLL(Cur().text, &st.limit); Next();
        }
    }
    void Update(Stmt& st) {
        ExpectKw("SET");
        for (;;) {
            std::string c = Ident(); ExpectSym("=");
            st.assigns.push_back(std::make_pair(c, Lit()));
            if (Sym(",")) { Next(); continue; }
            break;
        }
        Where(st);
    }
    std::vector<Tok> t_;
    size_t p_;
};
}  // namespace

Stmt ParseSql(const std::string& sql) { Parser p(sql); return p.Parse(); }

const char* OpText(CmpOp op) {
    switch (op) { case OP_EQ: return "="; case OP_NE: return "!="; case OP_LT: return "<"; case OP_LE: return "<="; case OP_GT: return ">"; default: return ">="; }
}
