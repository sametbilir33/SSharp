#ifndef SSHARP_CODEGEN_H
#define SSHARP_CODEGEN_H

#include "ast.h"

int codegen_write_x86_64_elf(Stmt *program, const char *path);

#endif
