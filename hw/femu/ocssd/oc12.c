#include "./oc12.h"

static inline int qemu_fls(int i)
{
    return 32 - clz32(i);
}

static inline bool is_oc12_admin_cmd(uint8_t opcode)
{
    return (opcode == OC12_ADM_CMD_IDENTITY ||
            opcode == OC12_ADM_CMD_GET_L2P_TBL ||
            opcode == OC12_ADM_CMD_GET_BB_TBL ||
            opcode == OC12_ADM_CMD_SET_BB_TBL);
}

static void oc12_tbl_initialize(NvmeNamespace *ns)
{
    uint32_t len = ns->tbl_entries;
    int i;

    for (i = 0; i < len; i++) {
        ns->tbl[i] = OC12_LBA_UNMAPPED;
    }
}

static uint64_t ppa2secidx(Oc12Ctrl *ln, uint64_t ppa)
{
    Oc12IdGroup *c = &ln->id_ctrl.groups[0];
    uint64_t ch, lun, pln, blk, pg, sec;
    uint64_t r;

    ch  = PPA_CH(ln, ppa);
    lun = PPA_LUN(ln, ppa);
    pln = PPA_PLN(ln, ppa);
    blk = PPA_BLK(ln, ppa);
    pg  = PPA_PG(ln, ppa);
    sec = PPA_SEC(ln, ppa);

    r  = sec;
    r += ch * c->num_ch;
    r += lun * c->num_lun;
    r += pln * c->num_pln;
    r += blk * c->num_blk;
    r += pg * c->num_pg;

    if (r > ln->params.total_units) {
        femu_err("Out-of-range PPA detected!"
                 "ch:%lu,lun:%lu,blk:%lu,pg:%lu,pl:%lu,sec:%lu\n", ch, lun, blk,
                 pg, pln, sec);
        return ~(0ULL);
    }

    return r;
}

#ifdef DEBUG_OC12
static void pr_ppa(Oc12Ctrl *ln, uint64_t ppa)
{
    uint64_t ch = PPA_CH(ln, ppa);
    uint64_t lun = PPA_LUN(ln, ppa);
    uint64_t blk = PPA_BLK(ln, ppa);
    uint64_t pg = PPA_PG(ln, ppa);
    uint64_t pln = PPA_PLN(ln, ppa);
    uint64_t sec = PPA_SEC(ln, ppa);

    femu_log("ppa(0x%lx): ch(%lu), lun(%lu), blk(%lu), pg(%lu), pl(%lu),"
             "sec(%lu)\n", ppa, ch, lun, blk, pg, pln, sec);
}
#endif

 /* Write a single out-of-bound (OOB) area entry */
static int oc12_write_oob_meta(Oc12Ctrl *ln, uint64_t ppa, void *meta)
{
    uint64_t sec_idx = ppa2secidx(ln, ppa);
    uint64_t oft = sec_idx * ln->meta_len + ln->int_meta_size;
    uint8_t *tgt_sos_meta_buf = &ln->meta_buf[oft];

    assert(oft + ln->params.sos < ln->meta_tbytes);
    memcpy(tgt_sos_meta_buf, meta, ln->params.sos);

    return 0;
}

/* Read a single out-of-bound (OOB) area entry */
static int oc12_read_oob_meta(Oc12Ctrl *ln, uint64_t ppa, void *meta)
{
    uint64_t sec_idx = ppa2secidx(ln, ppa);
    uint64_t oft = sec_idx * ln->meta_len + ln->int_meta_size;
    uint8_t *tgt_sos_meta_buf = &ln->meta_buf[oft];

    assert(oft + ln->params.sos < ln->meta_tbytes);
    memcpy(meta, tgt_sos_meta_buf, ln->params.sos);

    return 0;
}

static int oc12_meta_state_get(Oc12Ctrl *ln, uint64_t ppa, uint32_t *state)
{
    uint64_t sec_idx = ppa2secidx(ln, ppa);
    uint64_t oft = sec_idx * ln->meta_len;
    uint8_t *tgt_sec_meta_buf = &ln->meta_buf[oft];

    assert(oft + ln->meta_len < ln->meta_tbytes);
    /* Only need the internal oob area */
    memcpy(state, tgt_sec_meta_buf, ln->int_meta_size);

    return 0;
}

/*
 * Similar to oc12_meta_set_written, however, this function sets not a single
 * but multiple ppas, also checks if a block is marked bad
 */
static int oc12_meta_blk_set_erased(NvmeNamespace *ns, Oc12Ctrl *ln,
                                    uint64_t *psl, int nr_ppas)
{
    Oc12IdGroup *c = &ln->id_ctrl.groups[0];
    uint64_t mask = 0;
    uint32_t cur_state, state = OC12_SEC_ERASED;
    int sec, pg;
    int i;

    /* Disable erase state tracking due to its high computational overhead */
    return 0;

    if (ln->strict && nr_ppas != 1) {
        printf("_erase_meta: Erase command unfolds on device\n");
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    int pmode = Oc12PMODE_DUAL;
#if 1
    switch (pmode) {     // Check that pmode is supported
    case Oc12PMODE_DUAL:
        if (c->num_pln != 2) {
            printf("_erase_meta: Unsupported pmode(%d) for num_pln(%d)\n",
                   pmode, c->num_pln);
            return -1;
        }
        break;
    case Oc12PMODE_QUAD:
        if (c->num_pln != 4) {
            printf("_erase_meta: Unsupported pmode(%d) for num_pln(%d)\n",
                   pmode, c->num_pln);
            return -1;
        }
        break;
    case Oc12PMODE_SNGL:
        break;
    default:
        printf("_erase_meta: Unsupported pmode(%d)\n", pmode);
    }
#endif

    mask |= ln->ppaf.ch_mask;   // Construct mask
    mask |= ln->ppaf.lun_mask;
    mask |= ln->ppaf.blk_mask;

    for (i = 0; i < nr_ppas; ++i) {
        uint64_t ppa = psl[i];
        size_t pl_bgn, pl_end;
        size_t pl;

        if (pmode) {
            pl_bgn = 0;
            pl_end = c->num_pln - 1;
        } else {
            pl_bgn = (ppa & ln->ppaf.pln_mask) >> ln->ppaf.pln_offset;
            pl_end = pl_bgn;
        }

        for (pl = pl_bgn; pl <= pl_end; ++pl) {
            //uint32_t cur_state = 0;
            uint64_t ppa_pl;

            ppa_pl = ppa & mask;
            ppa_pl |= pl << ln->ppaf.pln_offset;

#if 0
            /* Check bad-block-table to error on bad blocks */
            if (ns->bbtbl[oc12_bbt_pos_get(ln, ppa_pl)]) {
                printf("_erase_meta: failed -- block is bad\n");
                return -1;
            }
#endif

            /* Check state of first sector to error on double-erase */
            if (oc12_meta_state_get(ln, ppa_pl, &cur_state)) {
                printf("_erase_meta: failed: reading current state\n");
                return -1;
            }
            if (cur_state == OC12_SEC_ERASED) {
                printf("_erase_meta: failed -- already erased\n");
            }

            for (pg = 0; pg < ln->params.pgs_per_blk; ++pg) {
                for (sec = 0; sec < ln->params.sec_per_pg; ++sec) {
                    uint64_t ppa_sec, off;

                    ppa_sec = ppa & mask;
                    ppa_sec |= pg << ln->ppaf.pg_offset;
                    ppa_sec |= pl << ln->ppaf.pln_offset;
                    ppa_sec |= sec << ln->ppaf.sec_offset;

                    //pr_ppa(ln, ppa_sec);
                    off = ppa2secidx(ln, ppa_sec);
                    memcpy(&ln->meta_buf[off * ln->meta_len], &state, ln->int_meta_size);
                }
            }
        }
    }

    return 0;
}

/* Internal metadata to track NAND program/erase status */
static int oc12_meta_state_set_written(Oc12Ctrl *ln, uint64_t ppa)
{
    /* The n-th sector in the flat addr space */
    uint64_t sec_idx = ppa2secidx(ln, ppa);
    uint64_t oft = sec_idx * ln->meta_len;
    uint32_t new_state = OC12_SEC_WRITTEN;
    uint8_t *tgt_sec_meta_buf = &ln->meta_buf[oft];

    assert(oft + ln->meta_len < ln->meta_tbytes);
    /* Make sure it was not already written */
#if 0
    uint32_t cur_state = ((uint32_t *)tgt_sec_meta_buf)[0];
    if (cur_state == OC12_SEC_WRITTEN) {
        femu_err("oc12_meta_state_set_written failed, already written!\n");
        return 1;
    }
#endif

    memcpy(tgt_sec_meta_buf, &new_state, ln->int_meta_size);

    return 0;
}

static void *oc12_meta_index(Oc12Ctrl *ln, void *meta, uint32_t index)
{
    return meta + (index * ln->params.sos);
}

static uint16_t oc12_rw_check_req(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                                  NvmeRequest *req, uint64_t *psl, uint32_t
                                  nr_pages, uint32_t nlb, uint64_t data_size,
                                  uint64_t meta_size)
{
    Oc12Ctrl *ln = n->oc12_ctrl;
    Oc12RwCmd *ocrw = (Oc12RwCmd *)cmd;
    uint64_t spba = le64_to_cpu(ocrw->spba);
    uint64_t slba = psl[0];
    uint64_t elba = psl[nr_pages-1];
    uint16_t is_write = (ocrw->opcode == OC12_CMD_WRITE);
    int i;

    assert(nr_pages == nlb);

    if (nlb > ln->params.max_sec_per_rq) {
        femu_err("oc12_rw: npages too large (%u). Max:%u supported\n", nlb,
                 ln->params.max_sec_per_rq);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if ((is_write) && (nlb < ln->params.sec_per_pl)) {
        femu_err("oc12_rw: I/O does not respect device write constrains."
                 "Sectors send: (%u). Min:%u sectors required\n", nlb,
                 ln->params.sec_per_pl);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (spba == OC12_PBA_UNMAPPED) {
        femu_err("oc12_rw: unmapped PBA\n");
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    for (i = 0; i < nr_pages; i++) {
        if (psl[i] > le64_to_cpu(ns->id_ns.nsze)) {
            return NVME_LBA_RANGE | NVME_DNR;
        }
    }
    if (n->id_ctrl.mdts && data_size > n->page_size * (1 << n->id_ctrl.mdts)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (meta_size) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (!req->is_write && find_next_bit(ns->uncorrectable, elba, slba) < elba) {
        return NVME_UNRECOVERED_READ;
    }

    return 0;
}

static void oc12_read_ppa_list(FemuCtrl *n, Oc12RwCmd *cmd, uint64_t *ppa_list)
{
    uint64_t spba = le64_to_cpu(cmd->spba);
    uint32_t nlb = le16_to_cpu(cmd->nlb) + 1;

    if (nlb > 1) {
        nvme_addr_read(n, spba, (void *)ppa_list, nlb * sizeof(uint64_t));
    } else {
        ppa_list[0] = spba;
    }
}

/*
 * Given the LBA list within a command, get the statistics about the access
 * frequency to different NAND flash pages, this helps us later decide how much
 * latency to emulate for the entire command.
 *
 * Results are stored in @bucket and @n
 */
static void parse_ppa_list(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req, AddrBucket *bucket, int *nr)
{
    Oc12RwCmd *ocrw = (Oc12RwCmd *)cmd;
    Oc12Ctrl *ln = n->oc12_ctrl;
    uint16_t nlb  = le16_to_cpu(ocrw->nlb) + 1;
    int max_sec_per_rq = ln->params.max_sec_per_rq;
    uint64_t cur_pg_addr, prev_pg_addr = ~(0ULL);
    int secs_idx = -1;
    uint64_t ppa;
    int i;

    memset(bucket, 0, sizeof(AddrBucket) * max_sec_per_rq);
    for (i = 0; i < nlb; i++) {
        ppa = ((uint64_t *)(req->slba))[i];
        //pr_ppa(ln, ppa);
        cur_pg_addr = (ppa & (~(ln->ppaf.sec_mask)) & (~(ln->ppaf.pln_mask)));
        if (cur_pg_addr == prev_pg_addr) {
            /* Accessing another secotr in the same NAND page */
            bucket[secs_idx].cnt++;
        } else {
            /* Accessing a new NAND page addr */
            secs_idx++;
            bucket[secs_idx].cnt++;
            bucket[secs_idx].ch = PPA_CH(ln, ppa);
            bucket[secs_idx].lun = PPA_LUN(ln, ppa);
            bucket[secs_idx].pg = PPA_PG(ln, ppa);
            bucket[secs_idx].page_type = get_page_type(n->flash_type, bucket[secs_idx].pg);

            prev_pg_addr = cur_pg_addr;
        }
    }

    *nr = secs_idx + 1;
}

static int oc12_advance_status(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                               NvmeRequest *req)
{
    Oc12RwCmd *ocrw = (Oc12RwCmd *)cmd;
    uint8_t opcode = ocrw->opcode;
    int ch, lun, lunid;
    int64_t io_done_ts = 0;
    int64_t total_time_need_to_emulate = 0;
    int64_t cur_time_need_to_emulate;
    Oc12Ctrl *ln = n->oc12_ctrl;
    Oc12IdGroup *c = &ln->id_ctrl.groups[0];
    int max_sec_per_rq = ln->params.max_sec_per_rq;
    uint8_t page_type;

    int64_t now = req->stime;
    uint64_t ppa;
    int i;

    /* Erase */
    if (opcode == OC12_CMD_ERASE) {
        ppa = req->slba;
        lun = PPA_LUN(ln, ppa);
        ch = PPA_CH(ln, ppa);
        lunid = ch * c->num_ch + lun;

        req->expire_time = advance_chip_timestamp(n, lunid, now, opcode, 0);
        return 0;
    }

    int secs_idx = -1;
    int si = 0;
    int nb_secs_to_write = 0;

    AddrBucket addr_bucket[max_sec_per_rq];
    parse_ppa_list(n, ns, cmd, req, addr_bucket, &secs_idx);

    /* Read & Write */
    assert(opcode == OC12_CMD_READ || opcode == OC12_CMD_WRITE);
    assert(secs_idx > 0);
    for (i = 0; i < secs_idx; i++) {
        ppa = ((uint64_t *)(req->slba))[si];
        nb_secs_to_write = addr_bucket[i].cnt;
        si += nb_secs_to_write;

        ch = addr_bucket[i].ch;
        lun = addr_bucket[i].lun;
        page_type = addr_bucket[i].page_type;
        lunid = ch * c->num_lun + lun;

        io_done_ts = 0;
        assert(ch < c->num_ch && lun < c->num_lun);

        int64_t chnl_end_ts, chip_end_ts;
        if (req->is_write) {
            /* Write data needs to be transferred through the channel first */
            chnl_end_ts = advance_channel_timestamp(n, ch, now, opcode);
            /* Then issue NAND Program to the target flash chip */
            io_done_ts = advance_chip_timestamp(n, lunid, chnl_end_ts, opcode, page_type);
        } else {
            chip_end_ts = advance_chip_timestamp(n, lunid, now, opcode, page_type);
            io_done_ts = advance_channel_timestamp(n, ch, chip_end_ts, opcode);
        }

        /* Coperd: the time need to emulate is (io_done_ts - now) */
        cur_time_need_to_emulate = io_done_ts - now;
        if (cur_time_need_to_emulate > total_time_need_to_emulate) {
            total_time_need_to_emulate = cur_time_need_to_emulate;
        }
    }

    req->expire_time = now + total_time_need_to_emulate;

    return 0;
}

static uint16_t nvme_map_dptr(FemuCtrl *n, size_t len, NvmeRequest *req)
{
    uint64_t prp1, prp2;

    switch (req->cmd.psdt) {
    case NVME_PSDT_PRP:
        prp1 = le64_to_cpu(req->cmd.dptr.prp1);
        prp2 = le64_to_cpu(req->cmd.dptr.prp2);

        return nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, len, n);
    case NVME_PSDT_SGL_MPTR_CONTIGUOUS:
    case NVME_PSDT_SGL_MPTR_SGL:
        return nvme_map_sgl(n, &req->qsg, req->cmd.dptr.sgl, len, &req->cmd);
    default:
        return NVME_INVALID_FIELD;
    }
}

static uint16_t oc12_read(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                          NvmeRequest *req)
{
    Oc12Ctrl *ln = n->oc12_ctrl;
    Oc12RwCmd *ocrw = (Oc12RwCmd *)cmd;
    uint16_t nlb  = le16_to_cpu(ocrw->nlb) + 1;     /* # of logical blocks */
    uint64_t prp1 = le64_to_cpu(ocrw->prp1);        /* PRP1 */
    uint64_t prp2 = le64_to_cpu(ocrw->prp2);        /* PRP2 */
    uint64_t meta = le64_to_cpu(ocrw->metadata);    /* OOB */
    const uint8_t lbaid = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t lbads = NVME_ID_NS_LBAF_DS(ns, lbaid);
    const uint16_t ms = NVME_ID_NS_LBAF_MS(ns, lbaid);
    uint64_t data_size = nlb << lbads;
    uint64_t meta_size = nlb * ms;
    uint64_t *psl;
    uint64_t ppa;
    void *msl;
    uint16_t err;
    int i;

    req->is_write = false;
    req->slba = (uint64_t)g_malloc0(sizeof(uint64_t) * nlb);
    /* To save some ugly type casts later */
    psl = (uint64_t *)req->slba;

    msl = g_malloc0(ln->params.sos * nlb);

    oc12_read_ppa_list(n, ocrw, psl);

    /* Must come after req->slba is correctly read from the host side */
    err = oc12_rw_check_req(n, ns, cmd, req, (uint64_t *)req->slba, nlb, nlb,
                            data_size, meta_size);
    if (err) {
        femu_err("oc12_rw: failed nvme_rw_check (0x%x)\n", err);
        goto fail_free;
    }

    for (i = 0; i < nlb; i++) {
        uint32_t state;
        ppa = psl[i];
        oc12_meta_state_get(ln, ppa, &state);
        if (meta) {
            oc12_read_oob_meta(ln, ppa, oc12_meta_index(ln, msl, i));
        }
        psl[i] = ns->start_block + (ppa << lbads);
    }

    /* DMA OOB metadata back to host first */
    if (meta) {
        nvme_addr_write(n, meta, (void *)msl, nlb * ln->params.sos);
    }

    /* DMA user data */
    if (nvme_map_dptr(n, data_size, req)) {
        femu_err("oc12_read: malformed prp (sz:%lu)\n", data_size);
        err = NVME_INVALID_FIELD | NVME_DNR;
        goto fail_free;
    }
    backend_rw(n->mbe, &req->qsg, psl, req->is_write);

    /* Timing Model */
    oc12_advance_status(n, ns, cmd, req);

    g_free(msl);
    g_free((void *)req->slba);

    return NVME_SUCCESS;

fail_free:
    g_free(msl);
    g_free((void *)req->slba);

    return err;
}

static uint16_t oc12_write(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    Oc12Ctrl *ln = n->oc12_ctrl;
    Oc12RwCmd *ocrw = (Oc12RwCmd *)cmd;
    uint16_t nlb  = le16_to_cpu(ocrw->nlb) + 1;     /* # of logical blocks */
    uint64_t prp1 = le64_to_cpu(ocrw->prp1);        /* PRP1 */
    uint64_t prp2 = le64_to_cpu(ocrw->prp2);        /* PRP2 */
    uint64_t meta = le64_to_cpu(ocrw->metadata);    /* OOB */
    const uint8_t lbaid = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t lbads = NVME_ID_NS_LBAF_DS(ns, lbaid);
    const uint16_t ms = NVME_ID_NS_LBAF_MS(ns, lbaid);
    uint64_t data_size = nlb << lbads;
    uint64_t meta_size = nlb * ms;
    uint64_t *psl;
    uint64_t ppa;
    void *msl;
    uint16_t err;
    int i;

    req->is_write = true;
    req->slba = (uint64_t)g_malloc0(sizeof(uint64_t) * nlb);
    psl = (uint64_t *)req->slba;

    msl = g_malloc0(ln->params.sos * nlb);

    oc12_read_ppa_list(n, ocrw, psl);

    /* Must come after req->slba is correctly read from the host side */

    err = oc12_rw_check_req(n, ns, cmd, req, psl, nlb, nlb, data_size,
                            meta_size);
    if (err) {
        femu_err("oc12_write: failed nvme_rw_check (0x%x)\n", err);
        goto fail_free;
    }

    /* Read host-passed metadata to a temporary buffer */
    if (meta) {
        nvme_addr_read(n, meta, (void *)msl, nlb * ln->params.sos);
    }

    for (i = 0; i < nlb; i++) {
        ppa = psl[i];
        oc12_meta_state_set_written(ln, ppa);
        if (meta) {
            oc12_write_oob_meta(ln, ppa, oc12_meta_index(ln, msl, i));
        }
        psl[i] = ns->start_block + (ppa << lbads);
    }

    /* DMA OOB metadata back to host first */
    if (meta) {
        nvme_addr_write(n, meta, (void *)msl, nlb * ln->params.sos);
    }

    /* DMA user data */
    if (nvme_map_dptr(n, data_size, req)) {
        femu_err("oc12_write: malformed prp (sz:%lu)\n", data_size);
        err = NVME_INVALID_FIELD | NVME_DNR;
        goto fail_free;
    }
    backend_rw(n->mbe, &req->qsg, psl, req->is_write);

    /* Timing Model */
    oc12_advance_status(n, ns, cmd, req);

    g_free(msl);
    g_free((void *)req->slba);

    return NVME_SUCCESS;

fail_free:
    g_free(msl);
    g_free((void *)req->slba);

    return err;
}

static uint32_t oc12_tbl_size(NvmeNamespace *ns)
{
    return ns->tbl_entries * sizeof(*(ns->tbl));
}

static uint16_t oc12_identity(FemuCtrl *n, NvmeCmd *cmd)
{
    NvmeIdentify *c = (NvmeIdentify *)cmd;
    uint32_t nsid = le32_to_cpu(c->nsid);
    uint64_t prp1 = le64_to_cpu(c->prp1);
    uint64_t prp2 = le64_to_cpu(c->prp2);

    if (nsid == 0 || nsid > n->num_namespaces) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    return dma_read_prp(n, (uint8_t *)&n->oc12_ctrl->id_ctrl,
                             sizeof(Oc12IdCtrl), prp1, prp2);
}

static uint16_t oc12_get_l2p_tbl(FemuCtrl *n, NvmeCmd *cmd)
{
    NvmeNamespace *ns;
    Oc12GetL2PTbl *gtbl = (Oc12GetL2PTbl*)cmd;
    uint64_t slba = le64_to_cpu(gtbl->slba);
    uint32_t nlb = le32_to_cpu(gtbl->nlb);
    uint64_t prp1 = le64_to_cpu(gtbl->prp1);
    uint64_t prp2 = le64_to_cpu(gtbl->prp2);
    uint32_t nsid = le32_to_cpu(gtbl->nsid);
    uint64_t xfer_len = nlb * sizeof(*(ns->tbl));

    if (nsid == 0 || nsid > n->num_namespaces) {
        return NVME_INVALID_NSID | NVME_DNR;
    }
    ns = &n->namespaces[nsid - 1];

    if (slba >= ns->tbl_entries) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if ((slba + nlb) > ns->tbl_entries) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (dma_read_prp(n, (uint8_t *)&ns->tbl[slba], xfer_len, prp1, prp2)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static uint16_t oc12_bbt_get(FemuCtrl *n, NvmeCmd *cmd)
{
    NvmeNamespace *ns;
    Oc12Ctrl *ln = n->oc12_ctrl;
    Oc12IdGroup *c = &ln->id_ctrl.groups[0];
    Oc12BbtGet *bbt_cmd = (Oc12BbtGet*)cmd;

    uint32_t nsid = le32_to_cpu(bbt_cmd->nsid);
    uint64_t prp1 = le64_to_cpu(bbt_cmd->prp1);
    uint64_t prp2 = le64_to_cpu(bbt_cmd->prp2);
    uint64_t ppa = le64_to_cpu(bbt_cmd->spba);
    int blks_per_lun = c->num_blk * c->num_pln;
    int lun, ch, lunid;
    Oc12Bbt *bbt;
    int ret = NVME_SUCCESS;

    if (nsid == 0 || nsid > n->num_namespaces) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = &n->namespaces[nsid - 1];
    ch = (ppa & ln->ppaf.ch_mask) >> ln->ppaf.ch_offset;
    lun = (ppa & ln->ppaf.lun_mask) >> ln->ppaf.lun_offset;
    lunid = ch * c->num_lun + lun;
    bbt = ns->bbtbl[lunid];

    if (dma_read_prp(n, (uint8_t*)bbt, sizeof(Oc12Bbt) + blks_per_lun,
                          prp1, prp2)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    return ret;
}

static uint16_t oc12_bbt_set(FemuCtrl *n, NvmeCmd *cmd)
{
    NvmeNamespace *ns;
    Oc12Ctrl *ln = n->oc12_ctrl;
    Oc12IdGroup *c = &ln->id_ctrl.groups[0];
    Oc12BbtSet *bbt_cmd = (Oc12BbtSet *)cmd;

    uint32_t nsid = le32_to_cpu(bbt_cmd->nsid);
    uint64_t prp2 = le64_to_cpu(bbt_cmd->prp2);
    uint32_t nlb  = le16_to_cpu(bbt_cmd->nlb) + 1;
    uint64_t spba = le64_to_cpu(bbt_cmd->spba);
    uint8_t value = bbt_cmd->value;
    uint64_t ppas[ln->params.max_sec_per_rq];
    int ch, lun, lunid, blk;
    int i;

    if (nsid == 0 || nsid > n->num_namespaces) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = &n->namespaces[nsid - 1];

    if (nlb == 1) {
        ppas[0] = spba;
        ch = (ppas[0] & ln->ppaf.ch_mask) >> ln->ppaf.ch_offset;
        lun = (ppas[0] & ln->ppaf.lun_mask) >> ln->ppaf.lun_offset;
        blk = (ppas[0] & ln->ppaf.blk_mask) >> ln->ppaf.blk_offset;
        lunid = ch * c->num_lun + lun;
        ns->bbtbl[lunid]->blk[blk] = value;

    } else {
        if (dma_write_prp(n, (uint8_t *)ppas, nlb * 8, spba, prp2)) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        for (i = 0; i < nlb; i++) {
            ch = (ppas[i] & ln->ppaf.ch_mask) >> ln->ppaf.ch_offset;
            lun = (ppas[i] & ln->ppaf.lun_mask) >> ln->ppaf.lun_offset;
            blk = (ppas[i] & ln->ppaf.blk_mask) >> ln->ppaf.blk_offset;
            lunid = ch * c->num_lun + lun;
            ns->bbtbl[lunid]->blk[blk] = value;
        }
    }

    return NVME_SUCCESS;
}

static int oc12_read_tbls(FemuCtrl *n)
{
    for (uint32_t i = 0; i < n->num_namespaces; i++) {
        NvmeNamespace *ns = &n->namespaces[i];
        uint32_t tbl_size = oc12_tbl_size(ns);
        femu_debug("tbl_size=%d\n", tbl_size);
        assert(tbl_size);
    }

    return 0;
}

static uint16_t oc12_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    case OC12_ADM_CMD_IDENTITY:
        femu_debug("oc12_cmd_identity\n");
        return oc12_identity(n, cmd);
    case OC12_ADM_CMD_GET_L2P_TBL:
        femu_debug("oc12_get_l2p_tbl\n");
        return oc12_get_l2p_tbl(n, cmd);
    case OC12_ADM_CMD_GET_BB_TBL:
        femu_debug("oc12_get_bb_tbl\n");
        return oc12_bbt_get(n, cmd);
    case OC12_ADM_CMD_SET_BB_TBL:
        femu_debug("oc12_set_bb_tbl\n");
        return oc12_bbt_set(n, cmd);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t oc12_erase_async(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                                 NvmeRequest *req)
{
    Oc12Ctrl *ln = n->oc12_ctrl;
    Oc12RwCmd *dm = (Oc12RwCmd *)cmd;
    uint32_t nlb = le16_to_cpu(dm->nlb) + 1;
    uint64_t psl[ln->params.max_sec_per_rq];

    oc12_read_ppa_list(n, dm, psl);

    oc12_meta_blk_set_erased(ns, ln, psl, nlb);

    oc12_advance_status(n, ns, cmd, req);

    return NVME_SUCCESS;
}

static void oc12_init_id_ctrl(Oc12Ctrl *ln)
{
    Oc12IdCtrl *ln_id = &ln->id_ctrl;

    ln_id->ver_id = 1;
    ln_id->dom = 0;
    ln_id->vmnt = 0;
    ln_id->cgrps = 1;
    ln_id->cap = cpu_to_le32(0x3);

    /* Addr format: CHANNEL | LUN | BLOCK | PAGE | PLANE | SECTOR */
    ln_id->ppaf.sect_offset = 0;
    ln_id->ppaf.sect_len    = qemu_fls(cpu_to_le16(ln->params.sec_per_pg) - 1);
    ln_id->ppaf.pln_offset  = ln_id->ppaf.sect_offset + ln_id->ppaf.sect_len;
    ln_id->ppaf.pln_len     = qemu_fls(cpu_to_le16(ln->params.num_pln) - 1);
    ln_id->ppaf.pg_offset   = ln_id->ppaf.pln_offset + ln_id->ppaf.pln_len;
    ln_id->ppaf.pg_len      = qemu_fls(cpu_to_le16(ln->params.pgs_per_blk) - 1);
    ln_id->ppaf.blk_offset  = ln_id->ppaf.pg_offset + ln_id->ppaf.pg_len;
    ln_id->ppaf.blk_len     = qemu_fls(cpu_to_le16(ln->id_ctrl.groups[0].num_blk) - 1);
    ln_id->ppaf.lun_offset  = ln_id->ppaf.blk_offset + ln_id->ppaf.blk_len;
    ln_id->ppaf.lun_len     = qemu_fls(cpu_to_le16(ln->params.num_lun) - 1);
    ln_id->ppaf.ch_offset   = ln_id->ppaf.lun_offset + ln_id->ppaf.lun_len;
    ln_id->ppaf.ch_len      = qemu_fls(cpu_to_le16(ln->params.num_ch) - 1);
}

static int oc12_init_meta(Oc12Ctrl *ln)
{
    /* Internal meta (state: ERASED / WRITTEN) */
    ln->int_meta_size = 4;

    /*
     * Internal meta are the first "ln->int_meta_size" bytes
     * Then comes the tgt_oob_len which is the following ln->param.sos bytes
     */
    ln->meta_len    = ln->int_meta_size + ln->params.sos;
    ln->meta_tbytes = ln->meta_len * ln->params.total_secs;

    /* Coperd: we put all the meta data into this buffer */
    femu_debug("Allocating OOB: %.1f MB\n", ln->meta_tbytes / 1024 / 1024.0);
    ln->meta_buf = g_malloc0(ln->meta_tbytes);

    return 0;
}

static int oc12_init_bbtbl(FemuCtrl *n, NvmeNamespace *ns)
{
    Oc12Ctrl *ln = n->oc12_ctrl;
    Oc12IdGroup *c = &ln->id_ctrl.groups[0];
    uint32_t nr_tt_luns;
    uint32_t blks_per_lun;
    int ret = 0;
    int i;

    nr_tt_luns = c->num_ch * c->num_lun;
    blks_per_lun = c->num_blk * c->num_pln;
    ns->bbtbl = g_malloc0(sizeof(Oc12Bbt *) * nr_tt_luns);

    for (i = 0; i < nr_tt_luns; i++) {
        /* Coperd: init per-lun bbtbl */
        Oc12Bbt *bbt = g_malloc0(sizeof(Oc12Bbt) + blks_per_lun);
        bbt->tblid[0] = 'B';
        bbt->tblid[1] = 'B';
        bbt->tblid[2] = 'L';
        bbt->tblid[3] = 'T';
        bbt->verid    = cpu_to_le16(1);
        bbt->tblks    = cpu_to_le32(blks_per_lun);
        ns->bbtbl[i]  = bbt;
    }

    return ret;
}

static void oc12_release_locks(FemuCtrl *n)
{
    int ret;
    int i;

    for (i = 0; i < FEMU_MAX_NUM_CHNLS; i++) {
        ret = pthread_spin_destroy(&n->chnl_locks[i]);
        assert(ret == 0);
    }

    for (i = 0; i < FEMU_MAX_NUM_CHIPS; i++) {
        ret = pthread_spin_destroy(&n->chip_locks[i]);
        assert(ret == 0);
    }
}

static int oc12_init_misc(FemuCtrl *n)
{
    int ret;
    int i;

	set_latency(n);

    for (i = 0; i < FEMU_MAX_NUM_CHNLS; i++) {
        n->chnl_next_avail_time[i] = 0;

        /* FIXME: Can we use PTHREAD_PROCESS_PRIVATE here? */
        ret = pthread_spin_init(&n->chnl_locks[i], PTHREAD_PROCESS_SHARED);
        assert(ret == 0);
    }

    for (i = 0; i < FEMU_MAX_NUM_CHIPS; i++) {
        n->chip_next_avail_time[i] = 0;

        /* FIXME: Can we use PTHREAD_PROCESS_PRIVATE here? */
        ret = pthread_spin_init(&n->chip_locks[i], PTHREAD_PROCESS_SHARED);
        assert(ret == 0);
    }

    return 0;
}

/* Pass-along the parameters from command line */
static int oc12_init_params(FemuCtrl *n)
{
    Oc12Ctrl *ln;
    Oc12Params *lps;

    ln = n->oc12_ctrl;
    lps = &ln->params;

    lps->sec_size = n->oc_params.sec_size;
    lps->sec_per_pg = n->oc_params.secs_per_pg;
    lps->pgs_per_blk = n->oc_params.pgs_per_blk;
    lps->max_sec_per_rq = n->oc_params.max_sec_per_rq;
    lps->num_ch = n->oc_params.num_ch;
    lps->num_lun = n->oc_params.num_lun;
    lps->num_pln = n->oc_params.num_pln;
    lps->sos = n->oc_params.sos;

    return 0;
}

static int oc12_init_more(FemuCtrl *n)
{
    Oc12Ctrl *ln;
    Oc12IdGroup *c;
    NvmeNamespace *ns;
    struct Oc12IdAddrFormat *ppaf;
    Oc12Params *lps;
    uint64_t chnl_blks;
    int ret = 0;
    int i;

    ln = n->oc12_ctrl = g_malloc0(sizeof(Oc12Ctrl));
    ppaf = &ln->id_ctrl.ppaf;
    lps = &ln->params;

    oc12_init_params(n);

    if (lps->mtype != 0)
        femu_err("FEMU: Only NAND Flash Memory supported at the moment\n");

    if ((lps->num_pln > 4) || (lps->num_pln == 3))
        femu_err("FEMU: Only 1/2/4-plane modes supported\n");

    oc12_init_misc(n);

    for (i = 0; i < n->num_namespaces; i++) {
        ns = &n->namespaces[i];

        chnl_blks = ns->ns_blks / (lps->sec_per_pg * lps->pgs_per_blk *
                                   lps->num_ch);

        femu_debug("chnl_blks=%"PRIu64",ns_blks=%"PRIu64",sec_per_pg=%d,"
                   "pgs_per_blk=%d\n", chnl_blks, ns->ns_blks, lps->sec_per_pg,
                   lps->pgs_per_blk);

        c = &ln->id_ctrl.groups[0];
        c->mtype = lps->mtype;
        c->fmtype = lps->fmtype;
        c->num_ch = lps->num_ch;
        c->num_lun = lps->num_lun;
        c->num_pln = lps->num_pln;

        assert(c->num_ch <= FEMU_MAX_NUM_CHNLS && c->num_lun <= FEMU_MAX_NUM_CHIPS);

        c->num_blk = cpu_to_le16(chnl_blks) / (c->num_lun * c->num_pln);
        c->num_pg = cpu_to_le16(lps->pgs_per_blk);
        c->csecs = cpu_to_le16(lps->sec_size);
        c->fpg_sz = cpu_to_le16(lps->sec_size * lps->sec_per_pg);
        c->sos = cpu_to_le16(lps->sos);

        femu_debug("num_ch=%d,num_lun=%d,num_pln=%d,num_blk=%d,num_pg=%d,"
                   "pg_sz=%d,sos=%d,csecs=%d\n", c->num_ch, c->num_lun,
                   c->num_pln, c->num_blk, c->num_pg, c->fpg_sz, c->sos,
                   c->csecs);

        c->trdt = cpu_to_le32(40000);
        c->trdm = cpu_to_le32(80000);
        c->tprt = cpu_to_le32(1500000);
        c->tprm = cpu_to_le32(3700000);
        c->tbet = cpu_to_le32(6000000);
        c->tbem = cpu_to_le32(20000000);

        switch (c->num_pln) {
        case 1:
            c->mpos = cpu_to_le32(0x10101); /* single plane */
            break;
        case 2:
            c->mpos = cpu_to_le32(0x20202); /* dual plane */
            break;
        case 4:
            c->mpos = cpu_to_le32(0x40404); /* quad plane */
            break;
        default:
            femu_err("Unsupported NAND plane type (%d)\n", c->num_pln);
            return -EINVAL;
        }

        c->cpar = cpu_to_le16(0);
        c->mccap = 1;
        ret = oc12_init_bbtbl(n, ns);
        if (ret)
            return ret;

        /* calculated values */
        lps->sec_per_pl = lps->sec_per_pg * c->num_pln;
        lps->sec_per_blk = lps->sec_per_pl * lps->pgs_per_blk;
        lps->sec_per_lun = lps->sec_per_blk * c->num_blk;
        lps->sec_per_ch = lps->sec_per_lun * c->num_lun;
        lps->total_secs = lps->sec_per_ch * c->num_ch;

        femu_debug("sec_per_pl=%d,sec_per_blk=%d,sec_per_lun=%d,total_secs=%d\n",
                   lps->sec_per_pl, lps->sec_per_blk, lps->sec_per_lun,
                   lps->total_secs);

        /* Calculated unit values for ordering */
        lps->pl_units = lps->sec_per_pg;
        lps->pg_units = lps->pl_units * c->num_pln;
        lps->blk_units = lps->pg_units * lps->pgs_per_blk;
        lps->lun_units = lps->blk_units * c->num_blk;
        lps->ch_units = lps->lun_units * c->num_lun;
        lps->total_units = lps->ch_units * c->num_ch;

        femu_debug("pl_units=%d,pg_units=%d,blk_units=%d,lun_units=%d,"
                   "ch_units=%d,total_units=%d\n", lps->pl_units, lps->pg_units,
                   lps->blk_units, lps->lun_units, lps->ch_units,
                   lps->total_units);

        oc12_init_id_ctrl(ln);

        /* Address format: CH | LUN | BLK | PG | PL | SEC */
        ln->ppaf.sec_offset = ppaf->sect_offset;
        ln->ppaf.pln_offset = ppaf->pln_offset;
        ln->ppaf.pg_offset  = ppaf->pg_offset;
        ln->ppaf.blk_offset = ppaf->blk_offset;
        ln->ppaf.lun_offset = ppaf->lun_offset;
        ln->ppaf.ch_offset  = ppaf->ch_offset;

        /* Address component selection MASK */
        ln->ppaf.sec_mask = ((1 << ppaf->sect_len) - 1) << ln->ppaf.sec_offset;
        ln->ppaf.pln_mask = ((1 << ppaf->pln_len) - 1)  << ln->ppaf.pln_offset;
        ln->ppaf.pg_mask  = ((1 << ppaf->pg_len) - 1)   << ln->ppaf.pg_offset;
        ln->ppaf.blk_mask = ((1 << ppaf->blk_len) - 1)  << ln->ppaf.blk_offset;
        ln->ppaf.lun_mask = ((1 << ppaf->lun_len) -1)   << ln->ppaf.lun_offset;
        ln->ppaf.ch_mask  = ((1 << ppaf->ch_len) - 1)   << ln->ppaf.ch_offset;

        ns->tbl_entries = ns->ns_blks;
        if (ns->tbl) {
            g_free(ns->tbl);
        }
        ns->tbl = qemu_memalign(4096, oc12_tbl_size(ns));
        oc12_tbl_initialize(ns);
    }

    init_nand_flash(n);

    ret = oc12_init_meta(ln);
    if (ret) {
        femu_err("oc12_init_meta failed\n");
        return ret;
    }

    ret = (n->oc12_ctrl->read_l2p_tbl) ? oc12_read_tbls(n) : 0;
    if (ret) {
        femu_err("read_l2p_tbl failed\n");
        return ret;
    }

    return 0;
}

static void oc12_exit(FemuCtrl *n)
{
    oc12_release_locks(n);
}

static uint16_t oc12_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                             NvmeRequest *req)
{
    /* Note: this is not the read/write path for OCSSD */
    return NVME_DNR;
}

static uint16_t oc12_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                            NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        return oc12_nvme_rw(n, ns, cmd, req);
    case OC12_CMD_READ:
        return oc12_read(n, ns, cmd, req);
    case OC12_CMD_WRITE:
        return oc12_write(n, ns, cmd, req);
    case OC12_CMD_ERASE:
        return oc12_erase_async(n, ns, cmd, req);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static void oc12_set_ctrl_str(FemuCtrl *n)
{
    static int fsid_voc12 = 0;
    const char *vocssd12_mn = "FEMU OpenChannel-SSD Controller (v1.2)";
    const char *vocssd12_sn   = "vOCSSD";

    nvme_set_ctrl_name(n, vocssd12_mn, vocssd12_sn, &fsid_voc12);
}

static void oc12_init(FemuCtrl *n, Error **errp)
{
    int i;

    NVME_CAP_SET_OC(n->bar.cap, 1);
    oc12_set_ctrl_str(n);

    for (i = 0; i < n->num_namespaces; i++) {
        NvmeNamespace *ns = &n->namespaces[i];
        NvmeIdNs *id_ns = &ns->id_ns;
        id_ns->vs[0] = 0x1;
    }

    oc12_init_more(n);
}

int nvme_register_ocssd12(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init             = oc12_init,
        .exit             = oc12_exit,
        .rw_check_req     = NULL,
        .admin_cmd        = oc12_admin_cmd,
        .io_cmd           = oc12_io_cmd,
        .get_log          = NULL,
    };

    return 0;
}

