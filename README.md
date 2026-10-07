# A Small Compiler to LLVM IR
A compiler for a small C-like language that emits LLVM IR as text. No LLVM libraries are needed to build the compiler.

Demonstrates: lexing, Pratt (precedence-climbing) parsing, ASTs, optimization passes, semantic analysis, code generation.
# Pipeline
source --> lexer --> Pratt parser --> AST --> optimizer --> semantic checks + IR codegen --> .ll

Optimizations: constant folding (guarded against divide-by-zero and signed overflow cases) and removal of unreachable statements after return.
Semantic checks: undefined variables, undefined functions, wrong argument counts, duplicate functions or parameters, and a required main(). Errors include line numbers.
Codegen: all values are 64-bit signed integers; locals use alloca slots hoisted to the entry block.
# The language
fn fib(n) {

    if n < 2 { return n; }

    return fib(n - 1) + fib(n - 2);

}

fn main() {

    let i = 0;

    while i < 10 { print(fib(i)); i = i + 1; }

    return 0;

}

Supports fn, let, assignment, if/else if/else, while, return, print, function calls, comparison and arithmetic operators, and // comments.
# Build and run
g++ -std=c++20 -O2 -Wall -Wextra project11_tinyc.cpp -o tinyc

./tinyc                  # compile the built-in demo, IR to stdout

./tinyc prog.tiny > prog.ll

clang prog.ll -o prog && ./prog      # needs LLVM 15+ (opaque pointers)

A summary of constant folds and removed dead statements is printed to stderr.
# Known limitations
Integers only: no strings, floats, arrays, or user-defined types.
No type checking beyond arity (there is only one type).
print is a built-in statement, not a function.
