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
                        struct dsm_vm_id, uint64_t,
                        void(*)(struct tx_buf_ele *, unsigned long),
                        unsigned long);
};

// dsm_unmap
void reg_dsm_functions(
                struct subvirtual_machine *(*_find_svm)(struct dsm_vm_id *),
                struct subvirtual_machine *(*_find_local_svm)(u16,
                                struct mm_struct *),
                int(*request_dsm_page)(conn_element *, struct dsm_vm_id,
                                struct dsm_vm_id, uint64_t,
                                void(*)(struct tx_buf_ele *, unsigned long),
                                unsigned long));
void dereg_dsm_functions(void);
int dsm_flag_page_remote(struct mm_struct *mm, struct dsm_vm_id id,
                unsigned long addr);

// dsm_page_request
struct page * dsm_extract_page_from_remote(dsm_message *msg);
struct page * dsm_extract_page(struct dsm_vm_id id,
                struct subvirtual_machine *route_e, unsigned long norm_addr);

// dsm_page_fault
int dsm_swap_wrapper(struct mm_struct *, struct vm_area_struct *, unsigned long,
                pte_t *, swp_entry_t *, pmd_t *, unsigned int);
int dsm_insert_page(struct mm_struct *, struct vm_area_struct *, pte_t *,
                unsigned long, struct page *, struct dsm_vm_id *);

extern struct dsm_functions *funcs;
extern unsigned long dst_addr;
extern struct page *kpage;

#endif /* DSM_PAGE_FAULT_H_ */
