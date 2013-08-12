#ifndef _HECA_PUSH_H
#define _HECA_PUSH_H

#define DSM_EXTRACT_SUCCESS     (1 << 0)
#define DSM_EXTRACT_REDIRECT    (1 << 1)
#define DSM_EXTRACT_FAIL        (1 << 2)

inline int dsm_is_congested(void);
inline void dsm_push_cache_release(struct subvirtual_machine *,
                struct dsm_page_cache **, int);
struct dsm_page_cache *dsm_push_cache_get_remove(struct subvirtual_machine *,
                unsigned long);
int dsm_extract_pte_data(struct dsm_pte_data *, struct mm_struct *,
                unsigned long);
int dsm_try_unmap_page(struct subvirtual_machine *, unsigned long,
                struct subvirtual_machine *, int);
int dsm_extract_page_from_remote(struct subvirtual_machine *,
                struct subvirtual_machine *, unsigned long, u16, pte_t *,
                struct page **, u32 *, int, struct memory_region *);
struct page *dsm_find_normal_page(struct mm_struct *, unsigned long);
int dsm_prepare_page_for_push(struct subvirtual_machine *,
                struct svm_list, struct page *, unsigned long,
                struct mm_struct *, u32);
int dsm_cancel_page_push(struct subvirtual_machine *, unsigned long,
                struct page *);
int push_back_if_remote_dsm_page(struct page *);
int dsm_flag_page_remote(struct mm_struct *, struct heca_space *, u32, unsigned long);
u32 dsm_query_pte_info(struct subvirtual_machine *, unsigned long);
void dsm_invalidate_readers(struct subvirtual_machine *, unsigned long, u32);
int dsm_pte_present(struct mm_struct *, unsigned long);

#endif /* _HECA_PUSH_H */

