#include <common/align.h>
#include <common/mm.h>
#include <common/rand.h>
#include <common/string.h>
#include <common/types.h>

#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/mm/linux_page_slice_file.h>
#include <linux_compat/proc/linux_exec_stack.h>
#include <linux_compat/proc_compat.h>
#include <modules/elf/elf.h>
#include <rendezvos/error.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread.h>

#define LINUX_EXEC_AUXV_MAX_PAIRS 24

/*
 * AT_HWCAP baseline. Do not set HWCAP_CPUID (1<<11): that makes glibc issue
 * EL0 `mrs …_el1` (e.g. MIDR_EL1), which traps as undefined unless the kernel
 * emulates ID-register MRS (Linux does; we do not yet). Seen on aarch64 as
 * ESR unknown/IL @ mrs midr_el1 shortly after busybox _start.
 */
#if defined(_AARCH64_)
/* HWCAP_FP | HWCAP_ASIMD | HWCAP_EVTSTRM */
#define LINUX_EXEC_AT_HWCAP_VAL ((1ULL << 0) | (1ULL << 1) | (1ULL << 2))
#else
#define LINUX_EXEC_AT_HWCAP_VAL 0ULL
#endif

typedef struct {
        u64 tag;
        u64 val;
} linux_exec_auxv_pair_t;

static error_t exec_store_u64(VSpace *vs, vaddr user_va, u64 value)
{
        return linux_mm_store_to_user(vs, (u64)user_va, &value, sizeof(value));
}

static vaddr linux_exec_elf_user_phdr_va(vaddr elf_start, u64 phoff)
{
        vaddr first_load = 0;

        for_each_program_header_64(elf_start)
        {
                if (phdr_ptr->p_type != PT_LOAD) {
                        continue;
                }
                if (first_load == 0) {
                        first_load = phdr_ptr->p_vaddr;
                }
                if (phoff >= phdr_ptr->p_offset
                    && phoff < phdr_ptr->p_offset + phdr_ptr->p_filesz) {
                        return phdr_ptr->p_vaddr + (phoff - phdr_ptr->p_offset);
                }
        }

        if (first_load != 0) {
                return first_load + phoff;
        }
        return 0;
}

bool linux_exec_elf_auxv_from_kva(vaddr elf_kva, linux_exec_elf_auxv_t *out)
{
        Elf64_Ehdr *eh;

        if (!out) {
                return false;
        }

        out->have_elf = false;
        out->phdr = 0;
        out->phent = 0;
        out->phnum = 0;
        out->entry = 0;

        if (!elf_kva || !check_elf_header(elf_kva)
            || get_elf_class(elf_kva) != ELFCLASS64) {
                return false;
        }

        eh = ELF64_HEADER(elf_kva);
        out->phdr = linux_exec_elf_user_phdr_va(elf_kva, eh->e_phoff);
        out->phent = eh->e_phentsize;
        out->phnum = eh->e_phnum;
        out->entry = eh->e_entry;
        out->have_elf = (out->phdr != 0 && out->phnum > 0 && out->phent != 0
                         && out->entry != 0);
        return out->have_elf;
}

bool linux_exec_elf_auxv_from_slice(struct page_slice *slice,
                                    linux_exec_elf_auxv_t *out)
{
        vaddr base;

        if (!slice) {
                return false;
        }
        base = linux_page_slice_file_base(slice);
        return linux_exec_elf_auxv_from_kva(base, out);
}

/* PRNG state for AT_RANDOM; advanced with core common/rand.h (rand64). */
static u64 linux_exec_rng_state = 1;

static void linux_exec_fill_random16(u8 buf[LINUX_EXEC_RANDOM_BYTES], vaddr mix)
{
        u64 s = linux_exec_rng_state;
        linux_proc_resource_t *task = linux_current_proc();
        Thread_Base *thr = get_cpu_current_thread();
        u64 lo, hi;

        s ^= (u64)mix;
        if (task) {
                s ^= (u64)task->pid << 32;
        }
        if (thr) {
                s ^= (u64)thr->tid;
        }
        s ^= (u64)percpu(cpu_number) << 48;
        if (s == 0) {
                s = 1;
        }

        /* Same advance pattern as core tests: next = rand64(next). */
        lo = rand64(s);
        hi = rand64(lo);
        linux_exec_rng_state = hi;

        for (u32 i = 0; i < 8; i++) {
                buf[i] = (u8)((lo >> (i * 8)) & 0xFFU);
                buf[i + 8] = (u8)((hi >> (i * 8)) & 0xFFU);
        }
}

static u32 linux_exec_auxv_fill_pairs(const linux_exec_elf_auxv_t *elf_auxv,
                                      vaddr random_va, vaddr execfn_va,
                                      linux_exec_auxv_pair_t *pairs, u32 cap)
{
        u32 n = 0;

#define LINUX_EXEC_AUXV_PUSH(aux_tag, aux_val)         \
        do {                                           \
                if (n < cap) {                         \
                        pairs[n].tag = (u64)(aux_tag); \
                        pairs[n].val = (u64)(aux_val); \
                        n++;                           \
                }                                      \
        } while (0)

        if (elf_auxv && elf_auxv->have_elf) {
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_PHDR, elf_auxv->phdr);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_PHENT, elf_auxv->phent);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_PHNUM, elf_auxv->phnum);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_PAGESZ, PAGE_SIZE);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_ENTRY, elf_auxv->entry);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_UID, 0);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_EUID, 0);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_GID, 0);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_EGID, 0);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_SECURE, 0);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_CLKTCK, 100);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_HWCAP, LINUX_EXEC_AT_HWCAP_VAL);
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_RANDOM, random_va);
                if (execfn_va) {
                        LINUX_EXEC_AUXV_PUSH(LINUX_AT_EXECFN, execfn_va);
                }
        } else {
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_PAGESZ, PAGE_SIZE);
        }

        LINUX_EXEC_AUXV_PUSH(LINUX_AT_NULL, 0);
#undef LINUX_EXEC_AUXV_PUSH
        return n;
}

vaddr linux_exec_build_initial_stack(VSpace *vs, vaddr stack_top, i64 argc,
                                     const char *kargv[], const char *execfn,
                                     const linux_exec_elf_auxv_t *elf_auxv,
                                     vaddr *argv_user_out)
{
        vaddr sp = stack_top;
        u64 strings_size = 0;
        vaddr strings_base;
        vaddr argv_ptr_area;
        vaddr random_va = 0;
        vaddr execfn_va = 0;
        linux_exec_auxv_pair_t pairs[LINUX_EXEC_AUXV_MAX_PAIRS];
        u32 pair_count;
        u8 random_bytes[LINUX_EXEC_RANDOM_BYTES];
        error_t e;
        i64 i;

        if (argv_user_out) {
                *argv_user_out = 0;
        }
        if (!vs || argc < 0 || !kargv) {
                return 0;
        }

        for (i = 0; i < argc; i++) {
                if (!kargv[i]) {
                        return 0;
                }
                strings_size += (u64)strlen(kargv[i]) + 1;
        }
        if (execfn) {
                strings_size += (u64)strlen(execfn) + 1;
        }

        sp -= strings_size;
        sp &= ~((vaddr)0xF);
        strings_base = sp;

        {
                vaddr string_va = strings_base;

                for (i = 0; i < argc; i++) {
                        size_t len = strlen(kargv[i]) + 1;

                        e = linux_mm_store_to_user(
                                vs, string_va, kargv[i], len);
                        if (e != REND_SUCCESS) {
                                return 0;
                        }
                        string_va += (vaddr)len;
                }
                if (execfn) {
                        size_t len = strlen(execfn) + 1;

                        execfn_va = string_va;
                        e = linux_mm_store_to_user(vs, string_va, execfn, len);
                        if (e != REND_SUCCESS) {
                                return 0;
                        }
                }
        }

        if (elf_auxv && elf_auxv->have_elf) {
                sp -= LINUX_EXEC_RANDOM_BYTES;
                sp &= ~((vaddr)0xF);
                random_va = sp;
                linux_exec_fill_random16(random_bytes, random_va);
                e = linux_mm_store_to_user(
                        vs, random_va, random_bytes, sizeof(random_bytes));
                if (e != REND_SUCCESS) {
                        return 0;
                }
        }

        pair_count = linux_exec_auxv_fill_pairs(elf_auxv,
                                                random_va,
                                                execfn_va,
                                                pairs,
                                                LINUX_EXEC_AUXV_MAX_PAIRS);

        /*
         * Contiguous low→high: [argc|argv…|NULL|envp NULL|auxv…|AT_NULL].
         * Align the final entry SP (argc address) per arch ABI by inserting
         * padding *above* auxv (between random/strings and auxv) — never
         * between envp and auxv, or glibc treats pad zeros as AT_NULL and
         * skips PHDR/ENTRY (user NULL deref at 0x0 on aarch64 busybox).
         *
         *   aarch64: entry SP % 16 == 0
         *   x86_64:  entry SP % 16 == 8  (as if _start were CALL'd)
         */
        {
                u64 auxv_bytes = (u64)pair_count * 2U * sizeof(u64);
                u64 vector_bytes = sizeof(u64) /* envp NULL */
                                   + (u64)(argc + 1) * sizeof(u64) /* argv */
                                   + sizeof(u64); /* argc */
                u64 total = auxv_bytes + vector_bytes;
                vaddr entry_sp = sp - total;
                vaddr aligned;

#if defined(_AARCH64_)
                aligned = entry_sp & ~((vaddr)0xF);
#elif defined(_X86_64_)
                aligned = (entry_sp & ~((vaddr)0xF)) | (vaddr)0x8;
                if (aligned > entry_sp) {
                        aligned -= (vaddr)0x10;
                }
#else
                aligned = entry_sp & ~((vaddr)0xF);
#endif
                if (aligned < entry_sp) {
                        sp -= (entry_sp - aligned);
                }
        }

        sp -= (u64)pair_count * 2U * sizeof(u64);

        for (u32 j = 0; j < pair_count; j++) {
                e = exec_store_u64(
                        vs, sp + (u64)j * 2U * sizeof(u64), pairs[j].tag);
                if (e != REND_SUCCESS) {
                        return 0;
                }
                e = exec_store_u64(vs,
                                   sp + ((u64)j * 2U + 1U) * sizeof(u64),
                                   pairs[j].val);
                if (e != REND_SUCCESS) {
                        return 0;
                }
        }

        sp -= sizeof(u64);
        e = exec_store_u64(vs, sp, 0);
        if (e != REND_SUCCESS) {
                return 0;
        }

        sp -= (u64)(argc + 1) * sizeof(u64);
        argv_ptr_area = sp;

        {
                vaddr string_va = strings_base;

                for (i = 0; i < argc; i++) {
                        e = exec_store_u64(vs,
                                           argv_ptr_area + (u64)i * sizeof(u64),
                                           (u64)string_va);
                        if (e != REND_SUCCESS) {
                                return 0;
                        }
                        string_va += (vaddr)(strlen(kargv[i]) + 1);
                }
        }

        e = exec_store_u64(vs, argv_ptr_area + (u64)argc * sizeof(u64), 0);
        if (e != REND_SUCCESS) {
                return 0;
        }

        sp -= sizeof(u64);
        e = exec_store_u64(vs, sp, (u64)argc);
        if (e != REND_SUCCESS) {
                return 0;
        }

        if (argv_user_out) {
                *argv_user_out = argv_ptr_area;
        }
        return sp;
}
