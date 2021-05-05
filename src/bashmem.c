#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <malloc.h>
#include "distorm.h"

#include "bashmem.h"
#include "dbg.h"
#include "fn.h"
#include "os.h"
#include "scopetypes.h"
#include "utils.h"

typedef struct {
    void *(*malloc)(size_t size);
    void *(*realloc)(void *ptr, size_t size);
    void (*free)(void *ptr);
    void *(*memalign)(size_t alignment, size_t size);
    void (*cfree)(void *ptr);
} bash_mem_fn_t;

bash_mem_fn_t g_mem_fn;

// fn prototypes
static void *bash_internal_malloc(size_t, const char *, int, int);
static void *bash_internal_realloc(void *, size_t, const char *, int, int);
static void bash_internal_free(void *, const char *, int, int);
static void *bash_internal_memalign(size_t, size_t, const char *, int, int);
static void bash_internal_cfree(void *, const char *, int, int);

typedef struct {
    // constant at build time
    const char *name;
    const void *fn_ptr;

    // set at runtime
    void *external_addr;
    void *internal_addr;
} patch_info_t;

patch_info_t bash_mem_func[] = {
    {"malloc",        bash_internal_malloc,        NULL, NULL},
    {"realloc",       bash_internal_realloc,       NULL, NULL},
    {"free",          bash_internal_free,          NULL, NULL},
    {"memalign",      bash_internal_memalign,      NULL, NULL},
    {"cfree",         bash_internal_cfree,         NULL, NULL},
};
const int bash_mem_func_count = sizeof(bash_mem_func)/sizeof(bash_mem_func[0]);

static int
glibcMemFuncsFound(void)
{
    g_mem_fn.malloc = dlsym(RTLD_NEXT, "malloc");
    g_mem_fn.realloc = dlsym(RTLD_NEXT, "realloc");
    g_mem_fn.free = dlsym(RTLD_NEXT, "free");
    g_mem_fn.memalign = dlsym(RTLD_NEXT, "memalign");
    g_mem_fn.cfree = dlsym(RTLD_NEXT, "cfree");

    return g_mem_fn.malloc &&
           g_mem_fn.realloc &&
           g_mem_fn.free &&
           g_mem_fn.memalign &&
           g_mem_fn.cfree;
}

int
in_bash_process(void)
{
    char *exe_path = NULL;
    osGetExePath(&exe_path);
    int is_bash = endsWith(exe_path, "/bash");
    if (exe_path) free(exe_path);
    return is_bash;
}

int
func_found_in_executable(const char *symbol)
{
    int func_found = FALSE;

    // open the exectuable (as opposed to a specific shared lib)
    void *exe_handle = g_fn.dlopen(NULL, RTLD_LAZY);
    if (!exe_handle) goto out;

    void *symbol_ptr = dlsym(exe_handle, symbol);
    if (!symbol_ptr) goto out;

    Dl_info symbol_info;
    void *lm, *es;
    if (!dladdr1(symbol_ptr, &symbol_info, &lm, RTLD_DL_LINKMAP) ||
        !dladdr1(symbol_ptr, &symbol_info, &es, RTLD_DL_SYMENT)) {
        goto out;
    }
    struct link_map *link_map = (struct link_map *)lm;
    ElfW(Sym) *elf_sym = (ElfW(Sym) *)es;
    int symbol_type = ELF64_ST_TYPE(elf_sym->st_info);
    int symbol_binding = ELF64_ST_BIND(elf_sym->st_info);
    int symbol_visibility = ELF64_ST_VISIBILITY(elf_sym->st_other);

    // return true iff l_name is empty, meaning that symbol
    // was found outside of a shared library, aka in the exectuable.
    func_found =
          (symbol_binding == STB_GLOBAL) &&
          (symbol_type == STT_FUNC) &&
          (symbol_visibility == STV_DEFAULT) &&
          (link_map->l_name[0] == '\0'); // not a shared library name

out:
    if (exe_handle) dlclose(exe_handle);
    return func_found;
}

static void *
bash_internal_malloc(size_t bytes, const char *file, int line, int flags)
{
    return g_mem_fn.malloc(bytes);
}

static void *
bash_internal_realloc(void *mem, size_t n, const char *file, int line, int flags)
{
    return g_mem_fn.realloc(mem, n);
}

static void
bash_internal_free(void *mem, const char *file, int line, int flags)
{
    g_mem_fn.free(mem);
}

static void *
bash_internal_memalign(size_t alignment, size_t size, const char *file, int line, int flags)
{
    return g_mem_fn.memalign(alignment, size);
}

// Defined here because it's widely deprecated
extern void cfree (void *__ptr);

static void
bash_internal_cfree(void *p, const char *file, int line, int flags)
{
    g_mem_fn.cfree(p);
}

static int
bashMemFuncsFound()
{
    int num_found = 0;

    void *exe_handle = g_fn.dlopen(NULL, RTLD_LAZY);
    if (!exe_handle) return FALSE;

    int i;
    for (i=0; i<bash_mem_func_count; i++) {
        patch_info_t *func = &bash_mem_func[i];
        void *func_ptr = dlsym(exe_handle, func->name);
        if (!func_ptr) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Couldn't find bash function %s", func->name);
            scopeLog(buf, -1, CFG_LOG_ERROR);
            continue;
        }

        const int MAX_INST = 15;
        const int DECODE_BYTES = 50;
        unsigned int asm_count = 0;
        _DecodedInst asm_inst[MAX_INST];
        int rc = distorm_decode((uint64_t)func_ptr, func_ptr, DECODE_BYTES,
                         Decode64Bits, asm_inst, MAX_INST, &asm_count);
        if (rc == DECRES_INPUTERR) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Couldn't disassemble bash function %s", func->name);
            scopeLog(buf, -1, CFG_LOG_ERROR);
            continue;
        }

        // look for the first jmp instruction
        int j;
        _DecodedInst *inst;
        for (j=0; j<asm_count; j++) {
            inst = &asm_inst[j];
            if (!strcmp((const char *)inst->mnemonic.p, "JMP") &&
                ((inst->size == 5) || (inst->size == 2))) {
                break;
            }
        }
        if (j==asm_count) {
            char buf[128];
            snprintf(buf, sizeof(buf), "For bash function %s, couldn't find "
                "a JMP instruction in the first %d instructions from 0x%p",
                func->name, asm_count, func_ptr);
            scopeLog(buf, -1, CFG_LOG_ERROR);
            continue;
        }

        // Calculate the destination of the JMP instruction, and save it
        // as the internal_addr that we want to hook later.  Assumes x86_64.
        int64_t addr = inst->offset; // the address of the current inst
        int64_t jmp_offset;
        switch (inst->size) {
            case 5:
                // (05) 0xe927f4ffff -> e9 (jmp) 0xfffff427
                jmp_offset = *(int *)(addr+1);
                break;
            case 2:
                // (02) 0xebec       -> eb (jmp) 0xec
                jmp_offset = *(char *)(addr+1);
                break;
            default:
                continue;
        }
        addr += jmp_offset;          // the relative offset from inst
        addr += inst->size;          // relative to the next instruction ptr

        // Save away what we found
        func->external_addr = func_ptr;
        func->internal_addr = (void *)addr;
        num_found++;
    }

    dlclose(exe_handle);
    return num_found == bash_mem_func_count;
}

static int
replaceBashMemFuncs()
{
    int i, rc;
    int num_patched=0;

    funchook_t *funchook = funchook_create();
    if (!funchook) {
        scopeLog("funchook_create failed", -1, CFG_LOG_ERROR);
        return FALSE;
    }

    // Setting funchook_set_debug_file is not possible when patching memory.
    // If funchook has a debug file, then it does a fopen of this file which
    // calls malloc to create a memory buffer.  In this scenario, the memory
    // buffer is created with bash's memory subsystem, but then after the
    // patching is complete, the fclose of this file will attempt to free the
    // memory buffer with a different memory subsystem than created it.
    // No bueno.
    //
    // funchook_set_debug_file(DEFAULT_LOG_PATH);

    for (i=0; i<bash_mem_func_count; i++) {
        patch_info_t *func = &bash_mem_func[i];
        void *addr_to_patch = func->internal_addr;
        rc = funchook_prepare(funchook, (void **)&addr_to_patch, (void *)func->fn_ptr);
        if (rc) {
            char buf[128];
            snprintf(buf, sizeof(buf), "funchook_prepare failed for %s at 0x%p",
                func->name, func->internal_addr);
            scopeLog(buf, -1, CFG_LOG_ERROR);
        } else {
            num_patched++;
        }
    }

    rc = funchook_install(funchook, 0);
    if (rc) {
        char buf[128];
        snprintf(buf, sizeof(buf), "ERROR: failed to install run_bash_mem_fix. (%s)\n",
                 funchook_error_message(funchook));
        scopeLog(buf, -1, CFG_LOG_ERROR);
    }

    return !rc && (num_patched == bash_mem_func_count);
}

int
run_bash_mem_fix(void)
{
    int successful = FALSE;

    // fill in g_mem_fn by looking up glibc funcs
    if (!glibcMemFuncsFound()) goto out;

    // fill in bash_mem_func by looking up external bash mem funcs
    // then finding where they jmp to within bash (internal bash mem funcs)
    if (!bashMemFuncsFound()) goto out;

    // using bash_mem_func structure, redirect bash internal funcs to ours
    // which will use glibc equivalents.  Voilla!  Now old bashes have their
    // memory subsystem upgraded to glibcs (which is thread safe and supports
    // the new libscope.so thread)
    if (!replaceBashMemFuncs()) goto out;

    successful = TRUE;
out:
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "run_bash_mem_fix was run %s",
                 (successful) ? "successfully" : "but failed");
        scopeLog(buf, -1, CFG_LOG_ERROR);
    }
    return successful;
}
