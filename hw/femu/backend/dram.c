#include "../nvme.h"

/* Coperd: FEMU Memory Backend (mbe) for emulated SSD */

#define MEMORY_RW_LATENCY_4KiB_BY_NS (8096)

int init_dram_backend(SsdDramBackend **mbe, int64_t nbytes)
{
    SsdDramBackend *b = *mbe = g_malloc0(sizeof(SsdDramBackend));

    b->size = nbytes;
    b->logical_space = g_malloc0(nbytes);

    if (mlock(b->logical_space, nbytes) == -1) {
        femu_err("Failed to pin the memory backend to the host DRAM\n");
        g_free(b->logical_space);
        abort();
    }

    return 0;
}

void free_dram_backend(SsdDramBackend *b)
{
    if (b->logical_space) {
        munlock(b->logical_space, b->size);
        g_free(b->logical_space);
    }
}

/* cur_len is as large as possible in every loop, and the max size is 4KiB, the unit is sector size. */
int backend_rw(SsdDramBackend *b, QEMUSGList *qsg, uint64_t *lbal, bool is_write)
{
    int sg_cur_index = 0;
    dma_addr_t sg_cur_byte = 0;
    dma_addr_t cur_addr, cur_len;
    uint64_t mb_oft = lbal[0];
    void *mb = b->logical_space;
    uint64_t total_size = 0;
    uint64_t start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    DMADirection dir = DMA_DIRECTION_FROM_DEVICE;

    if (is_write) {
        dir = DMA_DIRECTION_TO_DEVICE;
    }

    while (sg_cur_index < qsg->nsg) {
        cur_addr = qsg->sg[sg_cur_index].base + sg_cur_byte;
        cur_len = qsg->sg[sg_cur_index].len - sg_cur_byte;
        if (dma_memory_rw(qsg->as, cur_addr, mb + mb_oft, cur_len, dir, MEMTXATTRS_UNSPECIFIED)) {
            error_report("FEMU: dma_memory_rw error");
        }

        sg_cur_byte += cur_len;
        total_size += cur_len;
        if (sg_cur_byte == qsg->sg[sg_cur_index].len) {
            sg_cur_byte = 0;
            ++sg_cur_index;
        }

        if (b->femu_mode == FEMU_OCSSD_MODE) {
            mb_oft = lbal[sg_cur_index];
        } else if (b->femu_mode == FEMU_BBSSD_MODE ||
                   b->femu_mode == FEMU_NOSSD_MODE ||
                   b->femu_mode == FEMU_ZNSSD_MODE) {
            mb_oft += cur_len;
        } else {
            assert(0);
        }
    }

    qemu_sglist_destroy(qsg);

    uint64_t memory_rw_latency = ((total_size + 4 * KiB - 1) / (4 * KiB)) * MEMORY_RW_LATENCY_4KiB_BY_NS;
    while (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - start_ns < memory_rw_latency) { };

    return 0;
}

void backend_print(FILE* f, SsdDramBackend *b, uint64_t addr, uint64_t len, const char *info)
{
    char *mb = b->logical_space;
    char hex[len * 3 + 1];

    for (size_t i = 0; i < len; i++)
    {
        sprintf(hex + i * 2, " %02x", mb[addr + i]);
    }

    hex[len * 3] = '\0';

    fprintf(f, "/* ----------%14s %8s---------- */\n", __func__, info); 
    fprintf(f, "%s\n", hex);
}
