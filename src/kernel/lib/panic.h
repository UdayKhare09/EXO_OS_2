#pragma once

#define PANIC(msg) kpanic_at(__FILE__, __LINE__, msg)

__attribute__((noreturn)) void kpanic_at(const char *file, int line, const char *msg);
__attribute__((noreturn)) void kpanic(const char *fmt, ...);
/* Used by the Rust panic handler (src/rust/src/panic.rs) — no-arg halt. */
__attribute__((noreturn)) void kpanic_halt(void);

