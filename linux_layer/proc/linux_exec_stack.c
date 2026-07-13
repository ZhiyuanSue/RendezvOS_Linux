#include <common/align.h>
#include <common/mm.h>
#include <common/string.h>
#include <common/types.h>

#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/mm/linux_page_slice_file.h>
#include <linux_compat/proc/linux_exec_stack.h>
#include <modules/elf/elf.h>
#include <rendezvos/error.h>
#include <rendezvos/task/thread_loader.h>

#if defined(_AARCH64_)
#include <arch/aarch64/tcb_arch.h>
#elif defined(_X86_64_)
#include <arch/x86_64/tcb_arch.h>
#endif

#define LINUX_EXEC_SPAWN_MAX_ARGC 3
#define LINUX_EXEC_AUXV_MAX_PAIRS 16

static bool linux_exec_elf_has_interp(vaddr elf_start)
{
        if (!elf_start || !check_elf_header(elf_start)) {
                return false;
        }

        for_each_program_header_64(elf_start)
        {
                if (phdr_ptr->p_type == PT_INTERP) {
                        return true;
                }
        }
        return false;
}

static u32 linux_exec_elf_pt_note_count(vaddr elf_start)
{
        u32 count = 0;

        if (!elf_start || !check_elf_header(elf_start)) {
                return 0;
        }

        for_each_program_header_64(elf_start)
        {
                if (phdr_ptr->p_type == PT_NOTE) {
                        count++;
                }
        }
        return count;
}

static bool linux_exec_elf_needs_spawn_stack(struct page_slice *slice)
{
        vaddr base;

        if (!slice) {
                return false;
        }

        base = linux_page_slice_file_base(slice);
        if (!base || !check_elf_header(base)
            || get_elf_class(base) != ELFCLASS64) {
                return false;
        }

        if (linux_exec_elf_has_interp(base)) {
                return false;
        }

        /*
         * Path B musl harness ELFs: static, one PT_NOTE (build-id).
         * Static glibc (busybox): multiple PT_NOTE (ABI-tag, build-id, …).
         */
        return linux_exec_elf_pt_note_count(base) > 1;
}

static u8 linux_exec_spawn_default_argv(const char *kargv[LINUX_EXEC_SPAWN_MAX_ARGC + 1])
{
        kargv[0] = "ls";
        kargv[1] = "/bin";
        kargv[2] = NULL;
        return 2;
}

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

static void linux_exec_fill_random16(u8 buf[LINUX_EXEC_RANDOM_BYTES], vaddr mix)
{
        for (u32 i = 0; i < LINUX_EXEC_RANDOM_BYTES; i++) {
                buf[i] = (u8)(((mix >> ((i & 7U) * 8U)) ^ (0xA5U + i)) & 0xFFU);
        }
}

static u32 linux_exec_auxv_fill_pairs(const linux_exec_elf_auxv_t *elf_auxv,
                                      vaddr random_va,
                                      linux_exec_auxv_pair_t *pairs,
                                      u32 cap)
{
        u32 n = 0;

#define LINUX_EXEC_AUXV_PUSH(aux_tag, aux_val)                           \
        do {                                                             \
                if (n < cap) {                                           \
                        pairs[n].tag = (u64)(aux_tag);                   \
                        pairs[n].val = (u64)(aux_val);                   \
                        n++;                                             \
                }                                                        \
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
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_RANDOM, random_va);
        } else {
                LINUX_EXEC_AUXV_PUSH(LINUX_AT_PAGESZ, PAGE_SIZE);
        }

        LINUX_EXEC_AUXV_PUSH(LINUX_AT_NULL, 0);
#undef LINUX_EXEC_AUXV_PUSH
        return n;
}

vaddr linux_exec_build_initial_stack(VSpace *vs, vaddr stack_top, i64 argc,
                                     const char *kargv[],
                                     const linux_exec_elf_auxv_t *elf_auxv,
                                     vaddr *argv_user_out)
{
        vaddr sp = stack_top;
        u64 strings_size = 0;
        vaddr strings_base;
        vaddr argv_ptr_area;
        vaddr random_va = 0;
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

        sp -= strings_size;
        sp &= ~((vaddr)0xF);
        strings_base = sp;

        {
                vaddr string_va = strings_base;

                for (i = 0; i < argc; i++) {
                        size_t len = strlen(kargv[i]) + 1;

                        e = linux_mm_store_to_user(vs, string_va, kargv[i], len);
                        if (e != REND_SUCCESS) {
                                return 0;
                        }
                        string_va += (vaddr)len;
                }
        }

        if (elf_auxv && elf_auxv->have_elf) {
                sp -= LINUX_EXEC_RANDOM_BYTES;
                sp &= ~((vaddr)0xF);
                random_va = sp;
                linux_exec_fill_random16(random_bytes, random_va);
                e = linux_mm_store_to_user(vs,
                                           random_va,
                                           random_bytes,
                                           sizeof(random_bytes));
                if (e != REND_SUCCESS) {
                        return 0;
                }
        }

        pair_count = linux_exec_auxv_fill_pairs(
                elf_auxv, random_va, pairs, LINUX_EXEC_AUXV_MAX_PAIRS);
        sp -= (u64)pair_count * 2U * sizeof(u64);
        sp &= ~((vaddr)0xF);

        for (u32 j = 0; j < pair_count; j++) {
                e = exec_store_u64(vs, sp + (u64)j * 2U * sizeof(u64), pairs[j].tag);
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
        sp &= ~((vaddr)0xF);
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

error_t linux_exec_bootstrap_elf_spawn_stack(Thread_Base *thread, VSpace *vs,
                                             const elf_load_info_t *info)
{
        const char *kargv[LINUX_EXEC_SPAWN_MAX_ARGC + 1];
        linux_exec_elf_auxv_t elf_auxv;
        vaddr stack_top;
        vaddr sp;
        u8 argc;

        if (!thread || !vs || !info || !info->slice) {
                return -E_IN_PARAM;
        }
        if (!linux_vspace_is_user_table(vs)) {
                return -E_IN_PARAM;
        }
        if (!linux_exec_elf_needs_spawn_stack(info->slice)) {
                return REND_SUCCESS;
        }

        stack_top = info->user_sp + 8;
        argc = linux_exec_spawn_default_argv(kargv);

        if (!linux_exec_elf_auxv_from_slice(info->slice, &elf_auxv)) {
                elf_auxv.have_elf = false;
        }

        sp = linux_exec_build_initial_stack(
                vs, stack_top, (i64)argc, kargv, &elf_auxv, NULL);
        if (sp == 0) {
                return -E_RENDEZVOS;
        }

        arch_set_thread_user_sp(&thread->ctx, sp);
        return REND_SUCCESS;
}
