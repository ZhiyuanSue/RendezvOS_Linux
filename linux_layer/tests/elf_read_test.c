#include <modules/test/test.h>
#include <modules/elf/elf.h>
#include <modules/elf/elf_print.h>

#include <linux_compat/fs/vfs_kern_load.h>
#include <linux_compat/mm/linux_page_slice_file.h>
#include <linux_compat/test_manifest.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice_copy.h>
#include <rendezvos/smp/percpu.h>

static void elf_read_slice(struct page_slice *slice)
{
        vaddr base = linux_page_slice_file_base(slice);

        if (!base) {
                pr_error("[ERROR] missing pgoff 0 in ELF slice\n");
                return;
        }
        if (!check_elf_header(base)) {
                pr_error("[ERROR] bad elf file\n");
                return;
        }
        print_elf_header(base);
        if (get_elf_class(base) == ELFCLASS32) {
                pr_error("[ERROR] elf32 not supported in slice read test\n");
                return;
        }
        if (get_elf_class(base) != ELFCLASS64) {
                pr_error("[ERROR] bad elf file class\n");
                return;
        }

        for_each_program_header_64(base)
        {
                Elf64_Phdr phdr;
                u64 ph_off = (u64)((u8 *)phdr_ptr - (u8 *)base);

                if (page_slice_copy_to_buffer(slice, ph_off, &phdr, sizeof(phdr))
                    != REND_SUCCESS) {
                        pr_error("[ERROR] failed to read phdr at off %llx\n",
                                 (unsigned long long)ph_off);
                        return;
                }
                print_elf_ph64(&phdr);
        }

        for_each_section_header_64(base)
        {
                Elf64_Shdr shdr;
                u64 sh_off = (u64)((u8 *)shdr_ptr - (u8 *)base);

                if (page_slice_copy_to_buffer(slice, sh_off, &shdr, sizeof(shdr))
                    != REND_SUCCESS) {
                        pr_error("[ERROR] failed to read shdr at off %llx\n",
                                 (unsigned long long)sh_off);
                        return;
                }
                print_elf_sh64(&shdr);
        }
}

int elf_read_test(void)
{
        struct allocator *alloc = percpu(kallocator);
        u32 total = linux_user_test_count();
        u32 i;

        if (!alloc) {
                return -E_RENDEZVOS;
        }

        for (i = 0; i < total; i++) {
                const char *path = linux_user_test_path(i);
                struct page_slice *elf_slice = NULL;
                i64 ret;

                if (!path) {
                        continue;
                }

                ret = vfs_kern_read_file_slice(path, alloc, &elf_slice);
                if (ret < 0 || !elf_slice) {
                        pr_error("[elf_read_test] read slice %s failed: %lld\n",
                                 path,
                                 (long long)ret);
                        return (int)ret;
                }

                elf_read_slice(elf_slice);
                page_slice_destroy(&elf_slice);
        }

        return REND_SUCCESS;
}
