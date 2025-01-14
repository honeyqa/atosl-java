/*
 *  Copyright (c) 2013, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "jni.h"
#include "jni_md.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#undef NDEBUG
#include <assert.h>
#include <arpa/inet.h>
#include <string.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>

#include "dwarf.h"
#include "libdwarf.h"

#include "fbatosl.h"
#include "subprograms.h"
#include "common.h"

#define VERSION 1

#define DWARF_ASSERT(ret, err) \
        do { \
                if (ret == DW_DLV_ERROR) { \
                        fatal("dwarf_errmsg: %s", dwarf_errmsg(err)); \
                } \
        } while (0);

extern char *
cplus_demangle (const char *mangled, int options);

typedef unsigned long Dwarf_Word;

Dwarf_Unsigned
_dwarf_decode_u_leb128(Dwarf_Small * leb128,
                       Dwarf_Word * leb128_length);
#define DECODE_LEB128_UWORD(ptr, value)               \
        do {                                              \
                Dwarf_Word uleblen;                           \
                value = _dwarf_decode_u_leb128(ptr,&uleblen); \
                ptr += uleblen;                               \
        } while (0)

static struct {
        const char *name;
        cpu_type_t type;
        cpu_subtype_t subtype;
} arch_str_to_type[] = {
        {"i386", CPU_TYPE_I386, CPU_SUBTYPE_X86_ALL},
        {"armv6",  CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V6},
        {"armv7",  CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7},
        {"armv7s", CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7S},
        {"arm64",  CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL}
};

struct symbol_t {
        const char *name;
        union {
                struct nlist_t sym32;
                struct nlist_64 sym64;
        } sym;
        Dwarf_Addr addr;
        int thumb : 1;
};

struct function_t {
        const char *name;
        Dwarf_Addr addr;
};

/* Various addresses, parsed from the cmdline or the mach-o sections */
typedef struct aopts {
        Dwarf_Addr load_address;
        int use_globals;
        int use_cache;
        const char *dsym_filename;
        cpu_type_t cpu_type;
        cpu_subtype_t cpu_subtype;
        const char *cache_dir;
        int should_demangle;
} Options;

typedef int dwarf_mach_handle;

struct dwarf_section_t;
struct dwarf_section_t {
        struct section_t mach_section;
        struct dwarf_section_t *next;
};

struct dwarf_section_64_t;
struct dwarf_section_64_t {
        struct section_64_t mach_section;
        struct dwarf_section_64_t *next;
};

typedef struct ctxt {
        /* Symbols from symtab */
        struct symbol_t *symlist;
        uint32_t nsymbols;
        struct dwarf_subprogram_t *subprograms;

        Dwarf_Addr intended_addr;
        Dwarf_Addr linkedit_addr;

        struct fat_arch_t arch;

        uint8_t uuid[UUID_LEN];
        char uuid_str[UUID_LEN * 2 + 1];

        uint8_t is_64;
        uint8_t is_dwarf;

        const char *last_fun_name;
        Dwarf_Addr last_addr;

        Dwarf_Arange *arange_buf;
        Dwarf_Signed count;
} Context;

typedef struct {
        dwarf_mach_handle handle;
        Dwarf_Small length_size;
        Dwarf_Small pointer_size;
        Dwarf_Endianness endianness;

        Dwarf_Unsigned section_count;
        struct dwarf_section_t *sections;
        struct dwarf_section_64_t *sections_64;
        Context *ictxt;
} dwarf_mach_object_access_internals_t;

void dwarf_error_handler(Dwarf_Error err, Dwarf_Ptr ptr)
{
        fatal("dwarf error: %s", dwarf_errmsg(err));
}

char *demangle(const char *sym)
{
        char *demangled = NULL;

        if (strncmp(sym, "_Z", 2) == 0)
                demangled = cplus_demangle(sym, 0);
        else if (strncmp(sym, "__Z", 3) == 0)
                demangled = cplus_demangle(sym+1, 0);

        return demangled;
}

int parse_uuid(dwarf_mach_object_access_internals_t *obj, uint32_t cmdsize, Context *context)
{
        int ret;

        ret = _read(obj->handle, context->uuid, UUID_LEN);
        if (ret < 0)
                fatal_file(ret);

        sprintf(context->uuid_str, "%.02x%.02x%.02x%.02x%.02x%.02x%.02x%.02x%.02x%.02x%.02x%.02x%.02x%.02x%.02x%.02x",
                context->uuid[0],context->uuid[1],context->uuid[2],context->uuid[3],context->uuid[4],context->uuid[5],
                context->uuid[6],context->uuid[7],context->uuid[8],context->uuid[9],context->uuid[10],context->uuid[11],
                context->uuid[12],context->uuid[13],context->uuid[14],context->uuid[15]);

        return 0;
}

int parse_section(dwarf_mach_object_access_internals_t *obj)
{
        int ret;
        struct dwarf_section_t *s;

        s = malloc(sizeof(*s));
        if (!s)
                fatal("unable to allocate memory");

        memset(s, 0, sizeof(*s));

        ret = _read(obj->handle, &s->mach_section, sizeof(s->mach_section));
        if (ret < 0)
                fatal_file(ret);

        struct dwarf_section_t *sec = obj->sections;
        if (!sec)
                obj->sections = s;
        else {
                while (sec) {
                        if (sec->next == NULL) {
                                sec->next = s;
                                break;
                        } else {
                                sec = sec->next;
                        }
                }
        }

        obj->section_count++;

        return 0;
}

int parse_section_64(dwarf_mach_object_access_internals_t *obj)
{
        int ret;
        struct dwarf_section_64_t *s;

        s = malloc(sizeof(*s));
        if (!s)
                fatal("unable to allocate memory");

        memset(s, 0, sizeof(*s));

        ret = _read(obj->handle, &s->mach_section, sizeof(s->mach_section));
        if (ret < 0)
                fatal_file(ret);

        struct dwarf_section_64_t *sec = obj->sections_64;

        if (!sec) {
                obj->sections_64 = s;
        } else {
                while (sec) {
                        if (sec->next == NULL) {
                                sec->next = s;
                                break;
                        } else {
                                sec = sec->next;
                        }
                }
        }

        obj->section_count++;

        return 0;
}

int parse_segment(dwarf_mach_object_access_internals_t *obj, uint32_t cmdsize, Context *context)
{
        int err;
        int ret;
        struct segment_command_t segment;
        int i;

        ret = _read(obj->handle, &segment, sizeof(segment));
        if (ret < 0)
                fatal_file(ret);

        if (strcmp(segment.segname, "__TEXT") == 0) {
                context->intended_addr = segment.vmaddr;
        }

        if (strcmp(segment.segname, "__LINKEDIT") == 0) {
                context->linkedit_addr = segment.fileoff;
        }

        if (strcmp(segment.segname, "__DWARF") == 0) {
                context->is_dwarf = 1;
        }

        for (i = 0; i < segment.nsects; i++) {
                err = parse_section(obj);
                if (err)
                        fatal("unable to parse section in `%s`", segment.segname);
        }

        return 0;
}

int parse_segment_64(dwarf_mach_object_access_internals_t *obj, uint32_t cmdsize, Context *context)
{
        int err;
        int ret;
        struct segment_command_64_t segment;
        int i;

        ret = _read(obj->handle, &segment, sizeof(segment));
        if (ret < 0)
                fatal_file(ret);

        if (strcmp(segment.segname, "__TEXT") == 0) {
                context->intended_addr = segment.vmaddr;
        }

        if (strcmp(segment.segname, "__LINKEDIT") == 0) {
                context->linkedit_addr = segment.fileoff;
        }

        if (strcmp(segment.segname, "__DWARF") == 0) {
                context->is_dwarf = 1;
        }

        for (i = 0; i < segment.nsects; i++) {
                err = parse_section_64(obj);
                if (err)
                        fatal("unable to parse section in `%s`", segment.segname);
        }

        return 0;
}

int parse_symtab(dwarf_mach_object_access_internals_t *obj, uint32_t cmdsize, Context *context)
{
        int ret;
        off_t pos;
        int i;
        char *strtable;

        struct symtab_command_t symtab;
        struct symbol_t *current;

        ret = _read(obj->handle, &symtab, sizeof(symtab));
        if (ret < 0)
                fatal_file(ret);

        strtable = malloc(symtab.strsize);
        if (!strtable)
                fatal("unable to allocate memory");

        pos = lseek(obj->handle, 0, SEEK_CUR);
        if (pos < 0)
                fatal("error seeking: %s", strerror(errno));

        ret = lseek(obj->handle, context->arch.offset+symtab.stroff, SEEK_SET);
        if (ret < 0)
                fatal("error seeking: %s", strerror(errno));

        ret = _read(obj->handle, strtable, symtab.strsize);
        if (ret < 0)
                fatal_file(ret);

        ret = lseek(obj->handle, context->arch.offset+symtab.symoff, SEEK_SET);
        if (ret < 0)
                fatal("error seeking: %s", strerror(errno));

        context->nsymbols = symtab.nsyms;
        context->symlist = malloc(sizeof(struct symbol_t) * symtab.nsyms);
        if (!context->symlist)
                fatal("unable to allocate memory");
        current = context->symlist;

        for (i = 0; i < symtab.nsyms; i++) {
                ret = _read(obj->handle, context->is_64 ? (void*)&current->sym.sym64 : (void*)&current->sym.sym32, context->is_64 ? sizeof(current->sym.sym64) : sizeof(current->sym.sym32));
                if (ret < 0)
                        fatal_file(ret);

                if (context->is_64 ? current->sym.sym64.n_un.n_strx : current->sym.sym32.n_un.n_strx) {
                        if ((context->is_64 ? current->sym.sym64.n_un.n_strx : current->sym.sym32.n_un.n_strx) > symtab.strsize)
                                fatal("str offset (%d) greater than strsize (%d)",
                                      (context->is_64 ? current->sym.sym64.n_un.n_strx : current->sym.sym32.n_un.n_strx), symtab.strsize);
                        current->name = strtable+(context->is_64 ? current->sym.sym64.n_un.n_strx : current->sym.sym32.n_un.n_strx);
                }

                current++;
        }
        ret = lseek(obj->handle, pos, SEEK_SET);
        if (ret < 0)
                fatal("error seeking: %s", strerror(errno));
        return 0;
}

int compare_symbols(const void *a, const void *b)
{
        struct symbol_t *sym_a = (struct symbol_t *)a;
        struct symbol_t *sym_b = (struct symbol_t *)b;
        return sym_a->addr - sym_b->addr;
}

void print_symbol(const char *symbol, unsigned offset, char *stacktrace, Options *options)
{
        char *demangled = options->should_demangle ? demangle(symbol) : NULL;
        const char *name = demangled ? demangled : symbol;
        if (name[0] == '_')
                name++;

        sprintf(stacktrace, "%s%s (in %s) + %d",
                name,
                demangled ? "()" : "",
                basename((char *)options->dsym_filename),
                offset);
        if (demangled)
                free(demangled);
}

/* Print symbol name based on stabs information.
 * Currently only handles functions (N_FUN stabs)
 *
 * See README.stabs for stabs format information.
 *
 * Here we find pairs of N_FUN stabs. The first has the name of the function and its starting address;
 * the second has its size.
 *
 * We could also symbolicate global and static symbols (N_GSYM and N_STSYM) here,
 * but it's not necessary to do so since they'll be picked up by the generic symbol table
 * search later in this function.
 *
 * Return 1 if a symbol corresponding to search_addr was found; 0 otherwise.
 */
int handle_stabs_symbol(int is_fun_stab, Dwarf_Addr search_addr, const struct symbol_t *symbol, char *stacktrace, Options *options, Context *context)
{
        /* These are static since they need to persist across pairs of symbols. */

        if (is_fun_stab) {
                if (context->last_fun_name) { /* if this is non-null, the last symbol was an N_FUN stab as well. */
                        if (context->last_addr <= search_addr
                            && search_addr < context->last_addr + symbol->addr) {
                                print_symbol(context->last_fun_name, (unsigned int)(search_addr - context->last_addr), stacktrace, options);
                                return 1;
                        }
                        context->last_fun_name = NULL;
                } else { /* last_fun_name is null, so this is the first N_FUN in (possibly) a pair. */
                        context->last_fun_name = symbol->name;
                }
        } else {
                context->last_fun_name = NULL;
        }
        context->last_addr = symbol->addr;
        return 0;
}

int find_and_print_symtab_symbol(Dwarf_Addr slide, Dwarf_Addr addr, char *stacktrace, Options *options, Context *context)
{
        union {
                struct nlist_t nlist32;
                struct nlist_64 nlist64;
        } nlist;
        struct symbol_t *current;
        int found = 0;

        int i;
        int is_stab;
        uint8_t type;

        addr = addr - slide;
        current = context->symlist;

        for (i = 0; i < context->nsymbols; i++) {

                memcpy(context->is_64 ? (void*)&nlist.nlist64 : (void*)&nlist.nlist32, context->is_64 ? (void*)&current->sym.sym64 : (void*)&current->sym.sym32, context->is_64 ? sizeof(current->sym.sym64) : sizeof(current->sym.sym32));
                current->thumb = ((context->is_64 ? nlist.nlist64.n_desc : nlist.nlist32.n_desc) & N_ARM_THUMB_DEF) ? 1 : 0;

                current->addr = context->is_64 ? nlist.nlist64.n_value : nlist.nlist32.n_value;
                type = context->is_64 ? nlist.nlist64.n_type : nlist.nlist32.n_type;
                is_stab = type & N_STAB;

                if (handle_stabs_symbol(is_stab && type == N_FUN, addr, current, stacktrace, options, context))
                        return DW_DLV_OK;

                current++;

        }

        qsort(context->symlist, context->nsymbols, sizeof(*current), compare_symbols);
        current = context->symlist;

        for (i = 0; i < context->nsymbols; i++) {
                if (current->addr > addr) {
                        if (i < 1) {
                                /* Someone is asking about a symbol that comes before the first
                                 * one we know about. In that case we don't have a match for
                                 * them */
                                break;
                        }

                        struct symbol_t *prev = (current - 1);
                        print_symbol(prev->name, (unsigned int)(addr - prev->addr), stacktrace, options);
                        found = 1;
                        break;
                }
                current++;
        }

        return found ? DW_DLV_OK : DW_DLV_NO_ENTRY;
}

int parse_command(
        dwarf_mach_object_access_internals_t *obj,
        struct load_command_t load_command, Context *context)
{
        int ret = 0;
        int cmdsize;

        switch (load_command.cmd) {
        case LC_UUID:
                ret = parse_uuid(obj, load_command.cmdsize, context);
                break;
        case LC_SEGMENT:
                ret = parse_segment(obj, load_command.cmdsize, context);
                break;
        case LC_SEGMENT_64:
                ret = parse_segment_64(obj, load_command.cmdsize, context);
                break;
        case LC_SYMTAB:
                ret = parse_symtab(obj, load_command.cmdsize, context);
                break;
        /* Fallthrough */
        case LC_PREPAGE:
                cmdsize = load_command.cmdsize - sizeof(load_command);
                ret = lseek(obj->handle, cmdsize, SEEK_CUR);
                if (ret < 0)
                        fatal("error seeking: %s", strerror(errno));
                break;
        }

        return ret;
}

int dwarf_mach_object_access_internals_init(
        dwarf_mach_handle handle,
        void *obj_in,
        int *error,
        Context *context)
{
        int ret;
        struct mach_header_t header;
        struct load_command_t load_command;
        int i;

        dwarf_mach_object_access_internals_t *obj =
                (dwarf_mach_object_access_internals_t *)obj_in;

        obj->handle = handle;
        obj->length_size = 4;
        obj->pointer_size = 4;
        obj->endianness = DW_OBJECT_LSB;
        obj->sections = NULL;
        obj->sections_64 = NULL;
        obj->ictxt = context;

        ret = _read(obj->handle, &header, sizeof(header));
        if (ret < 0)
                fatal_file(ret);

        /* Need to skip 4 bytes of the reserved field of mach_header_64  */
        if (header.cputype == CPU_TYPE_ARM64 && header.cpusubtype == CPU_SUBTYPE_ARM64_ALL) {
                context->is_64 = 1;
                ret = lseek(obj->handle, sizeof(uint32_t), SEEK_CUR);
                if (ret < 0)
                        fatal_file(ret);
        }

        switch (header.filetype) {
        case MH_DSYM: // debug file
                break;
        case MH_DYLIB: // dynamic library
                break;
        case MH_DYLIB_STUB: // dynamic library stub
                break;
        case MH_EXECUTE: // executable file
                break;
        default:
                fatal("unsupported file type: 0x%x", header.filetype);
                assert(0);
        }

        for (i = 0; i < header.ncmds; i++) {
                ret = _read(obj->handle, &load_command, sizeof(load_command));
                if (ret < 0)
                        fatal_file(ret);

                ret = parse_command(obj, load_command, context);
                if (ret < 0)
                        fatal("unable to parse command %x", load_command.cmd);
        }

        return DW_DLV_OK;
}

Dwarf_Endianness dwarf_mach_object_access_get_byte_order(void *obj_in)
{
        dwarf_mach_object_access_internals_t *obj =
                (dwarf_mach_object_access_internals_t *)obj_in;
        return obj->endianness;
}

Dwarf_Unsigned dwarf_mach_object_access_get_section_count(void *obj_in)
{
        dwarf_mach_object_access_internals_t *obj =
                (dwarf_mach_object_access_internals_t *)obj_in;
        return obj->section_count;
}

int dwarf_mach_object_access_get_section_info(
        void *obj_in,
        Dwarf_Half section_index,
        Dwarf_Obj_Access_Section *ret_scn,
        int *error)
{
        int i;
        dwarf_mach_object_access_internals_t *obj =
                (dwarf_mach_object_access_internals_t *)obj_in;

        if (section_index >= obj->section_count) {
                *error = DW_DLE_MDE;
                return DW_DLV_ERROR;
        }

        if (obj->sections_64) {
                struct dwarf_section_64_t *sec = obj->sections_64;
                for (i = 0; i < section_index; i++) {
                        sec = sec->next;
                }
                sec->mach_section.sectname[1] = '.';
                ret_scn->size = sec->mach_section.size;
                ret_scn->addr = sec->mach_section.addr;
                ret_scn->name = sec->mach_section.sectname+1;
        } else {
                struct dwarf_section_t *sec = obj->sections;
                for (i = 0; i < section_index; i++) {
                        sec = sec->next;
                }
                sec->mach_section.sectname[1] = '.';
                ret_scn->size = sec->mach_section.size;
                ret_scn->addr = sec->mach_section.addr;
                ret_scn->name = sec->mach_section.sectname+1;
        }
        if (strcmp(ret_scn->name, ".debug_pubnames__DWARF") == 0)
                ret_scn->name = ".debug_pubnames";

        ret_scn->link = 0; /* rela section or from symtab to strtab */
        ret_scn->entrysize = 0;

        return DW_DLV_OK;
}

int dwarf_mach_object_access_load_section(
        void *obj_in,
        Dwarf_Half section_index,
        Dwarf_Small **section_data,
        int *error)
{
        void *addr;
        int i;
        int ret;

        dwarf_mach_object_access_internals_t *obj =
                (dwarf_mach_object_access_internals_t *)obj_in;

        if (section_index >= obj->section_count) {
                *error = DW_DLE_MDE;
                return DW_DLV_ERROR;
        }

        if (obj->sections_64) {
                struct dwarf_section_64_t *sec = obj->sections_64;
                for (i = 0; i < section_index; i++) {
                        sec = sec->next;
                }
                addr = malloc(sec->mach_section.size);
                if (!addr)
                        fatal("unable to allocate memory");
                ret = lseek(obj->handle, obj->ictxt->arch.offset + sec->mach_section.offset, SEEK_SET);
                if (ret < 0)
                        fatal("error seeking: %s", strerror(errno));
                ret = _read(obj->handle, addr, sec->mach_section.size);
                if (ret < 0)
                        fatal_file(ret);

        } else {
                struct dwarf_section_t *sec = obj->sections;
                for (i = 0; i < section_index; i++) {
                        sec = sec->next;
                }
                addr = malloc(sec->mach_section.size);
                if (!addr)
                        fatal("unable to allocate memory");
                ret = lseek(obj->handle, obj->ictxt->arch.offset + sec->mach_section.offset, SEEK_SET);
                if (ret < 0)
                        fatal("error seeking: %s", strerror(errno));
                ret = _read(obj->handle, addr, sec->mach_section.size);
                if (ret < 0)
                        fatal_file(ret);

        }
        *section_data = addr;

        return DW_DLV_OK;
}

int dwarf_mach_object_relocate_a_section(
        void *obj_in,
        Dwarf_Half section_index,
        Dwarf_Debug dbg,
        int *error)
{
        return DW_DLV_NO_ENTRY;
}

Dwarf_Small dwarf_mach_object_access_get_length_size(void *obj_in)
{
        dwarf_mach_object_access_internals_t *obj =
                (dwarf_mach_object_access_internals_t *)obj_in;
        return obj->length_size;
}

Dwarf_Small dwarf_mach_object_access_get_pointer_size(void *obj_in)
{
        dwarf_mach_object_access_internals_t *obj =
                (dwarf_mach_object_access_internals_t *)obj_in;
        return obj->pointer_size;
}

const struct Dwarf_Obj_Access_Methods_s
        dwarf_mach_object_access_methods = {
        dwarf_mach_object_access_get_section_info,
        dwarf_mach_object_access_get_byte_order,
        dwarf_mach_object_access_get_length_size,
        dwarf_mach_object_access_get_pointer_size,
        dwarf_mach_object_access_get_section_count,
        dwarf_mach_object_access_load_section,
        dwarf_mach_object_relocate_a_section
};

void dwarf_mach_object_access_init(
        dwarf_mach_handle handle,
        Dwarf_Obj_Access_Interface **ret_obj,
        int *err,
        Context *context)
{
        int res = 0;
        dwarf_mach_object_access_internals_t *internals = NULL;
        Dwarf_Obj_Access_Interface *intfc = NULL;

        internals = malloc(sizeof(*internals));
        if (!internals)
                fatal("unable to allocate memory");

        memset(internals, 0, sizeof(*internals));
        res = dwarf_mach_object_access_internals_init(handle, internals, err, context);
        if (res != DW_DLV_OK)
                fatal("error initializing dwarf internals");

        intfc = malloc(sizeof(Dwarf_Obj_Access_Interface));
        if (!intfc)
                fatal("unable to allocate memory");

        intfc->object = internals;
        intfc->methods = &dwarf_mach_object_access_methods;

        *ret_obj = intfc;
}

void dwarf_mach_object_access_finish(Dwarf_Obj_Access_Interface *obj)
{
        if (!obj)
                return;

        if (obj->object)
                free(obj->object);
        free(obj);
}

struct dwarf_subprogram_t *lookup_symbol(Dwarf_Addr addr, Context *context)
{
        struct dwarf_subprogram_t *subprogram = context->subprograms;

        while (subprogram) {
                if ((addr >= subprogram->lowpc) &&
                    (addr < subprogram->highpc)) {
                        return subprogram;
                }

                subprogram = subprogram->next;
        }

        return NULL;
}

int print_subprogram_symbol(Dwarf_Addr slide, Dwarf_Addr addr, char *stacktrace, Options *options, Context *context)
{
        char *demangled = NULL;

        addr -= slide;

        struct dwarf_subprogram_t *match = lookup_symbol(addr, context);

        if (match) {
                demangled = options->should_demangle ? demangle(match->name) : NULL;
                sprintf(stacktrace, "%s (in %s) + %d",
                        demangled ? : match->name,
                        basename((char *)options->dsym_filename),
                        (unsigned int)(addr - match->lowpc));
                if (demangled)
                        free(demangled);

        }

        return match ? 0 : -1;
}

int print_dwarf_symbol(Dwarf_Debug dbg, Dwarf_Addr slide, Dwarf_Addr addr, char *stacktrace, Options *options, Context *context)
{
        // static Dwarf_Arange *arange_buf = NULL;
        Dwarf_Line *linebuf = NULL;
        Dwarf_Signed linecount = 0;
        Dwarf_Off cu_die_offset = 0;
        Dwarf_Die cu_die = NULL;
        Dwarf_Unsigned segment = 0;
        Dwarf_Unsigned segment_entry_size = 0;
        Dwarf_Addr start = 0;
        Dwarf_Unsigned length = 0;
        Dwarf_Arange arange;
        // static Dwarf_Signed count;
        int ret;
        Dwarf_Error err;
        int i;
        int found = 0;

        addr -= slide;

        if (!context->arange_buf) {
                // &arange_buf &count
                ret = dwarf_get_aranges(dbg, &context->arange_buf, &context->count, &err);
                DWARF_ASSERT(ret, err);
        }

        ret = dwarf_get_arange(context->arange_buf, context->count, addr, &arange, &err);
        DWARF_ASSERT(ret, err);

        if (ret == DW_DLV_NO_ENTRY)
                return ret;

        ret = dwarf_get_arange_info_b(
                arange,
                &segment,
                &segment_entry_size,
                &start,
                &length,
                &cu_die_offset,
                &err);
        DWARF_ASSERT(ret, err);

        ret = dwarf_offdie(dbg, cu_die_offset, &cu_die, &err);
        DWARF_ASSERT(ret, err);

        /* ret = dwarf_print_lines(cu_die, &err, &errcnt); */
        /* DWARF_ASSERT(ret, err); */

        ret = dwarf_srclines(cu_die, &linebuf, &linecount, &err);
        DWARF_ASSERT(ret, err);

        for (i = 0; i < linecount; i++) {
                Dwarf_Line prevline;
                Dwarf_Line nextline;
                Dwarf_Line line = linebuf[i];

                Dwarf_Addr lineaddr;
                Dwarf_Addr lowaddr;
                Dwarf_Addr highaddr;

                ret = dwarf_lineaddr(line, &lineaddr, &err);
                DWARF_ASSERT(ret, err);

                if (i > 0) {
                        prevline = linebuf[i-1];
                        ret = dwarf_lineaddr(prevline, &lowaddr, &err);
                        DWARF_ASSERT(ret, err);
                        lowaddr += 1;
                } else {
                        lowaddr = lineaddr;
                }

                if (i < linecount - 1) {
                        nextline = linebuf[i+1];
                        ret = dwarf_lineaddr(nextline, &highaddr, &err);
                        DWARF_ASSERT(ret, err);
                        highaddr -= 1;
                } else {
                        highaddr = lineaddr;
                }

                if ((addr >= lowaddr) && (addr <= highaddr)) {
                        char *filename;
                        Dwarf_Unsigned lineno;
                        char *diename;
                        char *demangled;
                        struct dwarf_subprogram_t *symbol;
                        const char *name;

                        ret = dwarf_linesrc(line, &filename, &err);
                        DWARF_ASSERT(ret, err);

                        ret = dwarf_lineno(line, &lineno, &err);
                        DWARF_ASSERT(ret, err);

                        ret = dwarf_diename(cu_die, &diename, &err);
                        DWARF_ASSERT(ret, err);

                        symbol = lookup_symbol(addr, context);

                        name = symbol ? symbol->name : "(unknown)";

                        demangled = options->should_demangle ? demangle(name) : NULL;
                        sprintf(stacktrace, "%s (in %s) (%s:%d)",
                                demangled ? demangled : name,
                                basename((char *)options->dsym_filename),
                                basename(filename), (int)lineno);
                        found = 1;

                        if (demangled)
                                free(demangled);

                        dwarf_dealloc(dbg, diename, DW_DLA_STRING);
                        dwarf_dealloc(dbg, filename, DW_DLA_STRING);

                        break;
                }
        }

        dwarf_dealloc(dbg, arange, DW_DLA_ARANGE);
        dwarf_srclines_dealloc(dbg, linebuf, linecount);

        return found ? DW_DLV_OK : DW_DLV_NO_ENTRY;
}

JNIEXPORT jobjectArray JNICALL Java_io_honeyqa_atosl_Atosl_findArch
        (JNIEnv * env, jobject jobj, jstring dSYM){
        int i, j, k;
        int fd;
        int ret;
        uint32_t magic;

        Context *context;
        context = malloc(sizeof(*context));
        // Context initialize
        context->is_64 = 0;
        context->last_fun_name = NULL;
        context->arange_buf = NULL;

        Options *options;
        options = malloc(sizeof(*options));
        // Option initialize
        options->load_address = LONG_MAX;
        options->use_globals = 0;
        options->use_cache = 0;
        options->cpu_type = CPU_TYPE_ARM;
        options->cpu_subtype = CPU_SUBTYPE_ARM_V7S;
        options->should_demangle = 1;

        const char *n_dSYM = (*env)->GetStringUTFChars(env, dSYM, NULL);
        jobjectArray resultArr = (jobjectArray)(*env)->NewObjectArray(env, 7,
                                                                      (*env)->FindClass(env, "java/lang/String"),
                                                                      (*env)->NewStringUTF(env, ""));
        fd = open(n_dSYM, O_RDONLY);
        if (fd < 0) {
                // Release
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // unable to open file
                (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-2"));
                free(context);
                free(options);
                return resultArr;
        }
        ret = _read(fd, &magic, sizeof(magic));
        if (ret < 0) {
                // Release
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // unable to read file
                (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-3"));
                close(fd);
                free(context);
                free(options);
                return resultArr;
        }
        if (magic == FAT_CIGAM) {
                /* Find the architecture we want.. */
                uint32_t nfat_arch;

                ret = _read(fd, &nfat_arch, sizeof(nfat_arch));
                if (ret < 0) {
                        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                        // unsupported architecture
                        (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-1"));
                        close(fd);
                        free(context);
                        free(options);
                        return resultArr;
                }

                nfat_arch = ntohl(nfat_arch);

                if (nfat_arch == 0) {
                        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                        // unable to read fat arch
                        (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-9"));
                        close(fd);
                        free(context);
                        free(options);
                        return resultArr;
                }
                k = 2;
                for (i = 0; i < nfat_arch; i++) {
                        ret = _read(fd, &context->arch, sizeof(context->arch));
                        if (ret < 0) {
                                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                                // unable to read architecture
                                (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-4"));
                                close(fd);
                                free(context);
                                free(options);
                                return resultArr;
                        }

                        int cpu_type = ntohl(context->arch.cputype);
                        int cpu_subtype = ntohl(context->arch.cpusubtype);
                        for (j = 0; j < NUMOF(arch_str_to_type); j++) {
                                if((arch_str_to_type[j].type == cpu_type) &&
                                   arch_str_to_type[j].subtype == cpu_subtype) {
                                        (*env)->SetObjectArrayElement(env,
                                                                      resultArr,
                                                                      k++,
                                                                      (*env)->NewStringUTF(env, arch_str_to_type[j].name));
                                }
                        }
                }
                char* cnt;
                cnt = malloc(2 * sizeof(char));
                sprintf(cnt, "%d", k-2);
                (*env)->SetObjectArrayElement(env,
                                              resultArr,
                                              1,
                                              (*env)->NewStringUTF(env, cnt));
                free(cnt);
        } else {
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // unable to seek
                (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-5"));
                close(fd);
                free(context);
                free(options);
                return resultArr;
        }
        (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "0"));
        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
        close(fd);
        free(context);
        free(options);
        return resultArr;
}

JNIEXPORT jobjectArray JNICALL Java_io_honeyqa_atosl_Atosl_checkUUID
        (JNIEnv * env, jobject jobj, jstring arch, jstring dSYM){
        jobjectArray resultArr = (jobjectArray)(*env)->NewObjectArray(env, 2,
                                                                      (*env)->FindClass(env, "java/lang/String"),
                                                                      (*env)->NewStringUTF(env, ""));
        int fd;
        int ret;
        int i = 0;
        int found = 0;
        uint32_t magic;
        cpu_type_t cpu_type = -1;
        cpu_subtype_t cpu_subtype = -1;
        int derr = 0;
        Dwarf_Obj_Access_Interface *binary_interface = NULL;

        Context *context;
        context = malloc(sizeof(*context));
        // Context initialize
        context->is_64 = 0;
        context->last_fun_name = NULL;
        context->arange_buf = NULL;

        Options *options;
        options = malloc(sizeof(*options));
        // Option initialize
        options->load_address = LONG_MAX;
        options->use_globals = 0;
        options->use_cache = 0;
        options->cpu_type = CPU_TYPE_ARM;
        options->cpu_subtype = CPU_SUBTYPE_ARM_V7S;
        options->should_demangle = 1;

        // load from java
        const char *n_arch = (*env)->GetStringUTFChars(env, arch, NULL);
        const char *n_dSYM = (*env)->GetStringUTFChars(env, dSYM, NULL);

        // check CPU architecture
        for (i = 0; i < NUMOF(arch_str_to_type); i++) {
                if (strcmp(arch_str_to_type[i].name, n_arch) == 0) {
                        cpu_type = arch_str_to_type[i].type;
                        cpu_subtype = arch_str_to_type[i].subtype;
                        break;
                }
        }
        if ((cpu_type < 0) && (cpu_subtype < 0)) {
                // Release
                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // unsupported architecture
                (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-1"));
                free(context);
                free(options);
                return resultArr;
        }

        options->cpu_type = cpu_type;
        options->cpu_subtype = cpu_subtype;

        // filename
        options->dsym_filename = n_dSYM;

        fd = open(n_dSYM, O_RDONLY);
        if (fd < 0) {
                // Release
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // unable to open file
                (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-2"));
                free(context);
                free(options);
                return resultArr;
        }
        ret = _read(fd, &magic, sizeof(magic));
        if (ret < 0) {
                // Release
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // unable to read file
                (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-3"));
                close(fd);
                free(context);
                free(options);
                return resultArr;
        }
        if (magic == FAT_CIGAM) {
                /* Find the architecture we want.. */
                uint32_t nfat_arch;

                ret = _read(fd, &nfat_arch, sizeof(nfat_arch));
                if (ret < 0) {
                        // Release
                        (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                        // unable to read file
                        (*env)->SetObjectArrayElement(env, resultArr, 0,
                                                      (*env)->NewStringUTF(env, "-3"));
                        close(fd);
                        free(context);
                        free(options);
                        return resultArr;
                }
                nfat_arch = ntohl(nfat_arch);
                for (i = 0; i < nfat_arch; i++) {
                        ret = _read(fd, &context->arch, sizeof(context->arch));
                        if (ret < 0) {
                                // Release
                                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                                // unable to read architecture
                                (*env)->SetObjectArrayElement(env, resultArr, 0,
                                                              (*env)->NewStringUTF(env, "-4"));
                                close(fd);
                                free(context);
                                free(options);
                                return resultArr;
                        }

                        context->arch.cputype = ntohl(context->arch.cputype);
                        context->arch.cpusubtype = ntohl(context->arch.cpusubtype);
                        context->arch.offset = ntohl(context->arch.offset);
                        if ((context->arch.cputype == options->cpu_type) &&
                            (context->arch.cpusubtype == options->cpu_subtype)) {
                                /* good! */
                                ret = lseek(fd, context->arch.offset, SEEK_SET);
                                if (ret < 0) {
                                        // Release
                                        (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                                        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                                        // unable to seek architecture
                                        (*env)->SetObjectArrayElement(env, resultArr, 0,
                                                                      (*env)->NewStringUTF(env, "-5"));
                                        close(fd);
                                        free(context);
                                        free(options);
                                        return resultArr;
                                }
                                ret = _read(fd, &magic, sizeof(magic));
                                if (ret < 0) {
                                        // Release
                                        (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                                        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                                        // unable to read file
                                        (*env)->SetObjectArrayElement(env, resultArr, 0,
                                                                      (*env)->NewStringUTF(env, "-3"));
                                        close(fd);
                                        free(context);
                                        free(options);
                                        return resultArr;
                                }
                                found = 1;
                                break;
                        }
                }
        } else {
                found = 1;
        }
        if (!found) {
                // Release
                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // architecture not found
                (*env)->SetObjectArrayElement(env, resultArr, 0,
                                              (*env)->NewStringUTF(env, "-6"));
                close(fd);
                free(context);
                free(options);
                return resultArr;
        }
        if (magic != MH_MAGIC && magic != MH_MAGIC_64) {
                // Release
                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // invalid magic for architecture
                (*env)->SetObjectArrayElement(env, resultArr, 0,
                                              (*env)->NewStringUTF(env, "-7"));
                close(fd);
                free(context);
                free(options);
                return resultArr;
        }
        dwarf_mach_object_access_init(fd, &binary_interface, &derr, context);
        assert(binary_interface);
        // Release
        (*env)->ReleaseStringUTFChars(env, arch, n_arch);
        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
        (*env)->SetObjectArrayElement(env, resultArr, 0,
                                      (*env)->NewStringUTF(env, "0"));
        (*env)->SetObjectArrayElement(env, resultArr, 1,
                                      (*env)->NewStringUTF(env, context->uuid_str));
        close(fd);
        free(context);
        free(options);
        return resultArr;
}

JNIEXPORT jobjectArray JNICALL Java_io_honeyqa_atosl_Atosl_symbolicate
        (JNIEnv * env, jobject jobj, jstring arch, jstring dSYM, jobjectArray adr, jint adrlen){
        jobjectArray resultArr = (jobjectArray)(*env)->NewObjectArray(env, adrlen,
                                                                      (*env)->FindClass(env, "java/lang/String"),
                                                                      (*env)->NewStringUTF(env, ""));
        int fd;
        int ret;
        int i; // for
        Dwarf_Debug dbg = NULL;
        Dwarf_Error err;
        int derr = 0;
        Dwarf_Obj_Access_Interface *binary_interface = NULL;
        Dwarf_Ptr errarg = NULL;
        int found = 0;
        uint32_t magic;
        cpu_type_t cpu_type = -1;
        cpu_subtype_t cpu_subtype = -1;
        Dwarf_Addr address;

        Context *context;
        context = malloc(sizeof(*context));
        // Context initialize
        context->is_64 = 0;
        context->last_fun_name = NULL;
        context->arange_buf = NULL;

        Options *options;
        options = malloc(sizeof(*options));
        // Option initialize
        options->load_address = LONG_MAX;
        options->use_globals = 0;
        options->use_cache = 0;
        options->cpu_type = CPU_TYPE_ARM;
        options->cpu_subtype = CPU_SUBTYPE_ARM_V7S;
        options->should_demangle = 1;

        // load from java
        const char *n_arch = (*env)->GetStringUTFChars(env, arch, NULL);
        const char *n_dSYM = (*env)->GetStringUTFChars(env, dSYM, NULL);

        // check CPU architecture
        for (i = 0; i < NUMOF(arch_str_to_type); i++) {
                if (strcmp(arch_str_to_type[i].name, n_arch) == 0) {
                        cpu_type = arch_str_to_type[i].type;
                        cpu_subtype = arch_str_to_type[i].subtype;
                        break;
                }
        }
        if ((cpu_type < 0) && (cpu_subtype < 0)) {
                // Release
                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // unsupported architecture
                (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-1"));
                free(context);
                free(options);
                return resultArr;
        }
        options->cpu_type = cpu_type;
        options->cpu_subtype = cpu_subtype;

        // filename
        options->dsym_filename = n_dSYM;

        fd = open(options->dsym_filename, O_RDONLY);
        if (fd < 0) {
                // Release
                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // unable to open file
                (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-2"));
                free(context);
                free(options);
                return resultArr;
        }
        ret = _read(fd, &magic, sizeof(magic));
        if (ret < 0) {
                // Release
                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // unable to read file
                (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "-3"));
                close(fd);
                free(context);
                free(options);
                return resultArr;
        }
        if (magic == FAT_CIGAM) {
                /* Find the architecture we want.. */
                uint32_t nfat_arch;

                ret = _read(fd, &nfat_arch, sizeof(nfat_arch));
                if (ret < 0) {
                        // Release
                        (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                        // unable to read file
                        (*env)->SetObjectArrayElement(env, resultArr, 0,
                                                      (*env)->NewStringUTF(env, "-3"));
                        close(fd);
                        free(context);
                        free(options);
                        return resultArr;
                }
                nfat_arch = ntohl(nfat_arch);
                for (i = 0; i < nfat_arch; i++) {
                        ret = _read(fd, &context->arch, sizeof(context->arch));
                        if (ret < 0) {
                                // Release
                                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                                // unable to read architecture
                                (*env)->SetObjectArrayElement(env, resultArr, 0,
                                                              (*env)->NewStringUTF(env, "-4"));
                                close(fd);
                                free(context);
                                free(options);
                                return resultArr;
                        }

                        context->arch.cputype = ntohl(context->arch.cputype);
                        context->arch.cpusubtype = ntohl(context->arch.cpusubtype);
                        context->arch.offset = ntohl(context->arch.offset);
                        if ((context->arch.cputype == options->cpu_type) &&
                            (context->arch.cpusubtype == options->cpu_subtype)) {
                                /* good! */
                                ret = lseek(fd, context->arch.offset, SEEK_SET);
                                if (ret < 0) {
                                        // Release
                                        (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                                        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                                        // unable to seek architecture
                                        (*env)->SetObjectArrayElement(env, resultArr, 0,
                                                                      (*env)->NewStringUTF(env, "-5"));
                                        close(fd);
                                        free(context);
                                        free(options);
                                        return resultArr;
                                }
                                ret = _read(fd, &magic, sizeof(magic));
                                if (ret < 0) {
                                        // Release
                                        (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                                        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                                        // unable to read file
                                        (*env)->SetObjectArrayElement(env, resultArr, 0,
                                                                      (*env)->NewStringUTF(env, "-3"));
                                        close(fd);
                                        free(context);
                                        free(options);
                                        return resultArr;
                                }
                                found = 1;
                                break;
                        }
                }
        } else {
                found = 1;
        }
        if (!found) {
                // Release
                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // architecture not found
                (*env)->SetObjectArrayElement(env, resultArr, 0,
                                              (*env)->NewStringUTF(env, "-6"));
                close(fd);
                free(context);
                free(options);
                return resultArr;
        }
        if (magic != MH_MAGIC && magic != MH_MAGIC_64) {
                // Release
                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                // invalid magic for architecture
                (*env)->SetObjectArrayElement(env, resultArr, 0,
                                              (*env)->NewStringUTF(env, "-7"));
                close(fd);
                free(context);
                free(options);
                return resultArr;
        }
        dwarf_mach_object_access_init(fd, &binary_interface, &derr, context);
        assert(binary_interface);

        if (options->load_address == LONG_MAX)
                options->load_address = context->intended_addr;
        // load-address
        jstring lala = (jstring) (*env)->GetObjectArrayElement(env, adr, 0);
        const char *lalachar = (*env)->GetStringUTFChars(env, lala, 0);
        address = strtol(lalachar, (char **)NULL, 16);
        options->load_address = address;
        ret = dwarf_object_init(binary_interface,
                                dwarf_error_handler,
                                errarg, &dbg, &err);
        DWARF_ASSERT(ret, err);
        /* If there is dwarf info we'll use that to parse, otherwise we'll use the
         * symbol table */
        if (context->is_dwarf && ret == DW_DLV_OK) {

                struct subprograms_options_t opts = {
                        .persistent = options->use_cache,
                        .cache_dir = options->cache_dir,
                };
                context->subprograms =
                        subprograms_load(dbg,
                                         context->uuid,
                                         options->use_globals ? SUBPROGRAMS_GLOBALS :
                                         SUBPROGRAMS_CUS,
                                         &opts);
                for (i = 1; i < adrlen; i++) {
                        jstring arr_e = (jstring) (*env)->GetObjectArrayElement(env, adr, i);
                        const char *arr_v = (*env)->GetStringUTFChars(env, arr_e, 0);
                        Dwarf_Addr addr;
                        errno = 0;
                        addr = strtol(arr_v, (char **)NULL, 16);
                        if (errno != 0) {
                                // Release
                                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                                (*env)->ReleaseStringUTFChars(env, lala, lalachar);
                                (*env)->ReleaseStringUTFChars(env, arr_e, arr_v);
                                // invalid address
                                (*env)->SetObjectArrayElement(env, resultArr, 0,
                                                              (*env)->NewStringUTF(env, "-8"));
                                close(fd);
                                free(context);
                                free(options);
                                return resultArr;
                        }
                        char *stacktrace;
                        stacktrace = malloc(500 * sizeof(char));
                        ret = print_dwarf_symbol(dbg,
                                                 options->load_address - context->intended_addr,
                                                 addr, stacktrace, options, context);
                        if (ret != DW_DLV_OK) {
                                derr = print_subprogram_symbol(
                                        options->load_address - context->intended_addr, addr, stacktrace, options, context);
                        } else {
                                (*env)->SetObjectArrayElement(env, resultArr, i,
                                                              (*env)->NewStringUTF(env, stacktrace));
                        }
                        if ((ret != DW_DLV_OK) && derr) {
                                (*env)->SetObjectArrayElement(env, resultArr, i,
                                                              (*env)->NewStringUTF(env, arr_v));
                        } else {
                                (*env)->SetObjectArrayElement(env, resultArr, i,
                                                              (*env)->NewStringUTF(env, stacktrace));
                        }
                        free(stacktrace);
                        (*env)->ReleaseStringUTFChars(env, arr_e, arr_v);
                }

                dwarf_mach_object_access_finish(binary_interface);

                ret = dwarf_object_finish(dbg, &err);
                DWARF_ASSERT(ret, err);
        } else {
                for (i = 1; i < adrlen; i++) {
                        jstring arr_e = (jstring) (*env)->GetObjectArrayElement(env, adr, i);
                        const char *arr_v = (*env)->GetStringUTFChars(env, arr_e, 0);
                        Dwarf_Addr addr;
                        errno = 0;
                        addr = strtol(arr_v, (char **)NULL, 16);
                        if (errno != 0) {
                                // Release
                                (*env)->ReleaseStringUTFChars(env, arch, n_arch);
                                (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
                                (*env)->ReleaseStringUTFChars(env, lala, lalachar);
                                (*env)->ReleaseStringUTFChars(env, arr_e, arr_v);
                                // invalid address
                                (*env)->SetObjectArrayElement(env, resultArr, 0,
                                                              (*env)->NewStringUTF(env, "-8"));
                                close(fd);
                                free(context);
                                free(options);
                                return resultArr;
                        }
                        char *stacktrace;
                        stacktrace = malloc(500 * sizeof(char));
                        ret = find_and_print_symtab_symbol(
                                options->load_address - context->intended_addr,
                                addr, stacktrace, options, context);

                        if (ret != DW_DLV_OK) {
                                (*env)->SetObjectArrayElement(env, resultArr, i,
                                                              (*env)->NewStringUTF(env, arr_v));
                        } else {
                                (*env)->SetObjectArrayElement(env, resultArr, i,
                                                              (*env)->NewStringUTF(env, stacktrace));
                        }
                        free(stacktrace);
                        (*env)->ReleaseStringUTFChars(env, arr_e, arr_v);
                }
        }

        // Release
        (*env)->ReleaseStringUTFChars(env, arch, n_arch);
        (*env)->ReleaseStringUTFChars(env, dSYM, n_dSYM);
        (*env)->ReleaseStringUTFChars(env, lala, lalachar);
        // invalid address
        (*env)->SetObjectArrayElement(env, resultArr, 0, (*env)->NewStringUTF(env, "0"));
        close(fd);
        free(context);
        free(options);
        return resultArr;
}
