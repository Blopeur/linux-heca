/*
 * Benoit Hudzia <benoit.hudzia@sap.com> 2011 (c)
 * Roei Tell <roei.tell@sap.com> 2012 (c)
 * Aidan Shribman <aidan.shribman@sap.com> 2012 (c)
 */

#ifndef _HECA_CONN_H
#define _HECA_CONN_H

#include <linux/in.h>
#include "struct.h"

void init_kmem_request_cache(void);
void destroy_kmem_request_cache(void);
inline struct heca_request *alloc_dsm_request(void);
inline void release_dsm_request(struct heca_request *);
int add_dsm_request(struct heca_request *, struct heca_connection_element *,
                u16, u32, u32, u32, u32, unsigned long,
                int (*)(struct tx_buffer_element *), struct heca_page_cache *,
                struct page *, struct heca_page_pool_element *, int,
                struct heca_message *);
inline int request_queue_empty(struct heca_connection_element *);
inline int request_queue_full(struct heca_connection_element *);
void dsm_request_queue_merge(struct tx_buffer *);
void create_page_reclaim_request(struct tx_buffer_element *, u32, u32
                , u32, u32, uint64_t);
void create_page_request(struct heca_connection_element *,
                struct tx_buffer_element *, u32, u32, u32, u32, uint64_t,
                struct page *, struct heca_page_cache *,
                struct heca_page_pool_element *);
void create_page_pull_request(struct heca_connection_element *,
                struct tx_buffer_element *, u32, u32, u32, u32, uint64_t);
void listener_cq_handle(struct ib_cq *, void *);
int server_event_handler(struct rdma_cm_id *, struct rdma_cm_event *);
inline void dsm_msg_cpy(struct heca_message *, struct heca_message *);
void release_tx_element(struct heca_connection_element *,
                struct tx_buffer_element *);
void release_tx_element_reply(struct heca_connection_element *,
                struct tx_buffer_element *);
void try_release_tx_element(struct heca_connection_element *,
                struct tx_buffer_element *);
int connect_svm(__u32, __u32, unsigned long, unsigned short);
unsigned long inet_addr(const char *);
char *inet_ntoa(unsigned long, char *, int);
struct tx_buffer_element *try_get_next_empty_tx_ele(
                struct heca_connection_element *, int);
struct tx_buffer_element *try_get_next_empty_tx_reply_ele(
                struct heca_connection_element *);
int destroy_connection(struct heca_connection_element *);
int tx_dsm_send(struct heca_connection_element *, struct tx_buffer_element *);
char *port_ntoa(unsigned short, char *, int);
char *sockaddr_ntoa(struct sockaddr_in *, char *, int);
char *conn_ntoa(struct sockaddr_in *, struct sockaddr_in *, char *, int);
int dsm_send_tx_e(struct heca_connection_element *, struct tx_buffer_element *, int, int, u32,
                u32, u32, u32, unsigned long, unsigned long,
                struct heca_page_cache *, struct page *,
                struct heca_page_pool_element *, int,
                int (*)(struct tx_buffer_element *), struct heca_message *);

#endif /* _HECA_CONN_H */
