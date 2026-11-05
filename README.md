# S#

A hobby programming language written in C11.

S# is a statically typed programming language with a modern C#-inspired syntax and top-level statements.

The compiler directly generates x86-64 machine code and writes Linux ELF64 executables without invoking an external assembler, compiler, or linker during compilation.

## Features

- C#-inspired syntax
- Static typing
- Top-level statements
- Lexer and recursive-descent parser
- Semantic analysis
- Function declarations, parameters, and return types
- Integer and boolean expressions
- Arithmetic and comparison operators
- Logical operators with short-circuit evaluation
- Conditional statements (`if` / `else`)
- `while` and `for` loops
- Nested loops
- Variable declarations and assignments
- Direct x86-64 machine-code generation
- Native ELF64 executable generation
- `Console.WriteLine` with zero or one supported expression
- Linux `write` and `exit` syscalls without libc in generated executables

## Requirements

- GCC (to build the S# compiler)
- Make
- x86-64 Linux

## Build

```bash
make
```

## Compile and Run

```bash
./build/ssharp examples/hello.ss -o build/hello
./build/hello
```

If `-o` is omitted, the compiler writes the executable next to the source file, with the `.ss` extension removed:

```bash
./build/ssharp examples/hello.ss
./examples/hello
```

## Clean

```bash
make clean
```

## Current Limitations

The initial native backend does not support:

- Floating-point operations
- String interpolation
- String concatenation
- Classes and objects
- Garbage collection

Unsupported language features are reported as compile errors where implemented.

## Executable Format

Generated executables are Linux x86-64 ELF64 `ET_EXEC` images.

The compiler writes ELF binaries directly, without producing intermediate assembly or object files.

Generated programs use Linux system calls for output and process termination, without requiring libc.

## License

Personal hobby project.