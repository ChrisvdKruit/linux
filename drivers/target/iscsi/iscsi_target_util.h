#ifndef ISCSI_TARGET_UTIL_H
#define ISCSI_TARGET_UTIL_H

#define MARKER_SIZE	8

struct se_cmd;

struct se_offset_map {
	int                     map_reset;
	u32                     iovec_length;
	u32                     iscsi_offset;
	u32                     current_offset;
	u32                     orig_offset;
	u32                     sg_count;
	u32                     sg_current;
	u32                     sg_length;
	struct page		*sg_page;
	struct se_mem		*map_se_mem;
	struct se_mem		*map_orig_se_mem;
	void			*iovec_base;
} ____cacheline_aligned;

struct se_map_sg {
	int			sg_kmap_active:1;
	u32			data_length;
	u32			data_offset;
	void			*fabric_cmd;
	struct se_cmd		*se_cmd;
	struct iovec		*iov;
} ____cacheline_aligned;

struct se_unmap_sg {
	u32			data_length;
	u32			sg_count;
	u32			sg_offset;
	u32			padding;
	u32			t_offset;
	void			*fabric_cmd;
	struct se_cmd		*se_cmd;
	struct se_offset_map	lmap;
	struct se_mem		*cur_se_mem;
} ____cacheline_aligned;

extern void iscsi_attach_cmd_to_queue(struct iscsi_conn *, struct iscsi_cmd *);
extern void iscsi_remove_cmd_from_conn_list(struct iscsi_cmd *, struct iscsi_conn *);
extern void iscsi_ack_from_expstatsn(struct iscsi_conn *, u32);
extern void iscsi_remove_conn_from_list(struct iscsi_session *, struct iscsi_conn *);
extern int iscsi_add_r2t_to_list(struct iscsi_cmd *, u32, u32, int, u32);
extern struct iscsi_r2t *iscsi_get_r2t_for_eos(struct iscsi_cmd *, u32, u32);
extern struct iscsi_r2t *iscsi_get_r2t_from_list(struct iscsi_cmd *);
extern void iscsi_free_r2t(struct iscsi_r2t *, struct iscsi_cmd *);
extern void iscsi_free_r2ts_from_list(struct iscsi_cmd *);
extern struct iscsi_cmd *iscsi_allocate_cmd(struct iscsi_conn *);
extern struct iscsi_cmd *iscsi_allocate_se_cmd(struct iscsi_conn *, u32, int, int);
extern struct iscsi_cmd *iscsi_allocate_se_cmd_for_tmr(struct iscsi_conn *, u8);
extern int iscsi_decide_list_to_build(struct iscsi_cmd *, u32);
extern struct iscsi_seq *iscsi_get_seq_holder_for_datain(struct iscsi_cmd *, u32);
extern struct iscsi_seq *iscsi_get_seq_holder_for_r2t(struct iscsi_cmd *);
extern struct iscsi_r2t *iscsi_get_holder_for_r2tsn(struct iscsi_cmd *, u32);
extern int iscsi_check_received_cmdsn(struct iscsi_conn *, struct iscsi_cmd *, u32);
extern int iscsi_check_unsolicited_dataout(struct iscsi_cmd *, unsigned char *);
extern struct iscsi_cmd *iscsi_find_cmd_from_itt(struct iscsi_conn *, u32);
extern struct iscsi_cmd *iscsi_find_cmd_from_itt_or_dump(struct iscsi_conn *,
			u32, u32);
extern struct iscsi_cmd *iscsi_find_cmd_from_ttt(struct iscsi_conn *, u32);
extern int iscsi_find_cmd_for_recovery(struct iscsi_session *, struct iscsi_cmd **,
			struct iscsi_conn_recovery **, u32);
extern void iscsi_add_cmd_to_immediate_queue(struct iscsi_cmd *, struct iscsi_conn *, u8);
extern struct iscsi_queue_req *iscsi_get_cmd_from_immediate_queue(struct iscsi_conn *);
extern void iscsi_add_cmd_to_response_queue(struct iscsi_cmd *, struct iscsi_conn *, u8);
extern struct iscsi_queue_req *iscsi_get_cmd_from_response_queue(struct iscsi_conn *);
extern void iscsi_remove_cmd_from_tx_queues(struct iscsi_cmd *, struct iscsi_conn *);
extern void iscsi_free_queue_reqs_for_conn(struct iscsi_conn *);
extern void iscsi_release_cmd_direct(struct iscsi_cmd *);
extern void lio_release_cmd_direct(struct se_cmd *);
extern void __iscsi_release_cmd_to_pool(struct iscsi_cmd *, struct iscsi_session *);
extern void iscsi_release_cmd_to_pool(struct iscsi_cmd *);
extern void lio_release_cmd_to_pool(struct se_cmd *);
extern u64 iscsi_pack_lun(unsigned int);
extern u32 iscsi_unpack_lun(unsigned char *);
extern int iscsi_check_session_usage_count(struct iscsi_session *);
extern void iscsi_dec_session_usage_count(struct iscsi_session *);
extern void iscsi_inc_session_usage_count(struct iscsi_session *);
extern int iscsi_set_sync_and_steering_values(struct iscsi_conn *);
extern unsigned char *iscsi_ntoa(u32);
extern void iscsi_ntoa2(unsigned char *, u32);
extern const char *iscsi_ntop6(const unsigned char *, char *, size_t);
extern int iscsi_pton6(const char *, unsigned char *);
extern struct iscsi_conn *iscsi_get_conn_from_cid(struct iscsi_session *, u16);
extern struct iscsi_conn *iscsi_get_conn_from_cid_rcfr(struct iscsi_session *, u16);
extern void iscsi_check_conn_usage_count(struct iscsi_conn *);
extern void iscsi_dec_conn_usage_count(struct iscsi_conn *);
extern void iscsi_inc_conn_usage_count(struct iscsi_conn *);
extern void iscsi_async_msg_timer_function(unsigned long);
extern int iscsi_check_for_active_network_device(struct iscsi_conn *);
extern void iscsi_get_network_interface_from_conn(struct iscsi_conn *);
extern void iscsi_start_netif_timer(struct iscsi_conn *);
extern void iscsi_stop_netif_timer(struct iscsi_conn *);
extern void iscsi_mod_nopin_response_timer(struct iscsi_conn *);
extern void iscsi_start_nopin_response_timer(struct iscsi_conn *);
extern void iscsi_stop_nopin_response_timer(struct iscsi_conn *);
extern void __iscsi_start_nopin_timer(struct iscsi_conn *);
extern void iscsi_start_nopin_timer(struct iscsi_conn *);
extern void iscsi_stop_nopin_timer(struct iscsi_conn *);
extern int iscsi_allocate_iovecs_for_cmd(struct se_cmd *);
extern int iscsi_send_tx_data(struct iscsi_cmd *, struct iscsi_conn *, int);
extern int iscsi_fe_sendpage_sg(struct se_unmap_sg *, struct iscsi_conn *);
extern int iscsi_tx_login_rsp(struct iscsi_conn *, u8, u8);
extern void iscsi_print_session_params(struct iscsi_session *);
extern int iscsi_print_dev_to_proc(char *, char **, off_t, int);
extern int iscsi_print_sessions_to_proc(char *, char **, off_t, int);
extern int iscsi_print_tpg_to_proc(char *, char **, off_t, int);
extern int rx_data(struct iscsi_conn *, struct iovec *, int, int);
extern int tx_data(struct iscsi_conn *, struct iovec *, int, int);
extern void iscsi_collect_login_stats(struct iscsi_conn *, u8, u8);
extern struct iscsi_tiqn *iscsi_snmp_get_tiqn(struct iscsi_conn *);
extern int iscsi_build_sendtargets_response(struct iscsi_cmd *);

extern struct target_fabric_configfs *lio_target_fabric_configfs;
extern struct iscsi_global *iscsi_global;
extern struct kmem_cache *lio_cmd_cache;
extern struct kmem_cache *lio_qr_cache;
extern struct kmem_cache *lio_r2t_cache;

extern int iscsi_add_nopin(struct iscsi_conn *, int);

#endif /*** ISCSI_TARGET_UTIL_H ***/

