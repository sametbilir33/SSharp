#define _POSIX_C_SOURCE 200809L

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "elf_writer.h"

#define ELF_BASE_ADDRESS UINT64_C(0x400000)
#define ELF_PAGE_SIZE UINT64_C(0x1000)

static int write_all(int fd, const unsigned char *data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        ssize_t result = write(fd, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (result == 0)
            return 0;
        written += (size_t)result;
    }
    return 1;
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

int elf64_write_executable(
    const char *path,
    const unsigned char *code,
    size_t code_size,
    const unsigned char *data,
    size_t data_size,
    size_t entry_offset)
{
    if (!path || !code || code_size == 0 || entry_offset >= code_size ||
        (!data && data_size != 0)) {
        fprintf(stderr, "S# ELF: Invalid executable image.\n");
        return 0;
    }

    if ((uint64_t)code_size > (uint64_t)INT64_MAX ||
        (uint64_t)data_size >
            (uint64_t)INT64_MAX - (uint64_t)code_size) {
        fprintf(stderr, "S# ELF: Executable image is too large.\n");
        return 0;
    }

    uint64_t text_file_offset = ELF_PAGE_SIZE;
    uint64_t text_size = (uint64_t)code_size + (uint64_t)data_size;
    if (text_size > (uint64_t)INT64_MAX - 2 * ELF_PAGE_SIZE) {
        fprintf(stderr, "S# ELF: Executable image is too large.\n");
        return 0;
    }

    uint64_t writable_file_offset =
        align_up(text_file_offset + text_size, ELF_PAGE_SIZE);
    uint64_t file_size = writable_file_offset + ELF_PAGE_SIZE;
    if (file_size > (uint64_t)INT64_MAX) {
        fprintf(stderr, "S# ELF: Executable image is too large.\n");
        return 0;
    }

    Elf64_Ehdr header = {0};
    memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_ident[EI_OSABI] = ELFOSABI_SYSV;
    header.e_type = ET_EXEC;
    header.e_machine = EM_X86_64;
    header.e_version = EV_CURRENT;
    header.e_entry = ELF_BASE_ADDRESS + text_file_offset + entry_offset;
    header.e_phoff = sizeof(Elf64_Ehdr);
    header.e_ehsize = sizeof(Elf64_Ehdr);
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = 2;

    Elf64_Phdr program_headers[2] = {0};
    program_headers[0].p_type = PT_LOAD;
    program_headers[0].p_flags = PF_R | PF_X;
    program_headers[0].p_offset = text_file_offset;
    program_headers[0].p_vaddr = ELF_BASE_ADDRESS + text_file_offset;
    program_headers[0].p_paddr = program_headers[0].p_vaddr;
    program_headers[0].p_filesz = text_size;
    program_headers[0].p_memsz = text_size;
    program_headers[0].p_align = ELF_PAGE_SIZE;

    program_headers[1].p_type = PT_LOAD;
    program_headers[1].p_flags = PF_R | PF_W;
    program_headers[1].p_offset = writable_file_offset;
    program_headers[1].p_vaddr = ELF_BASE_ADDRESS + writable_file_offset;
    program_headers[1].p_paddr = program_headers[1].p_vaddr;
    program_headers[1].p_filesz = ELF_PAGE_SIZE;
    program_headers[1].p_memsz = ELF_PAGE_SIZE;
    program_headers[1].p_align = ELF_PAGE_SIZE;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        perror(path);
        return 0;
    }

    int success = 1;
    unsigned char zero_page[ELF_PAGE_SIZE] = {0};
    if (!write_all(fd, (const unsigned char *)&header, sizeof(header)) ||
        !write_all(
            fd,
            (const unsigned char *)program_headers,
            sizeof(program_headers))) {
        success = 0;
    }

    off_t current_offset = (off_t)(sizeof(header) + sizeof(program_headers));
    unsigned char zeros[ELF_PAGE_SIZE] = {0};
    while (success && current_offset < (off_t)text_file_offset) {
        size_t remaining = (size_t)((off_t)text_file_offset - current_offset);
        size_t chunk = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
        success = write_all(fd, zeros, chunk);
        current_offset += (off_t)chunk;
    }

    if (success)
        success = write_all(fd, code, code_size);
    if (success && data_size)
        success = write_all(fd, data, data_size);

    current_offset = (off_t)(text_file_offset + text_size);
    while (success && current_offset < (off_t)writable_file_offset) {
        size_t remaining =
            (size_t)((off_t)writable_file_offset - current_offset);
        size_t chunk = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
        success = write_all(fd, zeros, chunk);
        current_offset += (off_t)chunk;
    }

    if (success)
        success = write_all(fd, zero_page, sizeof(zero_page));
    if (success && fchmod(fd, 0755) != 0)
        success = 0;
    if (close(fd) != 0)
        success = 0;

    if (!success) {
        perror("S# ELF: failed to write executable");
        unlink(path);
        return 0;
    }

    return 1;
}
