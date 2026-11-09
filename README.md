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
- `Console.Write` and `Console.WriteLine` with multiple values
- String concatenation and common instance operations
- Composite string formatting for strings, integers, and booleans
- Core `Convert` methods for integral, floating-point, boolean, character, and string values
- `char` and `ulong` values returned by `Convert`
- Single-dimensional `int[]`, `bool[]`, and `string[]` arrays
- Linux `write` and `exit` syscalls without libc in generated executables

## Requirements

- GCC (to build the S# compiler)
- Make
- x86-64 Linux

## Project structure

The project keeps compiler stages split between dedicated source and header files, and the standard runtime helpers live in their own modules instead of being piled into a single giant source file.

```text
include/
  runtime/
    console.h
    math.h
    string.h
    string_native.h
    array_native.h
    convert_native.h
src/
  runtime/
    console.c
    math.c
    string.c
    string_native.c
    array_native.c
    convert_native.c
    convert_native.ld
```

This keeps feature-specific code easy to locate and prevents a small change from forcing a full-file search through a huge monolithic implementation.

## Console support

The native backend supports `Console.Write`, `Console.WriteLine`, and
`Console.ReadLine` and `Console.ReadKey`. Output methods support strings, integers, booleans, and values produced by
`Convert` (including `char`, `ulong`, and floating-point values); multiple
arguments are written consecutively.
`WriteLine` appends one line break after the final argument. Composite
formatting supports indexed `{0}` placeholders and escaped braces in a string
literal, with string, integer, and boolean arguments. Format specifiers and
non-literal format strings are not supported. `Console.Clear`,
`Console.ResetColor`, the `Title` setter, `ForegroundColor` and
`BackgroundColor` setters, and `SetCursorPosition` emit ANSI terminal control
sequences. Color setters accept `ConsoleColor` numeric values from 0 to 15.
For example:

```csharp
string name = Console.ReadLine();
Console.WriteLine("Hello ", name);
```

`ReadLine` grows its input buffer as needed. `ReadKey` switches a terminal to
single-byte input and restores its previous settings; when stdin is not a
terminal, it reads one byte. Returned input buffers remain allocated for the
lifetime of the generated process. `ReadKey` does not yet decode multi-byte
Unicode or escape-sequence keys. `Title`, color, and cursor-position property
getters are not supported because terminals do not provide a portable way to
query those values. `Console.Beep` is intentionally not implemented.

## String support

Strings support concatenation, `Length`, `Contains`, `StartsWith`, `EndsWith`,
`IndexOf`, `LastIndexOf`, `Replace`, both `Substring` overloads, `Trim`,
`TrimStart`, `TrimEnd`, `ToUpper`, `ToLower`, `String.IsNullOrEmpty`,
`String.IsNullOrWhiteSpace`, and `String.Format`. `String.Format` supports
indexed `{0}` placeholders and escaped braces, with string, integer, boolean,
and null values; format specifiers, alignment, and malformed format strings
are not supported.

String literals are stored as UTF-8 bytes and preserved when written. The
supported escapes are newline, carriage return, tab, backslash, and quote;
Unicode escape sequences are not implemented. Concatenating a null string
produces the other operand, as in C#.
`Length`, indexes returned by search methods, and `Substring` offsets count
bytes rather than Unicode scalar values. Case conversion is ASCII-only;
trimming recognizes ASCII whitespace. Character literals, string indexing,
interpolation, `Split`, and `Join` are not implemented.

Literals are immutable. Operations that produce strings allocate separate
buffers in the generated process, which remain valid until that process exits;
there is currently no garbage collector or explicit deallocation. Invalid
substring ranges, empty replacement patterns, malformed format strings, and
allocation failures terminate the generated process with a nonzero status.

## Array support

Single-dimensional `int[]`, `bool[]`, and `string[]` arrays can be declared
with an initializer (`int[] values = { 1, 2, 3 };`) or allocated at runtime
(`int[] values = new int[3];`). Newly allocated arrays are zero-initialized.
Arrays support indexed reads and writes plus the read-only `Length` property.
`Array.Sort`, `Array.Reverse`, `Array.IndexOf`, `Array.LastIndexOf`,
`Array.Copy`, `Array.Resize`, `Array.Clear`, and `Array.Fill` are supported
for one-dimensional arrays. `Array.Resize` returns a replacement array, so
assign its result back (for example, `values = Array.Resize(values, 8);`).
`Array.Copy` uses the five-argument source-index/destination-index/count form.
Sort orders integers numerically, booleans as `false` then `true`, and strings
by their UTF-8 byte sequence.

Every access and range operation checks bounds; negative lengths, invalid
indexes/ranges, and allocation failures terminate the generated program with
a nonzero status. Array storage is retained until process exit.

Multidimensional and jagged arrays, predicate-based `Array.Exists`,
`Array.Find`, and `Array.FindIndex` (lambdas are not yet supported), and
`Split`/`Join` are not implemented yet.

## Convert support

`Convert.ToByte`, `ToSByte`, `ToInt16`, `ToUInt16`, `ToInt32`, `ToUInt32`,
`ToInt64`, `ToUInt64`, `ToSingle`, `ToDouble`, `ToDecimal`, `ToBoolean`,
`ToChar`, and `ToString` accept one supported value. Integer conversions from
strings validate decimal digits and target ranges; floating-point to integer
conversions round to nearest with midpoint-to-even behavior. `ToChar` accepts
a BMP Unicode character or code point, and `char` values can be stored and
written as UTF-8. `ulong` values are supported for `ToUInt64` results and
string conversion.

Because S# has no distinct byte, sbyte, short, ushort, int, uint, or long
storage types, those integral conversion results use `int` while preserving
their checked target ranges; `ToUInt64` uses `ulong`.
Null converts to zero, `false`, the zero character, or an empty string
according to the target type. Invalid text and overflow terminate the
generated process with a nonzero exit status. `ToDecimal` currently uses the
backend's `double` representation, so it does not provide .NET `decimal`'s
96-bit precision or exact decimal arithmetic. Floating-point values can be
converted and written, but floating-point arithmetic and literals are not
implemented by the direct ELF backend. Floating-point `ToString` uses
invariant formatting with up to nine fractional digits; it is not guaranteed
to produce .NET's shortest round-trippable representation.

## Build

```bash
make
```

Run the integration tests with:

```bash
make test
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

The native backend does not support:

- Floating-point operations
- String interpolation
- String indexing or character literals
- Multidimensional and jagged arrays
- Array utility methods, including `Split` and `Join`
- Classes and objects
- Garbage collection

Unsupported language features are reported as compile errors where implemented.

## Executable Format

Generated executables are Linux x86-64 ELF64 `ET_EXEC` images.

The compiler writes ELF binaries directly, without producing intermediate assembly or object files.

Generated programs use Linux system calls for output and process termination, without requiring libc.

## License

Personal hobby project.