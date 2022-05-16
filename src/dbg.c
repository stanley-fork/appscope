#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "atomic.h"
#include "dbg.h"
#include "utils.h"
#include "scopestdlib.h"

#define UNW_LOCAL_ONLY
#include "libunwind.h"

#define MAX_INSTANCES_PER_LINE 2
#define MAX_NUM_LINES 256
#define SYMBOL_BT_NAME_LEN (256)

typedef struct {
    time_t time;
    int err; // errno
    char *str;
} occurrence_t;

typedef struct {
    const char *key;
    uint64_t count;
    occurrence_t instance[MAX_INSTANCES_PER_LINE];
} line_t;

struct _dbg_t {
    line_t lines[MAX_NUM_LINES];
};

dbg_t *g_dbg = NULL;
log_t *g_log = NULL;
proc_id_t g_proc = {0};
bool g_constructor_debug_enabled;
uint64_t g_cbuf_drop_count = 0;
bool g_ismusl = FALSE;

void
dbgInit()
{
    dbgDestroy();

    g_dbg = scope_calloc(1, sizeof(*g_dbg));
}

static char *
atomicSwapString(char **ptr, char *str)
{
    // This helper function just exists to hide all the type casting here
    // instead of in the calling code.
    char *prev_str;
    do {
        prev_str = *ptr;
    } while (!atomicCasU64((uint64_t*)ptr, (uint64_t)prev_str, (uint64_t)str));
    return prev_str;
}

static void
updateLine(line_t *line, char *str)
{
    if (!line) return;

    // Increment count atomically in a way so we know the original count.
    // That's so the determination of i below is unique when possible.
    uint64_t orig_count;
    uint64_t new_count;
    do {
        orig_count = line->count;
        new_count = orig_count + 1;
    } while (!atomicCasU64(&line->count, orig_count, new_count));

    // This keeps overwriting the latest one.
    int i = (orig_count < MAX_INSTANCES_PER_LINE )
            ? orig_count
            : MAX_INSTANCES_PER_LINE - 1;

    struct timeval tv;
    scope_gettimeofday(&tv, NULL);
    line->instance[i].time = tv.tv_sec;
    line->instance[i].err = scope_errno;

    // The atomic swap allows us to scope_free previous strings without leaking
    // memory and equally importantly, ensures we can't double-scope_free a str.
    char *prev_str = atomicSwapString(&line->instance[i].str, str);
    if (prev_str) scope_free(prev_str);
}

static void
resetLine(line_t *line)
{
    if (!line) return;
    int i;
    for (i=0; i < MAX_INSTANCES_PER_LINE; i++) {
        char *prev_str = atomicSwapString(&line->instance[i].str, NULL);
        if (prev_str) scope_free(prev_str);
    }
    scope_memset(line, 0, sizeof(*line));
}

void
dbgDestroy()
{
     if (!g_dbg) return;

     int i;
     for (i=0; i < MAX_NUM_LINES; i++) {
         resetLine(&g_dbg->lines[i]);
     }

    scope_free(g_dbg);
    g_dbg = NULL;
}

// Accessors
unsigned long long
dbgCountAllLines()
{
    if (!g_dbg) return 0ULL;

    unsigned long long i;
    for (i=0; i < MAX_NUM_LINES; i++) {
        if (!g_dbg->lines[i].key) break;
    }
    return i;
}

unsigned long long
dbgCountMatchingLines(const char *str)
{
    if (!g_dbg || !str) return 0ULL;
    unsigned long long result = 0ULL;
    int i;
    for (i=0; i < MAX_NUM_LINES; i++) {
        if (!g_dbg->lines[i].key) break;
        if (scope_strstr(g_dbg->lines[i].key, str)) {
            result++;
        }
    }
    return result;
}

static void
dbgOutputHeaderLine(FILE *f)
{
    if (!f) return;

    struct timeval tv;
    scope_gettimeofday(&tv, NULL);

    struct tm t;
    char buf[128] = {0};
    scope_strftime(buf, sizeof(buf), "%FT%TZ", scope_gmtime_r(&tv.tv_sec, &t));
    scope_fprintf(f, "Scope Version: %s   Dump From: %s\n", SCOPE_VER, buf);
}

void
dbgDumpAll(FILE *f)
{
    if (!f) return;

    dbgOutputHeaderLine(f);

    if (!g_dbg) return;

    int i;
    for (i=0; i < MAX_NUM_LINES; i++) {
        if (!g_dbg->lines[i].key) break;
        line_t *l = &g_dbg->lines[i];

        int j;
        for (j=0; j<MAX_INSTANCES_PER_LINE; j++) {
            occurrence_t *o = &l->instance[j];

            struct tm t;
            char t_str[64];
            if (!o->time) continue;
            if (!scope_gmtime_r(&o->time, &t)) continue;
            if (!scope_strftime(t_str, sizeof(t_str), "%s", &t)) continue;

            if (j==0) {
                scope_fprintf(f, "%lu: %s %s %d(%s) %s\n",
                    l->count, l->key,
                    t_str, o->err, scope_strerror(o->err), o->str);
            } else {
                scope_fprintf(f, "    %s %d(%s) %s\n",
                    t_str, o->err, scope_strerror(o->err), o->str);
            }
        }
    }
}

// Setters
static void
dbgAddLineHelper(const char * const key, char *str)
{
    line_t *line;
    int i;

search_for_spot:
    line = NULL;

    // See if a line already has our key
    for (i=0; i < MAX_NUM_LINES; i++) {
        if (!g_dbg->lines[i].key) break;

        // We don't have to do a scope_strcmp; comparing the pointers works
        // since the key is generated by compiler
        if (key == g_dbg->lines[i].key) {
            line = &g_dbg->lines[i];
            break;
        }
    }

    // A line already has our key, just update it
    if (line) {
        updateLine(line, str);
        return;
    }

    // If we're out of space for our key, give up.
    if (i >= MAX_NUM_LINES) {
        if (str) scope_free(str);
        return;
    }

    // Assign our key to a line in a real-time safe way.
    line = &g_dbg->lines[i];
    if (atomicCasU64((uint64_t*)&line->key, 0ULL, (uint64_t)key)) {
        updateLine(line, str);
    } else {
        // Another thread took the spot was thought was available.
        // Starting the search over will ensure we don't add one key twice.
        goto search_for_spot;
    }
}

void
dbgAddLine(const char *key, const char *fmt, ...)
{
    // TODO verify should this be called always
    // scopeBacktrace(CFG_LOG_ERROR);

    if (!g_dbg) return;

    // Create the string
    char *str = NULL;
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        int rv = scope_vasprintf(&str, fmt, args);
        va_end(args);
        if (rv == -1) {
            if (str) scope_free(str);
            str = NULL;
        }
    }
    dbgAddLineHelper(key, str);
}


// This is declared weak so it can be overridden by the strong
// definition in src/wrap.c.  The scope library will do this.
// This weak definition allows us to not have to define this symbol
// for unit tests and allows for a different definition for the
// scope executable.
void __attribute__((weak))
scopeLog(cfg_log_level_t level, const char *format, ...)
{
    return;
}

void
scopeBacktrace(cfg_log_level_t level)
{
    unw_cursor_t cursor;
    unw_context_t uc;
    unw_word_t ip;

    unw_getcontext(&uc);
    unw_init_local(&cursor, &uc);
    int frame_count = 0;
    scopeLog(level, "--- scopeBacktrace");
    while(unw_step(&cursor) > 0) {
        char symbol[SYMBOL_BT_NAME_LEN];
        unw_word_t offset;

        int ret = unw_get_reg(&cursor, UNW_REG_IP, &ip);
        if (ret) {
            continue;
        }

        ret = unw_get_proc_name(&cursor, symbol, SYMBOL_BT_NAME_LEN, &offset);
        if (!ret) {
            scopeLog(level, "#%d 0x%lx %s + %d", frame_count, (long)ip, symbol, (int)offset);
        } else {
            scopeLog(level, "#%d  0x%lx ?", frame_count, (long)ip);
        }
        frame_count++;
    }
}

void
scopeLogHex(cfg_log_level_t level, const void *data, size_t size, const char *format, ...)
{
    unsigned i; // current index into data

    char hex[16*3 + 5]; // one line of hex dump (+5 for 2x 2-byte pads plus \0)
    char txt[16 + 1];   // one line of ascii text (+1 for \0)

    const unsigned char *pdata = (const unsigned char *)data; // pos in data
    char *phex;                                               // pos in hex

    char buf[4096];
    va_list args;
    va_start(args, format);
    int buflen = scope_vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (buflen == -1) {
        scopeLog(level, "%s (format too long!) (%ld bytes)", format, size);
    } else {
        scopeLog(level, "%s (%ld bytes)", buf, size);
    }

    if (!size) return;

    hex[0] = '\0'; txt[0] = '\0'; phex = hex; // reset

    for (i = 0; i < size; ++i) {
        if (i && (i % 16) == 0) {
            scopeLog(level, "  %04x: %s %s", i-16, hex, txt);
            hex[0] = '\0'; txt[0] = '\0'; phex = hex; // reset
        }

        scope_sprintf(phex, " %02x", pdata[i]); phex += 3;
        if ((i % 8) == 7) {
            *phex++ = ' '; *phex = '\0';
        }

        if ((pdata[i] < 0x20) || (pdata[i] > 0x7e))
            txt[i % 16] = '.';
        else
            txt[i % 16] = pdata[i];
        txt[(i % 16) + 1] = '\0';
    }

    while ((i % 16) != 0) {
        *phex++ = ' '; *phex++ = ' '; *phex++ = ' '; *phex = '\0';
        if ((i % 8) == 7) {
            *phex++ = ' '; *phex = '\0';
        }
        i++;
    }
    scopeLog(level, "  %04x: %s %s", i-16, hex, txt);
}
