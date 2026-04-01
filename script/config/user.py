# This script is only used by the top-level build target `make user`.
# It generates/copies `build/link_app.o` that gets linked into the kernel image.
#
# Layout (after clone): $(ROOT)/user_payload/ = git root of user payload repo
# Inner build directory (must contain Makefile):
#   - default: user_payload/user/
#   - or repo root: user_payload/ if Makefile is at top level
#   - or set "inner" in user.json (e.g. "." for repo root, or "user")
#
# Environment:
#   RENDEZVOS_USER_SKIP_GIT=1 — do not run `git pull` / `git fetch` in repair path

import sys
import os
import json
import shutil
from typing import Optional

target_dir = "user_payload"
target_config_arch_list = [
    "aarch64",
    "loongarch",
    "riscv64",
    "x86_64",
]


def get_cross_prefix(arch: str) -> str:
    arch_to_prefix = {
        "aarch64": "aarch64-linux-gnu-",
        "x86_64": "x86_64-linux-gnu-",
        "riscv64": "riscv64-linux-gnu-",
        "loongarch": "loongarch64-linux-gnu-",
    }
    return arch_to_prefix.get(arch, "")


def _worktree_entries_except_git(user_dir: str):
    try:
        return [e for e in os.listdir(user_dir) if e != ".git"]
    except OSError:
        return None


def try_populate_user_payload_worktree(user_dir: str, pwd: str, skip_git: bool) -> bool:
    """
    Repair: only `.git` present, or sparse-checkout hiding files.
    """
    if not os.path.isdir(os.path.join(user_dir, ".git")):
        return False
    entries = _worktree_entries_except_git(user_dir)
    if entries is None:
        return False
    if len(entries) > 0:
        return True

    print(
        "INFO: user_payload worktree is empty (only .git). "
        "Repairing: sparse-checkout off, fetch, checkout, reset --hard..."
    )
    try:
        os.chdir(user_dir)
        os.system("git sparse-checkout disable 2>/dev/null || true")
        if not skip_git:
            st_fetch = os.system("git fetch origin")
            if st_fetch != 0:
                print("WARNING: git fetch origin failed")
        else:
            print(
                "INFO: RENDEZVOS_USER_SKIP_GIT set; skipping fetch "
                "(checkout/reset use local refs only)"
            )
        os.system(
            "git checkout -f -B main origin/main 2>/dev/null || "
            "git checkout -f -B master origin/master 2>/dev/null || "
            "git checkout -f main 2>/dev/null || "
            "git checkout -f master 2>/dev/null || "
            "git checkout -f FETCH_HEAD 2>/dev/null || true"
        )
        os.system("git reset --hard HEAD 2>/dev/null || true")
    finally:
        os.chdir(pwd)

    after = _worktree_entries_except_git(user_dir)
    return after is not None and len(after) > 0


def resolve_user_inner_build_dir(user_dir: str, user_json: dict) -> Optional[str]:
    """
    Find directory that contains the *inner* user Makefile.
    Order: explicit "inner" in JSON, then user/, then repo root.
    """
    explicit = user_json.get("inner")
    candidates = []
    if explicit is not None:
        if isinstance(explicit, str) and explicit.strip() in (".", "./", ""):
            candidates.append(os.path.normpath(user_dir))
        elif isinstance(explicit, str) and explicit.strip():
            candidates.append(os.path.normpath(os.path.join(user_dir, explicit.strip())))
    candidates.append(os.path.normpath(os.path.join(user_dir, "user")))
    candidates.append(os.path.normpath(user_dir))

    seen = set()
    for c in candidates:
        if c in seen:
            continue
        seen.add(c)
        mk = os.path.join(c, "Makefile")
        if os.path.isfile(mk):
            return c
    return None


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

    if not os.path.isfile(user_config_file):
        print("ERROR:no such an user config file")
        sys.exit(2)
    if arch not in target_config_arch_list:
        print("ERROR:no such an arch")
        sys.exit(1)

    user_dir = os.path.join(root_dir, target_dir)
    skip_git = os.environ.get("RENDEZVOS_USER_SKIP_GIT", "").lower() in (
        "1",
        "true",
        "yes",
    )

    with open(user_config_file, "r") as json_file:
        user_json = json.load(json_file)
        git_repo_link = user_json["git"]

        if not os.path.isdir(user_dir):
            git_repo_clone_cmd = f"git clone {git_repo_link} {user_dir}"
            status = os.system(git_repo_clone_cmd)
            if status != 0:
                print("ERROR:git clone repo " + git_repo_link + " fail")
                sys.exit(2)
        elif not os.path.isdir(os.path.join(user_dir, ".git")):
            print(
                f"ERROR: {user_dir} exists but is not a git repository "
                "(missing .git). Remove it or clone manually:\n"
                f"  git clone {git_repo_link} {user_dir}"
            )
            sys.exit(2)
        else:
            if skip_git:
                print(
                    "INFO: RENDEZVOS_USER_SKIP_GIT set; skipping git pull "
                    f"({user_dir})"
                )
            else:
                os.chdir(user_dir)
                status = os.system("git pull")
                if status != 0:
                    print(
                        "WARNING: git pull "
                        + git_repo_link
                        + " failed; continuing with local copy"
                    )
                os.chdir(pwd)

        try_populate_user_payload_worktree(user_dir, pwd, skip_git)

        using_file_system = user_json["filesystem"]
        user_user_dir = resolve_user_inner_build_dir(user_dir, user_json)
        user_user_build_dir = (
            os.path.join(user_user_dir, "build") if user_user_dir else ""
        )
        user_user_build_arch_dir = os.path.join(user_user_build_dir, arch)
        user_user_build_bin_dir = os.path.join(user_user_dir, "bin")

        if using_file_system:
            pass
        else:
            if not user_user_dir:
                top_list = None
                try:
                    top_list = sorted(os.listdir(user_dir))
                except OSError:
                    pass
                print(
                    "ERROR: cannot find inner user build (need Makefile).\n"
                    f"  Searched under: {user_dir}\n"
                    "  Tried: user.json 'inner' (if set), then user/, then repo root.\n"
                    f"  Clone URL: {git_repo_link}\n"
                    f"  Top-level entries: {top_list!r}\n"
                    '  Fix: add a Makefile under user/ or at repo root, or set '
                    '"inner" in user.json (e.g. \".\" for root).'
                )
                if top_list == [".git"]:
                    print(
                        f'\n  Worktree still empty: rm -rf "{user_dir}" '
                        "then make user ARCH=<arch> (with network).\n"
                    )
                sys.exit(2)

            cross_prefix = get_cross_prefix(arch)
            if not cross_prefix:
                print(
                    f"ERROR: unsupported arch for cross compiling user payload: {arch}"
                )
                sys.exit(1)

            user_cc = cross_prefix + "gcc"
            make_env = (
                f'SCRIPT_MAKE_DIR="{script_make_dir}" '
                f'BUILD="{build_dir}" '
                f'MODULES_DIR="{modules_dir}" '
                f'CC="{user_cc}" '
                f'AS="{cross_prefix}as"'
            )

            os.chdir(user_user_dir)
            make_clean_cmd = f"{make_env} make clean"
            status = os.system(make_clean_cmd)
            if status != 0:
                print("ERROR:make clean fail")
                sys.exit(2)

            make_all_cmd = f"{make_env} make all ARCH={arch}"
            status = os.system(make_all_cmd)
            if status != 0:
                print("ERROR:make all fail")
                sys.exit(2)

            root_makefile = os.path.join(user_dir, "Makefile")
            link_app_obj = os.path.join(user_dir, "link_app.o")
            if os.path.isfile(root_makefile):
                os.chdir(user_dir)
                make_all_cmd = f"{make_env} make all ARCH={arch}"
                status = os.system(make_all_cmd)
                if status != 0:
                    print("ERROR:make all fail (outer user_payload Makefile)")
                    sys.exit(2)
                link_app_obj = os.path.join(user_dir, "link_app.o")
            else:
                inner_obj = os.path.join(user_user_dir, "link_app.o")
                if os.path.isfile(inner_obj):
                    link_app_obj = inner_obj
                else:
                    print(
                        "ERROR: no Makefile at user_payload repo root and no link_app.o "
                        f"in inner dir {user_user_dir}\n"
                        "  Add a top-level Makefile that produces link_app.o, or build "
                        "link_app.o from the inner Makefile."
                    )
                    sys.exit(2)

            os.makedirs(build_dir, exist_ok=True)
            target_link_app_obj = os.path.join(build_dir, "link_app.o")
            shutil.copy2(link_app_obj, target_link_app_obj)
            with open(os.path.join(build_dir, "link_app.arch"), "w") as f:
                f.write(arch + "\n")
            os.chdir(pwd)
