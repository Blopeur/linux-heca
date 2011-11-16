/*
 * dsm_page_fault.h
 *
 *  Created on: 1 Aug 2011
 *      Author: john
 */

#ifndef DSM_PAGE_FAULT_H_
#define DSM_PAGE_FAULT_H_

#include <dsm/dsm_def.h>
#include <linux/swap.h>

struct dsm_functions {
        struct subvirtual_machine *(*_find_svm)(struct dsm_vm_id *); //_find_svm;
        struct subvirtual_machine *(*_find_local_svm)(u16, struct mm_struct *); //_find_local_svm;
        int (*request_dsm_page)(conn_element *, struct dsm_vm_id,
                        struct dsm_vm_id, uint64_t, struct page *,
                        void(*func)(struct tx_buf_ele *));

};

// dsm_unmap
void reg_dsm_functions(
                struct subvirtual_machine *(*_find_svm)(struct dsm_vm_id *),
                struct subvirtual_machine *(*_find_local_svm)(u16,
                                struct mm_struct *),
                int(*request_dsm_page)(conn_element *, struct dsm_vm_id,
                                struct dsm_vm_id, uint64_t, struct page *,
                                void(*func)(struct tx_buf_ele *)));
void dereg_dsm_functions(void);
int dsm_flag_page_remote(struct mm_struct *mm, struct dsm_vm_id id,
                unsigned long addr);

// dsm_page_fault
int dsm_swap_wrapper(struct mm_struct *, struct vm_area_struct *, unsigned long,
                pte_t *, pmd_t *, unsigned int, pte_t, swp_entry_t);
void signal_completion_page_request(struct tx_buf_ele *);

struct page *dsm_extract_page_from_remote(dsm_message *);

extern struct dsm_functions *funcs;

#endif /* DSM_PAGE_FAULT_H_ */
