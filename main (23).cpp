// "tinyc" - compiler for a small statically-scoped language, emitting LLVM IR text.
// Pipeline: lexer -> Pratt parser -> AST -> optimizations (constant folding, dead code removal)
//           -> semantic checks + LLVM IR generation.  No LLVM libraries needed to build the compiler.
// Build:    g++ -std=c++20 -O2 -Wall -Wextra project11_tinyc.cpp -o tinyc
// Use:      ./tinyc prog.tiny > prog.ll && clang prog.ll -o prog && ./prog      (LLVM 15+, opaque ptrs)
//           ./tinyc            (no args: compiles the built-in demo program to stdout)
//
// Language (all values are 64-bit signed ints):
//   fn fib(n) { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }
//   fn main() { let i = 0; while i < 10 { print(fib(i)); i = i + 1; } return 0; }
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ===================== Lexer =====================
enum class Tok { Num, Id, Kw, Sym, End };
struct Token { Tok type; std::string text; int line; };

[[noreturn]] static void fail(int line, const std::string& msg) {
    throw std::runtime_error("line " + std::to_string(line) + ": " + msg);
}

static std::vector<Token> lex(const std::string& src) {
    static const std::vector<std::string> kws = {"fn", "let", "if", "else", "while", "return", "print"};
    std::vector<Token> out;
    int line = 1;
    for (size_t i = 0; i < src.size();) {
        char c = src[i];
        if (c == '\n') { ++line; ++i; }
        else if (std::isspace(static_cast<unsigned char>(c))) ++i;
        else if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') { while (i < src.size() && src[i] != '\n') ++i; }
        else if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) ++j;
            out.push_back({Tok::Num, src.substr(i, j - i), line});
            i = j;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < src.size() && (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_')) ++j;
            std::string w = src.substr(i, j - i);
            bool kw = false;
            for (auto& k : kws) kw |= (k == w);
            out.push_back({kw ? Tok::Kw : Tok::Id, w, line});
            i = j;
        } else {
            std::string two = src.substr(i, 2);
            if (two == "<=" || two == ">=" || two == "==" || two == "!=") {
                out.push_back({Tok::Sym, two, line});
                i += 2;
            } else if (std::string("+-*/%<>=(){},;").find(c) != std::string::npos) {
                out.push_back({Tok::Sym, std::string(1, c), line});
                ++i;
            } else fail(line, std::string("unexpected character '") + c + "'");
        }
    }
    out.push_back({Tok::End, "", line});
    return out;
}

// ===================== AST =====================
struct Expr;
struct Stmt;
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

struct Expr {
    enum Kind { Num, Var, Bin, Neg, Call } kind;
    int line = 0;
    int64_t num = 0;
    std::string name;  // variable, callee, or binary operator
    ExprP l, r;
    std::vector<ExprP> args;
};
struct Stmt {
    enum Kind { Let, Assign, If, While, Return, ExprS, Print } kind;
    int line = 0;
    std::string name;
    ExprP e;
    std::vector<StmtP> body, els;
};
struct Func { std::string name; std::vector<std::string> params; std::vector<StmtP> body; int line; };

// ===================== Parser (Pratt / precedence climbing) =====================
class Parser {
public:
    explicit Parser(std::vector<Token> t) : t_(std::move(t)) {}

    std::vector<Func> program() {
        std::vector<Func> fs;
        while (cur().type != Tok::End) fs.push_back(func());
        return fs;
    }

private:
    const Token& cur() const { return t_[p_]; }
    bool is(const char* s) const { return (cur().type == Tok::Sym || cur().type == Tok::Kw) && cur().text == s; }
    bool accept(const char* s) { if (is(s)) { ++p_; return true; } return false; }
    void expect(const char* s) { if (!accept(s)) fail(cur().line, std::string("expected '") + s + "' but found '" + cur().text + "'"); }
    std::string ident() {
        if (cur().type != Tok::Id) fail(cur().line, "expected identifier");
        return t_[p_++].text;
    }

    Func func() {
        Func f;
        f.line = cur().line;
        expect("fn");
        f.name = ident();
        expect("(");
        if (!is(")")) { do f.params.push_back(ident()); while (accept(",")); }
        expect(")");
        f.body = block();
        return f;
    }

    std::vector<StmtP> block() {
        std::vector<StmtP> b;
        expect("{");
        while (!is("}")) {
            if (cur().type == Tok::End) fail(cur().line, "unterminated block");
            b.push_back(stmt());
        }
        expect("}");
        return b;
    }

    StmtP stmt() {
        auto s = std::make_unique<Stmt>();
        s->line = cur().line;
        if (accept("let")) {
            s->kind = Stmt::Let; s->name = ident(); expect("="); s->e = expr(0); expect(";");
        } else if (accept("if")) {
            s->kind = Stmt::If; s->e = expr(0); s->body = block();
            if (accept("else")) {
                if (is("if")) s->els.push_back(stmt());  // else-if chain
                else s->els = block();
            }
        } else if (accept("while")) {
            s->kind = Stmt::While; s->e = expr(0); s->body = block();
        } else if (accept("return")) {
            s->kind = Stmt::Return; s->e = expr(0); expect(";");
        } else if (accept("print")) {
            s->kind = Stmt::Print; expect("("); s->e = expr(0); expect(")"); expect(";");
        } else if (cur().type == Tok::Id && t_[p_ + 1].type == Tok::Sym && t_[p_ + 1].text == "=") {
            s->kind = Stmt::Assign; s->name = ident(); expect("="); s->e = expr(0); expect(";");
        } else {
            s->kind = Stmt::ExprS; s->e = expr(0); expect(";");
        }
        return s;
    }

    static int prec(const std::string& op) {
        if (op == "==" || op == "!=") return 1;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 2;
        if (op == "+" || op == "-") return 3;
        if (op == "*" || op == "/" || op == "%") return 4;
        return -1;
    }

    ExprP expr(int min_prec) {
        ExprP lhs = unary();
        while (cur().type == Tok::Sym && prec(cur().text) >= min_prec) {
            std::string op = cur().text;
            int line = cur().line;
            int pr = prec(op);
            ++p_;
            ExprP rhs = expr(pr + 1);  // +1 => left associative
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Bin; e->name = op; e->line = line;
            e->l = std::move(lhs); e->r = std::move(rhs);
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprP unary() {
        auto e = std::make_unique<Expr>();
        e->line = cur().line;
        if (accept("-")) { e->kind = Expr::Neg; e->l = unary(); return e; }
        if (cur().type == Tok::Num) { e->kind = Expr::Num; e->num = std::stoll(cur().text); ++p_; return e; }
        if (cur().type == Tok::Id) {
            e->name = ident();
            if (accept("(")) {
                e->kind = Expr::Call;
                if (!is(")")) { do e->args.push_back(expr(0)); while (accept(",")); }
                expect(")");
            } else e->kind = Expr::Var;
            return e;
        }
        if (accept("(")) { ExprP inner = expr(0); expect(")"); return inner; }
        fail(cur().line, "unexpected '" + cur().text + "' in expression");
    }

    std::vector<Token> t_;
    size_t p_ = 0;
};

// ===================== Optimizations =====================
struct OptStats { int folded = 0; int dead_stmts = 0; };

static void fold(ExprP& e, OptStats& st) {
    if (!e) return;
    fold(e->l, st); fold(e->r, st);
    for (auto& a : e->args) fold(a, st);
    if (e->kind == Expr::Neg && e->l->kind == Expr::Num) {
        e->kind = Expr::Num; e->num = -e->l->num; e->l.reset(); ++st.folded;
    } else if (e->kind == Expr::Bin && e->l->kind == Expr::Num && e->r->kind == Expr::Num) {
        int64_t a = e->l->num, b = e->r->num, r = 0;
        const std::string& op = e->name;
        if ((op == "/" || op == "%") && b == 0) return;  // leave runtime behavior alone
        if (op == "+") r = static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
        else if (op == "-") r = static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
        else if (op == "*") r = static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
        else if (op == "/") { if (a == INT64_MIN && b == -1) return; r = a / b; }
        else if (op == "%") { if (a == INT64_MIN && b == -1) return; r = a % b; }
        else if (op == "<") r = a < b; else if (op == ">") r = a > b;
        else if (op == "<=") r = a <= b; else if (op == ">=") r = a >= b;
        else if (op == "==") r = a == b; else if (op == "!=") r = a != b;
        e->kind = Expr::Num; e->num = r; e->l.reset(); e->r.reset(); ++st.folded;
    }
}

static void optimize(std::vector<StmtP>& body, OptStats& st) {
    for (size_t i = 0; i < body.size(); ++i) {
        Stmt& s = *body[i];
        fold(s.e, st);
        optimize(s.body, st);
        optimize(s.els, st);
        if (s.kind == Stmt::Return && i + 1 < body.size()) {  // unreachable code after return
            st.dead_stmts += static_cast<int>(body.size() - i - 1);
            body.resize(i + 1);
        }
    }
}

// ===================== Semantic checks + LLVM IR generation =====================
class CodeGen {
public:
    std::string generate(const std::vector<Func>& funcs) {
        for (auto& f : funcs) {
            if (arity_.count(f.name)) fail(f.line, "duplicate function '" + f.name + "'");
            arity_[f.name] = f.params.size();
        }
        if (!arity_.count("main") || arity_["main"] != 0) throw std::runtime_error("program needs 'fn main()' with no parameters");

        std::ostringstream mod;
        mod << "; generated by tinyc\n"
            << "@.fmt = private unnamed_addr constant [6 x i8] c\"%lld\\0A\\00\"\n"
            << "declare i32 @printf(ptr, ...)\n\n";
        for (auto& f : funcs) mod << function(f) << "\n";
        mod << "define i32 @main() {\n  %r = call i64 @tiny_main()\n  %t = trunc i64 %r to i32\n  ret i32 %t\n}\n";
        return mod.str();
    }

private:
    std::string tmp() { return "%t" + std::to_string(n_++); }
    std::string label(const char* base) { return std::string(base) + "." + std::to_string(n_++); }
    void inst(const std::string& s) { body_ << "  " << s << "\n"; }
    void place(const std::string& l) { body_ << l << ":\n"; }

    std::string lookup(const std::string& name, int line) {
        for (auto s = scopes_.rbegin(); s != scopes_.rend(); ++s) {
            auto f = s->find(name);
            if (f != s->end()) return f->second;
        }
        fail(line, "undefined variable '" + name + "'");
    }
    std::string declare(const std::string& name) {
        std::string slot = "%v." + name + "." + std::to_string(n_++);
        allocas_ << "  " << slot << " = alloca i64\n";  // all allocas hoisted to the entry block
        scopes_.back()[name] = slot;
        return slot;
    }

    std::string function(const Func& f) {
        body_.str(""); allocas_.str(""); scopes_.clear(); n_ = 0;
        scopes_.emplace_back();
        std::string sig;
        for (size_t i = 0; i < f.params.size(); ++i) sig += (i ? ", " : "") + std::string("i64 %p") + std::to_string(i);
        std::vector<std::string> slots;
        for (size_t i = 0; i < f.params.size(); ++i) {
            if (scopes_.back().count(f.params[i])) fail(f.line, "duplicate parameter '" + f.params[i] + "'");
            std::string slot = declare(f.params[i]);
            inst("store i64 %p" + std::to_string(i) + ", ptr " + slot);
        }
        block(f.body);
        inst("ret i64 0");  // implicit return value
        std::string name = f.name == "main" ? "tiny_main" : f.name;
        return "define i64 @" + name + "(" + sig + ") {\nentry:\n" + allocas_.str() + body_.str() + "}\n";
    }

    void block(const std::vector<StmtP>& b) {
        scopes_.emplace_back();
        for (auto& s : b) stmt(*s);
        scopes_.pop_back();
    }

    void stmt(const Stmt& s) {
        switch (s.kind) {
        case Stmt::Let: {
            std::string v = expr(*s.e);  // evaluate first: `let x = x` refers to the outer x
            inst("store i64 " + v + ", ptr " + declare(s.name));
            break;
        }
        case Stmt::Assign: {
            std::string v = expr(*s.e);
            inst("store i64 " + v + ", ptr " + lookup(s.name, s.line));
            break;
        }
        case Stmt::Print: {
            std::string v = expr(*s.e);
            inst(tmp() + " = call i32 (ptr, ...) @printf(ptr @.fmt, i64 " + v + ")");
            break;
        }
        case Stmt::ExprS: expr(*s.e); break;
        case Stmt::Return: {
            inst("ret i64 " + expr(*s.e));
            place(label("after.ret"));  // keep emitting into a fresh (dead) block
            break;
        }
        case Stmt::If: {
            std::string c = cond(*s.e);
            std::string th = label("then"), el = label("else"), end = label("endif");
            inst("br i1 " + c + ", label %" + th + ", label %" + el);
            place(th); block(s.body); inst("br label %" + end);
            place(el);
            if (!s.els.empty()) block(s.els);
            inst("br label %" + end);
            place(end);
            break;
        }
        case Stmt::While: {
            std::string head = label("while.head"), bd = label("while.body"), end = label("while.end");
            inst("br label %" + head);
            place(head);
            inst("br i1 " + cond(*s.e) + ", label %" + bd + ", label %" + end);
            place(bd); block(s.body); inst("br label %" + head);
            place(end);
            break;
        }
        }
    }

    std::string cond(const Expr& e) {
        std::string v = expr(e), t = tmp();
        inst(t + " = icmp ne i64 " + v + ", 0");
        return t;
    }

    std::string expr(const Expr& e) {
        switch (e.kind) {
        case Expr::Num: return std::to_string(e.num);
        case Expr::Var: {
            std::string t = tmp();
            inst(t + " = load i64, ptr " + lookup(e.name, e.line));
            return t;
        }
        case Expr::Neg: {
            std::string v = expr(*e.l), t = tmp();
            inst(t + " = sub i64 0, " + v);
            return t;
        }
        case Expr::Call: {
            auto f = arity_.find(e.name);
            if (f == arity_.end()) fail(e.line, "call to undefined function '" + e.name + "'");
            if (f->second != e.args.size())
                fail(e.line, "'" + e.name + "' expects " + std::to_string(f->second) + " argument(s), got " + std::to_string(e.args.size()));
            std::vector<std::string> vals;
            for (auto& a : e.args) vals.push_back(expr(*a));
            std::string list;
            for (size_t i = 0; i < vals.size(); ++i) list += (i ? ", " : "") + std::string("i64 ") + vals[i];
            std::string t = tmp();
            inst(t + " = call i64 @" + (e.name == "main" ? "tiny_main" : e.name) + "(" + list + ")");
            return t;
        }
        case Expr::Bin: {
            std::string a = expr(*e.l), b = expr(*e.r), t = tmp();
            const std::string& op = e.name;
            static const std::map<std::string, std::string> arith = {{"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "sdiv"}, {"%", "srem"}};
            static const std::map<std::string, std::string> cmp = {{"<", "slt"}, {">", "sgt"}, {"<=", "sle"}, {">=", "sge"}, {"==", "eq"}, {"!=", "ne"}};
            if (arith.count(op)) {
                inst(t + " = " + arith.at(op) + " i64 " + a + ", " + b);
                return t;
            }
            inst(t + " = icmp " + cmp.at(op) + " i64 " + a + ", " + b);
            std::string z = tmp();
            inst(z + " = zext i1 " + t + " to i64");
            return z;
        }
        }
        return "0";
    }

    std::map<std::string, size_t> arity_;
    std::vector<std::map<std::string, std::string>> scopes_;
    std::ostringstream body_, allocas_;
    int n_ = 0;
};

static const char* kDemo = R"(// demo: fibonacci + loops + constant folding
fn fib(n) {
  if n < 2 { return n; }
  return fib(n - 1) + fib(n - 2);
}
fn main() {
  let i = 0;
  while i < 10 {
    print(fib(i));
    i = i + 1;
  }
  print(2 * 3 + 4 * 5);      // folded to 26 at compile time
  return 0;
  print(999);                 // unreachable: removed
}
)";

int main(int argc, char** argv) {
    try {
        std::string src = kDemo;
        if (argc > 1) {
            std::ifstream in(argv[1]);
            if (!in) { std::cerr << "cannot open " << argv[1] << "\n"; return 1; }
            std::stringstream ss;
            ss << in.rdbuf();
            src = ss.str();
        }
        auto funcs = Parser(lex(src)).program();
        OptStats st;
        for (auto& f : funcs) optimize(f.body, st);
        std::cout << CodeGen().generate(funcs);
        std::cerr << "tinyc: " << funcs.size() << " functions, " << st.folded << " constant folds, "
                  << st.dead_stmts << " dead statements removed\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

