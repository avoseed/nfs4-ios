/* Recompiled modules linked into this build. */
#include "rt_host.h"
#include "nfs4_image.h"
#include "v2a_image.h"
#include "eacsnd_image.h"

#define MODULE_DECLS(p) \
    extern const GuestFn p##_functions[]; extern const uint32_t p##_function_count; \
    extern const HleImport p##_imports[]; extern const uint32_t p##_import_count; \
    extern const HleExport p##_exports[]; extern HleFn p##_imp[];
MODULE_DECLS(nfs4)
MODULE_DECLS(v2a)
MODULE_DECLS(eacsnd)

RtModule rt_modules[8];
int rt_module_count;

static void add(const char *file, uint32_t base, uint32_t entry, uint32_t stamp, const GuestFn *f, uint32_t nf,
                const HleImport *im, uint32_t ni, HleFn *imp, const HleExport *ex, int dll) {
    RtModule *m = &rt_modules[rt_module_count++];
    m->file = file; m->base = base; m->entry = entry; m->timestamp = stamp;
    m->funcs = f; m->nfuncs = nf; m->imports = im; m->nimports = ni; m->imp = imp; m->exports = ex;
    m->is_dll = dll;
}

#define ADD(p, P, file, dll) add(file, P##_BASE, P##_ENTRY, P##_TIMESTAMP, p##_functions, p##_function_count, \
                                 p##_imports, p##_import_count, p##_imp, p##_exports, dll)

void rt_register_modules(void) {
    ADD(nfs4, NFS4, "nfs4.exe", 0);
    /* the same voodoo2a.dll ships in drivers/nglide, drivers/glide3x and drivers/dgvoodoo */
    ADD(v2a, V2A, "drivers/glide3x/voodoo2a.dll", 1);
    ADD(eacsnd, EACSND, "eacsnd.dll", 1);
}
