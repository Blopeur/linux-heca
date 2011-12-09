/*
 * dsm_page_request.c
 *
 *  Created on: 1 Aug 2011
 *      Author: john
 */

#include <asm-generic/memory_model.h>
#include <linux/pagemap.h>
#include <linux/types.h>
#include "../../../mm/internal.h"
#include <linux/page-flags.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/hugetlb.h>
#include <linux/mm.h>
#include <linux/rmap.h>

#include <dsm/dsm_core.h>
#include <dsm/dsm_ctl.h>
#include <dsm/dsm_rb.h>

#include <linux/mmu_notifier.h>

#include <linux/ksm.h>
#include <linux/mmu_context.h>
#include <asm-generic/mman-common.h>

static struct page *_dsm_extract_page(struct dsm_vm_id id, struct mm_struct *mm,
                unsigned long addr, int flag) {
        spinlock_t *ptl;
        pte_t *pte;
        int r = 0;
        struct page *page = NULL;
        struct vm_area_struct *vma;
        pgd_t *pgd;
        pud_t *pud;
        pmd_t *pmd;
        pte_t pte_entry;
        swp_entry_t swp_e;
        printk("[request_page_insert] faulting for page %p  \n ", (void*) addr);
        retry:

        vma = find_vma(mm, addr);
        if (unlikely(!vma || vma->vm_start > addr)) {
                printk("[dsm_extract_page] no VMA or bad VMA \n");
                goto out;
        }

        pgd = pgd_offset(mm, addr);
        if (unlikely(!pgd_present(*pgd))) {
                printk("[dsm_extract_page] no pgd \n");
                goto out;
        }

        pud = pud_offset(pgd, addr);
        if (unlikely(!pud_present(*pud))) {
                printk("[dsm_extract_page] no pud \n");
                goto out;
        }

        pmd = pmd_offset(pud, addr);

        if (unlikely(pmd_none(*pmd))) {
                printk("[dsm_extract_page] no pmd error \n");
                __pte_alloc(mm, vma, pmd, addr);
                goto retry;
        }
        if (unlikely(pmd_bad(*pmd))) {
                pmd_clear_bad(pmd);
                printk("[dsm_extract_page] bad pmd \n");
                goto out;
        }
        if (unlikely(pmd_trans_huge(*pmd))) {
                printk("[dsm_extract_page] we have a huge pmd \n");
                spin_lock(&mm->page_table_lock);
                if (unlikely(pmd_trans_splitting(*pmd))) {
                        spin_unlock(&mm->page_table_lock);
                        wait_split_huge_page(vma->anon_vma, pmd);
                } else {
                        spin_unlock(&mm->page_table_lock);
                        split_huge_page_pmd(mm, pmd);
                }
                goto retry;
        }

        // we need to lock the tree before locking the pte because in page insert we do it in the same order => avoid deadlock
        pte = pte_offset_map(pmd, addr);

        pte_entry = *pte;

        if (unlikely(!pte_present(pte_entry))) {
                if (pte_none(pte_entry)) {
                        goto chain_fault;

                } else {
                        swp_e = pte_to_swp_entry(pte_entry);
                        if (non_swap_entry(swp_e)) {
                                if (is_dsm_entry(swp_e)) {

                                        page = page_is_in_dsm_cache(addr);
                                        if (page) {
//                                                if (flag & TRY_FLAG
//                                                ) {
//
//                                                        if (trylock_page(
//                                                                        page)) {
//
//                                                                if (!delete_from_dsm_cache(
//                                                                                page,
//                                                                                addr)) {
//
//                                                                        unlock_page(
//                                                                                        page);
//
//                                                                        return page;
//                                                                }
//                                                        }
//                                                        goto retry;
//                                                } else
                                                printk(
                                                                "[[EXTRACT_PAGE]] we chanin fault because the page is in cache \n");
                                                goto chain_fault;

                                        } else {
                                                printk(
                                                                "[[EXTRACT_PAGE]] page not present and swapped out somewhere else, no handling yet \n");
                                                BUG();
                                        }
                                } else if (is_migration_entry(swp_e)) {

                                        migration_entry_wait(mm, pmd, addr);
                                        goto retry;
                                } else {
                                        BUG();
                                }
                        } else {
                                chain_fault: get_user_pages(current, mm, addr,
                                                1, 1, 0, &page, NULL);

                                goto retry;
                        }
                }
        }
        pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
        if (unlikely(!pte_same(*pte, pte_entry))) {
                pte_unmap_unlock(pte, ptl);
                goto retry;
        }
        page = vm_normal_page(vma, addr, *pte);
        if (!page) {
                // DSM3 : follow_page uses - goto bad_page; when !ZERO_PAGE..? wtf
                if (pte_pfn(*pte) == (unsigned long) (void *) ZERO_PAGE(0))
                        goto bad_page;

                page = pte_page(*pte);
        }

        if (unlikely(PageTransHuge(page))) {
                printk("[dsm_extract_page] we have a huge page \n");
                if (!PageHuge(page) && PageAnon(page)) {
                        if (unlikely(split_huge_page(page))) {
                                printk(
                                                "[dsm_extract_page] failed at splitting page \n");
                                goto bad_page;
                        }

                }
        }
        if (unlikely(PageKsm(page))) {
                printk("[dsm_extract_page] KSM page\n");

                r = ksm_madvise(vma, addr, addr + PAGE_SIZE, MADV_UNMERGEABLE
                                , &vma->vm_flags);

                if (r) {
                        printk("[dsm_extract_page] ksm_madvise ret : %d\n", r);

                        // DSM1 : better ksm error handling required.
                        goto bad_page;
                }
        }

        if (unlikely(!trylock_page(page))) {
                printk("[[EXTRACT_PAGE]] cannot lock page\n");
                goto bad_page;
        }

        flush_cache_page(vma, addr, pte_pfn(*pte));
        ptep_clear_flush_notify(vma, addr, pte);
        set_pte_at(
                        mm,
                        addr,
                        pte,
                        swp_entry_to_pte(
                                        make_dsm_entry((uint16_t) id.dsm_id,
                                                        (uint8_t) id.svm_id)));

        page_remove_rmap(page);

        dec_mm_counter(mm, MM_ANONPAGES);
        // this is a page flagging without data exchange so we can free the page
        if (likely(!page_mapped(page)))
                try_to_free_swap(page);
        //DSM1 do we need a put_page???/
        unlock_page(page);
        isolate_lru_page(page);
        if (PageActive(page)) {
                ClearPageActive(page);
        }

        pte_unmap_unlock(pte, ptl);
        // if local

        out: printk("[request_page_insert] got page %p  \n ", page);
        dsm_stats_page_extract_update(NULL);
        return page;

        bad_page:

        pte_unmap_unlock(pte, ptl);

        // if local

        return NULL;

}

static struct page *dsm_extract_page(struct dsm_vm_id id,
                struct subvirtual_machine *svm, unsigned long addr, int flag) {

        struct mm_struct *mm;
        struct page * page;
        mm = svm->priv->mm;
        // if local
        use_mm(mm);
        down_read(&mm->mmap_sem);

        page = _dsm_extract_page(id, mm, addr, flag);
        up_read(&mm->mmap_sem);
        unuse_mm(mm);

        return page;

}

struct page *dsm_extract_page_from_remote(struct dsm_message *msg) {
        struct dsm_vm_id remote_id;
        struct dsm_vm_id local_id;
        struct subvirtual_machine *local_svm;
        struct page *page = NULL;
        unsigned long norm_addr;
        int flag = 0;
        if (!msg) {
                printk("[dsm_extract_page_from_remote] no message ! %p  \n",
                                msg);
                return NULL;
        }
        remote_id.dsm_id = u32_to_dsm_id(msg->dest);
        remote_id.svm_id = u32_to_vm_id(msg->dest);

        local_id.dsm_id = u32_to_dsm_id(msg->src);
        local_id.svm_id = u32_to_vm_id(msg->src);
        local_svm = funcs->_find_svm(&local_id);
        if (unlikely(!local_svm)) {
                printk(
                                "[dsm_extract_page_from_remote] coudln't find local_svm id:  [dsm %d / svm %d]  \n",
                                local_id.dsm_id, local_id.svm_id);
                return NULL;
        }

        norm_addr = msg->req_addr + local_svm->priv->offset;
        if (msg->type == TRY_REQUEST_PAGE
        )
                flag = TRY_FLAG;
        page = dsm_extract_page(remote_id, local_svm, norm_addr, flag);

        return page;
}
EXPORT_SYMBOL(dsm_extract_page_from_remote);

/*
 * Local node A sends a blue page to node B, the dsm_swp_entry on node A points to B.  Node C requests the page from Node A,
 * Node A forwards the request to Node B, which sends the page to Node C and sets the dsm_swp_entry to Node C.
 *
 * When Node D requests the page from Node A, the request needs to be passed along the whole chain until hitting Node C, which can
 * process the request and send the page.
 *
 * This function will allow the updating of PTE values along the chain.  Node C will send the update command to
 * Node A, it will update the dsm_swap_entry to point to Node C, then forward the command to each Node along the chain.
 *
 * Node D then requests the page from Node A, the request is now passed straight to Node C.  It is asynchronous, if Node A is not
 * updated on time, the next Node can still pass the request along fine - either to the next node or directly to the final.
 *
 */
int dsm_update_pte_entry(struct dsm_message *msg) // DSM1 - update all code
{
        spinlock_t *ptl;
        pte_t *pte;
        int r = 0;
        struct vm_area_struct *vma;
        struct dsm_vm_id id;
        pgd_t *pgd;
        pud_t *pud;
        pmd_t *pmd;
        pte_t pte_entry;
        struct subvirtual_machine *svm;
        swp_entry_t swp_e;
        struct mm_struct *mm;

        id.dsm_id = u32_to_dsm_id(msg->dest);
        id.svm_id = u32_to_vm_id(msg->dest);

        svm = funcs->_find_svm(&id);
        BUG_ON(!svm);
        mm = svm->priv->mm;
        down_read(&mm->mmap_sem);
        retry:

        vma = find_vma(mm, msg->req_addr);
        if (!vma || vma->vm_start > msg->req_addr)
                goto out;

        pgd = pgd_offset(mm, msg->req_addr);
        if (!pgd_present(*pgd))
                goto out;

        pud = pud_offset(pgd, msg->req_addr);
        if (!pud_present(*pud))
                goto out;

        pmd = pmd_offset(pud, msg->req_addr);
        BUG_ON(pmd_trans_huge(*pmd));
        if (!pmd_present(*pmd))
                goto out;

        pte = pte_offset_map_lock(mm, pmd, msg->req_addr, &ptl);
        pte_entry = *pte;

        if (!pte_present(pte_entry)) {
                if (pte_none(pte_entry)) {
                        set_pte_at(
                                        mm,
                                        msg->req_addr,
                                        pte,
                                        swp_entry_to_pte(
                                                        make_dsm_entry(
                                                                        (uint16_t) id.dsm_id,
                                                                        (uint8_t) id.svm_id)));
                } else {
                        swp_e = pte_to_swp_entry(pte_entry);
                        if (!non_swap_entry(swp_e)) {
                                if (is_dsm_entry(swp_e)) {
                                        // store old dest
                                        struct dsm_vm_id old;

                                        dsm_entry_to_val(
                                                        pte_to_swp_entry(
                                                                        pte_entry),
                                                        &old.dsm_id,
                                                        &old.svm_id);

                                        if (old.dsm_id != id.dsm_id
                                                        && old.svm_id
                                                                        != id.svm_id) {
                                                // update pte
                                                set_pte_at(
                                                                mm,
                                                                msg->req_addr,
                                                                pte,
                                                                swp_entry_to_pte(
                                                                                make_dsm_entry(
                                                                                                (uint16_t) id.dsm_id,
                                                                                                (uint8_t) id.svm_id)));

                                                // forward msg
                                                // DSM1: fwd message RDMA function call.
                                                // old.dsm_id, old.svm_id.
                                        }
                                } else if (is_migration_entry(swp_e)) {
                                        pte_unmap_unlock(pte, ptl);

                                        migration_entry_wait(mm, pmd,
                                                        msg->req_addr);

                                        goto retry;
                                } else {
                                        printk(
                                                        "[*] SWP_ENTRY - not dsm or migration.\n");
                                        BUG();
                                }
                        } else {
                                printk("[*] in swap no need to update\n");
                        }
                }
        }

        pte_unmap_unlock(pte, ptl);

        out:

        up_read(&mm->mmap_sem);

        return r;

}
EXPORT_SYMBOL(dsm_update_pte_entry);

