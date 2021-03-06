/* Copyright (c) 2015, Simone 'evilsocket' Margaritelli
   Copyright (c) 2015, Jorrit 'Chainfire' Jongma
   See LICENSE file for details */

#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "hook.h"

const char* _libhook_log_tag = "LIBHOOK";
int _libhook_log = 1;

void libhook_log(const char* log_tag) {
    _libhook_log_tag = log_tag;
    _libhook_log = log_tag == NULL ? 0 : 1;
}

typedef struct ld_module {
    uintptr_t address;
    std::string name;

    ld_module(uintptr_t a, const std::string& n) :
            address(a), name(n) {
    }
} ld_module_t;

typedef std::vector<ld_module_t> ld_modules_t;

typedef struct hook_t {
    const char *name;
    uintptr_t *original;
    uintptr_t hook;

    hook_t(const char* n, uintptr_t* o, uintptr_t h) :
            name(n), original(o), hook(h) {
    }
} hook_t;

typedef std::vector<hook_t> hooks_t;
hooks_t hooks;

static ld_modules_t get_modules() {
    ld_modules_t modules;
    char buffer[1024] = { 0 };
    uintptr_t address;
    std::string name;

    FILE *fp = fopen("/proc/self/maps", "rt");
    if (fp == NULL) {
        perror("fopen");
        goto done;
    }

    while (fgets(buffer, sizeof(buffer), fp)) {
        if (strstr(buffer, "r-xp")) {
            address = (uintptr_t) strtoul(buffer, NULL, 16);
            name = strrchr(buffer, ' ') + 1;
            name.resize(name.size() - 1);

            modules.push_back(ld_module_t(address, name));
        }
    }

    done:

    if (fp) {
        fclose(fp);
    }

    return modules;
}

static ElfW(Addr) patch_address( ElfW(Addr) addr, ElfW(Addr) newval) {
    ElfW(Addr) original = -1;
    size_t pagesize = sysconf(_SC_PAGESIZE);
    const void *aligned_pointer = (const void*) (addr & ~(pagesize - 1));

    mprotect(aligned_pointer, pagesize, PROT_WRITE | PROT_READ);

    original = *(ElfW(Addr)*) addr;
    *((ElfW(Addr)*) addr) = newval;

    mprotect(aligned_pointer, pagesize, PROT_READ);

    return original;
}

static ElfW(Addr) addhook(struct soinfo_common *soinfo, const char *symbol, ElfW(Addr) newval) {
#ifdef USE_RELA
    ElfW(Rela) *rel = NULL;
#else
    ElfW(Rel) *rel = NULL;
#endif
    size_t i;

    /* work in progress
    if ( (soinfo->version >= 2) && (soinfo->android_relocs_ != NULL) && (soinfo->android_relocs_size_ > 4) && (soinfo->android_relocs_[0] == 'A') && (soinfo->android_relocs_[1] == 'P') && (soinfo->android_relocs_[2] == 'S') && (soinfo->android_relocs_[3] == '2') ) {
        // loop android packed relocs table
        const uint8_t* packed_relocs = soinfo->android_relocs_ + 4;
        const size_t packed_relocs_size = soinfo->android_relocs_size_ - 4;
        packed_reloc_iterator<sleb128_decoder> iterator = packed_reloc_iterator<sleb128_decoder>(sleb128_decoder(packed_relocs, packed_relocs_size));

        for ( i = 0; iterator.has_next(); i++ ) {
#if defined(USE_RELA)
            ElfW(Rela) *rel = iterator.next();
#else
            ElfW(Rel) *rel = iterator.next();
#endif

            ElfW(Word) type = ELFW(R_TYPE)(rel->r_info);
            ElfW(Word) sym = ELFW(R_SYM)(rel->r_info);
#ifdef USE_RELA
            ElfW(Addr) reloc = ElfW(Addr)(rel->r_offset + rel->r_addend + soinfo->base);
#else
            ElfW(Addr) reloc = ElfW(Addr)(rel->r_offset + soinfo->base);
#endif
            if (sym_offset == sym) {
                ElfW(Addr) ret = patch_address(reloc, newval);
                HOOKLOG("[%s][0x%lx] hooked (AND)", symbol, (long unsigned int)ret);
                return ret;
            }
        }
    }
    */

    // loop reloc table to find the symbol by index
#ifdef USE_RELA
    for( i = 0, rel = soinfo->plt_rela; i < soinfo->plt_rela_count; ++i, ++rel ) {
#else
    for( i = 0, rel = soinfo->plt_rel; i < soinfo->plt_rel_count; ++i, ++rel ) {
#endif
        ElfW(Word) type = ELFW(R_TYPE)(rel->r_info);
        ElfW(Word) sym = ELFW(R_SYM)(rel->r_info);
        ElfW(Addr) reloc = ElfW(Addr)(rel->r_offset + soinfo->load_bias);

        if (strcmp(symbol, soinfo->strtab + (soinfo->symtab + sym)->st_name) == 0) {
            switch (type) {
            case R_ARM_JUMP_SLOT:
            case R_AARCH64_JUMP_SLOT:

            case R_386_JUMP_SLOT: // == R_X86_64_JUMP_SLOT

                {
                    ElfW(Addr) ret = patch_address(reloc, newval);
                    HOOKLOG("[%s][0x%lx] hooked (PLT) [%s]", symbol, (long unsigned int)ret, soinfo->strtab + (soinfo->symtab + sym)->st_name);
                    return ret;
                }

            default:

                HOOKLOG("[%s] Expected R_x_JUMP_SLOT, found 0x%X", symbol, type);

            }
        }
    }

    // loop dyn reloc table
#ifdef USE_RELA
    for( i = 0, rel = soinfo->rela; i < soinfo->rela_count; ++i, ++rel ) {
#else
    for( i = 0, rel = soinfo->rel; i < soinfo->rel_count; ++i, ++rel ) {
#endif
        ElfW(Word) type = ELFW(R_TYPE)(rel->r_info);
        ElfW(Word) sym = ELFW(R_SYM)(rel->r_info);
        ElfW(Addr) reloc = ElfW(Addr)(rel->r_offset + soinfo->load_bias);

        if (strcmp(symbol, soinfo->strtab + (soinfo->symtab + sym)->st_name) == 0) {
            switch (type) {
            case R_ARM_ABS32:
            case R_ARM_GLOB_DAT:

            case R_AARCH64_ABS64:
            case R_AARCH64_GLOB_DAT:

            case R_386_32: // -- R_X86_64_64
            case R_386_GLOB_DAT: // == R_X86_64_GLOB_DAT

                {
                    ElfW(Addr) ret = patch_address(reloc, newval);
                    HOOKLOG("[%s][0x%lx] hooked (GOT)", symbol, (long unsigned int)ret);
                    return ret;
                }

            default:

                HOOKLOG("[%s] Expected R_x_ABSxx or R_x_GLOB_DAT, found 0x%X", symbol, type);

            }
        }
    }

/* for hard-core debugging only
#ifdef USE_RELA
    HOOKLOG( "Unable to find symbol in the reloc tables ( plt_rela_count=%u - rela_count=%u ).", (unsigned int)soinfo->plt_rela_count, (unsigned int)soinfo->rela_count );
#else
    HOOKLOG(
            "Unable to find symbol in the reloc tables ( plt_rel_count=%u - rel_count=%u ).",
            (unsigned int )soinfo->plt_rel_count, (unsigned int )soinfo->rel_count);
#endif
*/

    return 0;
}

void _libhook_register(const char* name, uintptr_t* original, uintptr_t hook) {
    hooks.push_back(hook_t(name, original, hook));
}

void libhook_hook() {
    HOOKLOG("LIBRARY LOADED FROM PID %d", getpid());

    const char* libself = NULL;
    Dl_info info;
    if (dladdr((void*) &_libhook_register, &info) != 0) {
        libself = info.dli_fname;
    }
    HOOKLOG("LIBRARY NAME %s", libself == NULL ? "UNKNOWN" : libself);

    // get a list of all loaded modules inside this process.
    ld_modules_t modules = get_modules();

    HOOKLOG("Found %u loaded modules", (unsigned int)modules.size());
    HOOKLOG("Installing %u hooks", (unsigned int)hooks.size());

    // prevent linker from hiding structure
    uint32_t sdk = 0;
    uint32_t (*android_get_application_target_sdk_version)();
    void (*android_set_application_target_sdk_version)(uint32_t target);
    void* libdl = dlopen("libdl.so", RTLD_NOW);
    if (libdl != NULL) {
        android_get_application_target_sdk_version = (uint32_t (*)())dlsym(libdl, "android_get_application_target_sdk_version");
        android_set_application_target_sdk_version = (void (*)(uint32_t))dlsym(libdl, "android_set_application_target_sdk_version");
        if ((android_get_application_target_sdk_version != NULL) && (android_set_application_target_sdk_version != NULL)) {
            sdk = android_get_application_target_sdk_version();
            if (sdk > 23) {
                HOOKLOG("sdk %d --> 23", sdk);
                android_set_application_target_sdk_version(23);
            }
        }
    }

    for (ld_modules_t::const_iterator i = modules.begin(), ie = modules.end();
            i != ie; ++i) {
        // don't hook ourself
        if (i->name.find(libself) == std::string::npos) {
            // since we know the module is already loaded and mostly
            // we DO NOT want its constructors to be called again,
            // use RTLD_NOLOAD to just get its soinfo address.
            void* soinfo_base = dlopen(i->name.c_str(), 4 /* RTLD_NOLOAD */);
            if (!soinfo_base) {
                // possibly RTLD_NOLOAD isn't supported, load with the far less ideal NOW / LOCAL combo
                soinfo_base = dlopen(i->name.c_str(), RTLD_NOW | RTLD_LOCAL);
            }

            if (!soinfo_base) {
                HOOKLOG("[0x%lx] Error hooking %s: %s", (long unsigned int)i->address, i->name.c_str(), dlerror());
            } else {
                // find the correct structure, it has changed over time. Newer Android have FLAG_NEW_SOINFO set, which is easy to find.
                uint32_t flags_min = FLAG_NEW_SOINFO | FLAG_LINKED;
                uint32_t flags_max = (FLAG_NEW_SOINFO | FLAG_GNU_HASH | FLAG_LINKER | FLAG_EXE | FLAG_LINKED) + 0x1000 /* future */;

                struct soinfo_common* soinfo = NULL;
                uint32_t flags;

                // is even newer format? (we're getting silly with the naming here)
                if (!soinfo) {
                    flags = ((struct soinfo_compact2*)soinfo_base)->common.flags;
                    HOOKLOG("soinfo new2: flags:0x%x min:0x%x max:0x%x ok:%d", flags, flags_min, flags_max, ((flags >= flags_min) && (flags <= flags_max)) ? 1 : 0);
                    if ((flags >= flags_min) && (flags <= flags_max)) {
                        soinfo = &((struct soinfo_compact2*)soinfo_base)->common;
                    }
                }

                // is new format?
                if (!soinfo) {
                    flags = ((struct soinfo_compact*)soinfo_base)->common.flags;
                    HOOKLOG("soinfo new: flags:0x%x min:0x%x max:0x%x ok:%d", flags, flags_min, flags_max, ((flags >= flags_min) && (flags <= flags_max)) ? 1 : 0);
                    if ((flags >= flags_min) && (flags <= flags_max)) {
                        soinfo = &((struct soinfo_compact*)soinfo_base)->common;
                    }
                }

                // is old format?
                if (!soinfo) {
                    flags = ((struct soinfo_compat*)soinfo_base)->common.flags;
                    HOOKLOG("soinfo old: flags:0x%x min:0x%x max:0x%x ok:%d", flags, flags_min, flags_max, ((flags >= flags_min) && (flags <= flags_max)) ? 1 : 0);
                    if ((flags >= flags_min) && (flags <= flags_max)) {
                        soinfo = &((struct soinfo_compat*)soinfo_base)->common;
                    }
                }

#if !defined(__LP64__)
                // check old format without FLAG_NEW_SOINFO, 4.4 and older
                if (!soinfo) {
                    // field positions vary slightly in practise, mismatched with AOSP linker headers, not sure why

                    int search[7] = { 0, -1, 1, -2, 2, -3, 3 };

                    uintptr_t* test = (uintptr_t*)&((struct soinfo_compat*)soinfo_base)->common;

                    for (int i = 0; i < 7; i++) {
                        struct soinfo_common* compare = (struct soinfo_common*)(test + search[i]);
                        if (
                                ((compare->flags >= FLAG_LINKED) && (compare->flags <= (FLAG_LINKED | FLAG_EXE | FLAG_LINKER))) &&
                                (((uintptr_t)compare->strtab & 0xFFF00000) > 0) &&
                                (((uintptr_t)compare->strtab & 0xFFF00000) == ((uintptr_t)compare->symtab & 0xFFF00000)) &&
                                (compare->nbucket < 0xFFFF) &&
                                (compare->nchain < 0xFFFF)
                        ) {
                            HOOKLOG("soinfo 44: position:%d flags:0x%x", search[i], compare->flags);
                            soinfo = compare;
                            break;
                        }
                    }
                }
#endif

                if (!soinfo) {
                    HOOKLOG("[0x%lx] Error resolving soinfo %s", (long unsigned int)i->address, i->name.c_str());
                } else {
                    if (soinfo->version >= 2) {
                        HOOKLOG("[0x%lx::0x%x:%d (%d)] Hooking %s ...", (long unsigned int)i->address, soinfo->flags, soinfo->version, (int)soinfo->android_relocs_size_, i->name.c_str());
                    } else {
                        HOOKLOG("[0x%lx::0x%x:%d] Hooking %s ...", (long unsigned int)i->address, soinfo->flags, soinfo->version, i->name.c_str());
                    }

                    for (hooks_t::const_iterator j = hooks.begin(), je = hooks.end();
                            j != je; ++j) {
                        uintptr_t tmp = addhook(soinfo, j->name, j->hook);

                        // update the original pointer only if the reference we found is valid
                        // and the pointer itself doesn't have a value yet.
                        if (*(j->original) == 0 && tmp != 0) {
                            *(j->original) = tmp;

                            HOOKLOG("  %s - 0x%lx -> 0x%lx", j->name, (long unsigned int)tmp,
                                    (long unsigned int)j->hook);
                        }
                    }
                }
            }
        }
    }

    if (sdk > 0) {
        android_set_application_target_sdk_version(sdk);
        dlclose(libdl);
    }

    HOOKLOG("Done");
}
