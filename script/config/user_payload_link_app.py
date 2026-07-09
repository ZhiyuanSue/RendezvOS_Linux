# Canonical copy of user_payload/link_app.py — keep in sync when editing either file.

import os
import sys

link_app_file_name = "link_app.S"
manifest_order_name = "manifest.order"


def generate_stub_link_app(pwd):
    link_app_s_file = os.path.join(pwd, link_app_file_name)
    stub = (
        "\t.p2align 4\n"
        "\t.section .data.app\n"
        "\t.global _num_app\n"
        "_num_app:\n"
        "\t.quad 0\n"
    )
    with open(link_app_s_file, "w", encoding="utf-8") as f:
        f.write(stub)


def generate_link_app(pwd, elf_dir, elf_file_list):
    link_app_s_file = os.path.join(pwd, link_app_file_name)
    manager_str = (
        "\t.p2align 4\n"
        "\t.section .data.app\n"
        "\t.global _num_app\n"
        "_num_app:\n"
    )

    section_str = ""
    manager_str += "\t.quad " + str(len(elf_file_list)) + "\n"
    for file in elf_file_list:
        inc_bin_file = os.path.join(pwd, elf_dir, file)
        manager_str += (
            "\t.quad "
            + file
            + "_start\n"
            + "\t.quad "
            + file
            + "_end\n"
        )
        section_str += (
            "\t.p2align 4\n\t.section .user\n"
            "\t.global "
            + file
            + "_start\n"
            "\t.global "
            + file
            + "_end\n"
            + file
            + "_start:\n"
            + '\t.incbin "'
            + inc_bin_file
            + '"\n'
            + file
            + "_end:\n"
        )

    with open(link_app_s_file, "w", encoding="utf-8") as f:
        f.write(manager_str)
        f.write(section_str)


def write_manifest_order(pwd, elf_file_list):
    build_dir = os.path.join(pwd, "build")
    os.makedirs(build_dir, exist_ok=True)
    order_path = os.path.join(build_dir, manifest_order_name)
    with open(order_path, "w", encoding="utf-8") as f:
        for name in elf_file_list:
            f.write(name + "\n")


def collect_elf_files(elf_dir):
    if not os.path.exists(elf_dir):
        return []
    all_items = os.listdir(elf_dir)
    return sorted(
        f for f in all_items if os.path.isfile(os.path.join(elf_dir, f))
    )


if __name__ == "__main__":
    elf_dir = sys.argv[1]
    pwd = os.getcwd()
    filesystem_mode = os.environ.get("RENDEZVOS_FILESYSTEM_MODE", "") in (
        "1",
        "true",
        "yes",
    )

    elf_files = collect_elf_files(elf_dir)
    write_manifest_order(pwd, elf_files)

    if filesystem_mode:
        generate_stub_link_app(pwd)
    else:
        generate_link_app(pwd, elf_dir, elf_files)
