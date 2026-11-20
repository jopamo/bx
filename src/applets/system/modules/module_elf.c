#include "module_elf.h"

#ifdef __linux__
#include <elf.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/archive_codec.h"
#include "bx/diag.h"
#include "module_index.h"

unsigned char* bx_module_elf_read(int fd, const char* path, size_t* length,
                                  struct bx_diag_ctx* diag) {
    enum { MODULE_SIZE_LIMIT = 128 * 1024 * 1024 };
    size_t size;
    unsigned char* file = (unsigned char*)bx_module_read_regular(fd, MODULE_SIZE_LIMIT, &size);
    if (file == NULL) {
        bx_perror_path(diag, path);
        return NULL;
    }
    const struct bx_archive_codec* codec = bx_archive_codec_detect_magic(file, size);
    if (codec == NULL) {
        *length = size;
        return file;
    }
    if (codec == bx_archive_codec_bzip2()) {
        bx_diag(diag, "'%s': unsupported module compression", path);
        free(file);
        return NULL;
    }
    struct bx_archive_buffer input = {.data = file, .len = size};
    struct bx_archive_buffer decoded = {0};
    bool ok = bx_archive_codec_decode_buffer_limited(codec, &input, &decoded, MODULE_SIZE_LIMIT, diag);
    free(file);
    if (!ok) {
        free(decoded.data);
        return NULL;
    }
    if (decoded.len == 0) {
        bx_diag(diag, "'%s': expected an ELF module with valid .modinfo", path);
        free(decoded.data);
        return NULL;
    }
    *length = decoded.len;
    return decoded.data;
}

struct elf_layout {
    bool wide;
    bool big_endian;
};

struct elf_sections {
    struct elf_layout layout;
    const unsigned char* file;
    size_t size;
    const unsigned char* table;
    size_t section_size;
    size_t count;
    const unsigned char* names;
    size_t names_size;
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

static bool elf_sections_read(const unsigned char* file, size_t size, struct elf_sections* sections) {
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

    for (size_t i = 1; i < (size_t)entries; i++)
        if (ELF_FIELD(&elf, table + i * shsize, Shdr, sh_name) >= strsize)
            return false;
    *sections = (struct elf_sections){
        .layout = elf, .file = file, .size = size, .table = table,
        .section_size = shsize, .count = (size_t)entries,
        .names = names, .names_size = (size_t)strsize,
    };
    return true;
}

static const unsigned char* elf_section_bytes(const struct elf_sections* sections,
                                              const unsigned char* header, size_t* size) {
    uint64_t start = ELF_FIELD(&sections->layout, header, Shdr, sh_offset);
    uint64_t len = ELF_FIELD(&sections->layout, header, Shdr, sh_size);
    if ((ELF_FIELD(&sections->layout, header, Shdr, sh_flags) & SHF_COMPRESSED) ||
        start > sections->size || len > sections->size - (size_t)start)
        return NULL;
    *size = (size_t)len;
    return sections->file + (size_t)start;
}

bool bx_module_elf_info(const unsigned char* file, size_t size, const char** data, size_t* length) {
    struct elf_sections sections;
    if (!elf_sections_read(file, size, &sections))
        return false;
    static const char section[] = ".modinfo";
    const char* result = NULL;
    size_t result_len = 0;
    for (size_t i = 1; i < sections.count; i++) {
        const unsigned char* sh = sections.table + i * sections.section_size;
        size_t name = (size_t)ELF_FIELD(&sections.layout, sh, Shdr, sh_name);
        if (sections.names_size - name < sizeof(section) ||
            memcmp(sections.names + name, section, sizeof(section)) != 0)
            continue;
        if (result != NULL || ELF_FIELD(&sections.layout, sh, Shdr, sh_type) != SHT_PROGBITS)
            return false;
        result = (const char*)elf_section_bytes(&sections, sh, &result_len);
        if (result == NULL || result_len == 0)
            return false;
    }
    if (result == NULL || !metadata_records_valid(result, result_len))
        return false;
    *data = result;
    *length = result_len;
    return true;
}

bool bx_module_elf_undefined(const unsigned char* file, size_t size,
                             bx_module_elf_undefined_fn visit, void* user) {
    enum { SYMBOLS_MAX = 262144, SYMBOL_NAME_MAX = 4096 };
    struct elf_sections sections;
    if (!visit || !elf_sections_read(file, size, &sections))
        return false;
    const struct elf_layout* elf = &sections.layout;
    const unsigned char* header = NULL;
    for (size_t i = 1; i < sections.count; i++) {
        const unsigned char* sh = sections.table + i * sections.section_size;
        if (ELF_FIELD(elf, sh, Shdr, sh_type) == SHT_SYMTAB) {
            if (header != NULL)
                return false;
            header = sh;
        }
    }
    const size_t symsize = elf->wide ? sizeof(Elf64_Sym) : sizeof(Elf32_Sym);
    if (!header || ELF_FIELD(elf, header, Shdr, sh_entsize) != symsize)
        return false;
    size_t sym_bytes;
    const unsigned char* symbols = elf_section_bytes(&sections, header, &sym_bytes);
    if (!symbols || sym_bytes < symsize || sym_bytes % symsize != 0 ||
        sym_bytes / symsize > SYMBOLS_MAX)
        return false;
    uint64_t link = ELF_FIELD(elf, header, Shdr, sh_link);
    if (link == SHN_UNDEF || link >= sections.count)
        return false;
    const unsigned char* strings_header = sections.table + (size_t)link * sections.section_size;
    if (ELF_FIELD(elf, strings_header, Shdr, sh_type) != SHT_STRTAB)
        return false;
    size_t names_size;
    const unsigned char* names = elf_section_bytes(&sections, strings_header, &names_size);
    if (!names || names_size == 0 || names[0] != '\0' || names[names_size - 1] != '\0')
        return false;
    for (size_t offset = symsize; offset < sym_bytes; offset += symsize) {
        const unsigned char* sym = symbols + offset;
        uint64_t name = ELF_FIELD(elf, sym, Sym, st_name);
        uint64_t section = ELF_FIELD(elf, sym, Sym, st_shndx);
        if (name >= names_size || (section < SHN_LORESERVE && section >= sections.count))
            return false;
        unsigned binding = ELF64_ST_BIND(ELF_FIELD(elf, sym, Sym, st_info));
        if (section != SHN_UNDEF || (binding != STB_GLOBAL && binding != STB_WEAK))
            continue;
        const unsigned char* start = names + (size_t)name;
        size_t available = names_size - (size_t)name;
        const unsigned char* end = memchr(start, '\0',
            available > SYMBOL_NAME_MAX + 1u ? SYMBOL_NAME_MAX + 1u : available);
        if (!end || end == start ||
            !visit((const char*)start, (size_t)(end - start), binding == STB_WEAK, user))
            return false;
    }
    return true;
}
#endif
