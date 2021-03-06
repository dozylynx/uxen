/*
 * ept-p2m.c: use the EPT page table as p2m
 * Copyright (c) 2007, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 */
/*
 * uXen changes:
 *
 * Copyright 2011-2019, Bromium, Inc.
 * Author: Christian Limpach <Christian.Limpach@gmail.com>
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <xen/config.h>
#include <xen/domain_page.h>
#include <xen/sched.h>
#include <asm/current.h>
#include <asm/paging.h>
#include <asm/types.h>
#include <asm/domain.h>
#include <asm/p2m.h>
#include <asm/hvm/vmx/vmx.h>
#include <asm/hvm/vmx/vmcs.h>
#include <asm/hvm/pv.h>
#include <asm/mtrr.h>
#include <asm/hvm/cacheattr.h>
#include <xen/keyhandler.h>
#include <xen/softirq.h>

#include "mm-locks.h"

/* Override macros from asm/page.h to make them work with mfn_t */
#undef mfn_to_page
#define mfn_to_page(_m) __mfn_to_page(mfn_x(_m))
#undef mfn_valid
#define mfn_valid(_mfn) __mfn_valid(mfn_x(_mfn))
#undef mfn_valid_page
#define mfn_valid_page(_mfn) __mfn_valid_page(mfn_x(_mfn))
#undef mfn_valid_page_or_vframe
#define mfn_valid_page_or_vframe(_mfn) __mfn_valid_page_or_vframe(mfn_x(_mfn))
#undef page_to_mfn
#define page_to_mfn(_pg) _mfn(__page_to_mfn(_pg))

#define atomic_read_ept_entry(__pepte)                              \
    ( (ept_entry_t) { .epte = atomic_read64(&(__pepte)->epte) } )
#define atomic_write_ept_entry(__pepte, __epte)                     \
    atomic_write64(&(__pepte)->epte, (__epte).epte)

#define is_epte_present(ept_entry)      ((ept_entry)->epte & 0x7)
#define is_epte_superpage(ept_entry)    ((ept_entry)->sp)

static void ept_p2m_type_to_flags(ept_entry_t *entry, p2m_type_t type, p2m_access_t access)
{
    /* First apply type permissions */
    switch(type)
    {
        case p2m_invalid:
        case p2m_mmio_dm:
        default:
            entry->r = entry->w = entry->x = 0;
            break;
        case p2m_ram_rw:
            entry->r = entry->w = entry->x = 1;
            break;
        case p2m_mmio_direct:
            entry->r = entry->x = 1;
            entry->w = !rangeset_contains_singleton(mmio_ro_ranges,
                                                    entry->mfn);
            break;
        case p2m_populate_on_demand:
            /* allow reads if a valid mfn is set. */
            entry->r = entry->x = (__mfn_valid_page(entry->mfn) ? 1 : 0);
            entry->w = 0;
            break;
        case p2m_ram_logdirty:
        case p2m_ram_ro:
            entry->r = entry->x = 1;
            entry->w = 0;
            break;
    }


    /* Then restrict with access permissions */
    switch (access) 
    {
        case p2m_access_n:
            entry->r = entry->w = entry->x = 0;
            break;
        case p2m_access_r:
            entry->w = entry->x = 0;
            break;
        case p2m_access_w:
            entry->r = entry->x = 0;
            break;
        case p2m_access_x:
            entry->r = entry->w = 0;
            break;
        case p2m_access_rx:
        case p2m_access_rx2rw:
            entry->w = 0;
            break;
        case p2m_access_wx:
            entry->r = 0;
            break;
        case p2m_access_rw:
            entry->x = 0;
            break;           
        case p2m_access_rwx:
            break;
    }
    
}

#define GUEST_TABLE_MAP_FAILED  0
#define GUEST_TABLE_NORMAL_PAGE 1
#define GUEST_TABLE_SUPER_PAGE  2
#define GUEST_TABLE_POD_PAGE    3

static int ept_next_level(struct p2m_domain *p2m, bool_t read_only,
                          ept_entry_t **table, unsigned long gfn,
                          int next_level);

static ept_entry_t *
ept_map_asr_ptp(struct p2m_domain *p2m)
{
    struct domain *d = p2m->domain;

    if (p2m->pt_page_next) {
        /* asr is always page with index 0, if any pt pages have been
         * allocated from vmi_pt_pages */
        ASSERT(pt_page(d, 0).mfn == ept_get_asr(d));
        perfc_incr(p2m_map_ptp);
        return (ept_entry_t *)pt_page_va(d, 0);
    }

    perfc_incr(p2m_map_ptp_fallback);
    return map_domain_page(ept_get_asr(d));
}

static ept_entry_t *
ept_map_ptp(struct p2m_domain *p2m, ept_entry_t *e)
{
    struct domain *d = p2m->domain;

    do {
        uint16_t idx = e->ptp_idx;

        if (!idx)
            break;

        while (idx < pt_nr_pages(d)) {
            if (pt_page(d, idx).mfn == e->mfn)
                break;
            idx += (1 << PTP_IDX_BITS) - 1;
        }

        /* ensure PTP_IDX_BITS fits in ept_entry */
        BUILD_BUG_ON(PTP_IDX_BITS > 10);

        /* - in debug builds, trigger assert here since map_domain_page
         *   will also trigger an assert in debug builds
         * - in release builds, fallback to map_domain_page */
        ASSERT(idx < pt_nr_pages(d));
        if (idx >= pt_nr_pages(d))
            break;

        perfc_incr(p2m_map_ptp);
        return (ept_entry_t *)pt_page_va(d, idx);
    } while (0);

    perfc_incr(p2m_map_ptp_fallback);
    return map_domain_page(e->mfn);
}

static int
ept_map_ptp_gfn(struct p2m_domain *p2m, bool_t read_only,
                ept_entry_t **_table, unsigned long gfn, int *_target)
{
    struct domain *d = p2m->domain;
    ept_entry_t *table, *entry;
    uint16_t idx;
    unsigned int i;
    int ret = GUEST_TABLE_MAP_FAILED;

    idx = pt_gfn_idx(p2m, gfn, (*_target) + 1);
    if (idx < pt_nr_pages(d) && pt_page(d, idx).present) {
        *_table = (ept_entry_t *)pt_page_va(d, idx);
        return GUEST_TABLE_NORMAL_PAGE;
    }

    idx = pt_gfn_idx(p2m, gfn, (*_target) + 2);
    if (idx < pt_nr_pages(d) && pt_page(d, idx).present) {
        table = (ept_entry_t *)pt_page_va(d, idx);
        entry = table + pt_level_index(gfn, (*_target) + 1);

        if (p2m_is_pod(entry->sa_p2mt)) {
            ASSERT(*_target == 0);
            *_table = table;
            *_target = 1;
            return GUEST_TABLE_POD_PAGE;
        }

        if (is_epte_present(entry)) {
            *_table = ept_map_ptp(p2m, entry);
            return GUEST_TABLE_NORMAL_PAGE;
        }

        ASSERT(!entry->epte);

        if (read_only) {
            *_table = table;
            (*_target)++;
            return GUEST_TABLE_MAP_FAILED;
        }
    }

    if (!read_only)
        printk(XENLOG_DEBUG "%s: vm%u: top down fill gfn %lx level %d\n",
               __FUNCTION__, d->domain_id, gfn, (*_target) + 1);

    table = ept_map_asr_ptp(p2m);
    for (i = ept_get_wl(d); i > *_target; i--) {
        ret = ept_next_level(p2m, read_only, &table, gfn, i);
        if (ret != GUEST_TABLE_NORMAL_PAGE)
            break;
    }

    *_target = i;
    *_table = table;
    return ret;
}

#define ept_ptp_mapped(p2m, va) (({                                     \
                ((uintptr_t)(va) >= pt_page_va((p2m)->domain, 0) &&     \
                 (uintptr_t)(va) < pt_page_va((p2m)->domain,            \
                                              pt_nr_pages((p2m)->domain))); \
            }))

static void
ept_unmap_ptp(struct p2m_domain *p2m, const void *va)
{

    if (!ept_ptp_mapped(p2m, va))
        unmap_domain_page(va);
}

/* Fill in middle levels of ept table */
static int ept_set_middle_entry(struct p2m_domain *p2m, unsigned long gpfn,
                                int level, ept_entry_t *ept_entry)
{
    unsigned long mfn;
    uint16_t idx;

    mfn = p2m_alloc_ptp(p2m, gpfn, level, &idx);
    if (!__mfn_valid(mfn))
        return 0;

    ept_entry->epte = 0;
    ept_entry->mfn = mfn;

    ept_entry->r = ept_entry->w = ept_entry->x = 1;

    ept_entry->ptp_idx = idx;

    return 1;
}

/* free ept sub tree behind an entry */
static void ept_free_entry(struct p2m_domain *p2m, ept_entry_t *ept_entry, int level)
{
    /* End if the entry is a leaf entry. */
    if ( level == 0 || !is_epte_present(ept_entry) ||
         is_epte_superpage(ept_entry) )
        return;

    if ( level > 1 )
    {
        ept_entry_t *epte = ept_map_ptp(p2m, ept_entry);
        for ( int i = 0; i < EPT_PAGETABLE_ENTRIES; i++ )
            ept_free_entry(p2m, epte + i, level - 1);
        ept_unmap_ptp(p2m, epte);
    }
    
    p2m_free_ptp(p2m, ept_entry->mfn, ept_entry->ptp_idx);
}

static int
_ept_split_super_page(struct p2m_domain *p2m, ept_entry_t *ept_entry,
                      unsigned long gpfn, int level, int target)
{
    ept_entry_t new_ept, *table;
    uint64_t trunk;
    int rv = 1;

    /* End if the entry is a leaf entry or reaches the target level. */
    if ( level == 0 || level == target )
        return rv;

    ASSERT(is_epte_superpage(ept_entry));

    if ( !ept_set_middle_entry(p2m, gpfn, level, &new_ept) )
        return 0;

    if (level == 1 &&
        p2m->domain->clone_of &&
        !(p2m->domain->arch.hvm_domain.params[HVM_PARAM_CLONE_L1] &
          (HVM_PARAM_CLONE_L1_lazy_populate | HVM_PARAM_CLONE_L1_dynamic))) {
        struct p2m_domain *op2m = p2m_get_hostp2m(p2m->domain->clone_of);
        rv = !p2m_clone_l1(op2m, p2m, pt_level_mask(gpfn, 1), &new_ept, 0);
        if (rv)
            goto out;
    }

    table = ept_map_ptp(p2m, &new_ept);
    trunk = 1UL << ((level - 1) * EPT_TABLE_ORDER);

    for ( int i = 0; i < EPT_PAGETABLE_ENTRIES; i++ )
    {
        ept_entry_t *epte = table + i;

        epte->epte = 0;
        epte->emt = ept_entry->emt;
        epte->ipat = ept_entry->ipat;
        epte->sp = (level > 1) ? 1 : 0;
        epte->access = ept_entry->access;
        epte->sa_p2mt = ept_entry->sa_p2mt;
        /* when populating a populate on demand superpage, don't
         * "split" the mfn value since it's in most cases 0 or some
         * pod specific value, but not an actual mfn */
        if (!p2m_is_pod(ept_entry->sa_p2mt))
            epte->mfn = ept_entry->mfn + i * trunk;
#ifndef NDEBUG
        /* warn if mfn != 0 in pod case, since currently mfn is always
         * 0 in the superpage pod case */
        else if (ept_entry->mfn)
            WARN_ONCE();
#endif  /* NDEBUG */
        epte->rsvd2_snp = 0;

        ept_p2m_type_to_flags(epte, epte->sa_p2mt, epte->access);

        if ( (level - 1) == target )
            continue;

        ASSERT(is_epte_superpage(epte));

        rv = _ept_split_super_page(p2m, epte, gpfn + i * trunk,
                                   level - 1, target);
        if (!rv)
            break;
    }

    ept_unmap_ptp(p2m, table);

    if (level == 1 &&
        p2m_is_pod(ept_entry->sa_p2mt) &&
        !p2m_is_pod(new_ept.sa_p2mt)) {
        ASSERT(!is_template_domain(p2m->domain));
        atomic_dec(&p2m->domain->clone.l1_pod_pages);
        atomic_add(1 << PAGE_ORDER_2M, &p2m->domain->pod_pages);
    }

  out:
    /* Even failed we should install the newly allocated ept page. */
    *ept_entry = new_ept;

    return rv;
}

static int
ept_split_super_page(struct p2m_domain *p2m, ept_entry_t *ept_entry,
                     unsigned long gpfn, int level, int target)
{
    ept_entry_t split_ept_entry;
    int rv;

    ASSERT(p2m_locked_by_me(p2m));
    ASSERT(is_epte_superpage(ept_entry));

    split_ept_entry = atomic_read_ept_entry(ept_entry);

    rv = _ept_split_super_page(p2m, &split_ept_entry, gpfn, level, target);
    if (!rv) {
        ept_free_entry(p2m, &split_ept_entry, level);
        goto out;
    }

    /* now install the newly split ept sub-tree */
    /* NB: please make sure domian is paused and no in-fly VT-d DMA. */

    pv_split_super_page(p2m, level, gpfn, split_ept_entry, 0);
    atomic_write_ept_entry(ept_entry, split_ept_entry);

  out:
    return rv;
}

static int
ept_split_super_page_one(struct p2m_domain *p2m, void *entry,
                         unsigned long gpfn, int order)
{
    ept_entry_t *ept_entry = (ept_entry_t *)entry;
    int level;

    level = order / PAGETABLE_ORDER;
    if (!level)
        return 1;

    return !ept_split_super_page(p2m, ept_entry, gpfn, level, level - 1);
}

/* Take the currently mapped table, find the corresponding gfn entry,
 * and map the next table, if available.  If the entry is empty
 * and read_only is set, 
 * Return values:
 *  0: Failed to map.  Either read_only was set and the entry was
 *   empty, or allocating a new page failed.
 *  GUEST_TABLE_NORMAL_PAGE: next level mapped normally
 *  GUEST_TABLE_SUPER_PAGE:
 *   The next entry points to a superpage, and caller indicates
 *   that they are going to the superpage level, or are only doing
 *   a read.
 *  GUEST_TABLE_POD:
 *   The next entry is marked populate-on-demand.
 */
static int ept_next_level(struct p2m_domain *p2m, bool_t read_only,
                          ept_entry_t **table, unsigned long gfn,
                          int next_level)
{
    ept_entry_t *ept_entry, e;
    u32 index;

    index = pt_level_index(gfn, next_level);

    /* index must be falling into the page */
    ASSERT(index < EPT_PAGETABLE_ENTRIES);

    ept_entry = (*table) + index;

    /* ept_next_level() is called (sometimes) without a lock.  Read
     * the entry once, and act on the "cached" entry after that to
     * avoid races. */
    e = atomic_read_ept_entry(ept_entry);

    if ( !is_epte_present(&e) )
    {
        if (p2m_is_pod(e.sa_p2mt))
            return GUEST_TABLE_POD_PAGE;

        if ( read_only )
            return GUEST_TABLE_MAP_FAILED;

        if ( !ept_set_middle_entry(p2m, gfn, next_level, ept_entry) )
            return GUEST_TABLE_MAP_FAILED;
        else
            e = atomic_read_ept_entry(ept_entry); /* Refresh */
    }

    /* The only time sp would be set here is if we had hit a superpage */
    if ( is_epte_superpage(&e) )
        return GUEST_TABLE_SUPER_PAGE;

    ept_unmap_ptp(p2m, *table);
    *table = ept_map_ptp(p2m, &e);
    return GUEST_TABLE_NORMAL_PAGE;
}

int
ept_write_entry(struct p2m_domain *p2m, void *table, unsigned long gfn,
                mfn_t mfn, int target, p2m_type_t p2mt, p2m_access_t p2ma,
                int *needs_sync)
{
    struct domain *d = p2m->domain;
    unsigned long index = pt_level_index(gfn, target);
    bool_t mmio = p2m_is_mmio(p2mt);
    uint8_t ipat = 0;
    ept_entry_t *ept_entry = (ept_entry_t *)table + index;
    ept_entry_t old_entry;
    ept_entry_t new_entry = { .epte = 0 };

    /* Read-then-write is OK because we hold the p2m lock. */
    old_entry = *ept_entry;

    if (mfn_valid_page(mfn) || mmio
        || p2m_is_pod(p2mt))
    {
        /* Construct the new entry, and then write it once */
        new_entry.emt = epte_get_entry_emt(d, gfn, mfn, &ipat,
                                           p2m_is_mmio_direct(p2mt));

        new_entry.ipat = ipat;
        new_entry.sp = target ? 1 : 0;
        new_entry.sa_p2mt = p2mt;
        new_entry.access = p2ma;
        new_entry.rsvd2_snp = 0;

        new_entry.mfn = mfn_x(mfn);

        ept_p2m_type_to_flags(&new_entry, p2mt, p2ma);
    }

    /* No need to flush if the old entry wasn't valid */
    if (!is_epte_present(ept_entry))
        *needs_sync = 0;

    atomic_write_ept_entry(ept_entry, new_entry);
    pv_ept_write(p2m, target, gfn, new_entry.epte, *needs_sync);

    if (!target && old_entry.mfn != mfn_x(mfn)) {
        if (mfn_valid_page_or_vframe(mfn) &&
            mfn_x(mfn) != mfn_x(shared_zero_page))
            get_page_fast(mfn_to_page(mfn), NULL);
        if (__mfn_valid_page_or_vframe(old_entry.mfn) &&
            old_entry.mfn != mfn_x(shared_zero_page)) {
            if (old_entry.sa_p2mt == p2m_populate_on_demand)
                put_page_destructor(__mfn_to_page(old_entry.mfn),
                                    p2m_pod_free_page, d, gfn);
            else
                put_page(__mfn_to_page(old_entry.mfn));
        }
    }
    if (!target && old_entry.epte != new_entry.epte)
        p2m_update_pod_counts(d, old_entry.mfn, old_entry.sa_p2mt,
                              new_entry.mfn, new_entry.sa_p2mt);

    /* Track the highest gfn for which we have ever had a valid mapping */
    if (mfn_x(mfn) != INVALID_MFN &&
        (gfn + (1UL << (target * EPT_TABLE_ORDER)) - 1 > p2m->max_mapped_pfn))
        p2m->max_mapped_pfn = gfn + (1UL << (target * EPT_TABLE_ORDER)) - 1;

    return 0;
}

/*
 * ept_set_entry() computes 'need_modify_vtd_table' for itself,
 * by observing whether any gfn->mfn translations are modified.
 */
static int
ept_set_entry(struct p2m_domain *p2m, unsigned long gfn, mfn_t mfn, 
              unsigned int order, p2m_type_t p2mt, p2m_access_t p2ma)
{
    ept_entry_t *table, *ept_entry = NULL;
    u32 index;
    int i, target = order / EPT_TABLE_ORDER;
    int rv = 0;
    int ret = 0;
    int needs_sync = 1;
    struct domain *d = p2m->domain;
    ept_entry_t old_entry = { .epte = 0 };
    union p2m_l1_cache *l1c = &this_cpu(p2m_l1_cache);

    /*
     * the caller must make sure:
     * 1. passing valid gfn and mfn at order boundary.
     * 2. gfn not exceeding guest physical address width.
     * 3. passing a valid order.
     */
    if ((mfn_valid_page(mfn) &&
         ((gfn | mfn_x(mfn)) & ((1UL << order) - 1))) ||
        ((u64)gfn >> ((ept_get_wl(d) + 1) * EPT_TABLE_ORDER)) ||
        (order % EPT_TABLE_ORDER))
        return 0;

    ASSERT((target == 2 && hvm_hap_has_1gb(d)) ||
           (target == 1 && hvm_hap_has_2mb(d)) ||
           (target == 0));

    if (target || !l1c->se_l1.va ||
        p2m_l1_prefix(gfn, p2m) != l1c->se_l1_prefix) {
        l1c->se_l1.va = NULL;

        perfc_incr(p2m_set_entry_walk);
        i = target;
        ret = ept_map_ptp_gfn(p2m, 0, &table, gfn, &i);
        if (!target && !i && ret == GUEST_TABLE_NORMAL_PAGE) {
            int mapped = ept_ptp_mapped(p2m, table);
            l1c->se_l1_prefix = p2m_l1_prefix(gfn, p2m);
            if (mapped)
                l1c->se_l1.va = table;
            else {
                l1c->se_l1.mfn = mapped_domain_page_va_pfn(table);
                l1c->se_l1.is_mfn = 1;
            }
        }
    } else {
        perfc_incr(p2m_set_entry_cached);
        if (!l1c->se_l1.is_mfn) {
            table = l1c->se_l1.va;
            perfc_incr(p2m_map_ptp);
        } else {
            /* use map_domain_page here since we don't have an
             * ept_entry, but we also know that ept_map_ptp will
             * fallback to it anyway, and OK to free via ept_unmap_ptp
             * below */
            table = map_domain_page(l1c->se_l1.mfn);
            perfc_incr(p2m_map_ptp_fallback);
        }
        i = 0;
        ret = GUEST_TABLE_NORMAL_PAGE;
    }

    ASSERT(ret != GUEST_TABLE_POD_PAGE || i != target);

    index = pt_level_index(gfn, i);

    ept_entry = table + index;

    /* Non-l1 update -- invalidate the get_entry cache */
    if (target && is_epte_present(ept_entry))
        p2m_ge_l1_cache_invalidate(p2m, gfn, order);

    if (i > target) {
        /* If we're here with i > target, we must be at a leaf node, and
         * we need to break up the superpage. */
        if (!ept_split_super_page(p2m, ept_entry, gfn, i, target))
            goto out;

        /* then move to the level we want to make real changes */
        for ( ; i > target; i-- )
            ept_next_level(p2m, 0, &table, gfn, i);

        ASSERT(i == target);

        index = pt_level_index(gfn, i);
        ept_entry = table + index;
    }

    /* We reached the target level. */

    /* If we're here with target > 0, we need to check to see
     * if we're replacing a non-leaf entry (i.e., pointing to an N-1 table)
     * with a leaf entry (a 1GiB or 2MiB page), and handle things appropriately.
     */
    /* If we're replacing a non-leaf entry with a leaf entry (1GiB or 2MiB),
     * the intermediate tables will be freed below after the ept flush */
    if (target)
        old_entry = *ept_entry;

    /* No need to flush if new type is logdirty */
    /* XXX Could also skip if old type is logdirty, w/ check that mfn
     * is same, or check in pt_write_entry that only R->W changed */
    if (!target && p2m_is_logdirty(p2mt))
        needs_sync = 0;

    ept_write_entry(p2m, table, gfn, mfn, target, p2mt, p2ma, &needs_sync);

    /* Success */
    rv = 1;

  out:
    ept_unmap_ptp(p2m, table);

    if (needs_sync)
        pt_sync_domain(p2m->domain);

    /* Release the old intermediate tables, if any.  This has to be the
       last thing we do, after the ept_sync_domain() and removal
       from the iommu tables, so as to avoid a potential
       use-after-free. */
    if (is_epte_present(&old_entry)) {
        ASSERT(target);
        ept_free_entry(p2m, &old_entry, target);
    }

    return rv;
}

int
ept_ro_update_l2_entry(struct p2m_domain *p2m, unsigned long gfn,
                       int read_only, int *_need_sync)
{
    ept_entry_t *table = NULL, *ept_entry = NULL;
    u32 index;
    int i, target = 1, order = PAGE_ORDER_2M;
    int rv = 0;
    int ret = 0;
    struct domain *d = p2m->domain;
    ept_entry_t old_entry = { .epte = 0 };
    int need_sync = 0;

    /*
     * the caller must make sure:
     * 1. passing valid gfn and mfn at order boundary.
     * 2. gfn not exceeding guest physical address width.
     * 3. passing a valid order.
     */
    if ( ((gfn) & ((1UL << order) - 1)) ||
         ((u64)gfn >> ((ept_get_wl(d) + 1) * EPT_TABLE_ORDER)) ||
         (order % EPT_TABLE_ORDER) )
        goto out;

    table = ept_map_asr_ptp(p2m);
    for ( i = ept_get_wl(d); i > target; i-- )
    {
        /* have ept_next_level populate (2nd argument == 0) when
         * setting entries read only, i.e. read_only ? 0 : 1,
         * i.e. !read_only */
        ASSERT(!read_only || p2m_locked_by_me(p2m));
        ret = ept_next_level(p2m, !read_only, &table, gfn, i);
        if ( !ret )
            goto out;
        else if ( ret != GUEST_TABLE_NORMAL_PAGE )
            break;
    }

    ASSERT(ret != GUEST_TABLE_POD_PAGE || i != target);

    index = pt_level_index(gfn, i);
    ept_entry = table + index;

    if ( i == target )
    {
        /* We reached the target level. */
        ept_entry_t new_entry;

        /* Read-then-write is OK because we hold the p2m lock. */
        old_entry = *ept_entry;
        new_entry = old_entry;

        new_entry.w = !read_only;

        if (new_entry.w != old_entry.w) {
            atomic_write_ept_entry(ept_entry, new_entry);
            need_sync = *_need_sync && read_only;
            pv_ept_write(p2m, target, gfn, new_entry.epte, need_sync);
        }

        /* Success */
        rv = 1;
    }

  out:
    *_need_sync = need_sync;
    if (table)
        ept_unmap_ptp(p2m, table);

    return rv;
}

static void *
ept_map_l1_table(struct p2m_domain *p2m, unsigned long gpfn,
                 unsigned int *page_order)
{
    ept_entry_t *table = NULL;
    int i;

    if (page_order)
        *page_order = PAGE_ORDER_4K;

    /* This pfn is higher than the highest the p2m map currently holds */
    if (gpfn > p2m->max_mapped_pfn)
        return NULL;

    i = 0;
    ept_map_ptp_gfn(p2m, 1, &table, gpfn, &i);

    if (page_order)
        *page_order = i * EPT_TABLE_ORDER;
    if (i) {
        ept_unmap_ptp(p2m, table);
        table = NULL;
    }
    return table;
}

static void *
ept_map_entry_table(struct p2m_domain *p2m, void *_entry)
{
    ept_entry_t *entry = (ept_entry_t *)_entry;

    if (entry->epte == 0 || !p2m_is_valid(entry->sa_p2mt) ||
        !__mfn_valid_page(entry->mfn))
        return NULL;

    return ept_map_ptp(p2m, entry);
}

static mfn_t
ept_parse_entry(void *table, unsigned long index,
                p2m_type_t *t, p2m_access_t *a)
{
    mfn_t mfn;
    ept_entry_t *ept_entry = (ept_entry_t *)table + index;

    if (ept_entry->epte != 0 && p2m_is_valid(ept_entry->sa_p2mt)) {
        *t = ept_entry->sa_p2mt;
        *a = ept_entry->access;
        mfn = _mfn(ept_entry->mfn);
    } else {
        *t = p2m_mmio_dm;
        *a = p2m_access_n;
        mfn = _mfn(INVALID_MFN);
    }

    return mfn;
}

/* Read ept p2m entries */
static mfn_t ept_get_entry(struct p2m_domain *p2m,
                           unsigned long gfn, p2m_type_t *t, p2m_access_t* a,
                           p2m_query_t q, unsigned int *page_order)
{
    struct domain *d = p2m->domain;
    ept_entry_t *table = NULL;
    int ge_l1_cache_slot = ge_l1_cache_hash(gfn, p2m);
    ept_entry_t *ept_entry;
    u32 index;
    int i;
    int ret = 0;
    mfn_t mfn = _mfn(0);
    union p2m_l1_cache *l1c = &this_cpu(p2m_l1_cache);

    *t = p2m_mmio_dm;
    *a = p2m_access_n;
    if (page_order)
        *page_order = PAGE_ORDER_4K;

    /* This pfn is higher than the highest the p2m map currently holds */
    if ( gfn > p2m->max_mapped_pfn )
        return _mfn(INVALID_MFN);

    /* Should check if gfn obeys GAW here. */

    if (!l1c->ge_l1[ge_l1_cache_slot].va ||
        p2m_l1_prefix(gfn, p2m) != l1c->ge_l1_prefix[ge_l1_cache_slot]) {
        l1c->ge_l1[ge_l1_cache_slot].va = NULL;

        perfc_incr(p2m_get_entry_walk);
      retry:
        i = 0;
        ret = ept_map_ptp_gfn(p2m, 1, &table, gfn, &i);
        if (!ret) {
            if (page_order)
                *page_order = i * EPT_TABLE_ORDER;
            goto out;
        } else if (ret == GUEST_TABLE_POD_PAGE) {
            index = pt_level_index(gfn, i);
            ept_entry = table + index;

            if (q == p2m_query) {
                *t = p2m_populate_on_demand;
                mfn = _mfn(ept_entry->mfn);
                goto out;
            }

            /* Populate this superpage */
            ASSERT(i == 1);

            mfn = p2m_pod_demand_populate(p2m, gfn, PAGE_ORDER_2M, q,
                                          ept_entry);
            if (mfn_x(mfn))
                goto out;
            goto retry;
        }

        if (!i && ret == GUEST_TABLE_NORMAL_PAGE) {
            int mapped = ept_ptp_mapped(p2m, table);
            l1c->ge_l1_prefix[ge_l1_cache_slot] = p2m_l1_prefix(gfn, p2m);
            if (mapped)
                l1c->ge_l1[ge_l1_cache_slot].va = table;
            else {
                l1c->ge_l1[ge_l1_cache_slot].mfn =
                    mapped_domain_page_va_pfn(table);
                l1c->ge_l1[ge_l1_cache_slot].is_mfn = 1;
            }
        }
    } else {
        perfc_incr(p2m_get_entry_cached);
        if (!l1c->ge_l1[ge_l1_cache_slot].is_mfn) {
            table = l1c->ge_l1[ge_l1_cache_slot].va;
            perfc_incr(p2m_map_ptp);
        } else {
            /* use map_domain_page here since we don't have an
             * ept_entry, but we also know that ept_map_ptp will
             * fallback to it anyway, and OK to free via ept_unmap_ptp
             * below */
            table = map_domain_page(l1c->ge_l1[ge_l1_cache_slot].mfn);
            perfc_incr(p2m_map_ptp_fallback);
        }
        i = 0;
        ret = GUEST_TABLE_NORMAL_PAGE;
    }

    index = pt_level_index(gfn, i);
    ept_entry = table + index;

    mfn = _mfn(INVALID_MFN);

    if (is_p2m_zeroing_any(q)) {
        ASSERT(i == 0);
        if (p2m_pod_zero_share(p2m, gfn, q, ept_entry))
            goto out;
        /* set t/a/mfn below */

    } else if (p2m_is_pod(ept_entry->sa_p2mt) &&
               q != p2m_query) {

        if (q == p2m_alloc_r &&
            (d->clone_of || mfn_zero_page(ept_entry->mfn))) {
            *t = p2m_populate_on_demand;
            goto out;
        }

        ASSERT(i == 0);

        mfn = p2m_pod_demand_populate(p2m, gfn, PAGE_ORDER_4K, q, ept_entry);
        if (mfn_x(mfn))
            goto out;
    }

    /* Need to check for all-zeroes because typecode 0 is p2m_ram and an
     * entirely empty entry shouldn't have RAM type. */
    if (ept_entry->epte != 0 && p2m_is_valid(ept_entry->sa_p2mt)) {
        *t = ept_entry->sa_p2mt;
        *a = ept_entry->access;

        mfn = _mfn(ept_entry->mfn);

        if ( i )
        {
            /* 
             * We may meet super pages, and to split into 4k pages
             * to emulate p2m table
             */
            unsigned long split_mfn = mfn_x(mfn) + pt_level_offset(gfn, i);
            mfn = _mfn(split_mfn);
        }

        if ( page_order )
            *page_order = i * EPT_TABLE_ORDER;
    }

out:
    ept_unmap_ptp(p2m, table);
    return mfn_x(mfn) ? mfn : _mfn(INVALID_MFN);
}

/* WARNING: Only caller doesn't care about PoD pages.  So this function will
 * always return 0 for PoD pages, not populate them.  If that becomes necessary,
 * pass a p2m_query_t type along to distinguish. */
static ept_entry_t ept_get_entry_content(struct p2m_domain *p2m,
    unsigned long gfn, int *level)
{
    ept_entry_t *table = ept_map_asr_ptp(p2m);
    ept_entry_t *ept_entry;
    ept_entry_t content = { .epte = 0 };
    u32 index;
    int i;
    int ret=0;

DEBUG();
    /* This pfn is higher than the highest the p2m map currently holds */
    if ( gfn > p2m->max_mapped_pfn )
        goto out;

    for ( i = ept_get_wl(p2m->domain); i > 0; i-- )
    {
        ret = ept_next_level(p2m, 1, &table, gfn, i);
        if ( !ret || ret == GUEST_TABLE_POD_PAGE )
            goto out;
        else if ( ret == GUEST_TABLE_SUPER_PAGE )
            break;
    }

    index = pt_level_index(gfn, i);
    ept_entry = table + index;
    content = *ept_entry;
    *level = i;

 out:
    ept_unmap_ptp(p2m, table);
    return content;
}

void ept_walk_table(struct domain *d, unsigned long gfn)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    ept_entry_t *table = ept_map_asr_ptp(p2m);

    int i;

    gdprintk(XENLOG_ERR, "Walking EPT tables for vm%u gfn %lx\n",
             d->domain_id, gfn);

    /* This pfn is higher than the highest the p2m map currently holds */
    if ( gfn > p2m->max_mapped_pfn )
    {
        gdprintk(XENLOG_ERR, " gfn exceeds max_mapped_pfn %lx\n",
               p2m->max_mapped_pfn);
        goto out;
    }

    for ( i = ept_get_wl(d); i >= 0; i-- )
    {
        ept_entry_t *ept_entry, *next;
        u32 index;

        /* Stolen from ept_next_level */
        index = pt_level_index(gfn, i);
        ept_entry = table + index;

        gdprintk(XENLOG_ERR, " epte %"PRIx64"\n", ept_entry->epte);

        if ( (i == 0) || !is_epte_present(ept_entry) ||
             is_epte_superpage(ept_entry) )
            goto out;
        else
        {
            next = ept_map_ptp(p2m, ept_entry);

            ept_unmap_ptp(p2m, table);

            table = next;
        }
    }

out:
    ept_unmap_ptp(p2m, table);
    return;
}

/*
 * To test if the new emt type is the same with old,
 * return 1 to not to reset ept entry.
 */
static int need_modify_ept_entry(struct p2m_domain *p2m, unsigned long gfn,
                                 mfn_t mfn, uint8_t o_ipat, uint8_t o_emt,
                                 p2m_type_t p2mt)
{
    uint8_t ipat;
    uint8_t emt;
    bool_t direct_mmio = p2m_is_mmio_direct(p2mt);

DEBUG();
    emt = epte_get_entry_emt(p2m->domain, gfn, mfn, &ipat, direct_mmio);

    if ( (emt == o_emt) && (ipat == o_ipat) )
        return 0;

    return 1;
}

void ept_change_entry_emt_with_range(struct domain *d,
                                     unsigned long start_gfn,
                                     unsigned long end_gfn)
{
    unsigned long gfn;
    ept_entry_t e;
    mfn_t mfn;
    int order = 0;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

DEBUG();
    p2m_lock(p2m);
    for ( gfn = start_gfn; gfn <= end_gfn; gfn++ )
    {
        int level = 0;
        uint64_t trunk = 0;

        e = ept_get_entry_content(p2m, gfn, &level);
        if ( !is_epte_present(&e) || !p2m_has_emt(e.sa_p2mt) )
            continue;

        order = 0;
        mfn = _mfn(e.mfn);

        if ( is_epte_superpage(&e) )
        {
            while ( level )
            {
                trunk = (1UL << (level * EPT_TABLE_ORDER)) - 1;
                if ( !(gfn & trunk) && (gfn + trunk <= end_gfn) )
                {
                    /* gfn assigned with 2M or 1G, and the end covers more than
                     * the super page areas.
                     * Set emt for super page.
                     */
                    order = level * EPT_TABLE_ORDER;
                    if ( need_modify_ept_entry(p2m, gfn, mfn, 
                          e.ipat, e.emt, e.sa_p2mt) )
                        ept_set_entry(p2m, gfn, mfn, order, e.sa_p2mt, e.access);
                    gfn += trunk;
                    break;
                }
                level--;
             }
        }
        else /* gfn assigned with 4k */
        {
            if ( need_modify_ept_entry(p2m, gfn, mfn, e.ipat, e.emt, e.sa_p2mt) )
                ept_set_entry(p2m, gfn, mfn, order, e.sa_p2mt, e.access);
        }
    }
    p2m_unlock(p2m);
}

/*
 * Walk the whole p2m table, changing any entries of the old type
 * to the new type.  This is used in hardware-assisted paging to
 * quickly enable or diable log-dirty tracking
 */
static void ept_change_entry_type_page(mfn_t ept_page_mfn, int ept_page_level,
                                       p2m_type_t ot, p2m_type_t nt)
{
    ept_entry_t e, *epte = map_domain_page(mfn_x(ept_page_mfn));

    if (ax_pv_ept) {
        printk(KERN_ERR "AX_PV_EPT: changing page type - leaving to async path\n");
        //FIXME Eventually: ax_pv_ept_write(p2m, target, gfn << PAGE_SHIFT, new_entry, needs_sync);
    }
    if (xen_pv_ept)
        printk(KERN_ERR "XEN_PV_EPT: changing page type - leaving to async path\n");

DEBUG();
    for ( int i = 0; i < EPT_PAGETABLE_ENTRIES; i++ )
    {
        if ( !is_epte_present(epte + i) )
            continue;

        if ( (ept_page_level > 0) && !is_epte_superpage(epte + i) )
            ept_change_entry_type_page(_mfn(epte[i].mfn),
                                       ept_page_level - 1, ot, nt);
        else
        {
            e = atomic_read_ept_entry(&epte[i]);
            if ( e.sa_p2mt != ot )
                continue;

            e.sa_p2mt = nt;
            ept_p2m_type_to_flags(&e, nt, e.access);
            atomic_write_ept_entry(&epte[i], e);
        }
    }

    unmap_domain_page(epte);
}

static void ept_change_entry_type_global(struct p2m_domain *p2m,
                                         p2m_type_t ot, p2m_type_t nt)
{
    struct domain *d = p2m->domain;
DEBUG();
    if ( ept_get_asr(d) == 0 )
        return;

    BUG_ON(ot != nt && (p2m_is_mmio_direct(ot) || p2m_is_mmio_direct(nt)));

    ept_change_entry_type_page(_mfn(ept_get_asr(d)), ept_get_wl(d), ot, nt);
}

void ept_p2m_init(struct p2m_domain *p2m)
{
    p2m->set_entry = ept_set_entry;
    p2m->get_entry = ept_get_entry;
    p2m->map_l1_table = ept_map_l1_table;
    p2m->map_entry_table= ept_map_entry_table;
    p2m->unmap_table = ept_unmap_ptp;
    p2m->parse_entry = ept_parse_entry;
    p2m->write_entry = ept_write_entry;
    p2m->split_super_page_one = ept_split_super_page_one;
    p2m->change_entry_type_global = ept_change_entry_type_global;
    p2m->ro_update_l2_entry = ept_ro_update_l2_entry;

    p2m->p2m_l1_cache_id = p2m->domain->domain_id;
    open_softirq(P2M_L1_CACHE_CPU_SOFTIRQ, p2m_l1_cache_flush_softirq);

    p2m->ptp_idx_bits = PTP_IDX_BITS;

    p2m->virgin = 1;
}

static void ept_dump_p2m_table(unsigned char key)
{
    struct domain *d;
    ept_entry_t *table, *ept_entry;
    mfn_t mfn;
    int order;
    int i;
    int is_pod;
    int ret = 0;
    unsigned long index;
    unsigned long gfn;
    unsigned long record_counter = 0;
    struct p2m_domain *p2m;

    for_each_domain(d)
    {
        if ( !hap_enabled(d) )
            continue;

        p2m = p2m_get_hostp2m(d);
        printk("\nvm%u EPT p2m table: \n", d->domain_id);

        for ( gfn = 0; gfn <= p2m->max_mapped_pfn; gfn += (1 << order) )
        {
            mfn = _mfn(INVALID_MFN);
            table = ept_map_asr_ptp(p2m);
            for ( i = ept_get_wl(d); i > 0; i-- )
            {
                ret = ept_next_level(p2m, 1, &table, gfn, i);
                if ( ret != GUEST_TABLE_NORMAL_PAGE )
                    break;
            }

            order = i * EPT_TABLE_ORDER;

            if ( ret == GUEST_TABLE_MAP_FAILED )
                goto out;

            index = pt_level_index(gfn, i);
            ept_entry = table + index;
            if (p2m_is_valid(ept_entry->sa_p2mt)) {
                p2m_is_pod(ept_entry->sa_p2mt) ?
                ( mfn = _mfn(INVALID_MFN), is_pod = 1 ) :
                ( mfn = _mfn(ept_entry->mfn), is_pod = 0 );

                printk("gfn: %-16lx  mfn: %-16lx  order: %2d  is_pod: %d\n",
                       gfn, mfn_x(mfn), order, is_pod);

                if ( !(record_counter++ % 100) )
                    process_pending_softirqs();
            }
out:
            ept_unmap_ptp(p2m, table);
        }
    }
}

static struct keyhandler ept_p2m_table = {
    .diagnostic = 0,
    .u.fn = ept_dump_p2m_table,
    .desc = "dump ept p2m table"
};

void setup_ept_dump(void)
{
    register_keyhandler('D', &ept_p2m_table);
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
