#include "module_elf.h"

#ifdef __linux__
#include <elf.h>
#include <stdint.h>
#include <string.h>

struct elf_layout {
    bool wide;
    bool big_endian;
};

static uint64_t elf_number(const struct elf_layout* elf, const unsigned char* field, size_t width) {
    uint64_t number = 0;
    for (size_t i = 0; i < width; i++)
        number = (number << 8) | field[elf->big_endian ? i : width - i - 1];
    return number;
}

#define ELF_FIELD(elf, ptr, kind, field) \
    ((elf)->wide ? elf_number(elf, (ptr) + offsetof(Elf64_##kind, field), \
                              sizeof(((Elf64_##kind*)0)->field)) \
                 : elf_number(elf, (ptr) + offsetof(Elf32_##kind, field), \
                              sizeof(((Elf32_##kind*)0)->field)))

static bool metadata_records_valid(const char* data, size_t len) {
    while (len != 0) {
        const char* end = memchr(data, '\0', len);
        if (end == NULL)
            return false;
        if (end != data) {
            const char* eq = memchr(data, '=', (size_t)(end - data));
            if (eq == NULL || eq == data)
                return false;
            for (const char* p = data; p < eq; p++) {
                if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                      (*p >= '0' && *p <= '9') || *p == '_'))
                    return false;
            }
        }
        len -= (size_t)(end - data) + 1;
        data = end + 1;
    }
    return true;
}

bool bx_module_elf_info(const unsigned char* file, size_t size, const char** data, size_t* length) {
    if (size < EI_NIDENT || memcmp(file, ELFMAG, SELFMAG) != 0 ||
        (file[EI_CLASS] != ELFCLASS32 && file[EI_CLASS] != ELFCLASS64) ||
        (file[EI_DATA] != ELFDATA2LSB && file[EI_DATA] != ELFDATA2MSB) ||
        file[EI_VERSION] != EV_CURRENT)
        return false;
    const struct elf_layout elf = {file[EI_CLASS] == ELFCLASS64, file[EI_DATA] == ELFDATA2MSB};
    size_t ehsize = elf.wide ? sizeof(Elf64_Ehdr) : sizeof(Elf32_Ehdr);
    size_t shsize = elf.wide ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr);
    if (size < ehsize || ELF_FIELD(&elf, file, Ehdr, e_ehsize) != ehsize ||
        ELF_FIELD(&elf, file, Ehdr, e_type) != ET_REL ||
        ELF_FIELD(&elf, file, Ehdr, e_version) != EV_CURRENT ||
        ELF_FIELD(&elf, file, Ehdr, e_shentsize) != shsize)
        return false;

    uint64_t offset = ELF_FIELD(&elf, file, Ehdr, e_shoff);
    if (offset < ehsize || offset > size || size - (size_t)offset < shsize)
        return false;
    const unsigned char* table = file + (size_t)offset;
    uint64_t entries = ELF_FIELD(&elf, file, Ehdr, e_shnum);
    uint64_t strings_idx = ELF_FIELD(&elf, file, Ehdr, e_shstrndx);
    if (entries == 0)
        entries = ELF_FIELD(&elf, table, Shdr, sh_size);
    if (strings_idx == SHN_XINDEX)
        strings_idx = ELF_FIELD(&elf, table, Shdr, sh_link);
    if (entries > (size - (size_t)offset) / shsize || strings_idx == SHN_UNDEF ||
        strings_idx >= entries)
        return false;

    const unsigned char* strings = table + (size_t)strings_idx * shsize;
    uint64_t stroff = ELF_FIELD(&elf, strings, Shdr, sh_offset);
    uint64_t strsize = ELF_FIELD(&elf, strings, Shdr, sh_size);
    if (ELF_FIELD(&elf, strings, Shdr, sh_type) != SHT_STRTAB ||
        (ELF_FIELD(&elf, strings, Shdr, sh_flags) & SHF_COMPRESSED) ||
        stroff > size || strsize == 0 || strsize > size - (size_t)stroff)
        return false;
    const unsigned char* names = file + (size_t)stroff;
    if (names[0] != '\0' || names[strsize - 1] != '\0')
        return false;

    static const char section[] = ".modinfo";
    const char* result = NULL;
    size_t result_len = 0;
    for (size_t i = 1; i < (size_t)entries; i++) {
        const unsigned char* sh = table + i * shsize;
        uint64_t name = ELF_FIELD(&elf, sh, Shdr, sh_name);
        if (name >= strsize)
            return false;
        if (strsize - name < sizeof(section) ||
            memcmp(names + (size_t)name, section, sizeof(section)) != 0)
            continue;
        uint64_t start = ELF_FIELD(&elf, sh, Shdr, sh_offset);
        uint64_t len = ELF_FIELD(&elf, sh, Shdr, sh_size);
        if (result != NULL || ELF_FIELD(&elf, sh, Shdr, sh_type) != SHT_PROGBITS ||
            (ELF_FIELD(&elf, sh, Shdr, sh_flags) & SHF_COMPRESSED) ||
            start > size || len == 0 || len > size - (size_t)start)
            return false;
        result = (const char*)file + (size_t)start;
        result_len = (size_t)len;
    }
    if (result == NULL || !metadata_records_valid(result, result_len))
        return false;
    *data = result;
    *length = result_len;
    return true;
}
#endif
