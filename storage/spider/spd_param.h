/* Copyright (C) 2008-2017 Kentoku Shiba

  This program is free software); you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation); version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY); without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program); if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

my_bool spider_param_support_xa();
my_bool spider_param_connect_mutex();
uint spider_param_connect_error_interval();
uint spider_param_table_init_error_interval();
int spider_param_use_table_charset(int use_table_charset);
uint spider_param_conn_recycle_mode(THD *thd);
uint spider_param_conn_recycle_strict(THD *thd);
bool spider_param_sync_trx_isolation(THD *thd);
bool spider_param_use_consistent_snapshot(THD *thd);
bool spider_param_internal_xa(THD *thd);
uint spider_param_internal_xa_snapshot(THD *thd);
uint spider_param_force_commit(THD *thd);
uint spider_param_xa_register_mode(THD *thd);
longlong spider_param_internal_offset(THD *thd, longlong internal_offset);
longlong spider_param_internal_limit(THD *thd, longlong internal_limit);
longlong spider_param_split_read(THD *thd, longlong split_read);
double spider_param_semi_split_read(THD *thd, double semi_split_read);
longlong spider_param_semi_split_read_limit(THD *thd,
                                            longlong semi_split_read_limit);
int spider_param_init_sql_alloc_size(THD *thd, int init_sql_alloc_size);
int spider_param_reset_sql_alloc(THD *thd, int reset_sql_alloc);
int spider_param_multi_split_read(THD *thd, int multi_split_read);
int spider_param_max_order(THD *thd, int max_order);
int spider_param_semi_trx_isolation(THD *thd);
int spider_param_semi_table_lock(THD *thd, int semi_table_lock);
int spider_param_semi_table_lock_connection(THD *thd,
                                            int semi_table_lock_connection);
uint spider_param_block_size(THD *thd);
int spider_param_selupd_lock_mode(THD *thd, int selupd_lock_mode);
bool spider_param_sync_autocommit(THD *thd);
bool spider_param_sync_time_zone(THD *thd);
bool spider_param_use_default_database(THD *thd);
int spider_param_internal_sql_log_off(THD *thd);
int spider_param_bulk_size(THD *thd, int bulk_size);
int spider_param_bulk_update_mode(THD *thd, int bulk_update_mode);
int spider_param_bulk_update_size(THD *thd, int bulk_update_size);
int spider_param_internal_optimize(THD *thd, int internal_optimize);
int spider_param_internal_optimize_local(THD *thd, int internal_optimize_local);
bool spider_param_use_flash_logs(THD *thd);
int spider_param_use_snapshot_with_flush_tables(THD *thd);
bool spider_param_use_all_conns_snapshot(THD *thd);
bool spider_param_lock_exchange(THD *thd);
bool spider_param_internal_unlock(THD *thd);
bool spider_param_semi_trx(THD *thd);
int spider_param_connect_timeout(THD *thd, int connect_timeout);
int spider_param_net_read_timeout(THD *thd, int net_read_timeout);
int spider_param_net_write_timeout(THD *thd, int net_write_timeout);
int spider_param_quick_mode(THD *thd, int quick_mode);
longlong spider_param_quick_page_size(THD *thd, longlong quick_page_size);
int spider_param_low_mem_read(THD *thd, int low_mem_read);
int spider_param_select_column_mode(THD *thd, int select_column_mode);
int spider_param_bgs_mode(THD *thd, int bgs_mode);
int spider_param_bgs_dml(THD *thd);
bool spider_param_ignore_xa_log(THD *thd);
longlong spider_param_bgs_first_read(THD *thd, longlong bgs_first_read);
longlong spider_param_bgs_second_read(THD *thd, longlong bgs_second_read);
longlong spider_param_first_read(THD *thd, longlong first_read);
longlong spider_param_second_read(THD *thd, longlong second_read);
double spider_param_crd_interval(THD *thd, double crd_interval);
int spider_param_crd_mode(THD *thd, int crd_mode);
#ifdef WITH_PARTITION_STORAGE_ENGINE
int spider_param_crd_sync(THD *thd, int crd_sync);
#endif
int spider_param_crd_type(THD *thd, int crd_type);
double spider_param_crd_weight(THD *thd, double crd_weight);
int spider_param_crd_bg_mode(THD *thd, int crd_bg_mode);
double spider_param_sts_interval(THD *thd, double sts_interval);
int spider_param_sts_mode(THD *thd, int sts_mode);
#ifdef WITH_PARTITION_STORAGE_ENGINE
int spider_param_sts_sync(THD *thd, int sts_sync);
#endif
int spider_param_sts_bg_mode(THD *thd, int sts_bg_mode);
double spider_param_ping_interval_at_trx_start(THD *thd);
int spider_param_auto_increment_mode(THD *thd, int auto_increment_mode);
bool spider_param_same_server_link(THD *thd);
bool spider_param_with_begin_commit(THD *thd);
bool spider_param_get_conn_from_idx(THD *thd);
bool spider_param_client_found_rows(THD *thd);
bool spider_param_get_sts_or_crd();
bool spider_param_local_lock_table(THD *thd);
int spider_param_use_pushdown_udf(THD *thd, int use_pushdown_udf);
int spider_param_direct_dup_insert(THD *thd, int direct_dup_insert);
int spider_param_direct_insert_ignore(THD *thd);
uint spider_param_udf_table_lock_mutex_count();
uint spider_param_udf_table_mon_mutex_count();
longlong spider_param_udf_ds_bulk_insert_rows(THD *thd,
                                              longlong udf_ds_bulk_insert_rows);
int spider_param_udf_ds_table_loop_mode(THD *thd, int udf_ds_table_loop_mode);
char *spider_param_remote_access_charset();
int spider_param_remote_autocommit();
char *spider_param_remote_time_zone();
int spider_param_remote_sql_log_off();
int spider_param_remote_trx_isolation();
char *spider_param_remote_default_database();
longlong spider_param_connect_retry_interval(THD *thd);
int spider_param_connect_retry_count(THD *thd);
char *spider_param_bka_engine(THD *thd, char *bka_engine);
int spider_param_bka_mode(THD *thd, int bka_mode);
int spider_param_udf_ct_bulk_insert_interval(int udf_ct_bulk_insert_interval);
longlong spider_param_udf_ct_bulk_insert_rows(longlong udf_ct_bulk_insert_rows);
int spider_param_use_handler(THD *thd, int use_handler);
int spider_param_error_read_mode(THD *thd, int error_read_mode);
int spider_param_error_write_mode(THD *thd, int error_write_mode);
int spider_param_skip_default_condition(THD *thd, int skip_default_condition);
int spider_param_skip_parallel_search(THD *thd, int skip_parallel_search);
longlong spider_param_direct_order_limit(THD *thd, longlong direct_order_limit);
int spider_param_read_only_mode(THD *thd, int read_only_mode);
#ifdef HA_CAN_BULK_ACCESS
int spider_param_bulk_access_free(int bulk_access_free);
#endif
#if MYSQL_VERSION_ID < 50500
#else
int spider_param_udf_ds_use_real_table(THD *thd, int udf_ds_use_real_table);
#endif
my_bool spider_param_general_log();
int spider_param_idle_conn_recycle_interval();
int spider_param_conn_meta_invalid_max_count();
my_bool spider_param_index_hint_pushdown(THD *thd);
my_bool spider_param_enable_mem_calc();
my_bool spider_param_enable_trx_ha();
uint spider_param_max_connections();
uint spider_param_conn_wait_timeout();
my_bool spider_param_fetch_minimum_columns();
my_bool spider_param_ignore_autocommit();
my_bool spider_param_quick_mode_only_select();
uint spider_param_log_result_errors(THD *thd);
uint spider_param_log_result_error_with_sql(THD *thd);
uint spider_param_internal_xa_id_type(THD *thd);
int spider_param_casual_read(THD *thd, int casual_read);
my_bool spider_param_dry_access();
int spider_param_delete_all_rows_type(THD *thd, int delete_all_rows_type);
int spider_param_bka_table_name_type(THD *thd, int bka_table_name_type);
int spider_param_store_last_sts(int store_last_sts);
int spider_param_store_last_crd(int store_last_crd);
int spider_param_load_sts_at_startup(int load_sts_at_startup);
int spider_param_load_crd_at_startup(int load_crd_at_startup);
uint spider_param_table_sts_thread_count();
uint spider_param_table_crd_thread_count();
bool spider_param_trans_rollback(THD *thd);
bool spider_param_string_key_equal_to_like(THD *thd);
uint spider_param_active_conns_view_info_length();
my_bool spider_param_enable_active_conns_view();
bool spider_param_parallel_limit(THD *thd);
