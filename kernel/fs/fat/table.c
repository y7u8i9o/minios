#define KLOG_SUBSYS "fat"
#include "fat.h"
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <errno.h>

uint64_t fat_cluster_off(struct fat_sb *m, uint32_t cluster)
{
    return ((uint64_t)m->data_start + (uint64_t)(cluster - 2) * m->spc) * FAT_SECTOR_SIZE;
}

uint32_t fat_eoc(struct fat_sb *m)
{
    return m->type == 12 ? 0xfff : m->type == 16 ? 0xffff : 0x0fffffff;
}

bool fat_is_eoc(struct fat_sb *m, uint32_t v)
{
    return m->type == 12 ? v >= FAT12_EOC : m->type == 16 ? v >= FAT16_EOC : v >= FAT32_EOC;
}

static uint64_t fat_entry_off(struct fat_sb *m, uint32_t c, int copy)
{
    uint64_t base = ((uint64_t)m->fat_start + (uint64_t)copy * m->fat_sectors) * FAT_SECTOR_SIZE;
    if (m->type == 12)
        return base + c + c / 2;
    return base + (uint64_t)c * (m->type == 16 ? 2 : 4);
}

uint32_t fat_get(struct fat_sb *m, uint32_t c)
{
    if (c < 2 || c >= m->nclusters + 2)
        return fat_eoc(m);
    uint8_t raw[4] = {0};
    if (fat_read(m, fat_entry_off(m, c, 0), raw, m->type == 12 ? 2 : m->type == 16 ? 2 : 4) < 0)
        return fat_eoc(m);
    if (m->type == 12) {
        uint16_t v = (uint16_t)(raw[0] | raw[1] << 8);
        return c & 1 ? v >> 4 : v & 0xfff;
    }
    if (m->type == 16)
        return raw[0] | raw[1] << 8;
    return (raw[0] | raw[1] << 8 | raw[2] << 16 | (uint32_t)raw[3] << 24) & FAT32_MASK;
}

int fat_set(struct fat_sb *m, uint32_t c, uint32_t v)
{
    if (c < 2 || c >= m->nclusters + 2)
        return -EINVAL;
    for (uint32_t copy = 0; copy < m->nfats; copy++) {
        uint64_t off = fat_entry_off(m, c, (int)copy);
        uint8_t raw[4];
        int r;
        if (m->type == 12) {
            r = fat_read(m, off, raw, 2);
            if (r < 0)
                return r;
            if (c & 1) {
                raw[0] = (uint8_t)((raw[0] & 0x0f) | (v << 4));
                raw[1] = (uint8_t)(v >> 4);
            } else {
                raw[0] = (uint8_t)v;
                raw[1] = (uint8_t)((raw[1] & 0xf0) | ((v >> 8) & 0x0f));
            }
            r = fat_write(m, off, raw, 2);
        } else if (m->type == 16) {
            raw[0] = (uint8_t)v;
            raw[1] = (uint8_t)(v >> 8);
            r = fat_write(m, off, raw, 2);
        } else {
            r = fat_read(m, off, raw, 4);
            if (r < 0)
                return r;
            uint32_t old = raw[0] | raw[1] << 8 | raw[2] << 16 | (uint32_t)raw[3] << 24;
            uint32_t nv = (old & ~FAT32_MASK) | (v & FAT32_MASK);
            raw[0] = (uint8_t)nv;
            raw[1] = (uint8_t)(nv >> 8);
            raw[2] = (uint8_t)(nv >> 16);
            raw[3] = (uint8_t)(nv >> 24);
            r = fat_write(m, off, raw, 4);
        }
        if (r < 0)
            return r;
    }
    return 0;
}

uint32_t fat_count_free(struct fat_sb *m)
{
    uint32_t n = 0;
    for (uint32_t c = 2; c < m->nclusters + 2; c++)
        n += fat_get(m, c) == 0;
    return n;
}

static int zero_cluster(struct fat_sb *m, uint32_t c)
{
    static const uint8_t zeros[FAT_SECTOR_SIZE];
    uint64_t off = fat_cluster_off(m, c);
    for (uint32_t s = 0; s < m->spc; s++) {
        int r = fat_write(m, off + (uint64_t)s * FAT_SECTOR_SIZE, zeros, FAT_SECTOR_SIZE);
        if (r < 0)
            return r;
    }
    return 0;
}

uint32_t fat_alloc_cluster(struct fat_sb *m, uint32_t prev)
{
    mutex_lock(&m->lock);
    uint32_t found = 0;
    for (uint32_t n = 0; n < m->nclusters && !found; n++) {
        uint32_t c = m->next_free + n;
        if (c >= m->nclusters + 2)
            c -= m->nclusters;
        if (c < 2)
            c = 2;
        if (fat_get(m, c) == 0)
            found = c;
    }
    if (found) {
        if (fat_set(m, found, fat_eoc(m)) < 0 || zero_cluster(m, found) < 0 ||
            (prev && fat_set(m, prev, found) < 0)) {
            fat_set(m, found, 0);
            found = 0;
        } else {
            m->next_free = found + 1;
            if (m->free_clusters)
                m->free_clusters--;
        }
    }
    mutex_unlock(&m->lock);
    return found;
}

void fat_free_chain(struct fat_sb *m, uint32_t first)
{
    mutex_lock(&m->lock);
    uint32_t c = first;
    uint32_t guard = 0;
    while (c >= 2 && c < m->nclusters + 2 && guard++ <= m->nclusters) {
        uint32_t next = fat_get(m, c);
        fat_set(m, c, 0);
        m->free_clusters++;
        if (c < m->next_free)
            m->next_free = c;
        if (fat_is_eoc(m, next))
            break;
        c = next;
    }
    mutex_unlock(&m->lock);
}

uint32_t fat_cluster_at(struct inode *ino, uint32_t idx, bool alloc)
{
    struct fat_sb *m = fat_of(ino);
    struct fat_inode_info *info = ino->priv;
    if (!info->first_cluster) {
        if (!alloc)
            return 0;
        info->first_cluster = fat_alloc_cluster(m, 0);
        if (!info->first_cluster)
            return 0;
        info->walk_index = 0;
        info->walk_cluster = info->first_cluster;
        fat_inode_flush(ino);
    }
    uint32_t i = 0, c = info->first_cluster;
    if (info->walk_cluster && info->walk_index <= idx) {
        i = info->walk_index;
        c = info->walk_cluster;
    }
    uint32_t guard = 0;
    while (i < idx) {
        uint32_t next = fat_get(m, c);
        if (fat_is_eoc(m, next)) {
            if (!alloc)
                return 0;
            next = fat_alloc_cluster(m, c);
            if (!next)
                return 0;
        } else if (next < 2 || next >= m->nclusters + 2 || guard++ > m->nclusters) {
            klog_error("%s: broken chain from cluster %u", m->dev->name, info->first_cluster);
            return 0;
        }
        c = next;
        i++;
    }
    info->walk_index = i;
    info->walk_cluster = c;
    return c;
}
