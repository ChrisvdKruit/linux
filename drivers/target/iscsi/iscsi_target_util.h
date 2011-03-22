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
};

struct se_map_sg {
	int			sg_kmap_active:1;
	u32			data_length;
	u32			data_offset;
	void			*fabric_cmd;
	struct se_cmd		*se_cmd;
	struct kvec		*iov;
};

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
};

extern int iscsit_add_r2t_to_list(struct iscsi_cmd *, u32, u32, int, u32);
extern struct iscsi_r2t *iscsit_get_r2t_for_eos(struct iscsi_cmd *, u32, u32);
extern struct iscsi_r2t *iscsit_get_r2t_from_list(struct iscsi_cmd *);
extern void iscsit_free_r2t(struct iscsi_r2t *, struct iscsi_cmd *);
extern void iscsit_free_r2ts_from_list(struct iscsi_cmd *);
extern struct iscsi_cmd *iscsit_allocate_cmd(struct iscsi_conn *);
extern struct iscsi_cmd *iscsit_allocate_se_cmd(struct iscsi_conn *, u32, int, int);
extern struct iscsi_cmd *iscsit_allocate_se_cmd_for_tmr(struct iscsi_conn *, u8);
extern int iscsit_decide_list_to_build(struct iscsi_cmd *, u32);
extern struct iscsi_seq *iscsit_get_seq_holder_for_datain(struct iscsi_cmd *, u32);
extern struct iscsi_seq *iscsit_get_seq_holder_for_r2t(struct iscsi_cmd *);
extern struct iscsi_r2t *iscsit_get_holder_for_r2tsn(struct iscsi_cmd *, u32);
extern int iscsit_check_received_cmdsn(struct iscsi_conn *, struct iscsi_cmd *, u32);
extern int iscsit_check_unsolicited_dataout(struct iscsi_cmd *, unsigned char *);
extern struct iscsi_cmd *iscsit_find_cmd_from_itt(struct iscsi_conn *, u32);
extern struct iscsi_cmd *iscsit_find_cmd_from_itt_or_dump(struct iscsi_conn *,
			u32, u32);
extern struct iscsi_cmd *iscsit_find_cmd_from_ttt(struct iscsi_conn *, u32);
extern int iscsit_find_cmd_for_recovery(struct iscsi_session *, struct iscsi_cmd **,
			struct iscsi_conn_recovery **, u32);
extern void iscsit_add_cmd_to_immediate_queue(struct iscsi_cmd *, struct iscsi_conn *, u8);
extern struct iscsi_queue_req *iscsit_get_cmd_from_immediate_queue(struct iscsi_conn *);
extern void iscsit_add_cmd_to_response_queue(struct iscsi_cmd *, struct iscsi_conn *, u8);
extern struct iscsi_queue_req *iscsit_get_cmd_from_response_queue(struct iscsi_conn *);
extern void iscsit_remove_cmd_from_tx_queues(struct iscsi_cmd *, struct iscsi_conn *);
extern void iscsit_free_queue_reqs_for_conn(struct iscsi_conn *);
extern void iscsit_release_cmd(struct iscsi_cmd *);
extern int iscsit_check_session_usage_count(struct iscsi_session *);
extern void iscsit_dec_session_usage_count(struct iscsi_session *);
extern void iscsit_inc_session_usage_count(struct iscsi_session *);
extern int iscsit_set_sync_and_steering_values(struct iscsi_conn *);
extern void iscsit_ntoa2(unsigned char *, u32);
extern struct iscsi_conn *iscsit_get_conn_from_cid(struct iscsi_session *, u16);
extern struct iscsi_conn *iscsit_get_conn_from_cid_rcfr(struct iscsi_session *, u16);
extern void iscsit_check_conn_usage_count(struct iscsi_conn *);
extern void iscsit_dec_conn_usage_count(struct iscsi_conn *);
extern void iscsit_inc_conn_usage_count(struct iscsi_conn *);
extern void iscsit_mod_nopin_response_timer(struct iscsi_conn *);
extern void iscsit_start_nopin_response_timer(struct iscsi_conn *);
extern void iscsit_stop_nopin_response_timer(struct iscsi_conn *);
extern void __iscsit_start_nopin_timer(struct iscsi_conn *);
extern void iscsit_start_nopin_timer(struct iscsi_conn *);
extern void iscsit_stop_nopin_timer(struct iscsi_conn *);
extern int iscsit_send_tx_data(struct iscsi_cmd *, struct iscsi_conn *, int);
extern int iscsit_fe_sendpage_sg(struct se_unmap_sg *, struct iscsi_conn *);
extern int iscsit_tx_login_rsp(struct iscsi_conn *, u8, u8);
extern void iscsit_print_session_params(struct iscsi_session *);
extern int iscsit_print_dev_to_proc(char *, char **, off_t, int);
extern int iscsit_print_sessions_to_proc(char *, char **, off_t, int);
extern int iscsit_print_tpg_to_proc(char *, char **, off_t, int);
extern int rx_data(struct iscsi_conn *, struct kvec *, int, int);
extern int tx_data(struct iscsi_conn *, struct kvec *, int, int);
extern void iscsit_collect_login_stats(struct iscsi_conn *, u8, u8);
extern struct iscsi_tiqn *iscsit_snmp_get_tiqn(struct iscsi_conn *);
extern int iscsit_build_sendtargets_response(struct iscsi_cmd *);

extern struct target_fabric_configfs *lio_target_fabric_configfs;
extern struct iscsi_global *iscsi_global;
extern struct kmem_cache *lio_cmd_cache;
extern struct kmem_cache *lio_qr_cache;
extern struct kmem_cache *lio_r2t_cache;

#endif /*** ISCSI_TARGET_UTIL_H ***/

