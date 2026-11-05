#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "ast.h"

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(file);
        return NULL;
    }

    long file_size = ftell(file);
    if (file_size < 0) {
        perror("ftell");
        fclose(file);
        return NULL;
    }
    rewind(file);

    char *buffer = malloc((size_t)file_size + 1);
    if (!buffer) {
        fprintf(stderr, "S#: Out of memory.\n");
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, (size_t)file_size, file);
    if (bytes_read != (size_t)file_size) {
        fprintf(stderr, "S#: Failed to read file.\n");
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    fclose(file);
    return buffer;
}

static char *default_output_path(const char *input_path)
{
    size_t length = strlen(input_path);
    size_t output_length = length;
    if (length >= 3 && strcmp(input_path + length - 3, ".ss") == 0)
        output_length -= 3;

    char *output = malloc(output_length + 1);
    if (!output) {
        fprintf(stderr, "S#: Out of memory.\n");
        return NULL;
    }

    memcpy(output, input_path, output_length);
    output[output_length] = '\0';
    return output;
}

static int compile_file(const char *input_path, const char *output_path)
{
    char *source = read_file(input_path);
    if (!source)
        return 0;

    Lexer lexer;
    lexer_init(&lexer, source);
    Stmt *program = parser_parse(&lexer);

    if (parser_had_error()) {
        fprintf(stderr, "S#: Parsing failed.\n");
        ast_free_stmt(program);
        free(source);
        return 0;
    }

    if (!semantic_analyze(program)) {
        fprintf(stderr, "S#: Semantic analysis failed.\n");
        ast_free_stmt(program);
        free(source);
        return 0;
    }

    int generated = codegen_write_x86_64_elf(program, output_path);
    ast_free_stmt(program);
    free(source);
    return generated;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 4) {
        fprintf(stderr,
            "S# Native Compiler\n"
            "Usage: ssharp <file.ss> [-o output]\n");
        return EXIT_FAILURE;
    }

    const char *input_path = argv[1];
    const char *output_path = NULL;
    char *allocated_output = NULL;

    if (argc == 4) {
        if (strcmp(argv[2], "-o") != 0 || argv[3][0] == '\0') {
            fprintf(stderr,
                "Usage: ssharp <file.ss> [-o output]\n");
            return EXIT_FAILURE;
        }
        output_path = argv[3];
    } else {
        allocated_output = default_output_path(input_path);
        if (!allocated_output)
            return EXIT_FAILURE;
        output_path = allocated_output;
    }

    int success = compile_file(input_path, output_path);
    if (success)
        fprintf(stderr, "S#: Native ELF binary written to %s\n", output_path);

    free(allocated_output);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
