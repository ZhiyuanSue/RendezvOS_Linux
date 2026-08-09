#!/bin/busybox sh
# Optional smoke script under /tests/. Default PID1 argv comes from make
# CMDLINE (default: sh /tests/run_all.sh), not this file.
set -e
echo BOOT_SMOKE_OK
/bin/busybox ls /bin
echo BOOT_SMOKE_DONE
