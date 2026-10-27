/* Fixed-size ring buffer of previous command lines. */
#include "include/shell.h"

static char entries[HISTORY_SIZE][SHELL_LINE_MAX];
static int  first;   /* index of oldest entry */
static int  count;

static bool equals(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void history_init(void) { first = 0; count = 0; }

void history_add(const char *line) {
    if (!line || !*line) return;
    const char *last = history_get(count - 1);
    if (last && equals(last, line)) return;       /* skip consecutive duplicates */

    int slot;
    if (count < HISTORY_SIZE) slot = (first + count++) % HISTORY_SIZE;
    else { slot = first; first = (first + 1) % HISTORY_SIZE; }  /* overwrite oldest */

    int i = 0;
    while (line[i] && i < SHELL_LINE_MAX - 1) { entries[slot][i] = line[i]; i++; }
    entries[slot][i] = '\0';
}

int history_count(void) { return count; }

const char *history_get(int index) {
    if (index < 0 || index >= count) return NULL;
    return entries[(first + index) % HISTORY_SIZE];
}
