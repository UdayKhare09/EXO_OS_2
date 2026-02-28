Plan: Implement Dynamic Linking for EXO_OS
Full dynamic linking support: kernel-side ELF interpreter loading, demand-paged file-backed mmap, MAP_SHARED, page cache, physical page refcounting, and userspace dlopen/dlsym/dlclose via musl's dynamic linker. The kernel's role is to load the interpreter (ld-musl), pass the right auxv entries, and provide a robust mmap — musl handles symbol resolution, relocations, GOT/PLT patching, and dlopen in userspace.

Steps

Phase 1 — Physical Page Reference Counting
Add a uint32_t *page_refcounts array in pmm.c alongside the existing bitmap, allocated during pmm_init(). Size: one uint32_t per tracked physical page.
Add pmm_page_ref(phys), pmm_page_unref(phys) → returns new refcount, pmm_page_getref(phys) to pmm.h. On pmm_alloc_pages(), set refcount to 1. On pmm_free_pages(), assert refcount is 0.
Update COW clone in vmm.c:299 — when COW-marking a shared page, call pmm_page_ref() to bump refcount to 2.
Update COW fault handler in vmm.c:348 — after copying, pmm_page_unref() old page; free it if refcount hits 0.
Update vmm.c:159 — free_pt_level() should pmm_page_unref() every leaf page and free only when refcount reaches 0. Remove the current skip-COW-pages hack.
Update proc_syscalls.c:631 — use pmm_page_unref() instead of direct pmm_free_pages().
Phase 2 — Extend VMA for File-Backed Mappings
Extend vma_t in task.h:38 with new fields: vnode_t *file (NULL for anonymous), uint64_t file_offset (offset into file), uint64_t file_size (bytes of file data in this VMA), and uint32_t mmap_flags (to store MAP_SHARED vs MAP_PRIVATE). Add VMA_SHARED (128) and VMA_FILE (256) flag constants.
Update vma_list_clone() in proc_syscalls.c — for file-backed VMAs, bump vnode->refcount via vfs_vnode_get() on the copied VMA's file pointer.
Update task_destroy() in task.c:155 — when freeing VMAs, call vfs_vnode_put() for any non-NULL vma->file.
Phase 3 — Page Cache
Create new files src/kernel/mm/pagecache.c and src/kernel/mm/pagecache.h. Data structure: hash table keyed on (vnode_t *, page_offset) → pagecache_entry_t { vnode_t *vn; uint64_t offset; uintptr_t phys_page; uint32_t refcount; struct pagecache_entry *hash_next; }. LRU eviction list for reclaim.
Implement pagecache_lookup(vnode, offset) → returns physical page address or 0 (miss). On hit, bump refcount.
Implement pagecache_insert(vnode, offset, phys) → inserts page, sets refcount to 1. Bumps pmm_page_ref() on the physical page.
Implement pagecache_release(vnode, offset) → decrements refcount, moves to LRU tail when refcount=0 for potential eviction.
Implement pagecache_get_or_read(vnode, offset) → lookup, if miss: pmm_alloc_pages(1), read 4096 bytes from file via vnode->ops->read(), insert into cache, return phys. This is the main entry point for demand paging.
Phase 4 — Demand-Paged File-Backed mmap
Modify proc_syscalls.c:535 for file-backed MAP_PRIVATE:

Stop eagerly reading file content. Instead, just create the VMA with file, file_offset, file_size fields populated, and return the address. No physical pages allocated yet.
Keep MAP_ANONYMOUS behavior (eager for small mappings is fine, or also make it lazy — VMA_ANON demand-paging already works in the fault handler).
Modify vmm.c:366 Case 2 (not-present fault):

Find VMA via find_vma_for() (already done).
If VMA_ANON: allocate zero page (already done).
If VMA_FILE and !VMA_SHARED (MAP_PRIVATE): call pagecache_get_or_read(vma->file, file_page_offset), copy page content into a new private page, map the private page. This gives COW semantics.
If VMA_FILE and VMA_SHARED (MAP_SHARED): call pagecache_get_or_read(), map the same physical page directly (with pmm_page_ref()), with appropriate permissions. Multiple processes will share the same physical page.
Phase 5 — MAP_SHARED
Remove the -ENOSYS for MAP_SHARED in sys_mmap(). For MAP_SHARED | file-backed:

Create VMA with VMA_SHARED | VMA_FILE flags.
Pages are demand-paged from the page cache (step 16).
On write to a shared page: page is already writable (if VMA allows), write goes to the page cache page. No COW.
For MAP_SHARED | MAP_ANONYMOUS (shared anonymous memory): allocate pages normally, set VMA_SHARED. On fork, share the same physical pages (no COW) — bump pmm_page_ref() in vmm_clone_address_space() for shared VMAs instead of COW-marking them.

Update sys_munmap() — for shared pages, pmm_page_unref() instead of pmm_free_pages(). For page-cache-backed pages, call pagecache_release().

Add SMP-safe atomic refcounting to vnode_t.refcount in vfs.h:168 — use __atomic_fetch_add / __atomic_fetch_sub instead of bare ++/--.

Phase 6 — ELF Loader: Shared Object Support
Add a uint64_t load_bias parameter to elf_load() in elf.c. For ET_DYN ELFs, add load_bias to every p_vaddr before mapping. For ET_EXEC, load_bias is 0. Update elf_info_t.entry to include the bias. Add elf_info_t.load_base field (the actual base address the ELF was loaded at).

Add elf_load_interp() — a new function that:

Takes an interpreter path (from PT_INTERP), opens it via VFS, reads the ELF.
Loads the interpreter ELF into the same address space at a chosen base (e.g., starting from USER_MMAP_BASE, or a fixed high address like 0x7F0000000000).
Returns the interpreter's entry point and load base.
Extract PT_INTERP in elf_load(): when encountering a PT_INTERP segment, copy out the null-terminated interpreter path string (e.g., ld-musl-x86_64.so.1). Add char interp_path[256] and bool has_interp to elf_info_t.

Phase 7 — execve: Dynamic Executable Support
Modify proc_syscalls.c:212 after elf_load():

If info.has_interp is true:
a. Open info.interp_path via VFS, read into buffer, call elf_load() with a non-zero load_bias (use mmap_next as the base, advance mmap_next past the loaded interpreter).
b. Set the actual entry point to the interpreter's entry, not the executable's.
c. The executable's entry goes into AT_ENTRY auxv (musl reads this to find the real program).
Add these auxv entries to the proc_syscalls.c:365:

AT_BASE (7) — interpreter load base address.
AT_RANDOM (25) — pointer to 16 random bytes on the stack (musl uses this for stack canaries; can be filled with a simple PRNG or RDRAND).
AT_EXECFN (31) — pointer to the executable filename string on the stack.
Adjust AT_ENTRY to always be the main executable's entry point (not the interpreter's).
Phase 8 — Rootfs: Install Dynamic musl
Update the makefile to add a target that copies the system's dynamic musl files into the rootfs image:

ld-musl-x86_64.so.1 — the dynamic linker/interpreter (from system musl, typically at lib or lib).
libc.so → symlink to ld-musl-x86_64.so.1 (musl's libc.so and ld.so are the same binary).
/lib/libdl.so, /lib/libpthread.so, libm.so → empty stub archives or symlinks to libc.so (musl bundles everything in one binary).
Add a new makefile macro musl-link-dynamic alongside the existing musl-link (static). It should:

Compile with -fPIC (or -fPIE for executables).
Link with --dynamic-linker=/lib/ld-musl-x86_64.so.1 instead of -static.
Link against shared libc.so instead of static libc.a.
Add a test dynamic executable (e.g., tools/hello_dynamic.c) built with musl-link-dynamic to validate the pipeline end-to-end.

Phase 9 — dlopen / dlsym / dlclose
These are userspace functions provided by musl — no kernel work needed if the following are in place:

mmap (file-backed MAP_PRIVATE) — musl's dlopen() mmaps .so files ✓ (after Phase 4)
mprotect — musl patches page permissions for RELRO ✓ (already works)
open, read, fstat, close — musl reads ELF headers of .so files ✓ (already works)
AT_PHDR, AT_PHNUM, AT_PHENT in auxv ✓ (already provided)
Ensure musl's libdl.so functions (dlopen, dlsym, dlerror, dlclose) are accessible. Since musl bundles libdl into libc.so, programs linking -ldl just need the libc.so present in lib.

Add a test program tools/dlopen_test.c that:

Calls dlopen("libtest.so", RTLD_LAZY).
Calls dlsym(handle, "test_function").
Invokes the loaded function, verifies the return value.
Calls dlclose(handle).
Build a trivial libtest.so as well to exercise this path.
Phase 10 — Cleanup and Edge Cases
Update sys_mprotect() in proc_syscalls.c:706 to also update VMA flags, not just PTE flags — otherwise VMA metadata drifts out of sync with actual permissions.
Add MAP_DENYWRITE (ignored, for compat) and MAP_FIXED_NOREPLACE flag handling in sys_mmap() — musl's dynamic linker may pass these.
Handle mmap with offset != 0 for file-backed mappings — current code starts reading from file offset 0; need to honor the offset parameter properly for .so segment loading.
Add AT_UID, AT_GID, AT_EUID, AT_EGID auxv entries — musl references these for setuid checks during dynamic linking.
Verification

Unit test for refcounting: Write a kernel-side test that allocates a page, refs it to 3, unrefs back to 0, verifies it's freed.
Static regression: Rebuild existing static binaries (posix_suite, hello, busybox), run them — they must still work unchanged.
Dynamic hello: Build hello_dynamic with musl-link-dynamic, boot and run it. Verify ld-musl-x86_64.so.1 is loaded by the kernel, musl initializes, and puts() works.
Shared library test: Build libtest.so + dlopen_test, run it. Verify dlopen → dlsym → call → dlclose works.
MAP_SHARED test: Two processes mmap the same file with MAP_SHARED; one writes, the other reads — verify data is visible.
fork + dynamic: Fork a dynamically linked process, both parent and child should run correctly (tests COW + refcounting on shared library pages).
Memory usage: Run busybox (static) vs. a dynamically linked equivalent — verify no page leaks by checking PMM free page counts across process lifecycle.
Decisions

Interpreter load address: Use the mmap_next bump allocator to place the interpreter right after the main executable's segments — simple and avoids collisions.
Page cache scope: Start with a simple hash table with no eviction pressure (grow-only). Add LRU eviction as a follow-up when memory pressure management is added.
musl origin: Copy dynamic musl (ld-musl-x86_64.so.1, libc.so) from the host system's lib rather than building musl from source. Less complexity.
Demand paging granularity: Only file-backed mmap is demand-paged. Anonymous mmap and ELF PT_LOAD segments remain eagerly allocated initially — can be made lazy later.
dlopen is pure userspace: No new syscalls needed. musl's dlopen uses existing open/mmap/mprotect/close.