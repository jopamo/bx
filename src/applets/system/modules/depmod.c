/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s -n [-ae] -F SYSTEM_MAP [-b ROOT] [RELEASE]\n"
                 "       %s --write-bx-index [-ae] -F SYSTEM_MAP -b ROOT RELEASE\n"
                 "       %s --show-bx-aliases [-b ROOT] [RELEASE]\n"
                 "       %s --show-bx-softdeps [-b ROOT] [RELEASE]\n"
                 "       %s (--write-bx-aliases | --write-bx-softdeps) -b ROOT RELEASE\n"
                 "Each write mode creates one BX text index without replacing any entry.\n",
                 name, name, name, name, name);
}

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "fswalk/walk.h"
#include "lib/args_common.h"
#include "lib/fd_ops.h"
#include "lib/text_buffer.h"
#include "lib/xreadwrite.h"
#include "module_alias.h"
#include "module_elf.h"
#include "module_index.h"
#include "module_plan.h"
#include "module_symbols.h"
#include "module_system_map.h"

enum { FILES_MAX = 8192, ENTRIES_MAX = 262144, DEPTH_MAX = 48,
       REFS_BYTES_MAX = 32 * 1024 * 1024, REFS_MAX = 1048576,
       MODULE_BYTES_MAX = 1024 * 1024 * 1024, INDEX_EDGES_MAX = 16777216 };

struct module {
    char* path;
    size_t first;
    size_t edge_count;
    size_t rank;
    size_t install_order;
};

struct reference {
    char* name;
    size_t len;
    size_t provider;
    bool weak;
};

struct scan {
    struct bx_diag_ctx* diag;
    struct bx_module_symbols exports;
    struct module* modules;
    size_t count;
    size_t visited;
    size_t owner;
    struct reference* refs;
    size_t refs_count;
    size_t refs_capacity;
    size_t refs_bytes;
};

static bool index_append(struct bx_text_buffer* out, const char* text, size_t len);

enum metadata_format { SYMBOLS, ALIASES, SOFTDEPS };

static bool render_metadata(const struct module* module, const unsigned char* file,
                            size_t length, enum metadata_format format, struct bx_text_buffer* out) {
    const char* records;
    size_t size;
    if (!bx_module_elf_info(file, length, &records, &size)) {
        errno = EINVAL;
        return false;
    }
    struct bx_module_path key = bx_module_path_name(
        (struct bx_module_path){module->path, strlen(module->path)}, true);
    char name[NAME_MAX + 1];
    for (size_t i = 0; i < key.len; i++)
        name[i] = key.text[i] == '-' ? '_' : key.text[i];
    const char* field = format == ALIASES ? "alias=" : "softdep=";
    size_t field_len = strlen(field);
    for (const char* p = records; p < records + size; p += strlen(p) + 1) {
        if (strncmp(p, field, field_len) != 0)
            continue;
        const char* value = p + field_len;
        size_t len = strlen(value);
        struct bx_module_path span = {value, len};
        if (format == ALIASES) {
            if (!bx_module_alias_pattern_valid(span) ||
                !index_append(out, "alias ", 6) || !index_append(out, value, len) ||
                !index_append(out, " ", 1) || !index_append(out, name, key.len))
                return false;
        } else {
            struct bx_module_path pre, post;
            if (!bx_module_softdep_fields(span, &pre, &post) ||
                !index_append(out, "softdep ", 8) || !index_append(out, name, key.len) ||
                !index_append(out, " ", 1) || !index_append(out, value, len))
                return false;
        }
        if (!index_append(out, "\n", 1))
            return false;
    }
    return true;
}

static enum bx_walk_action walk_error(const char* path, int error, void* user) {
    struct scan* scan = user;
    bx_diag(scan->diag, "%s: %s", path, bx_strerror(error));
    return BX_WALK_ERROR;
}

static enum bx_walk_action collect_path(struct bx_walk_entry* entry, void* user) {
    struct scan* scan = user;
    if (entry->depth == 0)
        return BX_WALK_CONTINUE;
    if (entry->depth >= DEPTH_MAX || ++scan->visited > ENTRIES_MAX) {
        bx_diag(scan->diag, "module tree exceeds traversal limits");
        return BX_WALK_ERROR;
    }
    if (strncmp(entry->path, "./", 2) != 0) {
        bx_diag(scan->diag, "unexpected module entry path: '%s'", entry->path);
        return BX_WALK_ERROR;
    }
    const char* path = entry->path + 2;
    size_t len = strlen(path);
    if (entry->is_dir || !bx_module_path_name((struct bx_module_path){path, len}, true).len)
        return BX_WALK_CONTINUE;
    if (len >= PATH_MAX || !bx_module_path_valid(path, len)) {
        bx_diag(scan->diag, "invalid module path: '%s'", path);
        return BX_WALK_ERROR;
    }
    if (scan->count == FILES_MAX) {
        bx_diag(scan->diag, "module tree exceeds %d module files", FILES_MAX);
        return BX_WALK_ERROR;
    }
    char* copy = strdup(path);
    if (!copy) {
        bx_perror_path(scan->diag, path);
        return BX_WALK_ERROR;
    }
    scan->modules[scan->count++].path = copy;
    return BX_WALK_CONTINUE;
}

static int order_modules(const void* a, const void* b) {
    return strcmp(((const struct module*)a)->path, ((const struct module*)b)->path);
}

static int installed_order(const void* a, const void* b) {
    const struct module* left = a;
    const struct module* right = b;
    if (left->install_order != right->install_order)
        return (left->install_order > right->install_order) - (left->install_order < right->install_order);
    return order_modules(a, b);
}

static bool apply_modules_order(struct scan* scan, const struct bx_module_tree* tree) {
    int fd = bx_module_tree_open_for_load(tree, "modules.order");
    if (fd < 0 && errno == ENOENT)
        return true;
    size_t size;
    char* data = fd < 0 ? NULL : bx_module_read_regular(fd, BX_MODULE_INDEX_BYTES_LIMIT, &size);
    bx_fd_cleanup(&fd);
    if (!data) {
        bx_perror_path(scan->diag, "modules.order");
        return false;
    }
    bool ok = false;
    const char* end = data + size;
    size_t rank = 0;
    for (const char* p = data; p < end;) {
        const char* line = p;
        const char* nl = memchr(line, '\n', (size_t)(end - line));
        size_t len = (size_t)((nl ? nl : end) - line);
        p = nl ? nl + 1 : end;
        if (!len)
            continue;
        char path[PATH_MAX];
        if (++rank > ENTRIES_MAX || len >= sizeof(path) - sizeof(".zst") ||
            !bx_module_path_valid(line, len) ||
            !bx_module_path_name((struct bx_module_path){line, len}, false).len) {
            bx_diag(scan->diag, "invalid modules.order record");
            goto done;
        }
        memcpy(path, line, len);
        static const char* const suffixes[] = {"", ".gz", ".xz", ".zst"};
        struct module* match = NULL;
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
            strcpy(path + len, suffixes[i]);
            struct module key = {.path = path};
            struct module* item = bsearch(&key, scan->modules, scan->count,
                                          sizeof(*scan->modules), order_modules);
            if (item) {
                if (match || item->install_order) {
                    bx_diag(scan->diag, "duplicate installed module for modules.order record '%.*s'",
                            (int)len, line);
                    goto done;
                }
                match = item;
            }
        }
        if (match)
            match->install_order = rank;
    }
    for (size_t i = 0; i < scan->count; i++)
        if (!scan->modules[i].install_order)
            scan->modules[i].install_order = rank + 1;
    qsort(scan->modules, scan->count, sizeof(*scan->modules), installed_order);
    ok = true;
done:
    free(data);
    return ok;
}

static bool collect_symbol(const char* name, size_t len, enum bx_module_elf_symbol_kind kind,
                            void* user) {
    struct scan* scan = user;
    if (kind == BX_MODULE_ELF_EXPORT)
        return bx_module_symbols_add(&scan->exports, name, len, scan->owner);
    if (scan->refs_count == REFS_MAX || len + 1 > REFS_BYTES_MAX - scan->refs_bytes) {
        errno = EFBIG;
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (name[i] < '!' || name[i] > '~') {
            errno = EINVAL;
            return false;
        }
    }
    if (scan->refs_count == scan->refs_capacity) {
        size_t cap = scan->refs_capacity ? scan->refs_capacity * 2 : 256;
        struct reference* refs = realloc(scan->refs, cap * sizeof(*refs));
        if (!refs)
            return false;
        scan->refs = refs;
        scan->refs_capacity = cap;
    }
    char* copy = strndup(name, len);
    if (!copy)
        return false;
    scan->refs[scan->refs_count++] = (struct reference){.name = copy, .len = len,
                                                        .weak = kind == BX_MODULE_ELF_WEAK};
    scan->refs_bytes += len + 1;
    return true;
}

static bool read_map(struct scan* scan, const char* path) {
    int fd = bx_fd_open_cloexec(path, O_RDONLY | O_NONBLOCK | O_NOCTTY, 0);
    size_t size;
    char* data = fd < 0 ? NULL : bx_module_read_regular(fd, 64 * 1024 * 1024, &size);
    bx_fd_cleanup(&fd);
    if (!data) {
        bx_perror_path(scan->diag, path);
        return false;
    }
    errno = 0;
    bool ok = bx_module_system_map_exports(data, size, collect_symbol, scan);
    if (!ok)
        bx_diag(scan->diag, "%s: %s", path, errno ? bx_strerror(errno) : "invalid System.map");
    free(data);
    return ok;
}

static bool read_tree(struct scan* scan, const struct bx_module_tree* tree,
                      enum metadata_format format, struct bx_text_buffer* index) {
    int fd = bx_fd_dup_cloexec(tree->fd);
    DIR* stream = fd < 0 ? NULL : fdopendir(fd);
    if (!stream) {
        bx_perror_path(scan->diag, tree->path);
        bx_fd_cleanup(&fd);
        return false;
    }
    const struct bx_walk_opts opts = {.max_depth = DEPTH_MAX, .report_eacces = true,
        .cycle_mode = BX_WALK_CYCLE_DIR_REPEAT, .cycle_report = BX_WALK_CYCLE_ERROR};
    const struct bx_walk_ops ops = {.visit = collect_path, .error = walk_error};
    if (bx_walk_opened_dir(".", stream, &opts, &ops, scan) != 0)
        return false;
    qsort(scan->modules, scan->count, sizeof(*scan->modules), order_modules);
    if (format != SYMBOLS && !apply_modules_order(scan, tree))
        return false;
    size_t remaining = MODULE_BYTES_MAX;
    for (size_t i = 0; i < scan->count; i++) {
        struct module* module = &scan->modules[i];
        fd = bx_module_tree_open_for_load(tree, module->path);
        if (fd < 0) {
            bx_perror_path(scan->diag, module->path);
            return false;
        }
        size_t length;
        unsigned char* file = bx_module_elf_read(fd, module->path, &length, scan->diag);
        bx_fd_cleanup(&fd);
        if (!file)
            return false;
        if (length > remaining) {
            bx_diag(scan->diag, "decoded module tree exceeds 1 GiB");
            free(file);
            return false;
        }
        remaining -= length;
        scan->owner = i + 1;
        module->first = scan->refs_count;
        errno = 0;
        bool ok = format == SYMBOLS ? bx_module_elf_symbols(file, length, collect_symbol, scan) :
                                      render_metadata(module, file, length, format, index);
        if (!ok)
            bx_diag(scan->diag, "%s: %s", module->path,
                    errno ? bx_strerror(errno) : "invalid ELF module symbol table");
        free(file);
        if (!ok)
            return false;
    }
    return true;
}

static bool resolve(struct scan* scan) {
    bx_module_symbols_seal(&scan->exports);
    size_t* seen = calloc(scan->count + 1, sizeof(*seen));
    if (!seen) {
        bx_perror_path(scan->diag, "module dependency edges");
        return false;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < scan->count; i++) {
        struct module* module = &scan->modules[i];
        size_t end = i + 1 < scan->count ? scan->modules[i + 1].first : scan->refs_count;
        for (size_t j = module->first; j < end; j++) {
            const struct reference* ref = &scan->refs[j];
            size_t provider;
            enum bx_module_symbol_result result =
                bx_module_symbols_find(&scan->exports, ref->name, ref->len, &provider);
            if (result == BX_MODULE_SYMBOL_MISSING && ref->weak)
                continue;
            if (result != BX_MODULE_SYMBOL_FOUND || provider == i + 1) {
                bx_diag(scan->diag, "%s: %s symbol '%s'", module->path,
                        result == BX_MODULE_SYMBOL_AMBIGUOUS ? "ambiguous" :
                        result == BX_MODULE_SYMBOL_FOUND ? "self-referencing" : "unresolved",
                        ref->name);
                ok = false;
                break;
            }
            if (provider && seen[provider] != i + 1) {
                seen[provider] = i + 1;
                // Compact edges into the provider fields of this module's
                // reference range; names are kept for cleanup.
                scan->refs[module->first + module->edge_count++].provider = provider;
            }
        }
    }
    free(seen);
    return ok;
}

static bool rank_graph(struct scan* scan) {
    struct frame {size_t module, next;};
    struct frame* stack = calloc(scan->count + 1, sizeof(*stack));
    unsigned char* color = calloc(scan->count + 1, 1);
    bool ok = false;
    if (!stack || !color) {
        bx_perror_path(scan->diag, "module dependency graph");
        goto done;
    }
    size_t rank = 0;
    for (size_t i = 0; i < scan->count; i++) {
        if (color[i])
            continue;
        size_t depth = 1;
        stack[0] = (struct frame){.module = i};
        color[i] = 1;
        while (depth) {
            struct frame* frame = &stack[depth - 1];
            struct module* module = &scan->modules[frame->module];
            if (frame->next == module->edge_count) {
                module->rank = rank++;
                color[frame->module] = 2;
                depth--;
                continue;
            }
            size_t next = scan->refs[module->first + frame->next++].provider - 1;
            if (color[next] == 1) {
                bx_diag(scan->diag, "module dependency cycle: %s -> %s", module->path,
                        scan->modules[next].path);
                goto done;
            }
            if (color[next] == 0) {
                color[next] = 1;
                stack[depth++] = (struct frame){.module = next};
            }
        }
    }
    ok = true;
done:
    free(stack);
    free(color);
    return ok;
}

static int dependency_order(const void* a, const void* b) {
    const struct module* left = *(const struct module* const*)a;
    const struct module* right = *(const struct module* const*)b;
    return (left->rank < right->rank) - (left->rank > right->rank);
}

static bool index_append(struct bx_text_buffer* out, const char* text, size_t len) {
    if (len > BX_MODULE_INDEX_BYTES_LIMIT - out->length) {
        errno = EFBIG;
        return false;
    }
    return bx_text_buffer_append_span(out, text, len);
}

static bool render_deps(const struct scan* scan, struct bx_text_buffer* out) {
    size_t* seen = calloc(scan->count + 1, sizeof(*seen));
    const struct module** reachable = malloc((scan->count + 1) * sizeof(*reachable));
    bool ok = false;
    size_t work_left = INDEX_EDGES_MAX;
    if (!seen || !reachable)
        goto done;
    for (size_t i = 0; i < scan->count; i++) {
        const struct module* module = &scan->modules[i];
        size_t count = 1;
        reachable[0] = module;
        seen[i] = i + 1;
        for (size_t j = 0; j < count; j++) {
            const struct module* item = reachable[j];
            if (work_left < item->edge_count) {
                errno = EFBIG;
                goto done;
            }
            work_left -= item->edge_count;
            for (size_t k = 0; k < item->edge_count; k++) {
                size_t target = scan->refs[item->first + k].provider - 1;
                if (seen[target] != i + 1) {
                    seen[target] = i + 1;
                    reachable[count++] = &scan->modules[target];
                }
            }
        }
        // Reverse finish order puts every consumer before its providers.
        qsort(reachable + 1, count - 1, sizeof(*reachable), dependency_order);
        if (!index_append(out, module->path, strlen(module->path)) || !index_append(out, ":", 1))
            goto done;
        for (size_t j = 1; j < count; j++) {
            const char* path = reachable[j]->path;
            if (!index_append(out, " ", 1) || !index_append(out, path, strlen(path)))
                goto done;
        }
        if (!index_append(out, "\n", 1))
            goto done;
    }
    ok = true;
done:
    free(seen);
    free(reachable);
    return ok;
}

static bool publish_index(const struct bx_module_tree* tree, const char* name,
                          const struct bx_text_buffer* index, struct bx_diag_ctx* diag) {
    int fd = bx_fd_open_anonymous_file_at(tree->fd);
    bool ok = fd >= 0 &&
              bx_xwrite_all(fd, index->data, index->length) &&
              fchmod(fd, 0644) == 0 &&
              bx_fd_fsync(fd) == 0 &&
              bx_fd_publish_anonymous_file_at(fd, tree->fd, name) == 0;
    if (!ok) {
        bx_perror_path(diag, name);
    } else if (bx_fd_fsync(tree->fd) < 0) {
        bx_diag(diag, "%s published, but directory sync failed: %s", name, bx_strerror(errno));
        ok = false;
    }
    bx_fd_cleanup(&fd);
    return ok;
}
#endif

int bx_depmod_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {.progname = "depmod"};
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "depmod", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
#ifdef __linux__
    enum { WRITE_BX_INDEX = 256, SHOW_BX_ALIASES, SHOW_BX_SOFTDEPS,
           WRITE_BX_ALIASES, WRITE_BX_SOFTDEPS };
    static const struct option opts[] = {
        {"all", no_argument, NULL, 'a'}, {"errsyms", no_argument, NULL, 'e'},
        {"show", no_argument, NULL, 'n'}, {"dry-run", no_argument, NULL, 'n'},
        {"basedir", required_argument, NULL, 'b'}, {"filesyms", required_argument, NULL, 'F'},
        {"write-bx-index", no_argument, NULL, WRITE_BX_INDEX},
        {"show-bx-aliases", no_argument, NULL, SHOW_BX_ALIASES},
        {"show-bx-softdeps", no_argument, NULL, SHOW_BX_SOFTDEPS},
        {"write-bx-aliases", no_argument, NULL, WRITE_BX_ALIASES},
        {"write-bx-softdeps", no_argument, NULL, WRITE_BX_SOFTDEPS},
        {0},
    };
    const char* base = "/";
    const char* map = NULL;
    int operation = 0;
    bool base_set = false;
    int operand = 1;
    bx_args_getopt_reset();
    for (int opt; (opt = bx_args_getopt_long_interspersed(argc, argv, "-:aenb:F:", opts, &operand)) != -1;) {
        switch (opt) {
        case 'a': case 'e': break;
        case 'b': base = optarg; base_set = true; break;
        case 'F': map = optarg; break;
        case 'n': case WRITE_BX_INDEX: case SHOW_BX_ALIASES: case SHOW_BX_SOFTDEPS:
        case WRITE_BX_ALIASES: case WRITE_BX_SOFTDEPS:
            if (operation != 0 && operation != opt) {
                bx_diag(&diag, "select exactly one output mode");
                return 1;
            }
            operation = opt;
            break;
        case ':':
            bx_cli_diag_option_requires_arg(&diag, optopt, optind, argc, argv);
            return 1;
        default:
            bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    enum metadata_format format = operation == SHOW_BX_ALIASES || operation == WRITE_BX_ALIASES ? ALIASES :
                                  operation == SHOW_BX_SOFTDEPS || operation == WRITE_BX_SOFTDEPS ? SOFTDEPS : SYMBOLS;
    bool write_index = operation == WRITE_BX_INDEX || operation == WRITE_BX_ALIASES || operation == WRITE_BX_SOFTDEPS;
    if (operation == 0 ||
        (format != SYMBOLS ? map != NULL : !map || !*map) || !*base ||
        argc - optind > 1 || (write_index && (!base_set || argc - optind != 1))) {
        usage(stderr, "depmod");
        return 1;
    }
    struct bx_module_tree tree = {.fd = -1};
    struct scan scan = {.diag = &diag, .modules = calloc(FILES_MAX, sizeof(*scan.modules))};
    struct bx_text_buffer index = {0};
    int status = 1;
    if (!scan.modules) {
        bx_perror_path(&diag, "module tree inventory");
        return 1;
    }
    if (bx_module_tree_open(&tree, base, optind == argc ? NULL : argv[optind]) < 0) {
        bx_perror_path(&diag, tree.path ? tree.path : "module tree");
        goto done;
    }
    bool ready = format != SYMBOLS ? read_tree(&scan, &tree, format, &index) :
                 read_map(&scan, map) && read_tree(&scan, &tree, SYMBOLS, NULL) &&
                 resolve(&scan) && rank_graph(&scan);
    if (ready) {
        if (format == SYMBOLS && !render_deps(&scan, &index))
            bx_perror_path(&diag, "build modules.dep");
        else if (write_index) {
            const char* leaf = format == ALIASES ? "modules.alias" :
                               format == SOFTDEPS ? "modules.softdep" : "modules.dep";
            status = publish_index(&tree, leaf, &index, &diag) ? 0 : 1;
        }
        else if ((!index.length || fwrite(index.data, 1, index.length, stdout) == index.length) &&
                 fflush(stdout) == 0)
            status = 0;
        else
            bx_perror_path(&diag, "standard output");
    }
done:
    bx_text_buffer_destroy(&index);
    for (size_t i = 0; i < scan.refs_count; i++)
        free(scan.refs[i].name);
    free(scan.refs);
    for (size_t i = 0; i < scan.count; i++)
        free(scan.modules[i].path);
    free(scan.modules);
    bx_module_symbols_destroy(&scan.exports);
    bx_module_tree_close(&tree);
    return status;
#else
    bx_diag(&diag, "Linux module dependency generation is not supported on this platform");
    return 1;
#endif
}
