/*I don't think the multi cpu elf read test is need*/
#include <modules/test/test.h>
#include <modules/elf/elf.h>
#include <modules/elf/elf_print.h>

#include <rendezvos/mm/vmm.h>

extern u64 _num_app;

static void elf_read(vaddr elf_start)
{
        if (!check_elf_header(elf_start)) {
                pr_error("[ERROR] bad elf file\n");
                return;
        }
        print_elf_header(elf_start);
        if (get_elf_class(elf_start) == ELFCLASS32) {
                for_each_program_header_32(elf_start)
                {
                        print_elf_ph32(phdr_ptr);
                }
                for_each_section_header_32(elf_start)
                {
                        print_elf_sh32(shdr_ptr);
                }
        } else if (get_elf_class(elf_start) == ELFCLASS64) {
                for_each_program_header_64(elf_start)
                {
                        print_elf_ph64(phdr_ptr);
                }
                for_each_section_header_64(elf_start)
                {
                        print_elf_sh64(shdr_ptr);
                }
        } else {
                pr_error("[ERROR] bad elf file class\n");
                return;
        }
}

int elf_read_test(void)
{
        u64* app_start_ptr;
        for (u64 i = 0; i < _num_app; i++) {
                app_start_ptr =
                        (u64*)((vaddr)(&_num_app) + (i * 2 + 1) * sizeof(u64));
                elf_read((vaddr)(*app_start_ptr));
        }
        return REND_SUCCESS;
}
