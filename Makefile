
CC = gcc

CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Iinclude -O2
LDFLAGS = -lm

TARGET = build/ssharp

SOURCES = \
    src/main.c \
    src/token.c \
    src/lexer.c \
    src/ast.c \
    src/parser.c \
    src/semantic.c \
    src/codegen.c \
    src/elf_writer.c \
    src/runtime/console.c \
    src/runtime/math.c \
    src/runtime/string.c

STRING_NATIVE_OBJECT = build/string_native.o
STRING_NATIVE_BLOB = build/string_native.bin
STRING_NATIVE_LINK = build/string_native_blob.o
ARRAY_NATIVE_OBJECT = build/array_native.o
ARRAY_NATIVE_BLOB = build/array_native.bin
ARRAY_NATIVE_LINK = build/array_native_blob.o
CONVERT_NATIVE_OBJECT = build/convert_native.o
CONVERT_NATIVE_ELF = build/convert_native.elf
CONVERT_NATIVE_BLOB = build/convert_native.bin
CONVERT_NATIVE_LINK = build/convert_native_blob.o

HEADERS = \
    include/token.h \
    include/lexer.h \
    include/ast.h \
    include/parser.h \
    include/semantic.h \
    include/codegen.h \
    include/elf_writer.h \
    include/runtime/console.h \
    include/runtime/math.h \
    include/runtime/string.h \
    include/runtime/string_native.h \
    include/runtime/array_native.h \
    include/runtime/convert_native.h

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) $(STRING_NATIVE_LINK) $(ARRAY_NATIVE_LINK) $(CONVERT_NATIVE_LINK)
	mkdir -p build
	$(CC) $(CFLAGS) $(SOURCES) $(STRING_NATIVE_LINK) $(ARRAY_NATIVE_LINK) $(CONVERT_NATIVE_LINK) \
		-o $(TARGET) $(LDFLAGS)

$(STRING_NATIVE_OBJECT): src/runtime/string_native.c include/runtime/string_native.h
	mkdir -p build
	$(CC) $(CFLAGS) -fno-pie -fno-asynchronous-unwind-tables \
		-fno-unwind-tables -fno-builtin -fno-jump-tables \
		-fno-stack-protector -fno-tree-vectorize \
		-fno-tree-slp-vectorize \
		-c $< -o $@

$(STRING_NATIVE_BLOB): $(STRING_NATIVE_OBJECT)
	@if objdump -r -j .text.ssharp_string_native $< | grep -q 'R_X86_64'; then \
		echo "String runtime contains unresolved relocations"; exit 1; \
	fi
	objcopy --only-section=.text.ssharp_string_native -O binary $< $@

$(STRING_NATIVE_LINK): $(STRING_NATIVE_BLOB)
	cd build && ld -r -b binary string_native.bin -o string_native_blob.o

$(ARRAY_NATIVE_OBJECT): src/runtime/array_native.c include/runtime/array_native.h
	mkdir -p build
	$(CC) $(CFLAGS) -fno-pie -fno-asynchronous-unwind-tables \
		-fno-unwind-tables -fno-builtin -fno-jump-tables \
		-fno-stack-protector -fno-tree-vectorize \
		-fno-tree-slp-vectorize \
		-c $< -o $@

$(ARRAY_NATIVE_BLOB): $(ARRAY_NATIVE_OBJECT)
	@if objdump -r -j .text.ssharp_array_native $< | grep -q 'R_X86_64'; then \
		echo "Array runtime contains unresolved relocations"; exit 1; \
	fi
	objcopy --only-section=.text.ssharp_array_native -O binary $< $@

$(ARRAY_NATIVE_LINK): $(ARRAY_NATIVE_BLOB)
	cd build && ld -r -b binary array_native.bin -o array_native_blob.o

$(CONVERT_NATIVE_OBJECT): src/runtime/convert_native.c include/runtime/convert_native.h
	mkdir -p build
	$(CC) $(CFLAGS) -fPIC -fno-asynchronous-unwind-tables \
		-fno-unwind-tables -fno-builtin -fno-jump-tables \
		-fno-stack-protector -fno-tree-vectorize \
		-fno-tree-slp-vectorize \
		-c $< -o $@

$(CONVERT_NATIVE_ELF): $(CONVERT_NATIVE_OBJECT) src/runtime/convert_native.ld
	ld -T src/runtime/convert_native.ld -o $@ $<

$(CONVERT_NATIVE_BLOB): $(CONVERT_NATIVE_ELF)
	@if objdump -r -j .text.ssharp_convert_native $< | grep -q 'R_X86_64'; then \
		echo "Convert runtime contains unresolved relocations"; exit 1; \
	fi
	objcopy --only-section=.text.ssharp_convert_native -O binary $< $@

$(CONVERT_NATIVE_LINK): $(CONVERT_NATIVE_BLOB)
	cd build && ld -r -b binary convert_native.bin -o convert_native_blob.o

clean:
	rm -f $(TARGET) $(STRING_NATIVE_OBJECT) $(STRING_NATIVE_BLOB) \
		$(STRING_NATIVE_LINK) $(ARRAY_NATIVE_OBJECT) $(ARRAY_NATIVE_BLOB) \
		$(ARRAY_NATIVE_LINK)
	rm -f $(CONVERT_NATIVE_OBJECT) $(CONVERT_NATIVE_ELF) $(CONVERT_NATIVE_BLOB) \
		$(CONVERT_NATIVE_LINK)

run: $(TARGET)
	./$(TARGET) examples/hello.ss -o build/hello
	./build/hello

test: $(TARGET)
	./$(TARGET) examples/array-basic.ss -o build/array-basic-test
	./build/array-basic-test | diff -u tests/array-basic.expected -
	./$(TARGET) tests/array-out-of-bounds.ss -o build/array-out-of-bounds-test
	if ./build/array-out-of-bounds-test >/dev/null 2>&1; then \
		echo "Expected array bounds violation to fail"; exit 1; \
	fi
	./$(TARGET) tests/array-negative-index.ss -o build/array-negative-index-test
	if ./build/array-negative-index-test >/dev/null 2>&1; then \
		echo "Expected negative array index to fail"; exit 1; \
	fi
	./$(TARGET) tests/array-negative-length.ss -o build/array-negative-length-test
	if ./build/array-negative-length-test >/dev/null 2>&1; then \
		echo "Expected negative array length to fail"; exit 1; \
	fi
	./$(TARGET) tests/array-invalid-copy-range.ss -o build/array-invalid-copy-range-test
	if ./build/array-invalid-copy-range-test >/dev/null 2>&1; then \
		echo "Expected invalid Array.Copy range to fail"; exit 1; \
	fi
	./$(TARGET) tests/array-invalid-fill-range.ss -o build/array-invalid-fill-range-test
	if ./build/array-invalid-fill-range-test >/dev/null 2>&1; then \
		echo "Expected invalid Array.Fill range to fail"; exit 1; \
	fi
	./$(TARGET) tests/array-negative-fill-range.ss -o build/array-negative-fill-range-test
	if ./build/array-negative-fill-range-test >/dev/null 2>&1; then \
		echo "Expected negative Array.Fill count to fail"; exit 1; \
	fi
	./$(TARGET) examples/string-api.ss -o build/string-api-test
	./build/string-api-test | diff -u tests/string-api.expected -
	./$(TARGET) examples/console.ss -o build/console-test
	printf 'Samet\n' | ./build/console-test | diff -u tests/console.expected -
	./$(TARGET) examples/console-blank-line.ss -o build/console-blank-line-test
	test "$$(printf '' | ./build/console-blank-line-test | wc -c)" -eq 1
	./$(TARGET) examples/console-readkey.ss -o build/console-readkey-test
	printf 'K' | ./build/console-readkey-test | diff -u tests/console-readkey.expected -
	./$(TARGET) examples/console-terminal.ss -o build/console-terminal-test
	test "$$(./build/console-terminal-test)" = \
		"$$(printf '\033]0;SSharp\a\033[30m\033[91m\033[104m\033[97m\033[0m\033[35;13H')"
	./$(TARGET) examples/convert-api.ss -o build/convert-api-test
	./build/convert-api-test | diff -u tests/convert-api.expected -
	./$(TARGET) tests/convert-invalid-overflow.ss -o build/convert-invalid-overflow-test
	if ./build/convert-invalid-overflow-test >/dev/null 2>&1; then \
		echo "Expected Convert overflow to fail"; exit 1; \
	fi
	./$(TARGET) tests/convert-invalid-integer.ss -o build/convert-invalid-integer-test
	if ./build/convert-invalid-integer-test >/dev/null 2>&1; then \
		echo "Expected invalid Convert integer text to fail"; exit 1; \
	fi
	./$(TARGET) tests/convert-invalid-boolean.ss -o build/convert-invalid-boolean-test
	if ./build/convert-invalid-boolean-test >/dev/null 2>&1; then \
		echo "Expected invalid Convert boolean text to fail"; exit 1; \
	fi
	./$(TARGET) tests/console-invalid-color.ss -o build/console-invalid-color-test
	if ./build/console-invalid-color-test >/dev/null 2>&1; then \
		echo "Expected invalid ConsoleColor to fail"; exit 1; \
	fi
	./$(TARGET) tests/console-invalid-cursor.ss -o build/console-invalid-cursor-test
	if ./build/console-invalid-cursor-test >/dev/null 2>&1; then \
		echo "Expected negative cursor position to fail"; exit 1; \
	fi
	if ./$(TARGET) tests/console-property-getter.ss -o build/console-property-getter-test; then \
		echo "Expected unsupported Console property getter to fail"; exit 1; \
	fi
	if ./$(TARGET) tests/console-invalid-format.ss -o build/console-invalid-format-test; then \
		echo "Expected invalid composite format to fail"; exit 1; \
	fi

.PHONY: all clean run test
