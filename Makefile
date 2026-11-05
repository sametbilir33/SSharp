
CC = gcc

CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Iinclude -O2

TARGET = build/ssharp

SOURCES = \
    src/main.c \
    src/token.c \
    src/lexer.c \
    src/ast.c \
    src/parser.c \
    src/semantic.c \
    src/codegen.c \
    src/elf_writer.c

HEADERS = \
    include/token.h \
    include/lexer.h \
    include/ast.h \
    include/parser.h \
    include/semantic.h \
    include/codegen.h \
    include/elf_writer.h

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET) examples/hello.ss -o build/hello
	./build/hello

.PHONY: all clean run
