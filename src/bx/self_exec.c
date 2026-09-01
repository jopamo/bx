#define _GNU_SOURCE

#include "bx/self_exec.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#if defined(__linux__)
#include <link.h>
#endif
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/auxv.h>
#endif
#include <unistd.h>

#include "bx/libbx.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"

enum bx_self_exec_state {
    BX_SELF_EXEC_UNINITIALIZED = 0,
    BX_SELF_EXEC_READY,
    BX_SELF_EXEC_UNAVAILABLE,
};

enum bx_self_exec_image_kind {
    BX_SELF_EXEC_IMAGE_UNKNOWN = 0,
    BX_SELF_EXEC_IMAGE_NON_ELF,
    BX_SELF_EXEC_IMAGE_ELF,
};

static enum bx_self_exec_state bx_self_exec_state;
static int bx_self_exec_fd = -1;
static int bx_self_exec_error = ENOENT;
static char* bx_self_exec_path;

static bool bx_self_exec_pread_exact(
    int fd,
    void* buffer,
    size_t length,
    uintmax_t offset
) {
    off_t position = (off_t)offset;
    if (position < 0 || (uintmax_t)position != offset) {
        errno = EOVERFLOW;
        return false;
    }

    unsigned char* cursor = buffer;
    size_t remaining = length;
    while (remaining > 0u) {
        ssize_t count;
        do {
            count = pread(fd, cursor, remaining, position);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return false;
        }
        if (count == 0) {
            errno = ENOEXEC;
            return false;
        }
        cursor += (size_t)count;
        remaining -= (size_t)count;
        position += count;
    }
    return true;
}

#if defined(__linux__) && defined(AT_PHDR) && defined(AT_PHNUM) && \
    defined(AT_PHENT) && defined(AT_ENTRY)
static bool bx_self_exec_auxv_value(unsigned long type, uintptr_t* value_out) {
    errno = 0;
    unsigned long value = getauxval(type);
    if (value == 0ul) {
        if (errno == 0) {
            errno = ENOENT;
        }
        return false;
    }
    *value_out = (uintptr_t)value;
    return true;
}

static bool bx_self_exec_note_has_build_id(
    const unsigned char* note,
    size_t length
) {
    size_t offset = 0u;
    while (length - offset >= sizeof(ElfW(Nhdr))) {
        ElfW(Nhdr) header;
        memcpy(&header, note + offset, sizeof(header));
        offset += sizeof(header);

        size_t name_size = (size_t)header.n_namesz;
        size_t desc_size = (size_t)header.n_descsz;
        if (name_size > SIZE_MAX - 3u || desc_size > SIZE_MAX - 3u) {
            return false;
        }
        size_t aligned_name_size = (name_size + 3u) & ~(size_t)3u;
        size_t aligned_desc_size = (desc_size + 3u) & ~(size_t)3u;
        if (aligned_name_size > length - offset) {
            return false;
        }
        const unsigned char* name = note + offset;
        offset += aligned_name_size;
        if (aligned_desc_size > length - offset) {
            return false;
        }

        if (header.n_type == NT_GNU_BUILD_ID &&
            name_size == 4u &&
            desc_size >= 16u &&
            memcmp(name, "GNU", 4u) == 0) {
            return true;
        }
        offset += aligned_desc_size;
    }
    return false;
}

static bool bx_self_exec_note_is_mapped(
    const ElfW(Phdr)* note,
    const ElfW(Phdr)* headers,
    size_t header_count
) {
    if (note->p_filesz > UINTMAX_MAX - note->p_offset) {
        return false;
    }
    uintmax_t note_end =
        (uintmax_t)note->p_offset + (uintmax_t)note->p_filesz;

    for (size_t i = 0u; i < header_count; i++) {
        const ElfW(Phdr)* load = &headers[i];
        if (load->p_type != PT_LOAD ||
            load->p_filesz > UINTMAX_MAX - load->p_offset) {
            continue;
        }
        uintmax_t load_end =
            (uintmax_t)load->p_offset + (uintmax_t)load->p_filesz;
        if (note->p_offset < load->p_offset || note_end > load_end) {
            continue;
        }
        uintmax_t delta = (uintmax_t)note->p_offset -
            (uintmax_t)load->p_offset;
        if (delta <= UINTMAX_MAX - load->p_vaddr &&
            (uintmax_t)load->p_vaddr + delta ==
                (uintmax_t)note->p_vaddr) {
            return true;
        }
    }
    return false;
}

static bool bx_self_exec_fd_matches_loaded_image(
    int fd,
    const ElfW(Ehdr)* elf_header
) {
    uintptr_t loaded_phdr;
    uintptr_t loaded_phnum;
    uintptr_t loaded_phent;
    uintptr_t loaded_entry;
    if (!bx_self_exec_auxv_value(AT_PHDR, &loaded_phdr) ||
        !bx_self_exec_auxv_value(AT_PHNUM, &loaded_phnum) ||
        !bx_self_exec_auxv_value(AT_PHENT, &loaded_phent) ||
        !bx_self_exec_auxv_value(AT_ENTRY, &loaded_entry)) {
        return false;
    }
    if (loaded_phent != sizeof(ElfW(Phdr)) ||
        elf_header->e_phentsize != sizeof(ElfW(Phdr)) ||
        loaded_phnum != elf_header->e_phnum ||
        loaded_phnum == 0u ||
        loaded_phnum > SIZE_MAX / sizeof(ElfW(Phdr))) {
        errno = ESTALE;
        return false;
    }

    size_t phdr_size = (size_t)loaded_phnum * sizeof(ElfW(Phdr));
    ElfW(Phdr)* headers = malloc(phdr_size);
    if (headers == NULL) {
        return false;
    }
    if (!bx_self_exec_pread_exact(
            fd,
            headers,
            phdr_size,
            (uintmax_t)elf_header->e_phoff
        ) ||
        memcmp((const void*)loaded_phdr, headers, phdr_size) != 0) {
        int error = errno != 0 ? errno : ESTALE;
        free(headers);
        errno = error;
        return false;
    }

    uintptr_t linked_phdr = 0u;
    for (size_t i = 0u; i < (size_t)loaded_phnum; i++) {
        const ElfW(Phdr)* header = &headers[i];
        if (header->p_type == PT_PHDR &&
            header->p_vaddr <= UINTPTR_MAX) {
            linked_phdr = (uintptr_t)header->p_vaddr;
            break;
        }
    }
    if (linked_phdr == 0u) {
        uintmax_t phdr_offset = (uintmax_t)elf_header->e_phoff;
        for (size_t i = 0u; i < (size_t)loaded_phnum; i++) {
            const ElfW(Phdr)* header = &headers[i];
            if (header->p_type != PT_LOAD ||
                header->p_filesz > UINTMAX_MAX - header->p_offset) {
                continue;
            }
            uintmax_t load_end =
                (uintmax_t)header->p_offset +
                (uintmax_t)header->p_filesz;
            if (phdr_offset < header->p_offset ||
                phdr_offset > load_end ||
                phdr_size > load_end - phdr_offset) {
                continue;
            }
            uintmax_t delta =
                phdr_offset - (uintmax_t)header->p_offset;
            if (delta <= UINTPTR_MAX - header->p_vaddr) {
                linked_phdr = (uintptr_t)header->p_vaddr +
                    (uintptr_t)delta;
                break;
            }
        }
    }
    if (linked_phdr == 0u || loaded_phdr < linked_phdr) {
        free(headers);
        errno = ESTALE;
        return false;
    }

    uintptr_t load_bias = loaded_phdr - linked_phdr;
    if (elf_header->e_entry > UINTPTR_MAX - load_bias ||
        load_bias + (uintptr_t)elf_header->e_entry != loaded_entry) {
        free(headers);
        errno = ESTALE;
        return false;
    }

    bool matched_build_id = false;
    for (size_t i = 0u; i < (size_t)loaded_phnum; i++) {
        const ElfW(Phdr)* note = &headers[i];
        if (note->p_type != PT_NOTE ||
            note->p_filesz == 0u ||
            note->p_filesz > SIZE_MAX ||
            note->p_vaddr > UINTPTR_MAX - load_bias ||
            !bx_self_exec_note_is_mapped(
                note,
                headers,
                (size_t)loaded_phnum
            )) {
            continue;
        }

        size_t note_size = (size_t)note->p_filesz;
        unsigned char* note_data = malloc(note_size);
        if (note_data == NULL) {
            int error = errno;
            free(headers);
            errno = error;
            return false;
        }
        bool note_read = bx_self_exec_pread_exact(
            fd,
            note_data,
            note_size,
            (uintmax_t)note->p_offset
        );
        if (!note_read) {
            int error = errno;
            free(note_data);
            free(headers);
            errno = error;
            return false;
        }
        bool has_build_id = bx_self_exec_note_has_build_id(
            note_data,
            note_size
        );
        if (has_build_id &&
            memcmp(
                (const void*)(load_bias + (uintptr_t)note->p_vaddr),
                note_data,
                note_size
            ) == 0) {
            matched_build_id = true;
        }
        free(note_data);
        if (matched_build_id) {
            break;
        }
    }
    free(headers);
    if (!matched_build_id) {
        errno = ESTALE;
        return false;
    }
    return true;
}
#else
static bool bx_self_exec_fd_matches_loaded_image(
    int fd,
    const void* elf_header
) {
    (void)fd;
    (void)elf_header;
    errno = ENOSYS;
    return false;
}
#endif

static bool bx_self_exec_fd_is_current_image(
    int fd,
    enum bx_self_exec_image_kind* kind_out
) {
    ssize_t length;
    struct stat st;
    ElfW(Ehdr) elf_header;
    unsigned char magic[SELFMAG];

    *kind_out = BX_SELF_EXEC_IMAGE_UNKNOWN;
    if (fstat(fd, &st) != 0) {
        return false;
    }
    do {
        length = pread(fd, magic, sizeof(magic), 0);
    } while (length < 0 && errno == EINTR);
    if (length < 0) {
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        errno = ENOEXEC;
        return false;
    }
    if (length != (ssize_t)sizeof(magic) ||
        memcmp(magic, ELFMAG, SELFMAG) != 0) {
        *kind_out = BX_SELF_EXEC_IMAGE_NON_ELF;
        errno = ENOEXEC;
        return false;
    }
    *kind_out = BX_SELF_EXEC_IMAGE_ELF;
    if (!bx_self_exec_pread_exact(
            fd,
            &elf_header,
            sizeof(elf_header),
            0u
        ) ||
        elf_header.e_ehsize != sizeof(elf_header) ||
        elf_header.e_ident[EI_CLASS] !=
            (sizeof(uintptr_t) == 8u ? ELFCLASS64 : ELFCLASS32) ||
        elf_header.e_ident[EI_VERSION] != EV_CURRENT ||
        (elf_header.e_type != ET_EXEC && elf_header.e_type != ET_DYN)) {
        errno = ENOEXEC;
        return false;
    }
    return bx_self_exec_fd_matches_loaded_image(fd, &elf_header);
}

static int bx_self_exec_open_current_image(
    const char* path,
    enum bx_self_exec_image_kind* kind_out
) {
    *kind_out = BX_SELF_EXEC_IMAGE_UNKNOWN;
    if (path == NULL || path[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    int fd = bx_fd_open_cloexec(path, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    if (!bx_self_exec_fd_is_current_image(fd, kind_out)) {
        int error = errno != 0 ? errno : ENOEXEC;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static bool bx_self_exec_parse_handoff_fd(
    const char* path,
    int* fd_out
) {
    static const char prefix[] = "/dev/fd/";
    if (path == NULL || fd_out == NULL ||
        strncmp(path, prefix, sizeof(prefix) - 1u) != 0) {
        return false;
    }

    const char* cursor = path + sizeof(prefix) - 1u;
    if (*cursor == '\0') {
        return false;
    }
    unsigned int value = 0u;
    for (; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        unsigned int digit = (unsigned int)(*cursor - '0');
        if (value > ((unsigned int)INT_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    if (value < 3u) {
        return false;
    }
    *fd_out = (int)value;
    return true;
}

static const char* bx_self_exec_kernel_path(void) {
#if defined(__linux__) && defined(AT_EXECFN)
    errno = 0;
    unsigned long value = getauxval(AT_EXECFN);
    if (value == 0ul) {
        if (errno == 0) {
            errno = ENOENT;
        }
        return NULL;
    }
    const char* path = (const char*)value;
    if (path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }
    return path;
#else
    errno = ENOSYS;
    return NULL;
#endif
}

void bx_self_exec_discard_handoff(void) {
    int saved_errno = errno;
    const char* kernel_path = bx_self_exec_kernel_path();
    int inherited_fd = -1;
    if (bx_self_exec_parse_handoff_fd(kernel_path, &inherited_fd)) {
        close(inherited_fd);
    }
    errno = saved_errno;
}

bool bx_self_exec_initialize(const char* argv0) {
    if (bx_self_exec_state != BX_SELF_EXEC_UNINITIALIZED) {
        errno = bx_self_exec_state == BX_SELF_EXEC_READY ?
            0 :
            bx_self_exec_error;
        return bx_self_exec_state == BX_SELF_EXEC_READY;
    }

    const char* kernel_path = bx_self_exec_kernel_path();
    if (kernel_path == NULL) {
        bx_self_exec_error = errno != 0 ? errno : ENOENT;
        bx_self_exec_state = BX_SELF_EXEC_UNAVAILABLE;
        return false;
    }

    int inherited_fd = -1;
    int fd = -1;
    const char* selected_path = NULL;
    if (bx_self_exec_parse_handoff_fd(kernel_path, &inherited_fd)) {
        fd = bx_fd_dup_cloexec(inherited_fd);
        int handoff_error = fd < 0 ? errno : 0;
        enum bx_self_exec_image_kind handoff_kind =
            BX_SELF_EXEC_IMAGE_UNKNOWN;
        if (fd >= 0 &&
            !bx_self_exec_fd_is_current_image(fd, &handoff_kind)) {
            handoff_error = errno != 0 ? errno : ENOEXEC;
            close(fd);
            fd = -1;
        }
        close(inherited_fd);
        errno = handoff_error;
    }
    else {
        enum bx_self_exec_image_kind kernel_path_kind =
            BX_SELF_EXEC_IMAGE_UNKNOWN;
        fd = bx_self_exec_open_current_image(
            kernel_path,
            &kernel_path_kind
        );
        selected_path = kernel_path;
        if (fd < 0 &&
            kernel_path_kind == BX_SELF_EXEC_IMAGE_NON_ELF) {
            /*
             * For a kernel shebang invocation AT_EXECFN identifies the
             * wrapper, while argv[0] identifies the interpreter selected by
             * the kernel. Fall back only after proving that AT_EXECFN is not
             * an ELF image.
             */
            enum bx_self_exec_image_kind interpreter_kind =
                BX_SELF_EXEC_IMAGE_UNKNOWN;
            fd = bx_self_exec_open_current_image(
                argv0,
                &interpreter_kind
            );
            selected_path = argv0;
        }
    }
    if (fd < 0) {
        bx_self_exec_error = errno != 0 ? errno : ENOENT;
        bx_self_exec_state = BX_SELF_EXEC_UNAVAILABLE;
        return false;
    }

    char* normalized_path = NULL;
    if (selected_path != NULL) {
        normalized_path = bx_path_realpath_dup(selected_path);
        if (normalized_path == NULL) {
            normalized_path = bx_path_make_absolute_dup(selected_path);
        }
        if (normalized_path == NULL) {
            bx_self_exec_error = errno != 0 ? errno : ENOENT;
            close(fd);
            bx_self_exec_state = BX_SELF_EXEC_UNAVAILABLE;
            return false;
        }
    }

    bx_self_exec_fd = fd;
    bx_self_exec_path = normalized_path;
    bx_self_exec_error = 0;
    bx_self_exec_state = BX_SELF_EXEC_READY;
    return true;
}

int bx_self_exec_fd_dup(void) {
    if (bx_self_exec_state != BX_SELF_EXEC_READY ||
        bx_self_exec_fd < 0) {
        errno = bx_self_exec_error != 0 ? bx_self_exec_error : ENOENT;
        return -1;
    }
    return bx_fd_dup_cloexec(bx_self_exec_fd);
}

char* bx_self_exec_path_dup(void) {
    if (bx_self_exec_state != BX_SELF_EXEC_READY ||
        bx_self_exec_path == NULL) {
        errno = bx_self_exec_error != 0 ? bx_self_exec_error : ENOENT;
        return NULL;
    }
    return xstrdup(bx_self_exec_path);
}
