/* SPDX-License-Identifier: BSD-2-Clause */
#include "lib/fs_probe.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "lib/fat/compat.h"
#include "lib/swap_format.h"

static int read_at(int fd, void *buf, size_t size, uint64_t offset)
{
    if (offset > INT64_MAX || (uint64_t)(off_t)offset != offset) {
        errno = EOVERFLOW;
        return -1;
    }
    ssize_t n = bx_fat_pread(fd, buf, size, (off_t)offset);
    if (n >= 0 && (size_t)n == size)
        return 0;
    if (n >= 0)
        errno = EIO;
    return -1;
}

static void format_uuid(char *out, const uint8_t *uuid)
{
    static const uint8_t zero_uuid[16];
    static const char hex[] = "0123456789abcdef";
    if (memcmp(uuid, zero_uuid, sizeof(zero_uuid)) == 0)
        return;
    for (size_t i = 0; i < sizeof(zero_uuid); i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            *out++ = '-';
        *out++ = hex[uuid[i] >> 4];
        *out++ = hex[uuid[i] & 0x0f];
    }
    *out = 0;
}

static void fat_label(char *out, const uint8_t *name)
{
    size_t n = 11;
    while (n && name[n - 1] == ' ')
        n--;
    if (memcmp(name, "NO NAME    ", 11) == 0 || memchr(name, 0, n))
        n = 0;
    memcpy(out, name, n);
    out[n] = 0;
}

/* Limit the work on a corrupt chain (including cycles) and on very large
 * directories. A missing directory label can still fall back to the BPB. */
static int fat_root_label(int fd, char *out, uint64_t root, uint32_t root_bytes,
                          uint64_t data, uint64_t table, uint64_t table_bytes,
                          uint32_t cluster_bytes, uint32_t clusters, uint32_t cluster)
{
    const unsigned max_scan = 2 * 1024 * 1024;
    for (unsigned scanned = 0; scanned < max_scan;) {
        if (cluster) {
            if (cluster < 2 || cluster - 2 >= clusters)
                return 0;
            root = data + (uint64_t)(cluster - 2) * cluster_bytes;
            root_bytes = cluster_bytes;
        }
        for (uint32_t pos = 0; pos < root_bytes;) {
            uint8_t chunk[512];
            size_t n = root_bytes - pos;
            if (n > sizeof(chunk))
                n = sizeof(chunk);
            if (scanned == max_scan)
                return 0;
            if (read_at(fd, chunk, n, root + pos) < 0)
                return -1;
            for (size_t i = 0; i < n; i += 32) {
                uint8_t *entry = chunk + i;
                if (entry[0] == 0)
                    return 0;
                /* mkfs can set ARCHIVE alongside VOLUME; an LFN
                 * entry has the other low attribute bits set. */
                if (entry[0] != 0xe5 && (entry[11] & 0x1f) == 0x08) {
                    if (entry[0] == 0x05)
                        entry[0] = 0xe5;
                    fat_label(out, entry);
                    if (*out)
                        return 0;
                }
            }
            scanned += n;
            pos += n;
        }
        if (!cluster || (uint64_t)cluster * 4 + 4 > table_bytes)
            return 0;
        uint8_t next[4];
        if (read_at(fd, next, sizeof(next), table + (uint64_t)cluster * 4) < 0)
            return -1;
        cluster = bx_fat_le32dec(next) & 0x0fffffff;
        if (cluster < 2 || cluster >= 0x0ffffff0)
            return 0;
    }
    return 0;
}

static int probe_fat(int fd, uint64_t bytes, struct bx_fs_signature *fs)
{
    uint8_t bpb[512];
    if (bytes < sizeof(bpb))
        return 0;
    if (read_at(fd, bpb, sizeof(bpb), 0) < 0)
        return -1;
    if (bpb[510] != 0x55 || bpb[511] != 0xaa ||
        (bpb[0] != 0xe9 && !(bpb[0] == 0xeb && bpb[2] == 0x90)))
        return 0;

    uint32_t sector = bx_fat_le16dec(bpb + 11);
    uint32_t spc = bpb[13];
    uint32_t reserved = bx_fat_le16dec(bpb + 14);
    uint32_t fats = bpb[16];
    uint32_t root_bytes = bx_fat_le16dec(bpb + 17) * 32u;
    uint32_t total = bx_fat_le16dec(bpb + 19);
    uint32_t fat_sectors = bx_fat_le16dec(bpb + 22);
    if (sector < 512 || sector > 4096 || !powerof2(sector) ||
        !spc || spc > 128 || !powerof2(spc) || !reserved || !fats || fats > 2 ||
        (bpb[21] != 0xf0 && bpb[21] < 0xf8))
        return 0;
    if (!total)
        total = bx_fat_le32dec(bpb + 32);
    bool fat32 = !fat_sectors;
    if (fat32) {
        if (root_bytes || bx_fat_le16dec(bpb + 42) != 0)
            return 0;
        fat_sectors = bx_fat_le32dec(bpb + 36);
    } else if (!root_bytes) {
        return 0;
    }
    uint64_t root_sector = reserved + (uint64_t)fats * fat_sectors;
    uint64_t data_sector = root_sector + (root_bytes + sector - 1) / sector;
    if (!fat_sectors || data_sector >= total || total > bytes / sector)
        return 0;
    uint32_t clusters = (total - data_sector) / spc;
    uint32_t cluster = fat32 ? bx_fat_le32dec(bpb + 44) : 0;
    uint64_t fat_bytes = (uint64_t)fat_sectors * sector;
    uint64_t entries = (uint64_t)clusters + 2;
    uint64_t min_fat_bytes = fat32 ? entries * 4
        : clusters < 4085 ? (entries * 3 + 1) / 2 : entries * 2;
    if (!clusters || min_fat_bytes > fat_bytes ||
        (fat32 ? clusters > 0x0fffffee || cluster < 2 || cluster - 2 >= clusters
               : clusters >= 65525))
        return 0;
    unsigned active_fat = fat32 && (bpb[40] & 0x80) ? bpb[40] & 0x0f : 0;
    if (active_fat >= fats)
        return 0;

    unsigned extended = fat32 ? 64 : 36;
    if (bpb[extended + 2] == 0x29 || bpb[extended + 2] == 0x28) {
        uint32_t id = bx_fat_le32dec(bpb + extended + 3);
        snprintf(fs->uuid, sizeof(fs->uuid), "%04" PRIX32 "-%04" PRIX32,
                 id >> 16, id & 0xffff);
        if (bpb[extended + 2] == 0x29)
            fat_label(fs->fat_boot_label, bpb + extended + 7);
    }
    if (fat_root_label(fd, fs->label, root_sector * sector, root_bytes,
                       data_sector * sector, reserved * (uint64_t)sector + active_fat * fat_bytes,
                       fat_bytes, spc * sector, clusters, cluster) < 0)
        return -1;
    if (!fs->label[0])
        memcpy(fs->label, fs->fat_boot_label, sizeof(fs->fat_boot_label));
    fs->type = "vfat";
    fs->block_size = sector;
    return 1;
}

/* ext4 uses CRC32C with an all-ones seed and no final complement for the
 * superblock, independent of the volume's metadata checksum seed. */
static uint32_t ext_super_crc(const uint8_t *sb, size_t len)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; i++) {
        crc ^= sb[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0x82f63b78) : 0);
    }
    return crc;
}

static int probe_ext(int fd, uint64_t bytes, struct bx_fs_signature *fs)
{
    uint8_t sb[1024];
    if (bytes < 1024 + sizeof(sb))
        return 0;
    if (read_at(fd, sb, sizeof(sb), 1024) < 0)
        return -1;
    if (bx_fat_le16dec(sb + 56) != 0xef53)
        return 0;

    uint32_t log_size = bx_fat_le32dec(sb + 24);
    uint32_t compat = bx_fat_le32dec(sb + 92);
    uint32_t incompat = bx_fat_le32dec(sb + 96);
    uint32_t ro_compat = bx_fat_le32dec(sb + 100);
    uint64_t blocks = bx_fat_le32dec(sb + 4);
    if (incompat & 0x80) /* 64BIT */
        blocks |= (uint64_t)bx_fat_le32dec(sb + 0x150) << 32;
    if (log_size > 6 || !blocks || !bx_fat_le32dec(sb) ||
        !bx_fat_le32dec(sb + 32) || !bx_fat_le32dec(sb + 40) ||
        (incompat & 0x08)) /* JOURNAL_DEV: not a mountable filesystem */
        return 0;
    unsigned block_size = 1024u << log_size;
    if (blocks > bytes / block_size)
        return 0;
    if ((ro_compat & 0x400) && /* METADATA_CSUM */
        (sb[0x175] != 1 || ext_super_crc(sb, 1020) != bx_fat_le32dec(sb + 1020)))
        return 0;

    /* FILETYPE, RECOVER and META_BG plus the first three ro-compat bits
     * predate ext4. HAS_JOURNAL distinguishes ext3 from ext2. */
    fs->type = (incompat & ~UINT32_C(0x16)) || (ro_compat & ~UINT32_C(0x07))
        ? "ext4" : (compat & 0x04) ? "ext3" : "ext2";
    fs->block_size = block_size;
    memcpy(fs->label, sb + 120, 16);
    format_uuid(fs->uuid, sb + 104);
    return 1;
}

/* ECMA-119 stores these fields twice, in little- then big-endian order. */
static bool iso_dual(const uint8_t *p, unsigned width, uint32_t *value)
{
    uint32_t little = 0, big = 0;
    for (unsigned i = 0; i < width; i++) {
        little = (little << 8) | p[width - 1 - i];
        big = (big << 8) | p[width + i];
    }
    *value = little;
    return little == big;
}

static bool iso_valid_date(const uint8_t *date)
{
    bool nonzero = false;
    for (unsigned i = 0; i < 16; i++) {
        if (date[i] < '0' || date[i] > '9')
            return false;
        nonzero |= date[i] != '0';
    }
    return nonzero;
}

static int probe_iso9660(int fd, uint64_t bytes, struct bx_fs_signature *fs)
{
    enum { SECTOR = 2048, FIRST_DESCRIPTOR = 16, MAX_DESCRIPTORS = 64 };
    uint8_t pvd[SECTOR];
    uint64_t offset = (uint64_t)FIRST_DESCRIPTOR * SECTOR;

    /* A boot descriptor can precede the primary one. Bound the work
     * on media with no primary descriptor or terminator. */
    unsigned i;
    for (i = 0; i < MAX_DESCRIPTORS; i++, offset += SECTOR) {
        if (offset + SECTOR > bytes)
            return 0;
        if (read_at(fd, pvd, sizeof(pvd), offset) < 0)
            return -1;
        if (memcmp(pvd + 1, "CD001", 5) != 0 || pvd[0] == 255)
            return 0;
        if (pvd[0] == 1)
            break;
    }
    if (i == MAX_DESCRIPTORS || pvd[6] != 1 || pvd[881] != 1)
        return 0;

    uint32_t blocks, block_size, root_extent, root_size;
    const uint8_t *root = pvd + 156;
    if (!iso_dual(pvd + 80, 4, &blocks) ||
        !iso_dual(pvd + 128, 2, &block_size) ||
        (block_size != 512 && block_size != 1024 && block_size != 2048))
        return 0;
    uint64_t volume_size = (uint64_t)blocks * block_size;
    if (volume_size < offset + SECTOR || volume_size > bytes ||
        root[0] != 34 || !(root[25] & 2) || root[32] != 1 || root[33] != 0 ||
        !iso_dual(root + 2, 4, &root_extent) || !iso_dual(root + 10, 4, &root_size) ||
        !root_size || root_extent >= blocks ||
        ((uint64_t)root_extent + root[1]) * block_size + root_size > volume_size)
        return 0;

    /* The primary descriptor's 32-byte volume ID is space padded. */
    size_t len = 32;
    while (len && pvd[40 + len - 1] == ' ')
        len--;
    if (!memchr(pvd + 40, 0, len))
        memcpy(fs->label, pvd + 40, len);

    /* Conventional ISO UUID from modification time, or creation time
     * when it is absent. The final byte of each date is a zone offset. */
    const uint8_t *date = pvd + 830;
    if (!iso_valid_date(date))
        date = pvd + 813;
    if (iso_valid_date(date)) {
        size_t pos = 0;
        for (unsigned n = 0; n < 16; n++) {
            if (n >= 4 && (n % 2) == 0)
                fs->uuid[pos++] = '-';
            fs->uuid[pos++] = (char)date[n];
        }
    }
    fs->block_size = block_size;
    fs->type = "iso9660";
    return 1;
}

static uint32_t swap_u32(const uint8_t *value, bool little)
{
    if (little)
        return bx_fat_le32dec(value);
    return (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 |
           (uint32_t)value[2] << 8 | value[3];
}

static int probe_swap(int fd, uint64_t bytes, struct bx_fs_signature *fs)
{
    enum {
        HEADER_LEN = BX_SWAP_LABEL_OFFSET + BX_SWAP_LABEL_BYTES - BX_SWAP_VERSION_OFFSET,
        MAGIC_LEN = sizeof(BX_SWAP_V1_MAGIC) - 1,
    };
    uint8_t header[HEADER_LEN];
    if (bytes < 2 * BX_SWAP_MIN_PAGE_SIZE)
        return 0;
    if (read_at(fd, header, sizeof(header), BX_SWAP_VERSION_OFFSET) < 0)
        return -1;

    /* The numeric fields use the byte order of the machine that made
     * the header. The swap UUID is stored as bytes in either case. */
    bool little = swap_u32(header, true) == 1;
    if (!little && swap_u32(header, false) != 1)
        return 0;
    uint32_t last = swap_u32(header + (BX_SWAP_LAST_PAGE_OFFSET - BX_SWAP_VERSION_OFFSET), little);
    uint32_t bad = swap_u32(header + (BX_SWAP_BADPAGE_COUNT_OFFSET - BX_SWAP_VERSION_OFFSET), little);
    if (!last || bad > last)
        return 0;

    int matches = 0;
    for (unsigned page = BX_SWAP_MIN_PAGE_SIZE; page <= BX_SWAP_MAX_PAGE_SIZE; page *= 2) {
        if ((uint64_t)last + 1 > bytes / page)
            break;
        if (bad > (page - MAGIC_LEN - BX_SWAP_BADPAGES_OFFSET) / sizeof(uint32_t))
            continue;
        uint8_t magic[MAGIC_LEN];
        if (read_at(fd, magic, sizeof(magic), page - MAGIC_LEN) < 0)
            return -1;
        if (memcmp(magic, BX_SWAP_V1_MAGIC, sizeof(magic)) == 0 && ++matches > 1)
            return 2;
    }
    if (!matches)
        return 0;
    fs->type = "swap";
    format_uuid(fs->uuid, header + (BX_SWAP_UUID_OFFSET - BX_SWAP_VERSION_OFFSET));
    memcpy(fs->label, header + (BX_SWAP_LABEL_OFFSET - BX_SWAP_VERSION_OFFSET), BX_SWAP_LABEL_BYTES);
    return 1;
}

int bx_fs_probe(int fd, unsigned usages, struct bx_fs_signature *result)
{
    uint64_t bytes;
    unsigned sector;
    if (bx_fat_geometry(fd, &bytes, &sector) < 0)
        return -1;

    static const struct {
        enum bx_probe_usage usage;
        int (*probe)(int, uint64_t, struct bx_fs_signature *);
    } probes[] = {
        {BX_PROBE_FILESYSTEM, probe_fat},
        {BX_PROBE_FILESYSTEM, probe_ext},
        {BX_PROBE_FILESYSTEM, probe_iso9660},
        {BX_PROBE_OTHER, probe_swap},
    };
    struct bx_fs_signature found;
    bool matched = false;
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        if (!(usages & probes[i].usage))
            continue;
        struct bx_fs_signature candidate = {.usage = probes[i].usage};
        int match = probes[i].probe(fd, bytes, &candidate);
        if (match < 0)
            return -1;
        if (!match)
            continue;
        if (matched || match > 1)
            return 2;
        found = candidate;
        matched = true;
    }
    if (matched)
        *result = found;
    return matched;
}
