# 1/before we use the file system to get user test case
# we might need to use 'incbin' to include the user case
# 2/if we use file system, we also need to use musl cc to complie it
# and put it into a image, so we actually need a user test case
#
# This script is only used by the top-level build target `make user`.
# It generates/copies `build/link_app.o` that gets linked into the kernel image.

import sys
import os
import json
import shutil

target_dir = "build/user_payload"
target_config_arch_list = [
    "aarch64",
    "loongarch",
    "riscv64",
    "x86_64",
]


def get_cross_prefix(arch: str) -> str:
    # Must match toolchain names installed by build_env.sh
    arch_to_prefix = {
        "aarch64": "aarch64-linux-gnu-",
        "x86_64": "x86_64-linux-gnu-",
        "riscv64": "riscv64-linux-gnu-",
        "loongarch": "loongarch64-linux-gnu-",
    }
    return arch_to_prefix.get(arch, "")


if __name__ == "__main__":
    arch = sys.argv[1]
    root_dir = sys.argv[2]
    user_config_file = sys.argv[3]
    pwd = os.getcwd()
    kernel_dir = (
        os.path.join(root_dir, "core")
        if os.path.isdir(os.path.join(root_dir, "core"))
        else root_dir
    )
    script_make_dir = os.path.join(kernel_dir, "script", "make")
    build_dir = os.path.join(root_dir, "build")
    modules_dir = os.path.join(root_dir, "modules")

    if os.path.isfile(user_config_file) == False:
        print("ERROR:no such an user config file")
        exit(2)
    if arch not in target_config_arch_list:
        print("ERROR:no such an arch")
        exit(1)

    user_dir = os.path.join(root_dir, target_dir)
    with open(user_config_file, "r") as json_file:
        user_json = json.load(json_file)
        git_repo_link = user_json["git"]
        if os.path.isdir(user_dir) == False:
            git_repo_clone_cmd = f"git clone {git_repo_link} {user_dir}"
            status = os.system(git_repo_clone_cmd)
            if status != 0:
                print("ERROR:git clone repo " + git_repo_link + " fail")
                exit(2)
        else:
            # check the update
            os.chdir(user_dir)
            git_pull_cmd = f"git pull"
            status = os.system(git_pull_cmd)
            if status != 0:
                print(
                    "WARNING:git pull repo "
                    + git_repo_link
                    + " fail, continue with the local copy"
                )
            os.chdir(pwd)

        using_file_system = user_json["filesystem"]
        user_user_dir = os.path.join(user_dir, "user")
        user_user_build_dir = os.path.join(user_user_dir, "build")
        user_user_build_arch_dir = os.path.join(user_user_build_dir, arch)
        user_user_build_bin_dir = os.path.join(user_user_dir, "bin")

        if using_file_system == False:
            # using incbin
            cross_prefix = get_cross_prefix(arch)
            if not cross_prefix:
                print(
                    f"ERROR: unsupported arch for cross compiling user payload: {arch}"
                )
                exit(1)

            user_cc = cross_prefix + "gcc"
            make_env = (
                f'SCRIPT_MAKE_DIR="{script_make_dir}" '
                f'BUILD="{build_dir}" '
                f'MODULES_DIR="{modules_dir}" '
                f'CC="{user_cc}" '
                f'AS="{cross_prefix}as"'
            )

            # build the inner user test binary first
            os.chdir(user_user_dir)
            make_clean_cmd = f"{make_env} make clean"
            status = os.system(make_clean_cmd)
            if status != 0:
                print("ERROR:make clean fail")
                exit(2)

            make_all_cmd = f"{make_env} make all ARCH={arch}"
            status = os.system(make_all_cmd)
            if status != 0:
                print("ERROR:make all fail")
                exit(2)

            # generate link_app.S/link_app.o at build/user_payload/
            os.chdir(user_dir)
            make_all_cmd = f"{make_env} make all ARCH={arch}"
            status = os.system(make_all_cmd)
            if status != 0:
                print("ERROR:make all fail")
                exit(2)

            os.makedirs(build_dir, exist_ok=True)
            link_app_obj = os.path.join(user_dir, "link_app.o")
            target_link_app_obj = os.path.join(build_dir, "link_app.o")
            shutil.copy2(link_app_obj, target_link_app_obj)
            # Record the arch used to produce build/link_app.o so the top-level
            # build can detect stale payloads when ARCH changes.
            with open(os.path.join(build_dir, "link_app.arch"), "w") as f:
                f.write(arch + "\n")
            os.chdir(pwd)
        else:
            # using file system to test
            pass

