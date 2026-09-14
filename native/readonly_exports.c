#include "readonly_exports.h"
#include "readonly_memory.h"
#include <string.h>

#define MAX_SYMBOLS (1u << 20)
#define MAX_CHAIN 4096u

static bool image_contains(const CfModule *m, uintptr_t address, size_t length) {
    return address >= m->image_begin && address < m->image_end &&
           length <= m->image_end - address;
}

static bool contains(const CfExports *e, uintptr_t address, size_t length, unsigned flags) {
    for (size_t i = 0; i < e->load_count; ++i) {
        const Elf64_Phdr *p = &e->loads[i];
        uintptr_t begin = e->bias + p->p_vaddr;
        if ((p->p_flags & flags) == flags && address >= begin &&
            address - begin < p->p_memsz && length <= p->p_memsz - (address - begin))
            return true;
    }
    return false;
}

static bool read_at(const CfExports *e, uintptr_t address, void *out, size_t size) {
    return contains(e, address, size, PF_R) && cf_read_self(address, out, size);
}

static uintptr_t relative(const CfExports *e, uintptr_t value, size_t size) {
    if (value > UINTPTR_MAX - e->bias) return 0;
    uintptr_t address = e->bias + value;
    return contains(e, address, size, PF_R) ? address : 0;
}

bool cf_exports_open(const CfModule *module, CfExports *exports) {
    if (exports == NULL) return false;
    memset(exports, 0, sizeof(*exports));
    if (module == NULL || module->load_bias == 0) return false;
    Elf64_Ehdr header;
    if (!image_contains(module, module->image_begin, sizeof(header)) ||
        !cf_read_self(module->image_begin, &header, sizeof(header)) ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_type != ET_DYN || header.e_machine != EM_AARCH64 ||
        header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 || header.e_phnum > 64 ||
        header.e_phoff > UINTPTR_MAX - module->image_begin ||
        !image_contains(module, module->image_begin + header.e_phoff,
                         (size_t)header.e_phnum * sizeof(Elf64_Phdr))) return false;
    CfExports e = {.bias = module->load_bias};
    uintptr_t dynamic = 0;
    size_t dynamic_size = 0;
    for (size_t i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr p;
        if (!cf_read_self(module->image_begin + header.e_phoff + i * sizeof(p), &p, sizeof(p)))
            return false;
        if (p.p_type != PT_LOAD && p.p_type != PT_DYNAMIC) continue;
        if (p.p_vaddr > UINTPTR_MAX - e.bias ||
            !image_contains(module, e.bias + p.p_vaddr, p.p_memsz)) return false;
        if (p.p_type == PT_LOAD) {
            if (e.load_count == 32) return false;
            e.loads[e.load_count++] = p;
        } else {
            if (dynamic != 0) return false;
            dynamic = e.bias + p.p_vaddr;
            dynamic_size = p.p_memsz;
        }
    }
    if (dynamic == 0 || dynamic_size < sizeof(Elf64_Dyn) || dynamic_size > 16384 ||
        !contains(&e, dynamic, dynamic_size, PF_R)) return false;
    uintptr_t sym = 0, str = 0, sysv = 0, gnu = 0;
    size_t symbol_size = 0;
    bool terminated = false;
    for (size_t i = 0; i < dynamic_size / sizeof(Elf64_Dyn); ++i) {
        Elf64_Dyn d;
        if (!read_at(&e, dynamic + i * sizeof(d), &d, sizeof(d))) return false;
        if (d.d_tag == DT_NULL) { terminated = true; break; }
        switch (d.d_tag) {
            case DT_SYMTAB: sym = d.d_un.d_ptr; break;
            case DT_STRTAB: str = d.d_un.d_ptr; break;
            case DT_HASH: sysv = d.d_un.d_ptr; break;
            case DT_GNU_HASH: gnu = d.d_un.d_ptr; break;
            case DT_STRSZ: e.string_size = d.d_un.d_val; break;
            case DT_SYMENT: symbol_size = d.d_un.d_val; break;
        }
    }
    if (!terminated || symbol_size != sizeof(Elf64_Sym) || e.string_size == 0 ||
        sym == 0 || str == 0 || (sysv == 0 && gnu == 0)) return false;
    e.symbols = relative(&e, sym, sizeof(Elf64_Sym));
    e.strings = relative(&e, str, e.string_size);
    e.sysv_hash = sysv ? relative(&e, sysv, 8) : 0;
    e.gnu_hash = gnu ? relative(&e, gnu, 16) : 0;
    if (!e.symbols || !e.strings || (sysv && !e.sysv_hash) || (gnu && !e.gnu_hash)) return false;
    *exports = e;
    return true;
}

static uintptr_t match_symbol(const CfExports *e, uint32_t index, const char *name, size_t length) {
    if (index >= MAX_SYMBOLS || (size_t)index * sizeof(Elf64_Sym) > UINTPTR_MAX - e->symbols)
        return 0;
    Elf64_Sym s;
    if (!read_at(e, e->symbols + (size_t)index * sizeof(s), &s, sizeof(s)) ||
        s.st_shndx == SHN_UNDEF || s.st_shndx >= SHN_LORESERVE ||
        ELF64_ST_TYPE(s.st_info) != STT_FUNC ||
        (ELF64_ST_BIND(s.st_info) != STB_GLOBAL && ELF64_ST_BIND(s.st_info) != STB_WEAK) ||
        ((s.st_other & 3) != STV_DEFAULT && (s.st_other & 3) != STV_PROTECTED) ||
        s.st_name >= e->string_size || length + 1 > e->string_size - s.st_name ||
        s.st_value > UINTPTR_MAX - e->bias) return 0;
    char text[128];
    if (!read_at(e, e->strings + s.st_name, text, length + 1) ||
        memcmp(text, name, length + 1) != 0) return 0;
    uintptr_t address = e->bias + s.st_value;
    return !(address & 3) && contains(e, address, s.st_size > 4 ? s.st_size : 4, PF_X) ? address : 0;
}

uintptr_t cf_exports_function(const CfExports *e, const char *name) {
    if (e == NULL || name == NULL || e->load_count > 32) return 0;
    size_t length = strnlen(name, 128);
    if (length == 0 || length == 128) return 0;
    uint32_t hash = 0, gnu = 5381;
    for (size_t i = 0; i < length; ++i) {
        uint32_t c = (unsigned char)name[i];
        hash = (hash << 4) + c;
        uint32_t high = hash & 0xf0000000u;
        if (high) hash ^= high >> 24;
        hash &= ~high;
        gnu = gnu * 33 + c;
    }
    if (e->sysv_hash) {
        uint32_t h[2];
        if (!read_at(e, e->sysv_hash, h, sizeof(h)) || h[0] == 0 || h[0] > MAX_SYMBOLS ||
            h[1] == 0 || h[1] > MAX_SYMBOLS ||
            !contains(e, e->sysv_hash, 8 + ((size_t)h[0] + h[1]) * 4, PF_R)) return 0;
        uintptr_t buckets = e->sysv_hash + 8, chains = buckets + (size_t)h[0] * 4;
        uint32_t index;
        if (!read_at(e, buckets + (hash % h[0]) * 4, &index, 4)) return 0;
        for (unsigned step = 0; index != 0 && step < h[1] && step < MAX_CHAIN; ++step) {
            if (index >= h[1]) return 0;
            uintptr_t address = match_symbol(e, index, name, length);
            if (address) return address;
            if (!read_at(e, chains + (size_t)index * 4, &index, 4)) return 0;
        }
        return 0;
    }
    if (e->gnu_hash) {
        uint32_t h[4];
        if (!read_at(e, e->gnu_hash, h, sizeof(h)) || h[0] == 0 || h[0] > MAX_SYMBOLS ||
            h[1] >= MAX_SYMBOLS || h[2] == 0 || h[2] > MAX_SYMBOLS ||
            !contains(e, e->gnu_hash, 16 + (size_t)h[2] * 8 + (size_t)h[0] * 4, PF_R)) return 0;
        // Bloom is only an optimization. Exact bucket/chain/name checks suffice.
        uintptr_t buckets = e->gnu_hash + 16 + (size_t)h[2] * 8;
        uintptr_t chains = buckets + (size_t)h[0] * 4;
        uint32_t index;
        if (!read_at(e, buckets + (gnu % h[0]) * 4, &index, 4) || index < h[1]) return 0;
        for (unsigned step = 0; index < MAX_SYMBOLS && step < MAX_CHAIN; ++step, ++index) {
            uint32_t chain;
            size_t offset = (size_t)(index - h[1]) * 4;
            if (offset > UINTPTR_MAX - chains || !read_at(e, chains + offset, &chain, 4)) return 0;
            if ((chain | 1) == (gnu | 1)) {
                uintptr_t address = match_symbol(e, index, name, length);
                if (address) return address;
            }
            if (chain & 1) break;
        }
    }
    return 0;
}
