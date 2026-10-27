/* Integration test: MiniFS commands through the REAL tokenizer, parser, registry, redirection
 * and file system. Only the screen, timer and shell exit-status are stubbed.
 * `sh()` repeats the shell's execute_line() logic (lookup -> redirect_begin -> run -> redirect_end). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/shell.h"
#include "include/command.h"
#include "include/builtins.h"
#include "include/memory.h"
#include "include/heap.h"
#include "include/filesystem.h"
#include "include/redirect.h"
#include "include/capture.h"
#include "include/screen.h"
#include "include/timer.h"


/* ---- Phase 9: the commands now go through system calls, which need these ---- */
#include "include/interrupts.h"
#include "include/syscall.h"
#include "include/process.h"
#include "tests/low_stack.h"
void interrupt_register(uint32_t v, interrupt_handler_t h) { (void)v; (void)h; }
static char out[65536]; static size_t out_len;
static void out_reset(void) { out_len = 0; out[0] = 0; }
void screen_init(void) {} void screen_clear(void) {} void screen_set_color(uint8_t f, uint8_t b) { (void)f; (void)b; } void screen_set_default_color(uint8_t f, uint8_t b) { (void)f; (void)b; }
void screen_backspace(void) {}
void screen_putc(char c) { if (capture_write(c)) return; if (out_len + 1 < sizeof out) { out[out_len++] = c; out[out_len] = 0; } }   /* same rule as the real driver */
void screen_puts(const char *s) { while (*s) screen_putc(*s++); }
void screen_put_dec(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%u", v); screen_puts(b); }
void screen_put_hex(uint32_t v) { char b[16]; snprintf(b, sizeof b, "0x%08X", v); screen_puts(b); }
static uint32_t fake_ticks = 500;
uint32_t timer_ticks(void) { return fake_ticks; }
void timer_sleep_ms(uint32_t ms) { (void)ms; }
int keyboard_has_key(void) { return 0; }
int keyboard_getkey(void) { return 0; }
const char *exception_name(uint32_t v) { (void)v; return "exception"; }
int shell_last_exit(void) { return 0; }
int memory_commands_register(void) { return 0; }
int process_commands_register(void) { return 0; }

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n    output was: [%s]\n", __LINE__, #cond, out); } } while (0)

static bool owner_yes(void) { return true; }
static int sh(const char *line) {
    static char buf[SHELL_LINE_MAX * 2]; command_line_t cl;
    out_reset();
    int rc = parse_command_line(line, buf, sizeof buf, &cl);
    if (rc == PARSE_EMPTY) return 0;
    if (rc != PARSE_OK) { screen_puts("Error: "); screen_puts(parse_error_string(rc)); screen_putc('\n'); return 2; }
    const command_t *c = command_find(cl.argv[0]);
    if (!c) { screen_puts("Error: unknown command '"); screen_puts(cl.argv[0]); screen_puts("'\n"); return 127; }
    redirect_t rd; bool redirected = false;
    if (cl.redir != REDIR_NONE) {
        int rrc = redirect_begin(&rd, cl.redir_path, cl.redir, owner_yes);
        if (rrc != FS_OK) { screen_puts("Error: cannot redirect to '"); screen_puts(cl.redir_path); screen_puts("': "); screen_puts(fs_strerror(rrc)); screen_putc('\n'); return 1; }
        redirected = true;
    }
    int st = c->handler ? c->handler(cl.argc, cl.argv) : command_stub(c, cl.argc, cl.argv);
    if (redirected) st = redirect_end(&rd, st);
    return st;
}
static bool has(const char *s) { return strstr(out, s) != NULL; }
static bool file_is(const char *path, const char *text) {
    char b[4096]; size_t n = 0;
    return fs_read(path, b, sizeof b, 0, &n) == FS_OK && n == strlen(text) && memcmp(b, text, n) == 0;
}

static multiboot_mmap_entry_t map_buf[1]; static multiboot_info_t mbi;
static void fresh(void) {
    map_buf[0] = (multiboot_mmap_entry_t){ 20, 0x100000, 0x4000000, 1 };
    memset(&mbi, 0, sizeof mbi);
    mbi.flags = MULTIBOOT_FLAG_MMAP; mbi.mmap_addr = (uint32_t)(uintptr_t)map_buf; mbi.mmap_length = sizeof map_buf;
    memory_init(&mbi, 0x100000, 0x110000);
    fs_set_clock(timer_ticks); fs_set_user(FS_UID_ROOT, FS_UID_ROOT);
    fs_init();
    process_init();                                   /* commands call system calls, which belong to a process */
    syscall_init();
}

static int real_main(void) {
    fresh();
    CHECK(builtins_register() == 0);
    CHECK(command_find("ls")->handler && command_find("cat")->handler && command_find("mkdir")->handler && command_find("chmod")->handler);

    puts("the spec's demo sequence");
    CHECK(sh("mkdir project") == 0 && sh("cd project") == 0 && sh("pwd") == 0 && strcmp(out, "/project\n") == 0);
    CHECK(sh("touch hello.txt") == 0 && sh("ls") == 0 && has("hello.txt"));
    CHECK(sh("echo \"Hello MiniOS\" > hello.txt") == 0 && out_len == 0);               /* nothing printed: it went to the file */
    CHECK(file_is("/project/hello.txt", "Hello MiniOS\n"));
    CHECK(sh("cat hello.txt") == 0 && strcmp(out, "Hello MiniOS\n") == 0);
    CHECK(sh("echo second line >> hello.txt") == 0 && file_is("/project/hello.txt", "Hello MiniOS\nsecond line\n"));
    CHECK(sh("echo replaced > hello.txt") == 0 && file_is("/project/hello.txt", "replaced\n"));
    CHECK(sh("write notes.txt alpha beta") == 0 && sh("append notes.txt gamma") == 0 && file_is("/project/notes.txt", "alpha beta\ngamma\n"));
    CHECK(sh("cd /") == 0 && sh("tree /project") == 0 && has("hello.txt") && has("notes.txt") && has("0 directories, 2 files"));

    puts("error messages from the spec");
    CHECK(sh("cd unknown") == 1 && strstr(out, "Error: directory 'unknown' does not exist.") != NULL);
    CHECK(sh("rm missing.txt") == 1 && strstr(out, "Error: file 'missing.txt' not found.") != NULL);
    CHECK(sh("cat missing.txt") == 1 && has("file 'missing.txt' not found."));
    CHECK(sh("cd /project/hello.txt") == 1 && has("is not a directory"));
    CHECK(sh("mkdir /project") == 1 && has("already exists"));
    CHECK(sh("mkdir /nope/deeper") == 1 && has("does not exist"));
    CHECK(sh("rmdir /project") == 1 && has("not empty"));
    CHECK(sh("rmdir /project/hello.txt") == 1 && has("not a directory"));
    CHECK(sh("rm /project") == 1 && has("is a directory"));
    CHECK(sh("cat /project") == 1 && has("is a directory"));
    CHECK(sh("rmdir /") == 1 && has("cannot remove"));
    CHECK(sh("touch") == 1 && has("usage:") && sh("cat") == 1 && sh("mkdir") == 1 && sh("rm") == 1 && sh("cp a") == 1 && sh("mv a") == 1);

    puts("redirection edge cases");
    CHECK(sh("echo x > /nodir/file") == 1 && has("cannot redirect") && has("no such file"));
    CHECK(sh("echo x > /project") == 1 && has("cannot redirect") && has("is a directory"));
    CHECK(sh("cat nothing > out.txt") == 1 && has("not found") && fs_stat("/out.txt", NULL) != FS_OK);   /* failed command: error shown, file NOT created */
    CHECK(sh("cat /project/hello.txt > /project/hello.txt") == 0 && file_is("/project/hello.txt", "replaced\n"));   /* reads before it writes */
    CHECK(sh("echo > /project/empty") == 0 && file_is("/project/empty", "\n"));
    CHECK(sh("echo a >") == 2 && has("missing file name"));
    CHECK(sh("nosuchcmd > f") == 127 && fs_stat("/f", NULL) != FS_OK);
    fs_chmod("/project/notes.txt", 0444); fs_set_user(FS_UID_STUDENT, FS_UID_STUDENT);
    CHECK(sh("echo x > /project/notes.txt") == 1 && has("permission denied") && file_is("/project/notes.txt", "alpha beta\ngamma\n"));
    fs_set_user(FS_UID_ROOT, FS_UID_ROOT);
    sh("mkfile /big 1000"); sh("fill /manyx x 40000");
    CHECK(sh("cat /manyx > /copy_of_big") == 1 && has("larger than 32 KiB") && fs_stat("/copy_of_big", NULL) != FS_OK);   /* oversize output is refused, not truncated silently */
    CHECK(sh("cat /manyx") == 0 && out_len > 40000);
    sh("rm /big /manyx");
    CHECK(capture_active() == false);                                       /* capture always ends */

    puts("files");
    fresh();
    CHECK(sh("touch a b c") == 0 && sh("ls") == 0 && has("a  ") && has("c"));
    CHECK(sh("write f one two three") == 0 && sh("cp f g") == 0 && file_is("/g", "one two three\n"));
    CHECK(sh("cp f f") == 1 && has("same file") && sh("cp nofile x") == 1 && has("not found") && sh("cp f") == 1);
    CHECK(sh("mkdir d") == 0 && sh("cp f d") == 0 && file_is("/d/f", "one two three\n"));   /* copy INTO a directory */
    CHECK(sh("cp d d2") == 1 && has("use cp -r") && sh("cp -r d d2") == 0 && file_is("/d2/f", "one two three\n"));
    CHECK(sh("cp -r d d") == 1 && has("into itself"));
    CHECK(sh("mv g d") == 0 && file_is("/d/g", "one two three\n") && fs_stat("/g", NULL) != FS_OK);
    CHECK(sh("mv d/g d/h") == 0 && fs_stat("/d/h", NULL) == FS_OK);
    CHECK(sh("mv d d/sub") == 1 && has("into itself") && sh("mv nothing x") == 1 && has("not found") && sh("mv / x") == 1);
    CHECK(sh("rename d/h k") == 0 && fs_stat("/d/k", NULL) == FS_OK && sh("rename d/k x/y") == 1 && has("must not contain"));
    CHECK(sh("rename d/k f") == 1 && has("already exists") && sh("rename / x") == 1);
    CHECK(sh("stat f") == 0 && has("regular file") && has("Size   : 14 bytes") && has("0644") && has("-rw-r--r--"));
    CHECK(sh("stat d") == 0 && has("directory") && has("Entries"));
    CHECK(sh("stat nothing") == 1 && has("does not exist"));
    CHECK(sh("file f") == 0 && has("ASCII text") && sh("file d") == 0 && has("directory") && sh("file a") == 0 && has("empty"));
    CHECK(sh("mkfile blob 4") == 0 && sh("file blob") == 0 && has("binary data") && sh("mkfile blob 4") == 1 && has("already exists") && sh("mkfile bin 4") == 1);
    CHECK(sh("mkfile huge 999999") == 1 && has("0 to 262144") && sh("mkfile q abc") == 1);
    CHECK(sh("truncate f 3") == 0 && file_is("/f", "one") && sh("truncate f 999999") == 1 && sh("truncate nofile 1") == 1);
    CHECK(sh("filesize f") == 0 && strcmp(out, "3\n") == 0 && sh("filesize d") == 1);
    CHECK(sh("exists f") == 0 && has("yes") && sh("exists zzz") == 1 && has("no") && sh("isdir d") == 0 && sh("isdir f") == 1);
    CHECK(sh("backup f") == 0 && has("saved f.bak") && file_is("/f.bak", "one") && sh("write f changed") == 0 && sh("restore f") == 0 && file_is("/f", "one"));
    CHECK(sh("write f again") == 0 && sh("restore f.bak") == 0 && file_is("/f", "one") && sh("restore a") == 1);
    CHECK(sh("write x1 AAA") == 0 && sh("write x2 BBB") == 0 && sh("swapfiles x1 x2") == 0 && file_is("/x1", "BBB\n") && file_is("/x2", "AAA\n") && sh("swapfiles x1 none") == 1 && has("'none'"));
    CHECK(sh("fill pat ab 4") == 0 && file_is("/pat", "abababab") && sh("fill pat ab 0") == 1 && sh("fill pat x 999999") == 1);
    CHECK(sh("touchn num 3") == 0 && fs_stat("/num1", NULL) == FS_OK && fs_stat("/num3", NULL) == FS_OK && sh("touchn num 3") == 1 && has("already exists") && sh("touchn z 0") == 1 && sh("touchn z 101") == 1);
    CHECK(sh("readfile /pat 2 4") == 0 && has("read 4 byte(s) at offset 2") && has("abab") && sh("readfile /pat 100") == 0 && has("read 0 byte(s)") && sh("readfile /pat 0 0") == 1);
    CHECK(sh("note log hello there") == 0 && sh("cat log") == 0 && has("[+5s] hello there"));
    CHECK(sh("touch t1 t2") == 0 && sh("rm t1 t2 nonexist") == 1 && fs_stat("/t1", NULL) != FS_OK && fs_stat("/t2", NULL) != FS_OK);

    puts("trash and shred");
    CHECK(sh("write victim secret") == 0 && sh("trash victim") == 0 && fs_stat("/victim", NULL) != FS_OK && fs_stat("/tmp/.trash/victim", NULL) == FS_OK);
    CHECK(sh("write victim second") == 0 && sh("trash victim") == 0 && fs_stat("/tmp/.trash/victim.1", NULL) == FS_OK);   /* name clash handled */
    CHECK(sh("ls") == 0 && !has("victim") && sh("ls -a /tmp") == 0 && has(".trash"));
    CHECK(sh("undelete victim") == 0 && file_is("/victim", "secret\n") && sh("undelete victim") == 1 && has("nothing with that name"));
    CHECK(sh("undelete victim.1") == 0 && has("restored victim.1") && file_is("/victim.1", "second\n"));   /* restored under the name given */
    CHECK(sh("undelete /etc/motd") == 1 && sh("trash nothing") == 1 && has("not found"));
    CHECK(sh("emptytrash") == 0 && fs_stat("/tmp/.trash", NULL) != FS_OK && sh("emptytrash") == 0 && has("already empty"));
    CHECK(sh("write s1 wipe me") == 0 && sh("shred s1") == 0 && has("overwrote and deleted") && fs_stat("/s1", NULL) != FS_OK && sh("shred s1") == 1 && sh("shred d") == 1);

    puts("directories");
    fresh();
    CHECK(sh("mkdir -p a/b/c") == 0 && fs_stat("/a/b/c", NULL) == FS_OK && sh("mkpath x/y/z") == 0 && sh("mkdir p q r") == 0 && sh("mkdir p q") == 1);
    CHECK(sh("cd a/b") == 0 && sh("pwd") == 0 && strcmp(out, "/a/b\n") == 0 && sh("up") == 0 && sh("pwd") == 0 && strcmp(out, "/a\n") == 0);
    CHECK(sh("cd") == 0 && sh("pwd") == 0 && strcmp(out, "/home/admin\n") == 0 && sh("cd /") == 0 && sh("home") == 0 && strcmp(out, "") == 0);
    CHECK(sh("cd / extra") == 1 && has("usage:") && sh("cd ..") == 0 && sh("pwd") == 0 && strcmp(out, "/home\n") == 0);
    CHECK(sh("pushd /tmp") == 0 && has("/tmp /home") && sh("pushd /etc") == 0 && sh("dirs") == 0 && has("/etc /home /tmp") == false);
    CHECK(sh("popd") == 0 && sh("pwd") == 0 && strcmp(out, "/tmp\n") == 0 && sh("popd") == 0 && sh("popd") == 1 && has("empty") && sh("pushd /ghost") == 1);
    for (int i = 0; i < 8; i++) { char l[24]; snprintf(l, sizeof l, "pushd /usr"); sh(l); }
    CHECK(sh("pushd /usr") == 1 && has("full"));
    sh("cd /");
    CHECK(sh("ls") == 0 && has("bin/") && has("home/") && sh("ls -l /home") == 0 && has("drwxr-x---") && has("admin/") && has("student/"));
    CHECK(sh("ls -l /etc") == 0 && has("-rw-r--r--") && has("motd") && sh("ls /etc/motd") == 0 && has("motd"));
    CHECK(sh("ls /nonexistent") == 1 && has("directory '/nonexistent' does not exist.") && sh("ls -z") == 1 && has("unknown option"));
    CHECK(sh("lsdirs /") == 0 && has("tmp/") && sh("lsfiles /etc") == 0 && has("motd") && !has("/"));
    CHECK(sh("lsfiles /bin") == 0 && has("(empty)") && sh("dircount /") == 0 && has("entries") && sh("dircount /etc") == 0 && has("2 entries"));
    CHECK(sh("lsr /home") == 0 && has("/home/admin/") && has("/home/student/") && sh("lsr /nope") == 1);
    CHECK(sh("find motd") == 0 && has("/etc/motd") && sh("find *s* /etc") == 0 && has("hostname") && sh("find 'h?me'") == 0 && has("/home/"));
    CHECK(sh("find zzzz") == 1 && has("no match") && sh("find x /nowhere") == 1 && sh("find") == 1);
    CHECK(sh("tree /home") == 0 && has("|-- ") && has("2 directories, 0 files") && sh("tree /etc/motd") == 1 && has("not a directory"));
    CHECK(sh("cpdir /a /a_copy") == 0 && has("copied 0 file(s) in 3 directories") && fs_stat("/a_copy/b/c", NULL) == FS_OK && sh("cpdir /a /a_copy") == 1 && sh("cpdir /etc/motd /x") == 1 && sh("cpdir /a /a/inside") == 1 && has("into itself"));
    CHECK(sh("rmtree /a_copy") == 0 && has("removed 3 items") && sh("rmtree /") == 1 && has("refusing") && sh("rmtree /ghost") == 1);
    sh("cd /x/y"); CHECK(sh("rmtree /x") == 1 && has("refusing") && sh("rmdir /x/y") == 1 && has("cannot remove"));
    sh("cd /");

    puts("text tools");
    fresh();
    sh("echo \"line one\" > t.txt"); sh("echo \"Line two has Foo\" >> t.txt"); sh("echo \"line three\" >> t.txt"); sh("echo foo again >> t.txt");
    CHECK(sh("cat t.txt") == 0 && has("line one") && has("line three"));
    CHECK(sh("head -n 2 t.txt") == 0 && has("line one") && has("Line two") && !has("line three"));
    CHECK(sh("tail -n 2 t.txt") == 0 && has("line three") && has("foo again") && !has("line one"));
    CHECK(sh("head t.txt") == 0 && has("foo again") && sh("tail t.txt") == 0 && has("line one"));
    CHECK(sh("head -n 0 t.txt") == 0 && out_len == 0 && sh("tail -n 0 t.txt") == 0 && out_len == 0);
    CHECK(sh("head -n x t.txt") == 1 && sh("head") == 1 && sh("head a b") == 1 && sh("head nofile") == 1);
    CHECK(sh("wc t.txt") == 0 && has("      4") && has("t.txt") && sh("wc -l t.txt") == 0 && strstr(out, "      4 t.txt") != NULL);
    CHECK(sh("wc -w t.txt") == 0 && has("     10 t.txt") && sh("wc -c t.txt") == 0 && has("t.txt") && sh("wc t.txt /etc/hostname") == 0 && has("total") && sh("wc -q t.txt") == 1 && sh("wc") == 1);
    CHECK(sh("grep foo t.txt") == 0 && has("foo again") && !has("Foo") && sh("grep -i foo t.txt") == 0 && has("Line two has Foo") && has("foo again"));
    CHECK(sh("grep -n line t.txt") == 0 && has("1:line one") && has("3:line three"));
    CHECK(sh("grep zzz t.txt") == 1 && out_len == 0 && sh("grep x nofile") == 2 && sh("grep x") == 1 && sh("grep -q x f") == 1);
    CHECK(sh("grep line t.txt /etc/motd") == 0 && has("t.txt:line one"));
    CHECK(sh("hexdump /etc/hostname") == 0 && has("6D 69 6E 69 6F 73 0A") && has("|minios.|") && sh("hexdump t.txt 16") == 0 && has("more byte(s)") && sh("hexdump t.txt 0") == 1 && sh("hexdump") == 1);
    sh("write c1 same"); sh("write c2 same"); sh("write c3 sane");
    CHECK(sh("cmp c1 c2") == 0 && has("identical") && sh("cmp c1 c3") == 1 && has("differ at byte 3") && sh("cmp c1 none") == 2 && sh("cmp c1") == 1);
    sh("fill c4 same 1");
    CHECK(sh("cmp c1 c4") == 1 && has("shorter"));
    CHECK(sh("motd") == 0 && has("Welcome to MiniOS"));
    sh("mkfile zeros 20");
    CHECK(sh("cat zeros") == 0 && has("...."));                              /* control characters never reach the screen raw */

    puts("permissions and chmod");
    fresh();
    sh("write /tmp/p data");
    CHECK(sh("chmod 600 /tmp/p") == 0 && sh("stat /tmp/p") == 0 && has("0600") && has("-rw-------"));
    CHECK(sh("chmod +x /tmp/p") == 0 && sh("stat /tmp/p") == 0 && has("0711") && has("-rwx--x--x"));
    CHECK(sh("chmod go-x /tmp/p") == 0 && sh("stat /tmp/p") == 0 && has("-rwx------") && sh("chmod u=r /tmp/p") == 0 && sh("stat /tmp/p") == 0 && has("-r--------"));
    CHECK(sh("chmod a+rw /tmp/p") == 0 && sh("stat /tmp/p") == 0 && has("-rw-rw-rw-"));
    CHECK(sh("chmod 0755 /tmp/p /etc/motd") == 0 && sh("chmod 888 /tmp/p") == 1 && has("bad mode") && sh("chmod 7777 /tmp/p") == 1 && sh("chmod q+z /tmp/p") == 1 && sh("chmod 644") == 1);
    CHECK(sh("chmod 644 /tmp/ghost") == 1 && has("does not exist"));
    CHECK(sh("readonly /tmp/p") == 0 && sh("stat /tmp/p") == 0 && has("-r-xr-xr-x") && sh("writable /tmp/p") == 0 && sh("stat /tmp/p") == 0 && has("-rwxr-xr-x") && sh("readonly /nope") == 1 && sh("writable") == 1);
    fs_set_user(FS_UID_STUDENT, FS_UID_STUDENT);
    CHECK(sh("chmod 777 /etc/motd") == 1 && has("permission denied") && sh("rm /etc/motd") == 1 && has("permission denied") && sh("cat /home/admin/x") == 1);
    CHECK(sh("touch /etc/newfile") == 1 && has("permission denied") && sh("mkdir /usr/x") == 1);
    fs_set_user(FS_UID_ROOT, FS_UID_ROOT);

    puts("file-system info");
    fresh();
    CHECK(sh("df") == 0 && has("minifs") && has("ramfs") && has("12/512") && has("volatile"));
    CHECK(sh("du /etc") == 0 && has("52 B") == false && has("2 files, 1 directory") && sh("du") == 0 && sh("du /ghost") == 1);
    CHECK(sh("inodes") == 0 && has("12 used, 500 free, 512 total") && sh("freeblocks") == 0 && has("no disk blocks"));
    CHECK(sh("fsstat") == 0 && has("Files        : 2") && has("Directories  : 10") && sh("fsdump") == 0 && has("Persistence: none") && has("etc/"));
    CHECK(sh("fsck") == 0 && has("consistent") && has("12 nodes"));
    sh("mkdir junk"); sh("cd junk");
    CHECK(sh("mkfs") == 1 && has("--yes") && fs_stat("/junk", NULL) == FS_OK);
    CHECK(sh("mkfs please") == 1 && fs_stat("/junk", NULL) == FS_OK);
    CHECK(sh("mkfs --yes") == 0 && fs_stat("/junk", NULL) != FS_OK && sh("pwd") == 0 && strcmp(out, "/\n") == 0 && sh("fsck") == 0);
    CHECK(sh("ls /etc") == 0 && has("motd"));

    puts("commands built on system calls");
    fresh();
    CHECK(sh("sysinfo") == 0 && has("MiniOS system summary") && has("Version     : 0.16.0") && has("Hostname    : minios") && has("System calls:") && has("MiniFS (RAM)"));
    CHECK(sh("uname") == 0 && strcmp(out, "MiniOS\n") == 0 && sh("uname -a") == 0 && has("MiniOS minios 0.16.0") && sh("uname -x") == 1 && sh("uname a b") == 1);
    CHECK(sh("hostname") == 0 && strcmp(out, "minios\n") == 0);
    CHECK(sh("hostname lab-1") == 0 && sh("hostname") == 0 && strcmp(out, "lab-1\n") == 0 && sh("uname -a") == 0 && has("lab-1"));
    CHECK(sh("hostname -x") == 1 && has("letters, digits") && sh("hostname a/b") == 1 && sh("hostname \"a b\"") == 1 && sh("hostname 0123456789012345678901234567890123") == 1 && sh("hostname a b") == 1);
    CHECK(sh("hostname") == 0 && strcmp(out, "lab-1\n") == 0);                                   /* invalid names change nothing */
    fs_set_user(FS_UID_STUDENT, FS_UID_STUDENT);
    CHECK(sh("hostname hacked") == 1 && has("permission denied") && sh("hostname") == 0 && strcmp(out, "lab-1\n") == 0);
    fs_set_user(FS_UID_ROOT, FS_UID_ROOT);
    CHECK(sh("errno -1") == 0 && has("ENOENT") && has("no such file or directory") && sh("errno 15") == 0 && has("EBADF") && sh("errno EBADF") == 0 && has("-15") == false && has("EBADF"));
    CHECK(sh("errno ebadf") == 0 && has("bad file descriptor") && sh("errno 0") == 0 && has("OK") && sh("errno -26") == 0 && has("ESTATE") && sh("errno 14") == 0 && has("stale"));
    CHECK(sh("errno 33") == 1 && has("no such error code") && has("0 to -32") && sh("errno 30") == 0 && has("EPROTECT") && sh("errno 27") == 0 && has("EAUTH") && sh("errno foo") == 1 && sh("errno -99") == 1 && sh("errno") == 1 && has("usage:") && sh("errno 99999999999") == 1);
    CHECK(sh("fdlist") == 0 && has("console") && has("descriptors 0-2") && sh("fdinfo 1") == 0 && has("Target : console") && has("Access : w") && sh("fdinfo 0") == 0 && has("Access : r"));
    CHECK(sh("fdinfo 5") == 1 && has("bad file descriptor") && sh("fdinfo 99") == 1 && has("0 to 15") && sh("fdinfo abc") == 1 && sh("fdinfo") == 1);

    /* commands must not leak descriptors or memory: run a batch, then compare */
    sh("write /tmp/leak one two three"); sh("cat /tmp/leak");                                      /* warm up: creates the shell's resource block */
    heap_stats_t h0, h1; heap_stats(&h0);
    sh("cat /tmp/leak"); sh("head /tmp/leak"); sh("tail /tmp/leak"); sh("wc /tmp/leak /etc/motd"); sh("grep one /tmp/leak"); sh("hexdump /tmp/leak"); sh("cmp /tmp/leak /etc/motd");
    sh("file /tmp/leak"); sh("readfile /tmp/leak 2 4"); sh("ls /"); sh("ls -l /etc"); sh("dircount /"); sh("cat /nonexistent"); sh("cat /tmp"); sh("grep x /nonexistent");
    sh("write /tmp/s1 temp"); sh("append /tmp/s1 more"); sh("shred /tmp/s1"); sh("fill /tmp/f2 ab 50"); sh("note /tmp/f2 hi"); sh("rm /tmp/f2"); sh("mkdir /tmp/dd"); sh("cp /tmp/leak /tmp/dd/c"); sh("mv /tmp/dd/c /tmp/dd/d"); sh("rmtree /tmp/dd");
    heap_stats(&h1);
    CHECK(h1.used_blocks == h0.used_blocks && h1.payload_used == h0.payload_used);
    CHECK(sh("fdlist") == 0 && !has(" /") );                                                       /* no file descriptor left open: only the console lines */

    CHECK(heap_check(NULL) == HEAP_OK);
    { fs_check_report_t r; CHECK(fs_check(&r) == 0); }
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}

int main(void) { return run_on_low_stack(real_main); }
