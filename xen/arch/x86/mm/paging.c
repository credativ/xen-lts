/******************************************************************************
 * arch/x86/paging.c
 *
 * x86 specific paging support
 * Copyright (c) 2007 Advanced Micro Devices (Wei Huang)
 * Copyright (c) 2007 XenSource Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <xen/init.h>
#include <xen/guest_access.h>
#include <asm/paging.h>
#include <asm/shadow.h>
#include <asm/p2m.h>
#include <asm/hap.h>
#include <asm/event.h>
#include <asm/guest_access.h>
#include <xen/numa.h>
#include <xsm/xsm.h>

#include "mm-locks.h"

/* Printouts */
#define PAGING_PRINTK(_f, _a...)                                     \
    debugtrace_printk("pg: %s(): " _f, __func__, ##_a)
#define PAGING_ERROR(_f, _a...)                                      \
    printk("pg error: %s(): " _f, __func__, ##_a)
#define PAGING_DEBUG(flag, _f, _a...)                                \
    do {                                                             \
        if (PAGING_DEBUG_ ## flag)                                   \
            debugtrace_printk("pgdebug: %s(): " _f, __func__, ##_a); \
    } while (0)

/* Per-CPU variable for enforcing the lock ordering */
DEFINE_PER_CPU(int, mm_lock_level);

/* Override macros from asm/page.h to make them work with mfn_t */
#undef mfn_to_page
#define mfn_to_page(_m) __mfn_to_page(mfn_x(_m))
#undef mfn_valid
#define mfn_valid(_mfn) __mfn_valid(mfn_x(_mfn))
#undef page_to_mfn
#define page_to_mfn(_pg) _mfn(__page_to_mfn(_pg))

/************************************************/
/*              LOG DIRTY SUPPORT               */
/************************************************/

static mfn_t paging_new_log_dirty_page(struct domain *d)
{
    struct page_info *page;

    page = d->arch.paging.alloc_page(d);
    if ( unlikely(page == NULL) )
    {
        d->arch.paging.log_dirty.failed_allocs++;
        return _mfn(INVALID_MFN);
    }

    d->arch.paging.log_dirty.allocs++;

    return page_to_mfn(page);
}

/* Init a new leaf node; returns a mapping or NULL */
static unsigned long *paging_new_log_dirty_leaf(mfn_t mfn)
{
    unsigned long *leaf = NULL;
    if ( mfn_valid(mfn) )
    {
        leaf = map_domain_page(mfn_x(mfn));
        clear_page(leaf);
    }
    return leaf;
}

/* Init a new non-leaf node; returns a mapping or NULL */
static mfn_t *paging_new_log_dirty_node(mfn_t mfn)
{
    int i;
    mfn_t *node = NULL;
    if ( mfn_valid(mfn) )
    {
        node = map_domain_page(mfn_x(mfn));
        for ( i = 0; i < LOGDIRTY_NODE_ENTRIES; i++ )
            node[i] = _mfn(INVALID_MFN);
    }
    return node;
}

/* get the top of the log-dirty bitmap trie */
static mfn_t *paging_map_log_dirty_bitmap(struct domain *d)
{
    if ( likely(mfn_valid(d->arch.paging.log_dirty.top)) )
        return map_domain_page(mfn_x(d->arch.paging.log_dirty.top));
    return NULL;
}

static void paging_free_log_dirty_page(struct domain *d, mfn_t mfn)
{
    d->arch.paging.log_dirty.allocs--;
    d->arch.paging.free_page(d, mfn_to_page(mfn));
}

static int paging_free_log_dirty_bitmap(struct domain *d, int rc)
{
    mfn_t *l4, *l3, *l2;
    int i4, i3, i2;

    paging_lock(d);

    if ( !mfn_valid(d->arch.paging.log_dirty.top) )
    {
        paging_unlock(d);
        return 0;
    }

    if ( !d->arch.paging.preempt.dom )
    {
        memset(&d->arch.paging.preempt.log_dirty, 0,
               sizeof(d->arch.paging.preempt.log_dirty));
        ASSERT(rc <= 0);
        d->arch.paging.preempt.log_dirty.done = -rc;
    }
    else if ( d->arch.paging.preempt.dom != current->domain ||
              d->arch.paging.preempt.op != XEN_DOMCTL_SHADOW_OP_OFF )
    {
        paging_unlock(d);
        return -EBUSY;
    }

    l4 = map_domain_page(mfn_x(d->arch.paging.log_dirty.top));
    i4 = d->arch.paging.preempt.log_dirty.i4;
    i3 = d->arch.paging.preempt.log_dirty.i3;
    rc = 0;

    for ( ; i4 < LOGDIRTY_NODE_ENTRIES; i4++, i3 = 0 )
    {
        if ( !mfn_valid(l4[i4]) )
            continue;

        l3 = map_domain_page(mfn_x(l4[i4]));

        for ( ; i3 < LOGDIRTY_NODE_ENTRIES; i3++ )
        {
            if ( !mfn_valid(l3[i3]) )
                continue;

            l2 = map_domain_page(mfn_x(l3[i3]));

            for ( i2 = 0; i2 < LOGDIRTY_NODE_ENTRIES; i2++ )
                if ( mfn_valid(l2[i2]) )
                    paging_free_log_dirty_page(d, l2[i2]);

            unmap_domain_page(l2);
            paging_free_log_dirty_page(d, l3[i3]);
            l3[i3] = _mfn(INVALID_MFN);

            if ( i3 < LOGDIRTY_NODE_ENTRIES - 1 && hypercall_preempt_check() )
            {
                d->arch.paging.preempt.log_dirty.i3 = i3 + 1;
                d->arch.paging.preempt.log_dirty.i4 = i4;
                rc = -EAGAIN;
                break;
            }
        }

        unmap_domain_page(l3);
        if ( rc )
            break;
        paging_free_log_dirty_page(d, l4[i4]);
        l4[i4] = _mfn(INVALID_MFN);

        if ( i4 < LOGDIRTY_NODE_ENTRIES - 1 && hypercall_preempt_check() )
        {
            d->arch.paging.preempt.log_dirty.i3 = 0;
            d->arch.paging.preempt.log_dirty.i4 = i4 + 1;
            rc = -EAGAIN;
            break;
        }
    }

    unmap_domain_page(l4);

    if ( !rc )
    {
        paging_free_log_dirty_page(d, d->arch.paging.log_dirty.top);
        d->arch.paging.log_dirty.top = _mfn(INVALID_MFN);

        ASSERT(d->arch.paging.log_dirty.allocs == 0);
        d->arch.paging.log_dirty.failed_allocs = 0;

        rc = -d->arch.paging.preempt.log_dirty.done;
        d->arch.paging.preempt.dom = NULL;
    }
    else
    {
        d->arch.paging.preempt.dom = current->domain;
        d->arch.paging.preempt.op = XEN_DOMCTL_SHADOW_OP_OFF;
    }

    paging_unlock(d);

    return rc;
}

int paging_log_dirty_enable(struct domain *d)
{
    int ret;

    if ( paging_mode_log_dirty(d) )
        return -EINVAL;

    domain_pause(d);
    ret = d->arch.paging.log_dirty.enable_log_dirty(d);
    domain_unpause(d);

    return ret;
}

static int paging_log_dirty_disable(struct domain *d, bool_t resuming)
{
    int ret = 1;

    if ( !resuming )
    {
        domain_pause(d);
        /* Safe because the domain is paused. */
        ret = d->arch.paging.log_dirty.disable_log_dirty(d);
        ASSERT(ret <= 0);
    }

    if ( !paging_mode_log_dirty(d) )
    {
        ret = paging_free_log_dirty_bitmap(d, ret);
        if ( ret == -EAGAIN )
            return ret;
    }

    domain_unpause(d);

    return ret;
}

/* Mark a page as dirty */
void paging_mark_dirty(struct domain *d, unsigned long guest_mfn)
{
    unsigned long pfn;
    mfn_t gmfn, new_mfn;
    int changed;
    mfn_t mfn, *l4, *l3, *l2;
    unsigned long *l1;
    int i1, i2, i3, i4;

    gmfn = _mfn(guest_mfn);

    if ( !paging_mode_log_dirty(d) || !mfn_valid(gmfn) ||
         page_get_owner(mfn_to_page(gmfn)) != d )
        return;

    /* We /really/ mean PFN here, even for non-translated guests. */
    pfn = get_gpfn_from_mfn(mfn_x(gmfn));
    /* Shared MFNs should NEVER be marked dirty */
    BUG_ON(paging_mode_translate(d) && SHARED_M2P(pfn));

    /*
     * Values with the MSB set denote MFNs that aren't really part of the
     * domain's pseudo-physical memory map (e.g., the shared info frame).
     * Nothing to do here...
     */
    if ( unlikely(!VALID_M2P(pfn)) )
        return;

    i1 = L1_LOGDIRTY_IDX(pfn);
    i2 = L2_LOGDIRTY_IDX(pfn);
    i3 = L3_LOGDIRTY_IDX(pfn);
    i4 = L4_LOGDIRTY_IDX(pfn);

    /* We can't call paging.alloc_page() with the log-dirty lock held
     * and we almost never need to call it anyway, so assume that we
     * won't.  If we do hit a missing page, we'll unlock, allocate one
     * and start again. */
    new_mfn = _mfn(INVALID_MFN);

again:
    /* Recursive: this is called from inside the shadow code */
    paging_lock_recursive(d);

    l4 = paging_map_log_dirty_bitmap(d);
    if ( unlikely(!l4) )
    {
        l4 = paging_new_log_dirty_node(new_mfn);
        d->arch.paging.log_dirty.top = new_mfn;
        new_mfn = _mfn(INVALID_MFN);
    }
    if ( unlikely(!l4) )
        goto oom;

    mfn = l4[i4];
    if ( !mfn_valid(mfn) )
    {
        l3 = paging_new_log_dirty_node(new_mfn);
        mfn = l4[i4] = new_mfn;
        new_mfn = _mfn(INVALID_MFN);
    }
    else
        l3 = map_domain_page(mfn_x(mfn));
    unmap_domain_page(l4);
    if ( unlikely(!l3) )
        goto oom;

    mfn = l3[i3];
    if ( !mfn_valid(mfn) )
    {
        l2 = paging_new_log_dirty_node(new_mfn);
        mfn = l3[i3] = new_mfn;
        new_mfn = _mfn(INVALID_MFN);
    }
    else
        l2 = map_domain_page(mfn_x(mfn));
    unmap_domain_page(l3);
    if ( unlikely(!l2) )
        goto oom;

    mfn = l2[i2];
    if ( !mfn_valid(mfn) )
    {
        l1 = paging_new_log_dirty_leaf(new_mfn);
        mfn = l2[i2] = new_mfn;
        new_mfn = _mfn(INVALID_MFN);
    }
    else
        l1 = map_domain_page(mfn_x(mfn));
    unmap_domain_page(l2);
    if ( unlikely(!l1) )
        goto oom;

    changed = !__test_and_set_bit(i1, l1);
    unmap_domain_page(l1);
    if ( changed )
    {
        PAGING_DEBUG(LOGDIRTY, 
                     "marked mfn %" PRI_mfn " (pfn=%lx), dom %d\n",
                     mfn_x(gmfn), pfn, d->domain_id);
        d->arch.paging.log_dirty.dirty_count++;
    }

    paging_unlock(d);
    if ( mfn_valid(new_mfn) )
        paging_free_log_dirty_page(d, new_mfn);
    return;

oom:
    paging_unlock(d);
    new_mfn = paging_new_log_dirty_page(d);
    if ( !mfn_valid(new_mfn) )
        /* we've already recorded the failed allocation */
        return;
    goto again;
}


/* Is this guest page dirty? */
int paging_mfn_is_dirty(struct domain *d, mfn_t gmfn)
{
    unsigned long pfn;
    mfn_t mfn, *l4, *l3, *l2;
    unsigned long *l1;
    int rv = 0;

    /* Recursive: this is called from inside the shadow code */
    paging_lock_recursive(d);
    ASSERT(paging_mode_log_dirty(d));

    /* We /really/ mean PFN here, even for non-translated guests. */
    pfn = get_gpfn_from_mfn(mfn_x(gmfn));
    /* Shared pages are always read-only; invalid pages can't be dirty. */
    if ( unlikely(SHARED_M2P(pfn) || !VALID_M2P(pfn)) )
        goto out;

    mfn = d->arch.paging.log_dirty.top;
    if ( !mfn_valid(mfn) )
        goto out;

    l4 = map_domain_page(mfn_x(mfn));
    mfn = l4[L4_LOGDIRTY_IDX(pfn)];
    unmap_domain_page(l4);
    if ( !mfn_valid(mfn) )
        goto out;

    l3 = map_domain_page(mfn_x(mfn));
    mfn = l3[L3_LOGDIRTY_IDX(pfn)];
    unmap_domain_page(l3);
    if ( !mfn_valid(mfn) )
        goto out;

    l2 = map_domain_page(mfn_x(mfn));
    mfn = l2[L2_LOGDIRTY_IDX(pfn)];
    unmap_domain_page(l2);
    if ( !mfn_valid(mfn) )
        goto out;

    l1 = map_domain_page(mfn_x(mfn));
    rv = test_bit(L1_LOGDIRTY_IDX(pfn), l1);
    unmap_domain_page(l1);

out:
    paging_unlock(d);
    return rv;
}


/* Read a domain's log-dirty bitmap and stats.  If the operation is a CLEAN,
 * clear the bitmap and stats as well. */
static int paging_log_dirty_op(struct domain *d,
                               struct xen_domctl_shadow_op *sc,
                               bool_t resuming)
{
    int rv = 0, clean = 0, peek = 1;
    unsigned long pages = 0;
    mfn_t *l4, *l3, *l2;
    unsigned long *l1;
    int i4, i3, i2;

    if ( !resuming )
        domain_pause(d);
    paging_lock(d);

    if ( !d->arch.paging.preempt.dom )
        memset(&d->arch.paging.preempt.log_dirty, 0,
               sizeof(d->arch.paging.preempt.log_dirty));
    else if ( d->arch.paging.preempt.dom != current->domain ||
              d->arch.paging.preempt.op != sc->op )
    {
        paging_unlock(d);
        ASSERT(!resuming);
        domain_unpause(d);
        return -EBUSY;
    }

    clean = (sc->op == XEN_DOMCTL_SHADOW_OP_CLEAN);

    PAGING_DEBUG(LOGDIRTY, "log-dirty %s: dom %u faults=%u dirty=%u\n",
                 (clean) ? "clean" : "peek",
                 d->domain_id,
                 d->arch.paging.log_dirty.fault_count,
                 d->arch.paging.log_dirty.dirty_count);

    sc->stats.fault_count = d->arch.paging.log_dirty.fault_count;
    sc->stats.dirty_count = d->arch.paging.log_dirty.dirty_count;

    if ( guest_handle_is_null(sc->dirty_bitmap) )
        /* caller may have wanted just to clean the state or access stats. */
        peek = 0;

    if ( unlikely(d->arch.paging.log_dirty.failed_allocs) ) {
        printk("%s: %d failed page allocs while logging dirty pages\n",
               __FUNCTION__, d->arch.paging.log_dirty.failed_allocs);
        rv = -ENOMEM;
        goto out;
    }

    l4 = paging_map_log_dirty_bitmap(d);
    i4 = d->arch.paging.preempt.log_dirty.i4;
    i3 = d->arch.paging.preempt.log_dirty.i3;
    pages = d->arch.paging.preempt.log_dirty.done;

    for ( ; (pages < sc->pages) && (i4 < LOGDIRTY_NODE_ENTRIES); i4++, i3 = 0 )
    {
        l3 = (l4 && mfn_valid(l4[i4])) ? map_domain_page(mfn_x(l4[i4])) : NULL;
        for ( ; (pages < sc->pages) && (i3 < LOGDIRTY_NODE_ENTRIES); i3++ )
        {
            l2 = ((l3 && mfn_valid(l3[i3])) ?
                  map_domain_page(mfn_x(l3[i3])) : NULL);
            for ( i2 = 0;
                  (pages < sc->pages) && (i2 < LOGDIRTY_NODE_ENTRIES);
                  i2++ )
            {
                static unsigned long zeroes[PAGE_SIZE/BYTES_PER_LONG];
                unsigned int bytes = PAGE_SIZE;
                l1 = ((l2 && mfn_valid(l2[i2])) ?
                      map_domain_page(mfn_x(l2[i2])) : zeroes);
                if ( unlikely(((sc->pages - pages + 7) >> 3) < bytes) )
                    bytes = (unsigned int)((sc->pages - pages + 7) >> 3);
                if ( likely(peek) )
                {
                    if ( copy_to_guest_offset(sc->dirty_bitmap, pages >> 3,
                                              (uint8_t *)l1, bytes) != 0 )
                    {
                        rv = -EFAULT;
                        goto out;
                    }
                }
                if ( clean && l1 != zeroes )
                    clear_page(l1);
                pages += bytes << 3;
                if ( l1 != zeroes )
                    unmap_domain_page(l1);
            }
            if ( l2 )
                unmap_domain_page(l2);

            if ( i3 < LOGDIRTY_NODE_ENTRIES - 1 && hypercall_preempt_check() )
            {
                d->arch.paging.preempt.log_dirty.i4 = i4;
                d->arch.paging.preempt.log_dirty.i3 = i3 + 1;
                rv = -EAGAIN;
                break;
            }
        }
        if ( l3 )
            unmap_domain_page(l3);

        if ( !rv && i4 < LOGDIRTY_NODE_ENTRIES - 1 &&
             hypercall_preempt_check() )
        {
            d->arch.paging.preempt.log_dirty.i4 = i4 + 1;
            d->arch.paging.preempt.log_dirty.i3 = 0;
            rv = -EAGAIN;
        }
        if ( rv )
            break;
    }
    if ( l4 )
        unmap_domain_page(l4);

    if ( !rv )
    {
        d->arch.paging.preempt.dom = NULL;
        if ( clean )
        {
            d->arch.paging.log_dirty.fault_count = 0;
            d->arch.paging.log_dirty.dirty_count = 0;
        }
    }
    else
    {
        d->arch.paging.preempt.dom = current->domain;
        d->arch.paging.preempt.op = sc->op;
        d->arch.paging.preempt.log_dirty.done = pages;
    }
    paging_unlock(d);

    if ( rv )
    {
        /* Never leave the domain paused on real errors. */
        ASSERT(rv == -EAGAIN);
        return rv;
    }

    if ( pages < sc->pages )
        sc->pages = pages;
    if ( clean )
    {
        /* We need to further call clean_dirty_bitmap() functions of specific
         * paging modes (shadow or hap).  Safe because the domain is paused. */
        d->arch.paging.log_dirty.clean_dirty_bitmap(d);
    }
    domain_unpause(d);
    return rv;

 out:
    d->arch.paging.preempt.dom = NULL;
    paging_unlock(d);
    domain_unpause(d);
    return rv;
}

void paging_log_dirty_range(struct domain *d,
                           unsigned long begin_pfn,
                           unsigned long nr,
                           uint8_t *dirty_bitmap)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int i;
    unsigned long pfn;

    /*
     * Set l1e entries of P2M table to be read-only.
     *
     * On first write, it page faults, its entry is changed to read-write,
     * and on retry the write succeeds.
     *
     * We populate dirty_bitmap by looking for entries that have been
     * switched to read-write.
     */
    /* Cannot do p2m_lock because that is still a spinlock and also tries
     * to lock p2m_lock inside p2m_change_type. Hope that domain being
     * paused is enough...
    p2m_lock(p2m);
     */
    for ( i = 0, pfn = begin_pfn; pfn < begin_pfn + nr; i++, pfn++ )
    {
        p2m_type_t pt;

        pt = p2m_change_type(p2m, pfn, p2m_ram_rw, p2m_ram_logdirty);
        if ( pt == p2m_ram_rw )
            dirty_bitmap[i >> 3] |= (1 << (i & 7));
    }

    /* p2m_unlock(p2m); */

    flush_tlb_mask(&d->domain_dirty_cpumask);
}

/* Note that this function takes three function pointers. Callers must supply
 * these functions for log dirty code to call. This function usually is
 * invoked when paging is enabled. Check shadow_enable() and hap_enable() for
 * reference.
 *
 * These function pointers must not be followed with the log-dirty lock held.
 */
void paging_log_dirty_init(struct domain *d,
                           int    (*enable_log_dirty)(struct domain *d),
                           int    (*disable_log_dirty)(struct domain *d),
                           void   (*clean_dirty_bitmap)(struct domain *d))
{
    d->arch.paging.log_dirty.enable_log_dirty = enable_log_dirty;
    d->arch.paging.log_dirty.disable_log_dirty = disable_log_dirty;
    d->arch.paging.log_dirty.clean_dirty_bitmap = clean_dirty_bitmap;
}

/************************************************/
/*           CODE FOR PAGING SUPPORT            */
/************************************************/
/* Domain paging struct initialization. */
int paging_domain_init(struct domain *d, unsigned int domcr_flags)
{
    int rc;

    if ( (rc = p2m_init(d)) != 0 )
        return rc;

    /* This must be initialized separately from the rest of the
     * log-dirty init code as that can be called more than once and we
     * don't want to leak any active log-dirty bitmaps */
    d->arch.paging.log_dirty.top = _mfn(INVALID_MFN);

    mm_lock_init(&d->arch.paging.lock);

    /* The order of the *_init calls below is important, as the later
     * ones may rewrite some common fields.  Shadow pagetables are the
     * default... */
    shadow_domain_init(d, domcr_flags);

    /* ... but we will use hardware assistance if it's available. */
    if ( hap_enabled(d) )
        hap_domain_init(d);

    return 0;
}

/* vcpu paging struct initialization goes here */
void paging_vcpu_init(struct vcpu *v)
{
    if ( hap_enabled(v->domain) )
        hap_vcpu_init(v);
    else
        shadow_vcpu_init(v);
}


int paging_domctl(struct domain *d, xen_domctl_shadow_op_t *sc,
                  XEN_GUEST_HANDLE(void) u_domctl, bool_t resuming)
{
    int rc;

    if ( unlikely(d == current->domain) )
    {
        gdprintk(XENLOG_INFO, "Tried to do a paging op on itself.\n");
        return -EINVAL;
    }

    if ( unlikely(d->is_dying) )
    {
        gdprintk(XENLOG_INFO, "Ignoring paging op on dying domain %u\n",
                 d->domain_id);
        return 0;
    }

    if ( unlikely(d->vcpu == NULL) || unlikely(d->vcpu[0] == NULL) )
    {
        gdprintk(XENLOG_DEBUG, "Paging op on a domain (%u) with no vcpus\n",
                 d->domain_id);
        return -EINVAL;
    }

    if ( resuming
         ? (d->arch.paging.preempt.dom != current->domain ||
            d->arch.paging.preempt.op != sc->op)
         : (d->arch.paging.preempt.dom &&
            sc->op != XEN_DOMCTL_SHADOW_OP_GET_ALLOCATION) )
    {
        printk(XENLOG_G_DEBUG
               "d%d:v%d: Paging op %#x on Dom%u with unfinished prior op %#x by Dom%u\n",
               current->domain->domain_id, current->vcpu_id,
               sc->op, d->domain_id, d->arch.paging.preempt.op,
               d->arch.paging.preempt.dom
               ? d->arch.paging.preempt.dom->domain_id : DOMID_INVALID);
        return -EBUSY;
    }

    rc = xsm_shadow_control(d, sc->op);
    if ( rc )
        return rc;

    /* Code to handle log-dirty. Note that some log dirty operations
     * piggy-back on shadow operations. For example, when
     * XEN_DOMCTL_SHADOW_OP_OFF is called, it first checks whether log dirty
     * mode is enabled. If does, we disables log dirty and continues with
     * shadow code. For this reason, we need to further dispatch domctl
     * to next-level paging code (shadow or hap).
     */
    switch ( sc->op )
    {

    case XEN_DOMCTL_SHADOW_OP_ENABLE:
        if ( !(sc->mode & XEN_DOMCTL_SHADOW_ENABLE_LOG_DIRTY) )
            break;
        /* Else fall through... */
    case XEN_DOMCTL_SHADOW_OP_ENABLE_LOGDIRTY:
        if ( hap_enabled(d) )
            hap_logdirty_init(d);
        return paging_log_dirty_enable(d);

    case XEN_DOMCTL_SHADOW_OP_OFF:
        if ( (rc = paging_log_dirty_disable(d, resuming)) != 0 )
            return rc;
        break;

    case XEN_DOMCTL_SHADOW_OP_CLEAN:
    case XEN_DOMCTL_SHADOW_OP_PEEK:
        return paging_log_dirty_op(d, sc, resuming);
    }

    /* Here, dispatch domctl to the appropriate paging code */
    if ( hap_enabled(d) )
        return hap_domctl(d, sc, u_domctl);
    else
        return shadow_domctl(d, sc, u_domctl);
}

long paging_domctl_continuation(XEN_GUEST_HANDLE(xen_domctl_t) u_domctl)
{
    struct xen_domctl op;
    struct domain *d;
    int ret;

    if ( copy_from_guest(&op, u_domctl, 1) )
        return -EFAULT;

    if ( op.interface_version != XEN_DOMCTL_INTERFACE_VERSION ||
         op.cmd != XEN_DOMCTL_shadow_op )
        return -EBADRQC;

    d = rcu_lock_domain_by_id(op.domain);
    if ( d == NULL )
        return -ESRCH;

    ret = xsm_domctl(d, op.cmd);
    if ( !ret )
    {
        if ( domctl_lock_acquire() )
        {
            ret = paging_domctl(d, &op.u.shadow_op,
                                guest_handle_cast(u_domctl, void), 1);

            domctl_lock_release();
        }
        else
            ret = -EAGAIN;
    }

    rcu_unlock_domain(d);

    if ( ret == -EAGAIN )
        ret = hypercall_create_continuation(__HYPERVISOR_arch_1,
                                            "h", u_domctl);
    else if ( __copy_field_to_guest(u_domctl, &op, u.shadow_op) )
        ret = -EFAULT;

    return ret;
}

/* Call when destroying a domain */
int paging_teardown(struct domain *d)
{
    int rc;

    if ( hap_enabled(d) )
        hap_teardown(d);
    else
        shadow_teardown(d);

    /* clean up log dirty resources. */
    rc = paging_free_log_dirty_bitmap(d, 0);
    if ( rc == -EAGAIN )
        return rc;

    /* Move populate-on-demand cache back to domain_list for destruction */
    p2m_pod_empty_cache(d);

    return rc;
}

/* Call once all of the references to the domain have gone away */
void paging_final_teardown(struct domain *d)
{
    if ( hap_enabled(d) )
        hap_final_teardown(d);
    else
        shadow_final_teardown(d);

    p2m_final_teardown(d);
}

/* Enable an arbitrary paging-assistance mode.  Call once at domain
 * creation. */
int paging_enable(struct domain *d, u32 mode)
{
    if ( hap_enabled(d) )
        return hap_enable(d, mode | PG_HAP_enable);
    else
        return shadow_enable(d, mode | PG_SH_enable);
}

/* Called from the guest to indicate that a process is being torn down
 * and therefore its pagetables will soon be discarded */
void pagetable_dying(struct domain *d, paddr_t gpa)
{
    struct vcpu *v;

    ASSERT(paging_mode_shadow(d));

    v = d->vcpu[0];
    v->arch.paging.mode->shadow.pagetable_dying(v, gpa);
}

/* Print paging-assistance info to the console */
void paging_dump_domain_info(struct domain *d)
{
    if ( paging_mode_enabled(d) )
    {
        printk("    paging assistance: ");
        if ( paging_mode_shadow(d) )
            printk("shadow ");
        if ( paging_mode_hap(d) )
            printk("hap ");
        if ( paging_mode_refcounts(d) )
            printk("refcounts ");
        if ( paging_mode_log_dirty(d) )
            printk("log_dirty ");
        if ( paging_mode_translate(d) )
            printk("translate ");
        if ( paging_mode_external(d) )
            printk("external ");
        printk("\n");
    }
}

void paging_dump_vcpu_info(struct vcpu *v)
{
    if ( paging_mode_enabled(v->domain) )
    {
        printk("    paging assistance: ");
        if ( paging_mode_shadow(v->domain) )
        {
            if ( v->arch.paging.mode )
                printk("shadowed %u-on-%u\n",
                       v->arch.paging.mode->guest_levels,
                       v->arch.paging.mode->shadow.shadow_levels);
            else
                printk("not shadowed\n");
        }
        else if ( paging_mode_hap(v->domain) && v->arch.paging.mode )
            printk("hap, %u levels\n",
                   v->arch.paging.mode->guest_levels);
        else
            printk("none\n");
    }
}


/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
