#pragma once

#define PANIC(msg) kpanic_at(__FILE__, __LINE__, msg)

__attribute__((noreturn)) void kpanic_at(const char *file, int line, const char *msg);
__attribute__((noreturn)) void kpanic(const char *fmt, ...);
