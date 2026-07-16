rootfs/ — initramfs source tree (packed by script/rootfs/build_cpio.sh)

Two layers in integrated harness:

  1. Test PROGRAMS (static ELF64)
     Built by `make user` → copied to rootfs/tests/ + manifest
     Spawned by user_test_runner via VFS (initramfs / cpio).
     link_app.o is a stub (_num_app=0); see script/config/user.json
     "filesystem": true.

  2. Test DATA FILES (fixtures)
     e.g. ./text.txt, ./mnt/
     Also live under rootfs/ and are read at runtime via VFS.

Layout:

  text.txt              open / read / fstat fixtures
  mnt/                  openat directory tests
  tests/                generated — ELF binaries + manifest (gitignored)
  tests/manifest        one absolute path per line, e.g. /tests/test_echo
  bin/busybox           optional (make user with "busybox": true, or build_busybox.sh)

Commands:

  make user ARCH=x86_64          # build ELFs, pack rootfs/tests/, optional busybox → rootfs/bin/
  make rootfs ARCH=x86_64        # refresh build/rootfs.cpio
  make ARCH=x86_64 build run

Busybox (optional, user.json "busybox": true):
  Objects cached under .cache/busybox-$ARCH/ so make clean / config do not force
  a full recompile. make user only reinstalls applets into rootfs/bin/ when the
  cache stamp matches. Force rebuild: FORCE_BUSYBOX=1 make user ARCH=...

See doc/linux_compat/INITRAMFS_PLAN.md
See doc/linux_compat/ROOTFS.md for Git tracking (fixtures vs generated tests/)
