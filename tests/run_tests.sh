#!/bin/bash
# Builds and runs every host-side test suite against the REAL kernel sources.
# Usage: bash tests/run_tests.sh      (also run by `make test`)
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
CF="-std=gnu11 -Wall -Wextra -g -no-pie -DMEMORY_HOST_TEST -DMINIOS_HOST_TEST -I."
MEM="kernel/memory/pmm.c kernel/memory/heap.c kernel/memory/memory.c"
FS="kernel/filesystem/filesystem.c kernel/filesystem/path.c"
LOG="kernel/log/logger.c"
PROC="kernel/process/process.c kernel/process/scheduler.c kernel/process/workloads.c kernel/net/loopback.c tests/host_arch_stub.c $LOG"
SYS="kernel/syscall/syscall.c kernel/security/users.c kernel/sysinfo.c kernel/device/ramdisk.c kernel/filesystem/fsimage.c lib/sha256.c lib/usyscall.c lib/syserr.c"
PARSE="shell/tokenizer.c shell/parser.c shell/history.c"
REG="commands/command_registry.c commands/command_table.c"
LIBS="lib/numparse.c lib/glob.c lib/textutil.c lib/mathlib.c lib/shellvars.c lib/lineedit.c"
UTIL="commands/cmd_util.c"
CMD_ALL="commands/shell_commands.c commands/directory_commands.c commands/file_commands.c commands/text_commands.c commands/fsinfo_commands.c commands/syscall_commands.c commands/user_commands.c commands/shellenv_commands.c commands/textops_commands.c commands/sysinfo_commands.c commands/disk_commands.c commands/misc_commands.c commands/network_commands.c commands/diag_commands.c commands/fileextra_commands.c commands/log_commands.c commands/jobs_commands.c commands/blkextra_commands.c"

fail=0
suite() {   # name, sources...
    name="$1"; shift
    if ! gcc $CF "$@" -o "build/$name" 2>build/$name.log; then echo "BUILD FAILED: $name"; grep -E "error|undefined" build/$name.log | head -5; fail=1; return; fi
    timeout 120 "./build/$name" || fail=1
}

suite test_shell        tests/test_shell.c $PARSE
suite test_registry     tests/test_registry.c $REG
suite test_memory       tests/test_memory.c $MEM lib/numparse.c
suite test_memory_commands tests/test_memory_commands.c commands/memory_commands.c $UTIL $REG $PARSE $MEM $FS $PROC $SYS $LIBS
suite test_process      tests/test_process.c kernel/process/process.c kernel/process/scheduler.c tests/host_arch_stub.c $LOG $MEM
suite test_process_commands tests/test_process_commands.c commands/process_commands.c $REG $PARSE $PROC $MEM $FS $SYS $LIBS
suite test_text         tests/test_text.c lib/glob.c lib/textutil.c kernel/device/capture.c
suite test_fs           tests/test_fs.c kernel/filesystem/path.c $MEM
suite test_fs_commands  tests/test_fs_commands.c $CMD_ALL $UTIL $REG $PARSE shell/redirect.c kernel/device/capture.c $FS $PROC $MEM $SYS $LIBS
suite test_syscall      tests/test_syscall.c $PROC $MEM $FS $SYS
suite test_mouse       tests/test_mouse.c lib/mousepkt.c
suite test_lineedit    tests/test_lineedit.c lib/lineedit.c lib/scrollback.c lib/complete.c
suite test_logger      tests/test_logger.c $LOG
suite test_sha256       tests/test_sha256.c lib/sha256.c
suite test_mathlib      tests/test_mathlib.c lib/mathlib.c lib/shellvars.c
suite test_phase11      tests/test_phase11.c $CMD_ALL $UTIL $REG $PARSE shell/redirect.c kernel/device/capture.c $FS $PROC $MEM $SYS $LIBS
suite test_phase16      tests/test_phase16.c shell/pipeline.c shell/lineexec.c shell/compenv.c lib/complete.c $CMD_ALL $UTIL $REG $PARSE shell/redirect.c kernel/device/capture.c $FS $PROC $MEM $SYS $LIBS
suite test_phase15      tests/test_phase15.c shell/pipeline.c shell/lineexec.c $CMD_ALL $UTIL $REG $PARSE shell/redirect.c kernel/device/capture.c $FS $PROC $MEM $SYS $LIBS
suite test_phase14      tests/test_phase14.c shell/pipeline.c shell/lineexec.c $CMD_ALL $UTIL $REG $PARSE shell/redirect.c kernel/device/capture.c $FS $PROC $MEM $SYS $LIBS
suite test_phase13      tests/test_phase13.c shell/pipeline.c $CMD_ALL $UTIL $REG $PARSE shell/redirect.c kernel/device/capture.c $FS $PROC $MEM $SYS $LIBS
suite test_phase12      tests/test_phase12.c $CMD_ALL $UTIL $REG $PARSE shell/redirect.c kernel/device/capture.c $FS $PROC $MEM $SYS $LIBS
suite test_users        tests/test_users.c $CMD_ALL $UTIL $REG $PARSE shell/redirect.c kernel/device/capture.c $FS $PROC $MEM $SYS $LIBS

# The REAL boot/context.S switch, run natively as a 32-bit program (skipped where 32-bit binaries cannot run).
if gcc -m32 -ffreestanding -nostdlib -static -fno-pie -no-pie -fno-stack-protector -fno-omit-frame-pointer -O1 -Wall -Wextra -I. \
       tests/native_context.c kernel/cpu/context.c boot/context.S -o build/native_context 2>/dev/null && ./build/native_context; then :
else echo "native context test: skipped (32-bit binaries cannot be built/run here)"; fi
exit $fail
