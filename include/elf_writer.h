#ifndef SSHARP_ELF_WRITER_H
#define SSHARP_ELF_WRITER_H

#include <stddef.h>

int elf64_write_executable(
    const char *path,
    const unsigned char *code,
    size_t code_size,
    const unsigned char *data,
    size_t data_size,
    size_t entry_offset);

#endif
