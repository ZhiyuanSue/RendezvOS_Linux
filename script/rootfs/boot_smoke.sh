#!/bin/busybox sh
# Temporary Path B boot script (packed to /tests/boot_smoke.sh).
# Invoked as: /bin/busybox sh /tests/boot_smoke.sh
# Kernel cmdline override of this path is TODO (see BUSYBOX_BOOT_DEFERRALS).
set -e
echo BOOT_SMOKE_OK
/bin/busybox ls /bin
echo BOOT_SMOKE_DONE
