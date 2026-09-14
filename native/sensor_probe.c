#define _GNU_SOURCE
#include "sensor_probe.h"
#include "readonly_memory.h"
#include <android/sensor.h>
#include <elf.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#define MAX_DYNAMIC_BYTES (64u * 1024u)
#define MAX_RELOCATIONS (1u << 20)

typedef ssize_t (*GetEventsFn)(ASensorEventQueue *, ASensorEvent *, size_t);

static uintptr_t g_slot;
static uintptr_t g_original;
static uint32_t g_adjust_active;
static uint32_t g_adjust_x;
static uint32_t g_adjust_y;

static void set_error(char *out, size_t size, const char *text) {
    if (!out || size == 0) return;
    size_t i = 0;
    while (i + 1 < size && text[i]) {
        out[i] = text[i];
        ++i;
    }
    out[i] = 0;
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float bits_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool module_contains(const CfModule *module, uintptr_t address, size_t size) {
    return module && address >= module->image_begin && address < module->image_end &&
           size <= module->image_end - address;
}

static bool writable_load_contains(const CfModule *module, const Elf64_Phdr *loads,
                                   size_t load_count, uintptr_t address, size_t size) {
    for (size_t i = 0; i < load_count; ++i) {
        const Elf64_Phdr *load = &loads[i];
        if (!(load->p_flags & PF_W) || load->p_vaddr > UINTPTR_MAX - module->load_bias)
            continue;
        uintptr_t begin = module->load_bias + load->p_vaddr;
        if (address >= begin && address - begin < load->p_memsz &&
            size <= load->p_memsz - (address - begin)) return true;
    }
    return false;
}

static uintptr_t relative(const CfModule *module, uintptr_t value, size_t size) {
    if (value > UINTPTR_MAX - module->load_bias) return 0;
    uintptr_t address = module->load_bias + value;
    return module_contains(module, address, size) ? address : 0;
}

static bool symbol_name(const CfModule *module, uintptr_t strings, size_t string_size,
                        uint32_t offset, const char *expected) {
    size_t length = strlen(expected);
    if (offset >= string_size || length + 1 > string_size - offset ||
        !module_contains(module, strings + offset, length + 1)) return false;
    char name[64];
    if (length + 1 > sizeof(name) ||
        !cf_read_self(strings + offset, name, length + 1)) return false;
    return memcmp(name, expected, length + 1) == 0;
}

static ssize_t sensor_get_events(ASensorEventQueue *queue, ASensorEvent *events,
                                 size_t count) {
    uintptr_t original = __atomic_load_n(&g_original, __ATOMIC_ACQUIRE);
    if (!original) {
        errno = ENOSYS;
        return -1;
    }
    ssize_t received = ((GetEventsFn)original)(queue, events, count);
    if (received <= 0 || !events) return received;

    bool adjust = __atomic_load_n(&g_adjust_active, __ATOMIC_ACQUIRE) != 0;
    if (!adjust) return received;
    float add_x = bits_float(__atomic_load_n(&g_adjust_x, __ATOMIC_RELAXED));
    float add_y = bits_float(__atomic_load_n(&g_adjust_y, __ATOMIC_RELAXED));
    for (ssize_t i = 0; i < received; ++i) {
        if (events[i].type != ASENSOR_TYPE_GYROSCOPE) continue;
        events[i].data[0] += add_x;
        events[i].data[1] += add_y;
    }
    return received;
}

bool cf_sensor_probe_install(const CfModule *module, char *error, size_t error_size) {
    static const char wanted[] = "ASensorEventQueue_getEvents";
    if (error && error_size) error[0] = 0;
    if (!module || !error || error_size == 0 || g_slot) return false;

    Elf64_Ehdr header;
    if (!module_contains(module, module->image_begin, sizeof(header)) ||
        !cf_read_self(module->image_begin, &header, sizeof(header)) ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_machine != EM_AARCH64 ||
        header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 ||
        header.e_phnum > 64 || header.e_phoff > UINTPTR_MAX - module->image_begin ||
        !module_contains(module, module->image_begin + header.e_phoff,
                         (size_t)header.e_phnum * sizeof(Elf64_Phdr))) {
        set_error(error, error_size, "invalid Unity ELF header");
        return false;
    }

    Elf64_Phdr loads[32];
    size_t load_count = 0;
    uintptr_t dynamic = 0;
    size_t dynamic_size = 0;
    for (size_t i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr phdr;
        if (!cf_read_self(module->image_begin + header.e_phoff + i * sizeof(phdr),
                          &phdr, sizeof(phdr))) {
            set_error(error, error_size, "program header read failed");
            return false;
        }
        if (phdr.p_type == PT_LOAD) {
            if (load_count == sizeof(loads) / sizeof(loads[0])) {
                set_error(error, error_size, "too many load segments");
                return false;
            }
            loads[load_count++] = phdr;
        } else if (phdr.p_type == PT_DYNAMIC) {
            if (dynamic || phdr.p_vaddr > UINTPTR_MAX - module->load_bias) {
                set_error(error, error_size, "invalid dynamic segment");
                return false;
            }
            dynamic = module->load_bias + phdr.p_vaddr;
            dynamic_size = phdr.p_memsz;
        }
    }
    if (!dynamic || dynamic_size < sizeof(Elf64_Dyn) ||
        dynamic_size > MAX_DYNAMIC_BYTES ||
        !module_contains(module, dynamic, dynamic_size)) {
        set_error(error, error_size, "dynamic segment unavailable");
        return false;
    }

    uintptr_t symbols_value = 0, strings_value = 0, relocations_value = 0;
    size_t string_size = 0, relocations_size = 0, symbol_size = 0;
    int64_t relocation_kind = 0;
    bool terminated = false;
    for (size_t i = 0; i < dynamic_size / sizeof(Elf64_Dyn); ++i) {
        Elf64_Dyn entry;
        if (!cf_read_self(dynamic + i * sizeof(entry), &entry, sizeof(entry))) {
            set_error(error, error_size, "dynamic entry read failed");
            return false;
        }
        if (entry.d_tag == DT_NULL) { terminated = true; break; }
        switch (entry.d_tag) {
            case DT_SYMTAB: symbols_value = entry.d_un.d_ptr; break;
            case DT_SYMENT: symbol_size = entry.d_un.d_val; break;
            case DT_STRTAB: strings_value = entry.d_un.d_ptr; break;
            case DT_STRSZ: string_size = entry.d_un.d_val; break;
            case DT_JMPREL: relocations_value = entry.d_un.d_ptr; break;
            case DT_PLTRELSZ: relocations_size = entry.d_un.d_val; break;
            case DT_PLTREL: relocation_kind = entry.d_un.d_val; break;
        }
    }
    uintptr_t symbols = relative(module, symbols_value, sizeof(Elf64_Sym));
    uintptr_t strings = relative(module, strings_value, string_size);
    uintptr_t relocations = relative(module, relocations_value, relocations_size);
    if (!terminated || symbol_size != sizeof(Elf64_Sym) || relocation_kind != DT_RELA ||
        !symbols || !strings || !relocations || string_size == 0 ||
        relocations_size == 0 || relocations_size % sizeof(Elf64_Rela) != 0 ||
        relocations_size / sizeof(Elf64_Rela) > MAX_RELOCATIONS) {
        set_error(error, error_size, "unsupported Unity relocation table");
        return false;
    }

    uintptr_t match = 0;
    size_t count = relocations_size / sizeof(Elf64_Rela);
    for (size_t i = 0; i < count; ++i) {
        Elf64_Rela relocation;
        if (!cf_read_self(relocations + i * sizeof(relocation), &relocation,
                          sizeof(relocation))) {
            set_error(error, error_size, "relocation read failed");
            return false;
        }
        if (ELF64_R_TYPE(relocation.r_info) != R_AARCH64_JUMP_SLOT) continue;
        uint32_t index = ELF64_R_SYM(relocation.r_info);
        if ((size_t)index > (UINTPTR_MAX - symbols) / sizeof(Elf64_Sym)) continue;
        Elf64_Sym symbol;
        if (!cf_read_self(symbols + (size_t)index * sizeof(symbol), &symbol,
                          sizeof(symbol)) ||
            !symbol_name(module, strings, string_size, symbol.st_name, wanted)) continue;
        uintptr_t slot = relative(module, relocation.r_offset, sizeof(uintptr_t));
        if (!slot || (slot & (sizeof(uintptr_t) - 1)) ||
            !writable_load_contains(module, loads, load_count, slot, sizeof(uintptr_t)) ||
            match) {
            set_error(error, error_size, "sensor GOT slot is not uniquely writable");
            return false;
        }
        match = slot;
    }
    if (!match) {
        set_error(error, error_size, "sensor import not found");
        return false;
    }

    uintptr_t original;
    if (!cf_read_self(match, &original, sizeof(original)) || !original ||
        original == (uintptr_t)sensor_get_events) {
        set_error(error, error_size, "sensor import target invalid");
        return false;
    }
    __atomic_store_n(&g_original, original, __ATOMIC_RELEASE);
    uintptr_t replaced = __atomic_exchange_n((uintptr_t *)match,
                                              (uintptr_t)sensor_get_events,
                                              __ATOMIC_ACQ_REL);
    if (replaced != original) {
        __atomic_store_n((uintptr_t *)match, replaced, __ATOMIC_RELEASE);
        __atomic_store_n(&g_original, 0, __ATOMIC_RELEASE);
        set_error(error, error_size, "sensor import changed during install");
        return false;
    }
    g_slot = match;
    return true;
}

void cf_sensor_probe_set_adjustment(bool active, float x, float y) {
    if (!isfinite(x) || !isfinite(y) || fabsf(x) > .25f || fabsf(y) > .25f) {
        active = false;
        x = 0;
        y = 0;
    }
    __atomic_store_n(&g_adjust_x, float_bits(active ? x : 0), __ATOMIC_RELAXED);
    __atomic_store_n(&g_adjust_y, float_bits(active ? y : 0), __ATOMIC_RELAXED);
    __atomic_store_n(&g_adjust_active, active, __ATOMIC_RELEASE);
}

void cf_sensor_probe_shutdown(void) {
    cf_sensor_probe_set_adjustment(false, 0, 0);
    uintptr_t slot = g_slot;
    uintptr_t original = __atomic_load_n(&g_original, __ATOMIC_ACQUIRE);
    if (slot && original) {
        uintptr_t expected = (uintptr_t)sensor_get_events;
        __atomic_compare_exchange_n((uintptr_t *)slot, &expected, original, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    g_slot = 0;
    __atomic_store_n(&g_original, 0, __ATOMIC_RELEASE);
}
