/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 */

#include <linux/rmap.h>
#include <linux/ksm.h>
#include <linux/writeback.h>
#include <linux/mmu_notifier.h>
#include <linux/mmu_context.h>
#include <asm-generic/cacheflush.h>
#include "../../../mm/internal.h"

#include "hproc.h"


#include "ioctl.h"
#include "trace.h"
#include "struct.h"
#include "pull.h"
#include "push.h"
#include "base.h"
#include "ops.h"
#include "conn.h"
#include "base.h"

static struct kmem_cache *heca_delayed_fault_cache_kmem;
unsigned long zero_heca_pfn __read_mostly;

void init_heca_prefetch_cache_kmem(void)
{
        heca_delayed_fault_cache_kmem = kmem_cache_create("heca_delayed_fault_cache",
                        sizeof(struct heca_delayed_fault), 0,
                        SLAB_HWCACHE_ALIGN | SLAB_TEMPORARY, NULL);
}

void destroy_heca_prefetch_cache_kmem(void)
{
        kmem_cache_destroy(heca_delayed_fault_cache_kmem);
}

static struct heca_delayed_fault *alloc_heca_delayed_fault_cache_elm(
                unsigned long addr)
{
        struct heca_delayed_fault *hdf = kmem_cache_alloc(
                        heca_delayed_fault_cache_kmem, GFP_KERNEL);
        if (unlikely(!hdf))
                goto out;

        hdf->addr = addr;

out:
        return hdf;
}

static void free_heca_delayed_fault_cache_elm(struct heca_delayed_fault ** hdf)
{
        kmem_cache_free(heca_delayed_fault_cache_kmem, *hdf);
        *hdf = NULL;
}

int heca_zero_pfn_init(void)
{
        zero_heca_pfn = page_to_pfn(ZERO_PAGE(0));
        return 0;
}

void heca_zero_pfn_exit(void)
{
        zero_heca_pfn = 0;
}

static inline int is_heca_zero_pfn(unsigned long pfn)
{
        return pfn == zero_heca_pfn;
}

static int reuse_heca_page(struct page *page, unsigned long addr,
                struct heca_page_cache *hpc)
{
        int count;

        VM_BUG_ON(!PageLocked(page));
        if (unlikely(PageKsm(page)))
                return 0;

        count = page_mapcount(page);
        if (count == 0 && !PageWriteback(page)) {
                hpc->released = 1;
                if (!PageSwapBacked(page))
                        SetPageDirty(page);
        }

        return count <= 1;
}

static inline void cow_user_page(struct page *dst, struct page *src,
                unsigned long va, struct vm_area_struct *vma)
{
        if (unlikely(!src)) {
                void *kaddr = kmap_atomic(dst);
                void __user *uaddr = (void __user *) (va & PAGE_MASK);

                if (__copy_from_user_inatomic(kaddr, uaddr, PAGE_SIZE))
                        clear_page(kaddr);
                kunmap_atomic(kaddr);
                flush_dcache_page(dst);
        } else
                copy_user_highpage(dst, src, va, vma);
}

static int do_wp_heca_page(struct mm_struct *mm, struct vm_area_struct *vma,
                unsigned long address, pte_t *page_table, pmd_t *pmd,
                spinlock_t *ptl, pte_t orig_pte, unsigned long norm_address,
                struct heca_page_cache *hpc)
__releases(ptl)
{
        struct page *old_page, *new_page;
        pte_t entry;
        int ret = 0;
        int page_mkwrite = 0;
        struct page *dirty_page = NULL;
        unsigned long mmun_start = 0, mmun_end = 0;

        old_page = vm_normal_page(vma, address, orig_pte);
        if (!old_page) {
                if ((vma->vm_flags & (VM_WRITE | VM_SHARED))
                                == (VM_WRITE | VM_SHARED))
                        goto reuse;
                goto gotten;
        }

        if (PageAnon(old_page) && !PageKsm(old_page)) {
                if (!trylock_page(old_page)) {
                        page_cache_get(old_page);
                        pte_unmap_unlock(page_table, ptl);
                        lock_page(old_page);
                        page_table = pte_offset_map_lock(mm, pmd,
                                        address, &ptl);
                        if (!pte_same(*page_table, orig_pte)) {
                                unlock_page(old_page);
                                goto unlock;
                        }
                        page_cache_release(old_page);
                }
                if (reuse_heca_page(old_page, norm_address, hpc)) {
                        page_move_anon_rmap(old_page, vma, address);
                        unlock_page(old_page);
                        goto reuse;
                }
                unlock_page(old_page);
        } else if (unlikely((vma->vm_flags & (VM_WRITE | VM_SHARED))
                                == (VM_WRITE | VM_SHARED))) {

                if (vma->vm_ops && vma->vm_ops->page_mkwrite) {
                        struct vm_fault vmf;
                        int tmp;

                        vmf.virtual_address = (void __user *) (address & PAGE_MASK);
                        vmf.pgoff = old_page->index;
                        vmf.flags = FAULT_FLAG_WRITE | FAULT_FLAG_MKWRITE;
                        vmf.page = old_page;

                        page_cache_get(old_page);
                        pte_unmap_unlock(page_table, ptl);

                        tmp = vma->vm_ops->page_mkwrite(vma, &vmf);
                        if (unlikely(tmp & (VM_FAULT_ERROR | VM_FAULT_NOPAGE))){
                                ret = tmp;
                                goto unwritable_page;
                        }
                        if (unlikely(!(tmp & VM_FAULT_LOCKED))) {
                                lock_page(old_page);
                                if (!old_page->mapping) {
                                        ret = 0; /* retry the fault */
                                        unlock_page(old_page);
                                        goto unwritable_page;
                                }
                        } else
                                VM_BUG_ON(!PageLocked(old_page));

                        page_table = pte_offset_map_lock(mm, pmd,
                                        address, &ptl);
                        if (!pte_same(*page_table, orig_pte)) {
                                unlock_page(old_page);
                                goto unlock;
                        }

                        page_mkwrite = 1;
                }
                dirty_page = old_page;
                page_cache_get(dirty_page);

reuse:
                flush_cache_page(vma, address, pte_pfn(orig_pte));
                entry = pte_mkyoung(orig_pte);
                entry = maybe_mkwrite(pte_mkdirty(entry), vma);
                if (ptep_set_access_flags(vma, address, page_table, entry, 1))
                        update_mmu_cache(vma, address, page_table);


                pte_unmap_unlock(page_table, ptl);
                ret |= VM_FAULT_WRITE;

                if (!dirty_page)
                        return ret;

                if (!page_mkwrite) {
                        wait_on_page_locked(dirty_page);
                        set_page_dirty_balance(dirty_page, page_mkwrite);
                        if (vma->vm_file)
                                file_update_time(vma->vm_file);
                }
                put_page(dirty_page);
                if (page_mkwrite) {
                        struct address_space *mapping = dirty_page->mapping;

                        set_page_dirty(dirty_page);
                        unlock_page(dirty_page);
                        page_cache_release(dirty_page);
                        if (mapping) {
                                balance_dirty_pages_ratelimited(mapping);
                        }
                }

                return ret;
        }

        page_cache_get(old_page);
gotten:
        pte_unmap_unlock(page_table, ptl);

        if (unlikely(anon_vma_prepare(vma)))
                goto oom;

        if (is_heca_zero_pfn(pte_pfn(orig_pte))) {
                new_page = alloc_zeroed_user_highpage_movable(vma, address);
                if (!new_page)
                        goto oom;
        } else {
                new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, address);
                if (!new_page)
                        goto oom;
                cow_user_page(new_page, old_page, address, vma);
        }
        __SetPageUptodate(new_page);

        if (mem_cgroup_newpage_charge(new_page, mm, GFP_KERNEL))
                goto oom_free_new;

        mmun_start = address & PAGE_MASK;
        mmun_end = mmun_start + PAGE_SIZE;
        mmu_notifier_invalidate_range_start(mm, mmun_start, mmun_end);

        page_table = pte_offset_map_lock(mm, pmd, address, &ptl);
        if (likely(pte_same(*page_table, orig_pte))) {
                if (old_page) {
                        if (!PageAnon(old_page)) {
                                dec_mm_counter(mm, MM_FILEPAGES);
                                inc_mm_counter(mm, MM_ANONPAGES);
                        }
                } else
                        inc_mm_counter(mm, MM_ANONPAGES);
                flush_cache_page(vma, address, pte_pfn(orig_pte));
                entry = mk_pte(new_page, vma->vm_page_prot);
                entry = maybe_mkwrite(pte_mkdirty(entry), vma);

                ptep_clear_flush(vma, address, page_table);
                page_add_new_anon_rmap(new_page, vma, address);
                set_pte_at_notify(mm, address, page_table, entry);
                update_mmu_cache(vma, address, page_table);
                if (old_page) {
                        page_remove_rmap(old_page);
                }

                new_page = old_page;
                ret |= VM_FAULT_WRITE;
        } else
                mem_cgroup_uncharge_page(new_page);

        if (new_page)
                page_cache_release(new_page);
unlock:
        pte_unmap_unlock(page_table, ptl);
        if (mmun_end > mmun_start)
                mmu_notifier_invalidate_range_end(mm, mmun_start, mmun_end);
        if (old_page) {
                if ((ret & VM_FAULT_WRITE) && (vma->vm_flags & VM_LOCKED)) {
                        lock_page(old_page); /* LRU manipulation */
                        munlock_vma_page(old_page);
                        unlock_page(old_page);
                }
                page_cache_release(old_page);
        }

        return ret;

oom_free_new:
        page_cache_release(new_page);
oom:
        if (old_page) {
                if (page_mkwrite) {
                        unlock_page(old_page);
                        page_cache_release(old_page);
                }
                page_cache_release(old_page);
        }
        return VM_FAULT_OOM;

unwritable_page:
        page_cache_release(old_page);
        return ret;
}

inline void heca_release_pull_hpc(struct heca_page_cache **hpc)
{
        atomic_dec(&(*hpc)->nproc);
        if (atomic_cmpxchg(&(*hpc)->nproc, 1, 0) == 1) {
                int i;

                for (i = 0; i < (*hpc)->hprocs.num; i++) {
                        if (likely((*hpc)->pages[i]))
                                page_cache_release((*hpc)->pages[i]);
                }
                heca_dealloc_hpc(hpc);
        }
}

void dequeue_and_gup_cleanup(struct heca_process *hproc)
{
        struct heca_delayed_fault *ddf;
        struct heca_page_cache *hpc;
        struct llist_node *head, *node;

        head = llist_del_all(&hproc->delayed_gup);

        for (node = head; node; node = llist_next(node)) {
                ddf = llist_entry(node, struct heca_delayed_fault, node);
                /*
                 * we need to hold the dpc to guarantee it doesn't disappear while we
                 * do the if check
                 */
                hpc = heca_cache_get_hold(hproc, ddf->addr);
                if (hpc && (hpc->tag & (PREFETCH_TAG | PUSH_RES_TAG))) {
                        atomic_dec(&hpc->nproc);
                        heca_release_pull_hpc(&hpc);
                }
        }

        while (node) {
                ddf = llist_entry(node, struct heca_delayed_fault, node);
                node = llist_next(node);
                free_heca_delayed_fault_cache_elm(&ddf);
        }
}

static inline struct llist_node *llist_nodes_reverse(struct llist_node *llnode)
{
        struct llist_node *next, *tail = NULL;

        while (llnode) {
                next = llnode->next;
                llnode->next = tail;
                tail = llnode;
                llnode = next;
        }

        return tail;
}

int heca_initiate_fault(struct mm_struct *mm, unsigned long addr, int write)
{
        int r;

        might_sleep();

        use_mm(mm);
        down_read(&mm->mmap_sem);
        r = get_user_pages(current, mm, addr, 1, write, 0, NULL, NULL);
        up_read(&mm->mmap_sem);
        unuse_mm(mm);

        BUG_ON(r > 1);
        return r == 1;
}

static void heca_initiate_pull_gup(struct heca_page_cache *hpc, int delayed)
{
        struct heca_process *hproc = hpc->hproc;
        struct heca_memory_region *mr;

        if (delayed) {
                trace_heca_delayed_initiated_fault(hproc->hspace->hspace_id,
                                hproc->hproc_id, -1, -1, hpc->addr, 0,
                                hpc->tag);
        } else {
                trace_heca_immediate_initiated_fault(hproc->hspace->hspace_id,
                                hproc->hproc_id, -1, -1, hpc->addr, 0,
                                hpc->tag);
        }

        /* TODO: we do not allow deleting mrs; handle this case when we do */
        mr = search_heca_mr_by_addr(hproc, hpc->addr);
        if (unlikely(!mr))
                return;

        heca_initiate_fault(hproc->mm, hpc->addr, hpc->tag == PUSH_RES_TAG ||
                        (~mr->flags & MR_SHARED));
}

static void dequeue_and_gup(struct heca_process *hproc)
{
        struct heca_delayed_fault *hdf;
        struct heca_page_cache *hpc;
        struct llist_node *head, *node;

        head = llist_del_all(&hproc->delayed_gup);
        head = llist_nodes_reverse(head);
        for (node = head; node; node = llist_next(node)) {
                hdf = llist_entry(node, struct heca_delayed_fault, node);
                hpc = heca_cache_get_hold(hproc, hdf->addr);
                if (hpc) {
                        /*
                         * this might be another PUSH_RES or PREFETCH, if page has been
                         * faulted, pushed and re-brought in the meanwhile. but no harm
                         * in faulting it in anyway.
                         */
                        if (hpc->tag & (PREFETCH_TAG | PUSH_RES_TAG))
                                heca_initiate_pull_gup(hpc, 1);
                        heca_release_pull_hpc(&hpc);
                }
        }
        node = head;
        while (node) {
                hdf = llist_entry(node, struct heca_delayed_fault, node);
                node = llist_next(node);
                free_heca_delayed_fault_cache_elm(&hdf);
        }
}

void delayed_gup_work_fn(struct work_struct *w)
{
        struct heca_process *hproc;
        hproc = container_of(to_delayed_work(w), struct heca_process,
                        delayed_gup_work);
        dequeue_and_gup(hproc);
}

static inline void queue_ddf_for_delayed_gup(struct heca_delayed_fault *hdf,
                struct heca_process *hproc)
{
        llist_add(&hdf->node, &hproc->delayed_gup);
        schedule_delayed_work(&hproc->delayed_gup_work, GUP_DELAY);
}

static int heca_pull_req_success(struct page *page,
                struct heca_page_cache *hpc)
{
        int i, found;
        trace_heca_pull_req_complete(hpc->hproc->hspace->hspace_id,
                        hpc->hproc->hproc_id, -1, -1, hpc->addr, 0, hpc->tag);

        for (i = 0; i < hpc->hprocs.num; i++) {
                if (hpc->pages[i] == page)
                        goto unlock;
        }
        BUG();

unlock:
        found = atomic_read(&hpc->found);
        if (found < 0) {
                if (atomic_cmpxchg(&hpc->found, found, i) != found)
                        goto unlock;
                page_cache_get(page);
                lru_cache_add_anon(page);
                for (i = 0; i < hpc->hprocs.num; i++) {
                        if (likely(hpc->pages[i]))
                                SetPageUptodate(hpc->pages[i]);
                }
                trace_heca_pull_req_success(hpc->hproc->hspace->hspace_id,
                                hpc->hproc->hproc_id, -1, -1,
                                hpc->addr, 0, hpc->tag);
                unlock_page(hpc->pages[0]);
                lru_add_drain();

                /*
                 * Try to delay faulting pages that were prefetched or pushed to 
                 * us, to be processed async. This is both to speed up
                 * processing of the msg queue; and to avoid a deadlock when
                 * a write fault has preceded us, sent a claim request, locked
                 * the page and is now waiting for the claim ack that we are
                 * blocking in queue.
                 */
                if (hpc->tag & (PREFETCH_TAG | PUSH_RES_TAG)) {
                        struct heca_delayed_fault *hdf;

                        hdf = alloc_heca_delayed_fault_cache_elm(hpc->addr);
                        if (likely(hdf)) {
                                queue_ddf_for_delayed_gup(hdf, hpc->hproc);
                        } else {
                                heca_printk(KERN_ERR "[%lu] Could not delay "
                                                "fault, risking deadlock.\n");
                                heca_initiate_pull_gup(hpc, 0);
                        }
                }
        }

        return 1;
}

/* last failure should also account for the fault/gup refcount */
int heca_pull_req_failure(struct heca_page_cache *hpc)
{
        int found, i;

        trace_heca_try_pull_req_complete_fail(hpc->hproc->hspace->hspace_id,
                        hpc->hproc->hproc_id, -1, -1, hpc->addr, 0, hpc->tag);

retry:
        /*
         * a successful request will set found >= 0. otherwise, the negative value
         * indicates the count of failed responses + 1. if everyone failed, we need
         * to clean up.
         */
        found = atomic_read(&hpc->found);
        if (found < 0) {
                if (atomic_cmpxchg(&hpc->found, found, found - 1) != found)
                        goto retry;

                /* -found == hproc_num <-> -(found-1) == hproc_num+1 */
                if (found * -1 == hpc->hprocs.num) {
                        for (i = 0; i < hpc->hprocs.num; i++) {
                                if (likely(hpc->pages[i]))
                                        SetPageUptodate(hpc->pages[i]);
                        }
                        unlock_page(hpc->pages[0]);
                        heca_cache_release(hpc->hproc, hpc->addr);
                        atomic_dec(&hpc->nproc);
                }
        }

        return -EFAULT;
}

static int heca_pull_req_complete(struct tx_buffer_element *tx_e)
{
        struct heca_page_cache *hpc = tx_e->wrk_req->hpc;
        struct page *page = tx_e->wrk_req->dst_addr->mem_page;
        int r;

        r = unlikely(tx_e->hmsg_buffer->type == MSG_RES_PAGE_FAIL) ?
                heca_pull_req_failure(hpc) :
                heca_pull_req_success(page, hpc);

        tx_e->wrk_req->dst_addr->mem_page = NULL;
        heca_release_pull_hpc(&hpc);
        return r;
}

static struct page *heca_get_remote_page(struct vm_area_struct *vma,
                unsigned long addr, struct heca_page_cache *hpc,
                struct heca_process *fault_hproc,
                struct heca_memory_region *fault_mr,
                struct heca_process *remote_hproc, int tag, int i,
                struct heca_page_pool_element *ppe)
{
        struct page *page = NULL;

        if (!hpc->pages[i]) {
                ppe = heca_fetch_ready_ppe(remote_hproc->connection);
                hpc->pages[i] = ppe ? ppe->mem_page :
                        alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, addr);
        }
        page = hpc->pages[i];
        if (unlikely(!page))
                goto out;
        SetPageSwapBacked(page);

        trace_heca_get_remote_page(fault_hproc->hspace->hspace_id,
                        fault_hproc->hproc_id, remote_hproc->hproc_id,
                        fault_mr->hmr_id, addr, addr - fault_mr->addr, tag);

        heca_request_page(page, remote_hproc, fault_hproc, fault_mr, addr,
                        heca_pull_req_complete, tag, hpc, ppe);

out:
        return page;
}

static struct heca_page_cache *heca_cache_add_send(
                struct heca_process *fault_hproc,
                struct heca_memory_region *fault_mr,
                struct heca_process_list hprocs, unsigned long norm_addr,
                int nproc, int tag,
                struct vm_area_struct *vma, pte_t orig_pte,
                pte_t *ptep, int alloc)
{
        struct heca_page_cache *new_hpc = NULL, *found_hpc = NULL;
        struct page *page = NULL;
        struct heca_page_pool_element *ppe = NULL;
        int r;
        struct heca_process *first_hproc = NULL;

        trace_heca_cache_add_send(fault_hproc->hspace->hspace_id,
                        fault_hproc->hproc_id, -1, fault_mr->hmr_id, norm_addr,
                        norm_addr - fault_mr->addr, tag);

        do {
                found_hpc = heca_cache_get_hold(fault_hproc, norm_addr);
                if (unlikely(found_hpc))
                        goto fail;

                if (likely(!new_hpc)) {
                        new_hpc = heca_alloc_hpc(fault_hproc, norm_addr, hprocs,
                                        hprocs.num + nproc, tag);
                        if (!new_hpc)
                                goto fail;
                }

                if (likely(!page)) {
                        if (likely(hprocs.ids[0])) {
                                first_hproc = find_hproc(fault_hproc->hspace,
                                                hprocs.ids[0]);
                                if (likely(first_hproc))
                                        ppe = heca_fetch_ready_ppe(first_hproc->connection);
                        }
                        if (ppe) {
                                page = ppe->mem_page;
                        } else if (alloc) {
                                page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma,
                                                norm_addr);
                                if (unlikely(!page))
                                        goto fail;
                        } else {
                                goto fail;
                        }
                        __set_page_locked(page);
                        SetPageSwapBacked(page);
                        new_hpc->pages[0] = page;
                }

                r = radix_tree_preload(GFP_HIGHUSER_MOVABLE & GFP_KERNEL);
                if (unlikely(r))
                        goto fail;

                spin_lock_irq(&fault_hproc->page_cache_spinlock);
                r = radix_tree_insert(&fault_hproc->page_cache,
                                norm_addr, new_hpc);
                spin_unlock_irq(&fault_hproc->page_cache_spinlock);
                radix_tree_preload_end();

                if (likely(!r)) {
                        if (unlikely(!pte_same(*ptep, orig_pte))) {
                                radix_tree_delete(&fault_hproc->page_cache,
                                                norm_addr);
                                goto fail;
                        }
                        if (likely(first_hproc)) {
                                heca_get_remote_page(vma, norm_addr, new_hpc,
                                                fault_hproc, fault_mr,
                                                first_hproc, tag, r, ppe);
                                hproc_put(first_hproc);
                        }
                        for (r = 1; r < hprocs.num; r++) {
                                struct heca_process *remote_hproc;

                                remote_hproc = find_hproc(fault_hproc->hspace,
                                                hprocs.ids[r]);
                                if (likely(remote_hproc)) {
                                        heca_get_remote_page(vma, norm_addr,
                                                        new_hpc, fault_hproc,
                                                        fault_mr, remote_hproc,
                                                        tag, r, NULL);
                                        hproc_put(remote_hproc);
                                }
                        }
                        return new_hpc;
                }
        } while (r != -ENOMEM);

fail:
        if (new_hpc) {
                if (page) {
                        ClearPageSwapBacked(page);
                        unlock_page(page);
                        if (ppe)
                                heca_ppe_clear_release(first_hproc->connection,
                                                &ppe);
                        else
                                page_cache_release(page);
                }
                heca_dealloc_hpc(&new_hpc);
        }
        if (first_hproc)
                hproc_put(first_hproc);
        return found_hpc;
}

/*
 * return -1 on fault or stuff missing
 * return 1 if we send a request
 *  return 0 if dpc present
 *
 * TODO: no real need to normalize the address here, we're already receiving
 * a normalized one
 */
static int get_heca_page(struct mm_struct *mm, unsigned long addr,
                struct heca_process *fault_hproc, struct heca_memory_region *mr,
                int tag)
{
        pte_t *pte;
        pgd_t *pgd;
        pud_t *pud;
        pmd_t *pmd;
        pte_t pte_entry;
        swp_entry_t swp_e;
        struct vm_area_struct *vma;
        unsigned long norm_addr = addr & PAGE_MASK;
        struct heca_page_cache *hpc = NULL;
        int ret = 0;

        if (norm_addr < mr->addr || norm_addr >= mr->addr + mr->sz)
                goto out;

        hpc = heca_cache_get(fault_hproc, norm_addr);
        if (!hpc) {
                ret = -1;
                vma = find_vma(mm, addr);
                if (unlikely(!vma || vma->vm_start > addr))
                        goto out;

                pgd = pgd_offset(mm, addr);
                if (unlikely(!pgd_present(*pgd)))
                        goto out;

                pud = pud_offset(pgd, addr);
                if (unlikely(!pud_present(*pud)))
                        goto out;

                pmd = pmd_offset(pud, addr);
                if (unlikely(pmd_none(*pmd)))
                        goto out;

                if (unlikely(pmd_bad(*pmd))) {
                        pmd_clear_bad(pmd);
                        goto out;
                }
                if (unlikely(pmd_trans_huge(*pmd)))
                        goto out;

                pte = pte_offset_map(pmd, addr);
                pte_entry = *pte;

                if (!pte_present(pte_entry) && !pte_none(pte_entry)) {
                        swp_e = pte_to_swp_entry(pte_entry);
                        if (non_swap_entry(swp_e) && is_heca_entry(swp_e)) {
                                struct heca_swp_data dsd;

                                if (swp_entry_to_heca_data(swp_e, &dsd) < 0)
                                        goto out;

                                if (dsd.flags & HECA_INFLIGHT)
                                        goto out;

                                /*
                                 * refcount for hpc:
                                 *  +1 for every hproc we send to
                                 *  +1 for the fault that comes after fetching
                                 */
                                heca_cache_add_send(fault_hproc, mr, dsd.hprocs,
                                                norm_addr, 2, tag, vma,
                                                pte_entry, pte,
                                                tag != PREFETCH_TAG);
                                ret = 1;
                        }
                }
        }

out:
        return ret;
}

static int inflight_wait(pte_t *page_table, pte_t *orig_pte, swp_entry_t *entry,
                struct heca_swp_data *hsd)
{
        pte_t pte;
        swp_entry_t swp_entry;
        int ret = 0;

        do {
                cond_resched();
                pte = *page_table;
                if (!pte_same(pte, *orig_pte)) {
                        if (pte_present(pte)) {
                                ret = 1;
                                break;
                        }

                        if (!pte_none(pte) && !pte_file(pte)) {
                                swp_entry = pte_to_swp_entry(pte);
                                if (non_swap_entry(swp_entry)
                                                && is_heca_entry(swp_entry)
                                                && heca_swp_entry_same(swp_entry,
                                                        *entry)) {
                                        struct heca_swp_data tmp_dsd;

                                        if (swp_entry_to_heca_data(swp_entry,
                                                                &tmp_dsd) < 0) {
                                                ret = -EFAULT;
                                                goto out;
                                        }

                                        if (tmp_dsd.flags & HECA_INFLIGHT) {
                                                continue;
                                        } else {
                                                *orig_pte = pte;
                                                *entry = swp_entry;
                                                *hsd = tmp_dsd;
                                                break;
                                        }
                                }
                        }
                }
        } while (1);

out:
        return ret;
}

static int heca_fault_do_readahead(struct mm_struct *mm, unsigned long addr,
                struct heca_process *hproc, struct heca_memory_region *mr,
                struct heca_page_cache *hpc)
{
        int max_retry = 20, cont_back = 1, cont_forward = 1, j = 1;

        do {
                if (cont_forward == 1)
                        cont_forward = get_heca_page(mm, addr + j * PAGE_SIZE,
                                        hproc, mr, PREFETCH_TAG);
                if (cont_back == 1) {
                        if (addr > j * PAGE_SIZE)
                                cont_back = get_heca_page(mm,
                                                addr - j * PAGE_SIZE, hproc, mr,
                                                PREFETCH_TAG);
                        else
                                cont_back = 0;
                }
                if (trylock_page(hpc->pages[0])) {
                        hproc_put(hproc);
                        return 1;
                }
                j++;
        } while (j < max_retry && (cont_back == 1 || cont_forward == 1));

        return 0;
}

static int heca_maintain_notify(struct heca_process *hproc,
                struct heca_memory_region *mr, unsigned long addr,
                u32 exclude_id)
{
        struct heca_process *owner;
        struct heca_process_list hprocs;
        int r = -EFAULT, i;

        rcu_read_lock();
        hprocs = heca_descriptor_to_hprocs(mr->descriptor);
        rcu_read_unlock();

        for_each_valid_hproc(hprocs, i) {
                /* the page returned home to us, its owners */
                if (hprocs.ids[i] == hproc->hproc_id) {
                        r = 0;
                        break;
                }

                if (hprocs.ids[i] == exclude_id)
                        continue;

                owner = find_hproc(hproc->hspace, hprocs.ids[i]);
                if (likely(owner)) {
                        r = heca_claim_page(hproc, owner, mr, addr, NULL, 0);
                        hproc_put(owner);
                        break;
                }
        }

        return r;
}

int heca_do_page_fault(struct mm_struct *mm, struct vm_area_struct *vma,
                unsigned long address, pte_t *page_table, pmd_t *pmd,
                unsigned int flags, pte_t orig_pte, swp_entry_t entry)
{
        struct heca_swp_data hsd;
        struct heca_process *fault_hproc;
        struct heca_memory_region *fault_mr;
        unsigned long norm_addr = address & PAGE_MASK;
        spinlock_t *ptl;
        int ret = 0, found = -1, exclusive = 0, write,
            finalize_write = 0, read_fault = 0;
        struct heca_page_cache *hpc = NULL;
        struct page *found_page, *swapcache = NULL;
        struct mem_cgroup *ptr;
        pte_t pte;
        u32 hspace_id, hproc_id, mr_id; /* used only for trace record later */
        unsigned long shared_addr; /* used only for trace record later */
        struct heca_space *hspace;

        /* if the data in the swp_entry is invalid, we have nothing to do */
        if (swp_entry_to_heca_data(entry, &hsd) < 0)
                return VM_FAULT_ERROR;

        hspace = find_hspace(hsd.hprocs.hspace_id);
        if (unlikely(!hspace))
                return VM_FAULT_ERROR;

        fault_hproc = find_local_hproc_in_hspace(hspace, mm);
        if (unlikely(!fault_hproc))
                return VM_FAULT_ERROR;

        fault_mr = search_heca_mr_by_addr(fault_hproc, norm_addr);
        if (unlikely(!fault_mr)) {
                hproc_put(fault_hproc);
                return VM_FAULT_ERROR;
        }

        if ((fault_mr->flags & MR_SHARED) && ~flags & FAULT_FLAG_WRITE)
                read_fault = 1;

        /* these scalars are for tracing */
        hspace_id = hspace->hspace_id;
        hproc_id = fault_hproc->hproc_id;
        mr_id = fault_mr->hmr_id;
        shared_addr = norm_addr - fault_mr->addr;
        trace_heca_do_page_fault(hspace_id, hproc_id, -1, mr_id, norm_addr,
                        shared_addr, hsd.flags);

        /*
         * If page is absent since we're answering a remote fault, wait for it
         * to finish before faulting ourselves.
         */
        if (unlikely(hsd.flags && hsd.flags & HECA_INFLIGHT)) {
                int inflight = inflight_wait(page_table, &orig_pte,
                                &entry, &hsd);

                if (inflight) {
                        if (inflight == -EFAULT) {
                                ret = VM_FAULT_ERROR;
                        } else {
                                ret |= VM_FAULT_RETRY;
                                up_read(&mm->mmap_sem);
                        }
                        hproc_put(fault_hproc);
                        goto out_no_dpc;
                }
        }

        hpc = heca_cache_get_hold(fault_hproc, norm_addr);
        if (!hpc) {
                /*
                 * refcount for dpc:
                 *  +1 for every hproc sent to
                 *  +1 for the current do_heca_page_fault
                 *  +1 for the final, successful do_heca_page_fault
                 */
                hpc = heca_cache_add_send(fault_hproc, fault_mr, hsd.hprocs,
                                norm_addr, 3, read_fault? READ_TAG : PULL_TAG,
                                vma, orig_pte, page_table, 1);

                if (unlikely(!hpc)) {
                        page_table = pte_offset_map_lock(mm, pmd,
                                        address, &ptl);
                        if (likely(pte_same(*page_table, orig_pte)))
                                ret = VM_FAULT_OOM;
                        pte_unmap_unlock(page_table, ptl);
                        hproc_put(fault_hproc);
                        goto out_no_dpc;
                }
                ret = VM_FAULT_MAJOR;
                count_vm_event(PGMAJFAULT);
                mem_cgroup_count_vm_event(mm, PGMAJFAULT);

        /*
         * we requested a read copy initially, but now we need to write. as a read
         * response is already underway, we will try to fault it in ASAP and then
         * go through heca_write_fault to claim it. it's better than discarding it
         * and write faulting, as a page is already ready and page contents already
         * being transferred to us (CLAIM req is cheaper).
         */
        } else if (!read_fault && (hpc->tag & READ_TAG ||
                                (hpc->tag & PREFETCH_TAG &&
                                 fault_mr->flags & MR_SHARED))) {
                finalize_write = 1;
                goto lock;
        }

        /*
         * do not prefetch if we have the NOWAIT flag, as prefetch will be
         * triggered in the async PF
         */
        if (hpc->tag != PUSH_RES_TAG && flags & FAULT_FLAG_ALLOW_RETRY &&
                        ~flags & FAULT_FLAG_RETRY_NOWAIT) {
                if (heca_fault_do_readahead(mm, norm_addr, fault_hproc,
                                        fault_mr, hpc))
                        goto resolve;
        }

        /*
         * KVM will send a NOWAIT flag and will freeze the faulting thread itself,
         * so we just re-throw immediately. Otherwise, we wait until the bitlock is
         * cleared, then re-throw the fault.
         */
lock:
        hproc_put(fault_hproc);
        if (!lock_page_or_retry(hpc->pages[0], mm, flags)) {
                ret |= VM_FAULT_RETRY;
                goto out;
        }

resolve:
        found = atomic_read(&hpc->found);
        if (unlikely(found < 0)) {
                unlock_page(hpc->pages[0]);
                ret = VM_FAULT_ERROR;
                goto out;
        }

        /*
         * In this critical section, we lock the updated page (if it's the
         * first one, it was locked in advance), increment its refcount, the
         * pte_offset_map is locked and dpc refcount is already incremented.
         */
        found_page = hpc->pages[found];
        if (found)
                __set_page_locked(found_page);
        page_cache_get(found_page);

        swapcache = found_page;
        found_page = ksm_might_need_to_copy(found_page, vma, address);
        if (unlikely(!found_page)) {
                ret = VM_FAULT_OOM;
                found_page = swapcache;
                goto out_page;
        }

        if (mem_cgroup_try_charge_swapin(mm, found_page, GFP_KERNEL, &ptr)) {
                ret = VM_FAULT_OOM;
                goto out_page;
        }

        page_table = pte_offset_map_lock(mm, pmd, address, &ptl);
        if (unlikely(!pte_same(*(page_table), orig_pte)))
                goto out_nomap;
        if (unlikely(!PageUptodate(found_page))) {
                ret = VM_FAULT_SIGBUS;
                goto out_nomap;
        }

        inc_mm_counter(mm, MM_ANONPAGES);
        pte = mk_pte(found_page, vma->vm_page_prot);

        write = !finalize_write && !read_fault;
        if (likely(reuse_heca_page(found_page, norm_addr, hpc))) {
                if (write) {
                        pte = maybe_mkwrite(pte_mkdirty(pte), vma);
                        flags &= ~FAULT_FLAG_WRITE;
                        ret |= VM_FAULT_WRITE;
                } else {
                        pte = pte_mkclean(pte_wrprotect(pte));
                }
                exclusive = 1;
        }

        flush_icache_page(vma, found_page);
        set_pte_at(mm, address, page_table, pte);

        if (found_page == swapcache)
                do_page_add_anon_rmap(found_page, vma, address, exclusive);
        else
                page_add_new_anon_rmap(found_page, vma, address);
        mem_cgroup_commit_charge_swapin(found_page, ptr);

        unlock_page(found_page);
        if (found)
                unlock_page(hpc->pages[0]);

        if (found_page != swapcache) {
                unlock_page(swapcache);
                page_cache_release(swapcache);
        }

        /*
         * if we're faulting for write in _this_ critical section (!finalize_write),
         * and the page cannot be reused.
         */
        if (write && flags & FAULT_FLAG_WRITE) {
                ret |= do_wp_heca_page(mm, vma, address, page_table, pmd, ptl,
                                pte, norm_addr, hpc);
                if (ret & VM_FAULT_ERROR)
                        ret &= VM_FAULT_ERROR;
                goto out;
        }

        update_mmu_cache(vma, address, page_table);
        if (hpc->released == 1)
                heca_cache_release(hpc->hproc, hpc->addr);
        pte_unmap_unlock(pte, ptl);

        if (write)
                heca_maintain_notify(hpc->hproc, fault_mr, hpc->addr,
                                hpc->hprocs.ids[found]);
        else
                heca_flag_page_read(hpc->hproc, hpc->addr,
                                hpc->hprocs.ids[found]);
        page_cache_release(found_page);
        atomic_dec(&hpc->nproc);
        trace_heca_do_page_fault_complete(hspace_id, hproc_id, -1, mr_id,
                        norm_addr, shared_addr, hpc->tag);
        goto out;

out_nomap:
        pte_unmap_unlock(page_table, ptl);
        mem_cgroup_cancel_charge_swapin(ptr);

out_page:
        unlock_page(found_page);
        if (found)
                unlock_page(hpc->pages[0]);

        page_cache_release(found_page);
        if (found_page != swapcache) {
                unlock_page(swapcache);
                page_cache_release(swapcache);
        }

out:
        if (likely(hpc)) {
                if (ret & VM_FAULT_RETRY && !(ret & VM_FAULT_ERROR))
                        atomic_dec(&hpc->nproc);
                else
                        heca_release_pull_hpc(&hpc);
        }

out_no_dpc:
        if (unlikely(finalize_write
                                && ~ret & (VM_FAULT_RETRY | VM_FAULT_ERROR))) {
                page_table = pte_offset_map_lock(mm, pmd, address, &ptl);
                if (heca_write_fault(mm, vma, address, pmd, page_table,
                                        ptl, flags) > 0){
                        ret |= VM_FAULT_WRITE;
                } else {
                        if (!pte_write(*page_table))
                                ret |= VM_FAULT_ERROR;
                        pte_unmap_unlock(page_table, ptl);
                }
        }
        return ret;
}

int heca_trigger_page_pull(struct heca_space *hspace,
                struct heca_process *local_hproc, struct heca_memory_region *mr,
                unsigned long norm_addr)
{
        int r = 0;
        struct mm_struct *mm = local_hproc->mm;

        /*
         * FIXME: There is a possible yet very unlikely edge case here. If a
         * page is faulted-in to us, we extract it and then it is pushed to us,
         * the original hpc might still be in the tree due to some slow
         * operation (slow hpc release, slow answering RRAIM node). In this case
         * the push will silently fail and we will lose the page and have a
         * memleak on the remote node.
         */
        down_read(&mm->mmap_sem);
        r = get_heca_page(mm, norm_addr + mr->addr, local_hproc, mr,
                        PUSH_RES_TAG);
        up_read(&mm->mmap_sem);

        return r;
}

/*
 * we arrive with mmap_sem held and pte locked. if the address is ours, we leave
 * with mmap_sem still held, but pte unmapped and unlocked.
 */
int heca_write_fault(struct mm_struct *mm, struct vm_area_struct *vma,
                unsigned long address, pmd_t *pmd, pte_t *ptep, spinlock_t *ptl,
                unsigned int flags)
{
        unsigned long addr = address & PAGE_MASK;
        struct heca_process *hproc = NULL;
        struct heca_memory_region *mr;
        struct page *page;
        struct heca_pte_data pd;
        pte_t pte;
        struct heca_page_cache *hpc = NULL;
        u32 maintainer_id = 0;

        hproc = find_local_hproc_from_mm(mm);
        if (!hproc)
                return 0;

        mr = search_heca_mr_by_addr(hproc, addr);
        if (!mr) {
                hproc_put(hproc);
                return 0;
        }

        trace_heca_write_fault(hproc->hspace->hspace_id, hproc->hproc_id, -1,
                        mr->hmr_id, addr, addr - mr->addr, 0);

retry:
        pte = *ptep;
        if (unlikely(pte_write(pte))) {
                spin_unlock(ptl);
                goto out;
        }

        if (unlikely(!pte_present(pte))) {
                /*
                 * this only happens on retries, and means someone else has requested
                 * the page before us. so we do a full regular write fault.
                 */
                spin_unlock(ptl);
                get_user_pages(current, mm, addr, 1, 1, 0, NULL, NULL);
                goto out;
        }

        /* should always succeed, as our vmas should never be VM_MIXEDMAP */
        page = vm_normal_page(vma, addr, pte);
        BUG_ON(!page);
        page_cache_get(page);

        hpc = heca_cache_get_hold(hproc, addr);
        if (hpc) {
                /* these should be handled in the first do_heca_page_fault */
                BUG_ON(hpc->tag == PUSH_RES_TAG);

                if (hpc->tag == CLAIM_TAG) {
                        spin_unlock(ptl);
                        goto wait;
                }

                /* the pull req which brought the page hasn't been cleaned-up yet */
                hpc->tag = CLAIM_TAG;
        } else {
                int r = heca_cache_add(hproc, addr, 2, CLAIM_TAG, &hpc);

                if (unlikely(r < 0)) {
                        if (r == -ENOMEM)
                                return r;

                        goto retry;
                }
        }

        if (!trylock_page(page)) {
                /*
                 * if we can't lock the page, unlock the pte and resched. since we hold
                 * the mmap_sem, the ptl will stay there and wait for us.
                 */
                spin_unlock(ptl);
                page_cache_release(page);
                heca_release_pull_hpc(&hpc);
                if (!hpc)
                        heca_cache_release(hproc, addr);
                might_sleep();
                cond_resched();
                spin_lock(ptl);
                goto retry;
        }

        /*
         * we leave the critical section. as we invalidate the page copies,
         * someone else can take the page - any maintainer will answer requests
         * by order of arrival. but we ourselves will not answer any read
         * request, as the hpc is in-place.
         *
         * any subsequent write-fault will now encounter the CLAIM_TAG hpc and
         * know it can back away.
         */
        spin_unlock(ptl);

        /*
         * this races with try_discard_read_copy (which locks the page and checks
         * for heca_cache_get), and with process_page_claim (which locks the pte
         * and checks for heca_cache_get). so no locking needed.
         */
        if (heca_lookup_page_read(hproc, addr))
                maintainer_id = heca_extract_page_read(hproc, addr);

        if (maintainer_id) {
                struct heca_process *mnt_hproc;

                mnt_hproc = find_hproc(hproc->hspace, maintainer_id);
                if (!mnt_hproc) {
                        /*
                         * TODO: the maintainer has left the cluster, and left us hanging.
                         * we need to broadcast invalidation cluster-wide and assume
                         * maintenance for the page. for now, we assume maintenance, even
                         * though it will create several different copies of the page.
                         */
                        unlock_page(page);
                        goto write;
                }

                hpc->redirect_hproc_id = mnt_hproc->hproc_id;

                /* required as the page is locked, and will unlock only on claim_ack */
                page_cache_get(page);
                heca_claim_page(hproc, mnt_hproc, mr, addr, page, 1);
                hproc_put(mnt_hproc);

        } else {
                heca_invalidate_readers(hproc, addr, 0);

                /*
                 * TODO: with strict coherency policy, only the last ACK from readers
                 * will unlock the page. meanwhile, it costs very little to leave this
                 * here (+2 atomic operations)
                 */
                unlock_page(page);
        }

        /* by now, all read copies have been invalidated at least once */
wait:
        up_read(&mm->mmap_sem);
        wait_on_page_locked(page);

        /*
         * we must release the lock before sleeping, and now re-grab it and
         * re-query the page table. otherwise a nasty edge case might occur when
         * someone tries to grab the lock for write, while a page extraction
         * request is processed (requiring the lock for read) before the ACK.
         */
        down_read(&mm->mmap_sem);
        if (unlikely(heca_extract_pte_data(&pd, mm, address))) {
                up_read(&mm->mmap_sem);
                return 0;
        }
        ptep = pd.pte;

write:
        spin_lock(ptl);
        if (unlikely(!pte_same(*ptep, pte))) {
                page_cache_release(page);
                heca_release_pull_hpc(&hpc);
                if (!hpc)
                        heca_cache_release(hproc, addr);
                goto retry;
        }
        pte = pte_mkyoung(maybe_mkwrite(pte_mkdirty(pte), vma));
        if (ptep_set_access_flags(vma, addr, ptep, pte, 1))
                update_mmu_cache(vma, addr, ptep);
        page_cache_release(page);
        pte_unmap_unlock(ptep, ptl);

        heca_maintain_notify(hproc, mr, addr, maintainer_id);

        /* compatible with a pre-existing dpc, and with a newly created dpc */
        heca_cache_release(hproc, addr);
        heca_release_pull_hpc(&hpc);

out:
        hproc_put(hproc);
        return 1;
}

