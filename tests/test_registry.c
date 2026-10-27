/* Host-side unit tests for the command registry (run: make test).
 * Compiles the REAL registry + table with the host gcc. Includes negative tests:
 * the validator is fed deliberately corrupted tables to prove it can fail. */
#include <stdio.h>
#include <string.h>
#include "include/command.h"

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; \
    printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static int dummy_handler(int argc, const char **argv) { (void)argc; (void)argv; return 0; }

static void test_table_shape(void) {
    puts("table shape");
    CHECK(command_count() == 300);
    CHECK(command_count() == COMMAND_EXPECTED);
    CHECK(command_validate(NULL) == CMDV_OK);
    CHECK(command_count_level(LEVEL_L1) == 51);
    CHECK(command_count_level(LEVEL_L2) == 219);
    CHECK(command_count_level(LEVEL_L3) == 30);
    CHECK(command_count_level(LEVEL_L1) + command_count_level(LEVEL_L2) + command_count_level(LEVEL_L3) == 300);
    CHECK(command_count_level(0) == 0 && command_count_level(4) == 0);

    static const size_t split[CAT_COUNT] = { 25, 35, 20, 30, 15, 20, 15, 25, 25, 30, 20, 20, 20 };
    for (int c = 0; c < CAT_COUNT; c++) CHECK(command_count_category((cmd_category_t)c, NULL) == split[c]);

    /* independent duplicate check (does not rely on command_validate) */
    int dups = 0;
    for (size_t i = 0; i < command_count(); i++)
        for (size_t j = i + 1; j < command_count(); j++)
            if (strcmp(command_at(i)->name, command_at(j)->name) == 0) dups++;
    CHECK(dups == 0);

    /* every entry is complete */
    int incomplete = 0;
    for (size_t i = 0; i < command_count(); i++) {
        const command_t *c = command_at(i);
        if (c->id != i + 1 || !*c->name || !*c->desc || !*c->subsys) incomplete++;
    }
    CHECK(incomplete == 0);
}

static void test_lookup(void) {
    puts("lookup");
    const command_t *c = command_find("help");
    CHECK(c && c->id == 1 && c->level == LEVEL_L1 && c->category == CAT_SHELL);
    c = command_find("credits");
    CHECK(c && c->id == 300 && c->category == CAT_OTHER);
    c = command_find("ping");
    CHECK(c && c->level == LEVEL_L3 && c->category == CAT_NETWORK);
    CHECK(command_find("nosuchcommand") == NULL);
    CHECK(command_find("") == NULL);
    CHECK(command_find(NULL) == NULL);
    CHECK(command_find("HELP") == NULL);              /* case sensitive */
    CHECK(command_find("hel") == NULL);               /* no prefix match */
    CHECK(command_at(0) && command_at(299) && command_at(300) == NULL);
    CHECK(strcmp(command_at(61)->name, "ls") == 0);   /* #62 */
}

static void test_names(void) {
    puts("names");
    CHECK(strcmp(command_category_name(CAT_FILE), "file") == 0);
    CHECK(strcmp(command_category_name(CAT_COUNT), "?") == 0);
    CHECK(command_category_from_name("file") == CAT_FILE);
    CHECK(command_category_from_name("dir") == CAT_DIR);
    CHECK(command_category_from_name("proc") == CAT_PROCESS);
    CHECK(command_category_from_name("net") == CAT_NETWORK);
    CHECK(command_category_from_name("perm") == CAT_SECURITY);
    CHECK(command_category_from_name("bogus") == -1);
    CHECK(command_category_from_name("") == -1 && command_category_from_name(NULL) == -1);
    CHECK(strcmp(command_level_name(LEVEL_L2), "L2") == 0);
    CHECK(strcmp(command_level_name(9), "L?") == 0);
}

static void test_binding(void) {
    puts("binding");
    size_t before = command_count_implemented();
    CHECK(before == 0);                                       /* table starts all-stub */
    CHECK(command_bind("nosuchcommand", dummy_handler) == CMD_BIND_UNKNOWN);
    CHECK(command_bind("help", NULL) == CMD_BIND_NULLFN);
    CHECK(command_count_implemented() == before);             /* failures bind nothing */
    CHECK(command_bind("help", dummy_handler) == CMD_BIND_OK);
    CHECK(command_find("help")->handler == dummy_handler);
    CHECK(command_count_implemented() == before + 1);
    CHECK(command_bind("help", dummy_handler) == CMD_BIND_OK);   /* re-bind is fine */
    CHECK(command_count_implemented() == before + 1);
    size_t impl;
    command_count_category(CAT_SHELL, &impl);
    CHECK(impl == 1);
    command_table[0].handler = NULL;                          /* restore */
    CHECK(command_count_implemented() == 0);
}

static void test_planned_phase(void) {
    puts("planned phase");
    CHECK(command_planned_phase(command_find("help")) == 4);       /* SH */
    CHECK(command_planned_phase(command_find("ls")) == 8);         /* FS */
    CHECK(command_planned_phase(command_find("ps")) == 7);         /* PROC */
    CHECK(command_planned_phase(command_find("mem")) == 6);        /* MEM */
    CHECK(command_planned_phase(command_find("chmod")) == 10);     /* SEC+FS -> latest */
    CHECK(command_planned_phase(command_find("schedtrace")) == 12);/* PROC+LOG */
    CHECK(command_planned_phase(command_find("ping")) == 11);      /* NET */
    CHECK(command_planned_phase(NULL) == 11);
    CHECK(strcmp(command_planned_phase_what(command_find("ls")), "MiniFS") == 0);
}

/* Negative tests: corrupt one field, expect the matching error, then restore. */
static void test_validator_catches_errors(void) {
    puts("validator (negative tests)");
    size_t bad = 99;
    command_t saved = command_table[10];

    command_table[10].name = command_table[9].name;
    CHECK(command_validate(&bad) == CMDV_DUPLICATE && bad == 10);
    command_table[10] = saved;

    command_table[10].name = "";
    CHECK(command_validate(&bad) == CMDV_EMPTY_NAME && bad == 10);
    command_table[10] = saved;

    command_table[10].name = "Bad Name";
    CHECK(command_validate(&bad) == CMDV_BAD_NAME);
    command_table[10] = saved;

    command_table[10].level = 4;
    CHECK(command_validate(&bad) == CMDV_BAD_LEVEL);
    command_table[10].level = 0;
    CHECK(command_validate(&bad) == CMDV_BAD_LEVEL);
    command_table[10] = saved;

    command_table[10].category = CAT_COUNT;
    CHECK(command_validate(&bad) == CMDV_BAD_CATEGORY);
    command_table[10] = saved;

    command_table[10].id = 999;
    CHECK(command_validate(&bad) == CMDV_BAD_ID);
    command_table[10] = saved;

    command_table[10].desc = "";
    CHECK(command_validate(&bad) == CMDV_NO_DESC);
    command_table[10] = saved;

    command_table[10].subsys = NULL;
    CHECK(command_validate(&bad) == CMDV_NO_SUBSYS);
    command_table[10] = saved;

    command_table[10].category = CAT_FILE;                     /* shell command moved to "file" */
    CHECK(command_validate(&bad) == CMDV_CATEGORY_SPLIT);
    command_table[10] = saved;

    CHECK(command_validate(&bad) == CMDV_OK);                  /* table fully restored */
    CHECK(strlen(command_validate_error(CMDV_DUPLICATE)) > 0);
    CHECK(strlen(command_validate_error(12345)) > 0);
}

int main(void) {
    test_table_shape(); test_lookup(); test_names(); test_binding();
    test_planned_phase(); test_validator_catches_errors();
    printf("\n%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
