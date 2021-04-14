/* Copyright (C) 2008-2017 Kentoku Shiba

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#define MYSQL_SERVER 1
#include <my_global.h>
#include "mysql_version.h"
#include "spd_environ.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "sql_servers.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "tztime.h"
#endif
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "ha_spider.h"
#include "spd_db_conn.h"
#include "spd_trx.h"
#include "spd_table.h"
#include "spd_direct_sql.h"
#include "spd_ping_table.h"
#include "spd_malloc.h"
#include "spd_err.h"
#include "spd_conn.h"
#include <mysql.h>

#ifdef SPIDER_HAS_NEXT_THREAD_ID
#define SPIDER_set_next_thread_id(A)
#else
extern ulong *spd_db_att_thread_id;
inline void SPIDER_set_next_thread_id(THD *A) {
  pthread_mutex_lock(&LOCK_thread_count);
  A->thread_id = (*spd_db_att_thread_id)++;
  pthread_mutex_unlock(&LOCK_thread_count);
}
#endif

extern handlerton *spider_hton_ptr;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];
// pthread_mutex_t spider_conn_id_mutex;
pthread_mutex_t spider_ipport_conn_mutex;
volatile longlong spider_conn_id = 0;

extern pthread_attr_t spider_pt_attr;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key spd_key_mutex_mta_conn;
extern PSI_mutex_key spd_key_mutex_conn_i;
extern PSI_cond_key spd_key_cond_conn_i;
extern PSI_mutex_key spd_key_mutex_bg_conn_chain;
extern PSI_mutex_key spd_key_mutex_bg_conn_sync;
extern PSI_mutex_key spd_key_mutex_bg_conn;
extern PSI_mutex_key spd_key_mutex_bg_job_stack;
extern PSI_mutex_key spd_key_mutex_bg_mon;
extern PSI_cond_key spd_key_cond_bg_conn_sync;
extern PSI_cond_key spd_key_cond_bg_conn;
extern PSI_cond_key spd_key_cond_bg_sts;
extern PSI_cond_key spd_key_cond_bg_sts_sync;
extern PSI_cond_key spd_key_cond_bg_crd;
extern PSI_cond_key spd_key_cond_bg_crd_sync;
extern PSI_cond_key spd_key_cond_bg_mon;
extern PSI_cond_key spd_key_cond_bg_mon_sleep;
extern PSI_thread_key spd_key_thd_bg;
extern PSI_thread_key spd_key_thd_bg_sts;
extern PSI_thread_key spd_key_thd_bg_crd;
extern PSI_thread_key spd_key_thd_bg_mon;
#endif

extern pthread_mutex_t spider_tbl_mutex;
extern SPIDER_TRX *spider_global_trx;

extern HASH spider_open_tables;
SPIDER_CONN_POOL spd_connect_pools;
HASH spider_ipport_conns;
HASH spider_for_sts_conns;
HASH spider_for_sts_share;
long spider_conn_mutex_id = 0;

const char *spider_open_connections_func_name;
const char *spider_open_connections_file_name;
ulong spider_open_connections_line_no;
// pthread_mutex_t spider_conn_mutex;
pthread_mutex_t spider_conn_meta_mutex;
HASH spider_conn_meta_info;

extern PSI_thread_key spd_key_thd_conn_rcyc;
volatile bool conn_rcyc_init = FALSE;
pthread_t conn_rcyc_thread;

extern PSI_thread_key spd_key_thd_get_status;
volatile bool get_status_init = FALSE;
pthread_t get_status_thread;

/**
  conn_queue is a queue (actually stack) to store SPIDER_CONN
  in order to reduce memcpy the whole SPIDER_CONN, we insert
  SPIDER_CONN ** into the queue
  @todo: change it from stack to queue, which can increase
         the performance of recycling.
*/
typedef struct{
  pthread_mutex_t mtx;   // mutex of the queue
  bool mtx_inited;       // whether the mutex has inited
  DYNAMIC_ARRAY *q_ptr;  // pointer of the queue (actually stack)
  char *hash_key;        // hash key of conn_queue
  uint key_len;          // length of the key
}conn_queue;

typedef struct {
  SPIDER_CONN_POOL *hash_info;
  DYNAMIC_STRING_ARRAY *arr_info[2];
} delegate_param;

/* HASH free function */
static void conn_pool_hash_free(void *entry) {
  conn_queue *cq = (conn_queue *)entry;
  assert(cq->q_ptr);
  clear_dynamic_array(cq->q_ptr); /* ignore failure */
  my_free(cq->q_ptr);
  cq->q_ptr = NULL;
  my_free(cq);
}

/** 
  Init spider open connections
  1. init rwlock, 2. init hash
  @param    get_key     get_key function, how to find key from the struct
  @param    init_cap    init capacity of the hash (init alloc)
  @param    charset     character set info
  @return   false if OK | TRUE if OOM
*/
bool SPIDER_CONN_POOL::init(my_hash_get_key get_key, uint init_cap,
                            CHARSET_INFO *charset) {
  mysql_rwlock_init(0, &rw_lock);
  if (my_hash_init(&connections, charset, init_cap, 0, 0,
                   (my_hash_get_key)get_key, 
                   (void (*)(void*))conn_pool_hash_free, HASH_UNIQUE))
    return true; /* out of memory */
  conn_inited = true;
  return false; /* OK */
}

/**
  Destroy spider open connections
  1. destroy hash 2. destroy rwlock
*/
void SPIDER_CONN_POOL::destroy() {
  if (conn_inited) {
    mysql_rwlock_destroy(&rw_lock);
    my_hash_free(&connections);
    conn_inited = false;
  }
}

/**
  Put the connection back to connection pool
  @param    conn    spider connection
  @return   FALSE if OK | TRUE if OOM
*/
bool SPIDER_CONN_POOL::put_conn(SPIDER_CONN *conn) {
  void *record = NULL;
  conn_queue *cq;
  my_bool ret;
  my_hash_value_type v = conn->conn_key_hash_value;
  
  mysql_rwlock_rdlock(&rw_lock);
  while (!(record = my_hash_search_using_hash_value(&connections, v,
          (uchar *)conn->conn_key, conn->conn_key_length))) {
    mysql_rwlock_unlock(&rw_lock);
    // if not exists, we need to create and insert a queue into the hash
    cq = (conn_queue *)my_malloc(sizeof(conn_queue), MY_ZEROFILL | MY_WME);
    if (!cq) return true; /* OOM */
    mysql_mutex_init(0, &cq->mtx, MY_MUTEX_INIT_FAST);
    cq->mtx_inited = true;
    cq->q_ptr = (DYNAMIC_ARRAY *)my_malloc(sizeof(DYNAMIC_ARRAY), MY_WME);
    if (!cq->q_ptr) { my_free(cq); cq->mtx_inited = false; return true; /* OOM */ }
    if (my_init_dynamic_array(cq->q_ptr, sizeof(SPIDER_CONN **), 64, 64, MYF(0))) {
      my_free(cq); cq->mtx_inited = false; return true;
    }
    cq->hash_key = conn->conn_key;
    cq->key_len = conn->conn_key_length;
    mysql_rwlock_wrlock(&rw_lock);
    if (my_hash_insert(&connections, (uchar *)cq)) {
      /* insert failed means some other thread has inserted it for us*/
      my_free(cq->q_ptr);
      my_free(cq);
    } else {
      record = (void *)cq;
      break;
    }
  }
  mysql_rwlock_unlock(&rw_lock);
  /* code reaches here means we got the queue */
  cq = (conn_queue *)(record);
  pthread_mutex_lock(&cq->mtx);
  ret = insert_dynamic(cq->q_ptr, (void *)(&conn)); /* SPIDER_CONN ** */
  pthread_mutex_unlock(&cq->mtx);
  return !!ret; // return TRUE means OOM
}

/**
  Search and delete spider conn from the pool using hash value
  @param    v       hash value
  @param    key     hash key
  @param    key_len length of the key
  @return   NULL if search/delete failed | SPD_CONN *
*/
SPIDER_CONN *SPIDER_CONN_POOL::get_conn(my_hash_value_type v,
                                        uchar *key, uint key_len) {
  SPIDER_CONN *spd_conn = NULL;
  SPIDER_CONN **conn_ptr = NULL;
  conn_queue *cq = NULL;

  mysql_rwlock_rdlock(&rw_lock);
  if (!(cq = (conn_queue *)my_hash_search_using_hash_value(
        &connections, v, key, key_len))) {
    mysql_rwlock_unlock(&rw_lock);
    return NULL; /* no queue of this hash value exist */
  }
  mysql_rwlock_unlock(&rw_lock);

  pthread_mutex_lock(&cq->mtx);
  conn_ptr = (SPIDER_CONN **)pop_dynamic(cq->q_ptr);
  if (conn_ptr) spd_conn = *conn_ptr;
  pthread_mutex_unlock(&cq->mtx);
  return spd_conn; /* NULL means the queue by this hash value is empty */
}

/**
  Search and delete spider conn from the pool using hash key
  @param    key     hash key
  @param    key_len length of the key
  @return NULL if search/delete failed | SPD_CONN *
*/
SPIDER_CONN *SPIDER_CONN_POOL::get_conn_by_key(uchar *key, uint key_len) {
  SPIDER_CONN *spd_conn = NULL;
  SPIDER_CONN **conn_ptr = NULL;
  conn_queue *cq = NULL;

  mysql_rwlock_rdlock(&rw_lock);
  if (!(cq = (conn_queue *)my_hash_search(&connections, key, key_len))) {
    mysql_rwlock_unlock(&rw_lock);
    return NULL; /* no queue of this hash key exist */
  }
  mysql_rwlock_unlock(&rw_lock);

  pthread_mutex_lock(&cq->mtx);
  conn_ptr = (SPIDER_CONN **)pop_dynamic(cq->q_ptr);
  if (conn_ptr) spd_conn = *conn_ptr;
  pthread_mutex_unlock(&cq->mtx);
  return spd_conn; /* NULL means the queue by this hash key is empty */
}

/* iterate through all elements and execute my_polling_last_visited */
void SPIDER_CONN_POOL::iterate(my_hash_delegate_func iter_func, void *param) {
  my_hash_delegate(&connections, iter_func, param);
}

/**
   calculate hash from key
   it utilize the charset of spd_connect_pool to calculate hash value
   @note this function is not only used in spd_connect_pool, but also
   other scenarios where hash value of conn_keys must be calculated
*/
my_hash_value_type SPIDER_CONN_POOL::calc_hash(const uchar *key, 
                                               size_t length) {
  return my_hash_sort(connections.charset, key, length);
}

/* for spider_open_connections and trx_conn_hash */
uchar *spider_conn_get_key(SPIDER_CONN *conn, size_t *length,
                           my_bool not_used __attribute__((unused))) {
  DBUG_ENTER("spider_conn_get_key");
  *length = conn->conn_key_length;
  DBUG_PRINT("info", ("spider conn_kind=%u", conn->conn_kind));
#ifndef DBUG_OFF
  spider_print_keys(conn->conn_key, conn->conn_key_length);
#endif
  DBUG_RETURN((uchar *)conn->conn_key);
}

/* for spider connection pool get key */
uchar *spider_conn_pool_get_key(void *record, size_t *length,
                                my_bool not_used __attribute__((unused))) {
  DBUG_ENTER("spider_conn_pool_get_key");
  conn_queue *cq = (conn_queue *)record;
  *length = cq->key_len;
#ifndef DBUG_OFF
  spider_print_keys(cq->hash_key, cq->key_len);
#endif
  DBUG_RETURN((uchar *)(cq->hash_key));
}

uchar *spider_ipport_conn_get_key(SPIDER_IP_PORT_CONN *ip_port, size_t *length,
                                  my_bool not_used __attribute__((unused))) {
  DBUG_ENTER("spider_ipport_conn_get_key");
  *length = ip_port->key_len;
  DBUG_RETURN((uchar *)ip_port->key);
}

uchar *spider_for_sts_conn_get_key(SPIDER_FOR_STS_CONN *sts_conn,
                                   size_t *length,
                                   my_bool not_used __attribute__((unused))) {
  DBUG_ENTER("spider_for_sts_conn_get_key");
  *length = sts_conn->key_len;
  DBUG_RETURN((uchar *)sts_conn->key);
}

uchar *spider_conn_meta_get_key(SPIDER_CONN_META_INFO *meta, size_t *length,
                                my_bool not_used __attribute__((unused))) {
  DBUG_ENTER("spider_conn_meta_get_key");
  *length = meta->key_len;
#ifndef DBUG_OFF
  spider_print_keys(meta->key, meta->key_len);
#endif
  DBUG_RETURN((uchar *)meta->key);
}

uchar *spider_xid_get_hash_key(const uchar *ptr, size_t *length,
                               my_bool not_used __attribute__((unused))) {
  *length = ((XID_STATE *)ptr)->xid.key_length();
  return ((XID_STATE *)ptr)->xid.key();
}

void spider_xid_free_hash(void *ptr) {
  if (!((XID_STATE *)ptr)->check_has_uncommitted_xa()) my_free(ptr);
}

int spider_reset_conn_setted_parameter(SPIDER_CONN *conn, THD *thd) {
  DBUG_ENTER("spider_reset_conn_setted_parameter");
  conn->autocommit = spider_param_remote_autocommit();
  conn->sql_log_off = spider_param_remote_sql_log_off();
  if (thd && spider_param_remote_time_zone()) {
    int tz_length = strlen(spider_param_remote_time_zone());
    String tz_str(spider_param_remote_time_zone(), tz_length,
                  &my_charset_latin1);
    conn->time_zone = my_tz_find(thd, &tz_str);
  } else
    conn->time_zone = NULL;
  conn->trx_isolation = spider_param_remote_trx_isolation();
  DBUG_PRINT("info", ("spider conn->trx_isolation=%d", conn->trx_isolation));
  if (spider_param_remote_access_charset()) {
    if (!(conn->access_charset =
              get_charset_by_csname(spider_param_remote_access_charset(),
                                    MY_CS_PRIMARY, MYF(MY_WME))))
      DBUG_RETURN(ER_UNKNOWN_CHARACTER_SET);
  } else
    conn->access_charset = NULL;
  char *default_database = spider_param_remote_default_database();
  if (default_database) {
    uint default_database_length = strlen(default_database);
    if (conn->default_database.reserve(default_database_length + 1))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    conn->default_database.q_append(default_database,
                                    default_database_length + 1);
    conn->default_database.length(default_database_length);
  } else
    conn->default_database.length(0);
  DBUG_RETURN(0);
}

int spider_free_conn_alloc(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_free_conn_alloc");
  spider_free_conn_thread(conn);
  spider_db_disconnect(conn);
  if (conn->db_conn) {
    delete conn->db_conn;
    conn->db_conn = NULL;
  }
  DBUG_ASSERT(!conn->mta_conn_mutex_file_pos.file_name);
  pthread_mutex_destroy(&conn->mta_conn_mutex);
  conn->default_database.free();
  DBUG_RETURN(0);
}

void spider_free_conn_from_trx(SPIDER_TRX *trx, SPIDER_CONN *conn, bool another,
                               bool trx_free, int *roop_count) {
  ha_spider *spider;
  SPIDER_IP_PORT_CONN *ip_port_conn = conn->ip_port_conn;
  THD *thd = current_thd;
  DBUG_ENTER("spider_free_conn_from_trx");
  spider_conn_clear_queue(conn);
  conn->use_for_active_standby = FALSE;
  conn->error_mode = 1;

  if (thd->current_global_server_version !=
      get_modify_server_version()) {  // global server version changed
    ulong server_version = get_server_version_by_name(conn->server_name);
    ulong current_verion = conn->current_key_version;
    if (server_version !=
        current_verion) {  // conn->server_version is different from
                           // share->server_version (old conn)
      conn->server_lost =
          TRUE;  // server version changed, free old version conn
      if (spider_param_error_when_flush_server()) {  // flush server force,
                                                     // report error 12701
        my_message(ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM,
                   ER_SPIDER_REMOTE_SERVER_GONE_AWAY_STR, MYF(0));
      }
    }
  }

  if (trx_free ||
      ((conn->server_lost || spider_param_conn_recycle_mode(trx->thd) != 2) &&
       !conn->opened_handlers)) {
    conn->thd = NULL;
    if (another) {
      ha_spider *next_spider;
      my_hash_delete(&trx->trx_another_conn_hash, (uchar *)conn);
      spider = (ha_spider *)conn->another_ha_first;
      while (spider) {
        next_spider = spider->next;
        spider_free_tmp_dbton_handler(spider);
        spider_free_tmp_dbton_share(spider->share);
        spider_free_tmp_share_alloc(spider->share);
        spider_free(spider_current_trx, spider->share, MYF(0));
        delete spider;
        spider = next_spider;
      }
      conn->another_ha_first = NULL;
      conn->another_ha_last = NULL;
    } else {
      my_hash_delete(&trx->trx_conn_hash, (uchar *)conn);
    }

    if (!trx_free && !conn->server_lost &&
        /* !conn->queued_connect &&*/
        /*  failed to create conn, don't need to free */
        spider_param_conn_recycle_mode(trx->thd) == 1 &&
        !(thd->is_error()) /*if thd->is_error, must be free,not recycle in the
                              connect pool*/
    ) {
      /* conn_recycle_mode == 1 */
      *conn->conn_key = '0';
      conn->casual_read_base_conn = NULL;
      if (conn->quick_target &&
          spider_db_free_result((ha_spider *)conn->quick_target, FALSE)) {
        spider_free_conn(conn);
      } else {
        // pthread_mutex_lock(&spider_conn_mutex);
        // to avoid memcpy, we insert SPIDER_CONN ** to spd_connect_pools
        if (spd_connect_pools.put_conn(conn)) {
          // pthread_mutex_unlock(&spider_conn_mutex);
          spider_free_conn(conn);
        } else {
          if (ip_port_conn) { /* exists */
            if (ip_port_conn->waiting_count) {
              pthread_mutex_lock(&ip_port_conn->mutex);
              pthread_cond_signal(&ip_port_conn->cond);
              pthread_mutex_unlock(&ip_port_conn->mutex);
            }
          }
          /************************************************************************/
          /* Create conn_meta whose status is updated then when CONN object is
          pushed
          /* into spider_open_connections
          /************************************************************************/
          // if (!spider_add_conn_meta_info(conn)) {
          //	spider_my_err_logging("[ERROR] spider_add_conn_meta_info failed
          // for conn within conn_id=[%ull]!\n", conn->conn_id);
          //}
          // pthread_mutex_unlock(&spider_conn_mutex);
        }
      }
    } else {
      /* conn_recycle_mode == 0 */
      spider_free_conn(conn);
    }
  } else if (roop_count)
    (*roop_count)++;
  DBUG_VOID_RETURN;
}

SPIDER_CONN *spider_create_conn(SPIDER_SHARE *share, ha_spider *spider,
                                int link_idx, int base_link_idx, uint conn_kind,
                                int *error_num) {
  int *need_mon;
  SPIDER_CONN *conn;
  SPIDER_IP_PORT_CONN *ip_port_conn;
  char *tmp_name, *tmp_server_name, *tmp_host, *tmp_username, *tmp_password,
      *tmp_socket;
  char *tmp_wrapper, *tmp_ssl_ca, *tmp_ssl_capath, *tmp_ssl_cert;
  char *tmp_ssl_cipher, *tmp_ssl_key, *tmp_default_file, *tmp_default_group;
  DBUG_ENTER("spider_create_conn");

  if (!(conn = (SPIDER_CONN *)spider_bulk_malloc(
            spider_current_trx, 18, MYF(MY_WME | MY_ZEROFILL), &conn,
            sizeof(*conn), &tmp_name, share->conn_keys_lengths[link_idx] + 1,
            &tmp_server_name, share->server_names_lengths[link_idx] + 1,
            &tmp_host, share->tgt_hosts_lengths[link_idx] + 1, &tmp_username,
            share->tgt_usernames_lengths[link_idx] + 1, &tmp_password,
            share->tgt_passwords_lengths[link_idx] + 1, &tmp_socket,
            share->tgt_sockets_lengths[link_idx] + 1, &tmp_wrapper,
            share->tgt_wrappers_lengths[link_idx] + 1, &tmp_ssl_ca,
            share->tgt_ssl_cas_lengths[link_idx] + 1, &tmp_ssl_capath,
            share->tgt_ssl_capaths_lengths[link_idx] + 1, &tmp_ssl_cert,
            share->tgt_ssl_certs_lengths[link_idx] + 1, &tmp_ssl_cipher,
            share->tgt_ssl_ciphers_lengths[link_idx] + 1, &tmp_ssl_key,
            share->tgt_ssl_keys_lengths[link_idx] + 1, &tmp_default_file,
            share->tgt_default_files_lengths[link_idx] + 1, &tmp_default_group,
            share->tgt_default_groups_lengths[link_idx] + 1, &need_mon,
            sizeof(int), NullS))) {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_alloc_conn;
  }

  conn->default_database.init_calc_mem(75);
  conn->conn_key_length = share->conn_keys_lengths[link_idx];
  conn->conn_key = tmp_name;
  memcpy(conn->conn_key, share->conn_keys[link_idx],
         share->conn_keys_lengths[link_idx]);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  conn->conn_key_hash_value = share->conn_keys_hash_value[link_idx];
#endif
  conn->tgt_host_length = share->tgt_hosts_lengths[link_idx];
  conn->tgt_host = tmp_host;
  memcpy(conn->tgt_host, share->tgt_hosts[link_idx],
         share->tgt_hosts_lengths[link_idx]);
  conn->server_names_length = share->server_names_lengths[link_idx];
  conn->server_name = tmp_server_name;
  memcpy(conn->server_name, share->server_names[link_idx],
         share->server_names_lengths[link_idx]);
  conn->tgt_username_length = share->tgt_usernames_lengths[link_idx];
  conn->tgt_username = tmp_username;
  memcpy(conn->tgt_username, share->tgt_usernames[link_idx],
         share->tgt_usernames_lengths[link_idx]);
  conn->tgt_password_length = share->tgt_passwords_lengths[link_idx];
  conn->tgt_password = tmp_password;
  memcpy(conn->tgt_password, share->tgt_passwords[link_idx],
         share->tgt_passwords_lengths[link_idx]);
  conn->tgt_socket_length = share->tgt_sockets_lengths[link_idx];
  conn->tgt_socket = tmp_socket;
  memcpy(conn->tgt_socket, share->tgt_sockets[link_idx],
         share->tgt_sockets_lengths[link_idx]);
  conn->tgt_wrapper_length = share->tgt_wrappers_lengths[link_idx];
  conn->tgt_wrapper = tmp_wrapper;
  memcpy(conn->tgt_wrapper, share->tgt_wrappers[link_idx],
         share->tgt_wrappers_lengths[link_idx]);
  conn->tgt_ssl_ca_length = share->tgt_ssl_cas_lengths[link_idx];
  if (conn->tgt_ssl_ca_length) {
    conn->tgt_ssl_ca = tmp_ssl_ca;
    memcpy(conn->tgt_ssl_ca, share->tgt_ssl_cas[link_idx],
           share->tgt_ssl_cas_lengths[link_idx]);
  } else
    conn->tgt_ssl_ca = NULL;
  conn->tgt_ssl_capath_length = share->tgt_ssl_capaths_lengths[link_idx];
  if (conn->tgt_ssl_capath_length) {
    conn->tgt_ssl_capath = tmp_ssl_capath;
    memcpy(conn->tgt_ssl_capath, share->tgt_ssl_capaths[link_idx],
           share->tgt_ssl_capaths_lengths[link_idx]);
  } else
    conn->tgt_ssl_capath = NULL;
  conn->tgt_ssl_cert_length = share->tgt_ssl_certs_lengths[link_idx];
  if (conn->tgt_ssl_cert_length) {
    conn->tgt_ssl_cert = tmp_ssl_cert;
    memcpy(conn->tgt_ssl_cert, share->tgt_ssl_certs[link_idx],
           share->tgt_ssl_certs_lengths[link_idx]);
  } else
    conn->tgt_ssl_cert = NULL;
  conn->tgt_ssl_cipher_length = share->tgt_ssl_ciphers_lengths[link_idx];
  if (conn->tgt_ssl_cipher_length) {
    conn->tgt_ssl_cipher = tmp_ssl_cipher;
    memcpy(conn->tgt_ssl_cipher, share->tgt_ssl_ciphers[link_idx],
           share->tgt_ssl_ciphers_lengths[link_idx]);
  } else
    conn->tgt_ssl_cipher = NULL;
  conn->tgt_ssl_key_length = share->tgt_ssl_keys_lengths[link_idx];
  if (conn->tgt_ssl_key_length) {
    conn->tgt_ssl_key = tmp_ssl_key;
    memcpy(conn->tgt_ssl_key, share->tgt_ssl_keys[link_idx],
           share->tgt_ssl_keys_lengths[link_idx]);
  } else
    conn->tgt_ssl_key = NULL;
  conn->tgt_default_file_length = share->tgt_default_files_lengths[link_idx];
  if (conn->tgt_default_file_length) {
    conn->tgt_default_file = tmp_default_file;
    memcpy(conn->tgt_default_file, share->tgt_default_files[link_idx],
           share->tgt_default_files_lengths[link_idx]);
  } else
    conn->tgt_default_file = NULL;
  conn->tgt_default_group_length = share->tgt_default_groups_lengths[link_idx];
  if (conn->tgt_default_group_length) {
    conn->tgt_default_group = tmp_default_group;
    memcpy(conn->tgt_default_group, share->tgt_default_groups[link_idx],
           share->tgt_default_groups_lengths[link_idx]);
  } else
    conn->tgt_default_group = NULL;
  conn->tgt_port = share->tgt_ports[link_idx];
  conn->tgt_ssl_vsc = share->tgt_ssl_vscs[link_idx];
  conn->dbton_id = share->sql_dbton_ids[link_idx];
  conn->bg_conn_working = false;
  if (conn->dbton_id == SPIDER_DBTON_SIZE) {
    my_printf_error(ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM,
                    ER_SPIDER_SQL_WRAPPER_IS_INVALID_STR, MYF(0),
                    conn->tgt_wrapper);
    *error_num = ER_SPIDER_SQL_WRAPPER_IS_INVALID_NUM;
    goto error_invalid_wrapper;
  }
  if (!(conn->db_conn = spider_dbton[conn->dbton_id].create_db_conn(conn))) {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_db_conn_create;
  }
  if ((*error_num = conn->db_conn->init())) {
    goto error_db_conn_init;
  }
  conn->join_trx = 0;
  conn->thd = NULL;
  conn->table_lock = 0;
  conn->semi_trx_isolation = -2;
  conn->semi_trx_isolation_chk = FALSE;
  conn->link_idx = base_link_idx;
  conn->conn_kind = conn_kind;
  conn->conn_need_mon = need_mon;
  if (spider)
    conn->need_mon = &spider->need_mons[base_link_idx];
  else
    conn->need_mon = need_mon;

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&conn->mta_conn_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_mta_conn, &conn->mta_conn_mutex,
                       MY_MUTEX_INIT_FAST))
#endif
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_mta_conn_mutex_init;
  }

  spider_conn_queue_connect(share, conn, link_idx);
  conn->ping_time = (time_t)time((time_t *)0);
  conn->connect_error_time = conn->ping_time;
  my_atomic_add64(&spider_conn_id, 1LL);
  conn->conn_id = spider_conn_id;

  pthread_mutex_lock(&spider_ipport_conn_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if ((ip_port_conn = (SPIDER_IP_PORT_CONN *)my_hash_search_using_hash_value(
           &spider_ipport_conns, conn->conn_key_hash_value,
           (uchar *)conn->conn_key, conn->conn_key_length)))
#else
  if ((ip_port_conn = (SPIDER_IP_PORT_CONN *)my_hash_search(
           &spider_ipport_conns, (uchar *)conn->conn_key,
           conn->conn_key_length)))
#endif
  { /* exists, +1 */
    pthread_mutex_unlock(&spider_ipport_conn_mutex);
    pthread_mutex_lock(&ip_port_conn->mutex);
    if (spider_param_max_connections()) { /* enable conncetion pool */
      if (ip_port_conn->ip_port_count >=
          spider_param_max_connections()) { /* bigger than the max num of
                                               connections, free conn and
                                               return NULL */
        pthread_mutex_unlock(&ip_port_conn->mutex);
        *error_num = ER_SPIDER_CON_COUNT_ERROR;
        goto error_too_many_ipport_count;
      }
    }
    ip_port_conn->ip_port_count++;
    pthread_mutex_unlock(&ip_port_conn->mutex);
  } else {  // do not exist
    ip_port_conn = spider_create_ipport_conn(conn);
    if (!ip_port_conn) {
      /* failed, always do not effect 'create conn' */
      pthread_mutex_unlock(&spider_ipport_conn_mutex);
      DBUG_RETURN(conn);
    }
    if (my_hash_insert(&spider_ipport_conns, (uchar *)ip_port_conn)) {
      /* insert failed, always do not effect 'create conn' */
      pthread_mutex_unlock(&spider_ipport_conn_mutex);
      DBUG_RETURN(conn);
    }
    pthread_mutex_unlock(&spider_ipport_conn_mutex);
  }
  conn->current_key_version = share->conn_key_version;
  conn->ip_port_conn = ip_port_conn;
  conn->last_visited = time(NULL);
  if (!spider_add_conn_meta_info(conn)) {
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN(conn);

/*
error_init_lock_table_hash:
  DBUG_ASSERT(!conn->mta_conn_mutex_file_pos.file_name);
  pthread_mutex_destroy(&conn->mta_conn_mutex);
*/
error_too_many_ipport_count:
error_mta_conn_mutex_init:
error_db_conn_init:
  delete conn->db_conn;
error_db_conn_create:
error_invalid_wrapper:
  spider_free(spider_current_trx, conn, MYF(0));
error_alloc_conn:
  DBUG_RETURN(NULL);
}

SPIDER_CONN *spider_get_conn(SPIDER_SHARE *share, int link_idx, SPIDER_TRX *trx,
                             ha_spider *spider, bool another, bool thd_chg,
                             uint conn_kind, int *error_num) {
  THD *thd = current_thd;
  SPIDER_CONN *conn = NULL;
  int base_link_idx = link_idx;
  ulong current_server_version = get_modify_server_version();
  DBUG_ENTER("spider_get_conn");
  DBUG_PRINT("info", ("spider conn_kind=%u", conn_kind));

  if (spider) link_idx = spider->conn_link_idx[base_link_idx];
  DBUG_PRINT("info", ("spider link_idx=%u", link_idx));
  DBUG_PRINT("info", ("spider base_link_idx=%u", base_link_idx));

  if (share->modify_server_version != current_server_version) {
    spider_update_conn_keys(share, link_idx);
    share->modify_server_version = current_server_version;
  }
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if ((another &&
       !(conn = (SPIDER_CONN *)my_hash_search_using_hash_value(
             &trx->trx_another_conn_hash, share->conn_keys_hash_value[link_idx],
             (uchar *)share->conn_keys[link_idx],
             share->conn_keys_lengths[link_idx]))) ||
      (!another &&
       !(conn = (SPIDER_CONN *)my_hash_search_using_hash_value(
             &trx->trx_conn_hash, share->conn_keys_hash_value[link_idx],
             (uchar *)share->conn_keys[link_idx],
             share->conn_keys_lengths[link_idx]))))
#else
  if ((another &&
       !(conn = (SPIDER_CONN *)my_hash_search(
             &trx->trx_another_conn_hash, (uchar *)share->conn_keys[link_idx],
             share->conn_keys_lengths[link_idx]))) ||
      (!another &&
       !(conn = (SPIDER_CONN *)my_hash_search(
             &trx->trx_conn_hash, (uchar *)share->conn_keys[link_idx],
             share->conn_keys_lengths[link_idx]))))
#endif
  {
    if (!trx->thd || ((spider_param_conn_recycle_mode(trx->thd) & 1) ||
                      spider_param_conn_recycle_strict(trx->thd))) {
      // pthread_mutex_lock(&spider_conn_mutex);
      if (!(conn = spd_connect_pools.get_conn(
                share->conn_keys_hash_value[link_idx],
                (uchar *)share->conn_keys[link_idx],
                share->conn_keys_lengths[link_idx])))
      {
        // pthread_mutex_unlock(&spider_conn_mutex);
        if (spider_param_max_connections()) { /* enable connection pool */
          conn = spider_get_conn_from_idle_connection(
              share, link_idx, share->conn_keys[link_idx], spider, conn_kind,
              base_link_idx, error_num);
          /* failed get conn, goto error */
          if (!conn) goto error;

        } else { /* did not enable conncetion pool , create_conn */
          DBUG_PRINT("info", ("spider create new conn"));
          if (!(conn = spider_create_conn(share, spider, link_idx,
                                          base_link_idx, conn_kind, error_num)))
            goto error;
          *conn->conn_key = *(share->conn_keys[link_idx]);
          if (spider) {
            spider->conns[base_link_idx] = conn;
            if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
              conn->use_for_active_standby = TRUE;
          }
        }
      } else {
        // my_hash_delete(&spider_open_connections, (uchar *)conn);
        // pthread_mutex_unlock(&spider_conn_mutex);
        DBUG_PRINT("info", ("spider get global conn"));
        if (spider) {
          spider->conns[base_link_idx] = conn;
          if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
            conn->use_for_active_standby = TRUE;
        }
      }
    } else {
      DBUG_PRINT("info", ("spider create new conn"));
      /* conn_recycle_strict = 0 and conn_recycle_mode = 0 or 2 */
      if (!(conn = spider_create_conn(share, spider, link_idx, base_link_idx,
                                      conn_kind, error_num)))
        goto error;
      *conn->conn_key = *(share->conn_keys[link_idx]);
      if (spider) {
        spider->conns[base_link_idx] = conn;
        if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
          conn->use_for_active_standby = TRUE;
      }
    }
    conn->thd = trx->thd;
    conn->priority = share->priority;

    if (another) {
      uint old_elements = trx->trx_another_conn_hash.array.max_element;
      if (my_hash_insert(&trx->trx_another_conn_hash, (uchar *)conn)) {
        spider_free_conn(conn);
        *error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
      if (trx->trx_another_conn_hash.array.max_element > old_elements) {
        spider_alloc_calc_mem(
            spider_current_trx, trx->trx_another_conn_hash,
            (trx->trx_another_conn_hash.array.max_element - old_elements) *
                trx->trx_another_conn_hash.array.size_of_element);
      }
    } else {
      uint old_elements = trx->trx_conn_hash.array.max_element;
      if (my_hash_insert(&trx->trx_conn_hash, (uchar *)conn)) {
        spider_free_conn(conn);
        *error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
      if (trx->trx_conn_hash.array.max_element > old_elements) {
        spider_alloc_calc_mem(
            spider_current_trx, trx->trx_conn_hash,
            (trx->trx_conn_hash.array.max_element - old_elements) *
                trx->trx_conn_hash.array.size_of_element);
      }
    }
  } else if (spider) {
    spider->conns[base_link_idx] = conn;
    if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
      conn->use_for_active_standby = TRUE;
  }
  conn->link_idx = base_link_idx;

  if (conn->queued_connect)
    spider_conn_queue_connect_rewrite(share, conn, link_idx);

  if (conn->queued_ping) {
    if (spider)
      spider_conn_queue_ping_rewrite(spider, conn, base_link_idx);
    else
      conn->queued_ping = FALSE;
  }

  DBUG_PRINT("info", ("spider conn=%p", conn));
  DBUG_RETURN(conn);

error:
  DBUG_RETURN(NULL);
}

int spider_free_conn(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_free_conn");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  SPIDER_IP_PORT_CONN *ip_port_conn = conn->ip_port_conn;
  if (ip_port_conn) { /* free conn, ip_port_count-- */
    pthread_mutex_lock(&ip_port_conn->mutex);
    if (ip_port_conn->ip_port_count > 0) ip_port_conn->ip_port_count--;
    pthread_mutex_unlock(&ip_port_conn->mutex);
  }
  conn->bg_conn_working = false;
  spider_free_conn_alloc(conn);
  spider_free(spider_current_trx, conn, MYF(0));
  DBUG_RETURN(0);
}

int spider_check_and_get_casual_read_conn(THD *thd, ha_spider *spider,
                                          int link_idx) {
  int error_num;
  DBUG_ENTER("spider_check_and_get_casual_read_conn");
  if (spider->result_list.casual_read[link_idx]) {
    SPIDER_CONN *conn = spider->spider_get_conn_by_idx(link_idx);
    if (!conn) {
      error_num = ER_SPIDER_CON_COUNT_ERROR;
      DBUG_RETURN(error_num);
    }
    if (conn->casual_read_query_id != thd->query_id) {
      conn->casual_read_query_id = thd->query_id;
      conn->casual_read_current_id = 2;
    }
    if (spider->result_list.casual_read[link_idx] == 1) {
      spider->result_list.casual_read[link_idx] = conn->casual_read_current_id;
      ++conn->casual_read_current_id;
      if (conn->casual_read_current_id > 63) {
        conn->casual_read_current_id = 2;
      }
    }
    char first_byte_bak = *spider->share->conn_keys[link_idx];
    *spider->share->conn_keys[link_idx] =
        '0' + spider->result_list.casual_read[link_idx];
    if (!(spider->conns[link_idx] = spider_get_conn(
              spider->share, link_idx, spider->trx, spider, FALSE, TRUE,
              SPIDER_CONN_KIND_MYSQL, &error_num))) {
      *spider->share->conn_keys[link_idx] = first_byte_bak;
      DBUG_RETURN(error_num);
    }
    *spider->share->conn_keys[link_idx] = first_byte_bak;
    spider->conns[link_idx]->casual_read_base_conn = conn;
    conn = spider->conns[link_idx];
    spider_check_and_set_autocommit(thd, conn, NULL);
  }
  DBUG_RETURN(0);
}

int spider_check_and_init_casual_read(THD *thd, ha_spider *spider,
                                      int link_idx) {
  int error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  DBUG_ENTER("spider_check_and_init_casual_read");
  if (spider_param_sync_autocommit(thd) &&
      (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) &&
      (result_list->direct_order_limit
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
       || result_list->direct_aggregate
#endif
       )) {
    if (!result_list->casual_read[link_idx]) {
      result_list->casual_read[link_idx] =
          spider_param_casual_read(thd, share->casual_read);
    }
    if ((error_num =
             spider_check_and_get_casual_read_conn(thd, spider, link_idx))) {
      DBUG_RETURN(error_num);
    }
    conn = spider->spider_get_conn_by_idx(link_idx);
    if (!conn) {
      error_num = ER_SPIDER_CON_COUNT_ERROR;
      DBUG_RETURN(error_num);
    }
    if (conn->casual_read_base_conn &&
        (error_num = spider_create_conn_thread(conn))) {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

void spider_conn_queue_connect(SPIDER_SHARE *share, SPIDER_CONN *conn,
                               int link_idx) {
  DBUG_ENTER("spider_conn_queue_connect");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_connect = TRUE;
  /*
    conn->queued_connect_share = share;
    conn->queued_connect_link_idx = link_idx;
  */
  DBUG_VOID_RETURN;
}

void spider_conn_queue_connect_rewrite(SPIDER_SHARE *share, SPIDER_CONN *conn,
                                       int link_idx) {
  DBUG_ENTER("spider_conn_queue_connect_rewrite");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_connect_share = share;
  conn->queued_connect_link_idx = link_idx;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_ping(ha_spider *spider, SPIDER_CONN *conn,
                            int link_idx) {
  DBUG_ENTER("spider_conn_queue_ping");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_ping = TRUE;
  conn->queued_ping_spider = spider;
  conn->queued_ping_link_idx = link_idx;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_ping_rewrite(ha_spider *spider, SPIDER_CONN *conn,
                                    int link_idx) {
  DBUG_ENTER("spider_conn_queue_ping_rewrite");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_ping_spider = spider;
  conn->queued_ping_link_idx = link_idx;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_trx_isolation(SPIDER_CONN *conn, int trx_isolation) {
  DBUG_ENTER("spider_conn_queue_trx_isolation");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_trx_isolation = TRUE;
  conn->queued_trx_isolation_val = trx_isolation;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_semi_trx_isolation(SPIDER_CONN *conn,
                                          int trx_isolation) {
  DBUG_ENTER("spider_conn_queue_semi_trx_isolation");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_semi_trx_isolation = TRUE;
  conn->queued_semi_trx_isolation_val = trx_isolation;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_autocommit(SPIDER_CONN *conn, bool autocommit) {
  DBUG_ENTER("spider_conn_queue_autocommit");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_autocommit = TRUE;
  conn->queued_autocommit_val = autocommit;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_sql_log_off(SPIDER_CONN *conn, bool sql_log_off) {
  DBUG_ENTER("spider_conn_queue_sql_log_off");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_sql_log_off = TRUE;
  conn->queued_sql_log_off_val = sql_log_off;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_time_zone(SPIDER_CONN *conn, Time_zone *time_zone) {
  DBUG_ENTER("spider_conn_queue_time_zone");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_time_zone = TRUE;
  conn->queued_time_zone_val = time_zone;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_start_transaction(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_conn_queue_start_transaction");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  DBUG_ASSERT(!conn->trx_start);
  conn->queued_trx_start = TRUE;
  conn->trx_start = TRUE;
  DBUG_VOID_RETURN;
}

void spider_conn_queue_xa_start(SPIDER_CONN *conn, XID *xid) {
  DBUG_ENTER("spider_conn_queue_xa_start");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  conn->queued_xa_start = TRUE;
  conn->queued_xa_start_xid = xid;
  DBUG_VOID_RETURN;
}

void spider_conn_clear_queue(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_conn_clear_queue");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  /*
    conn->queued_connect = FALSE;
    conn->queued_ping = FALSE;
  */
  conn->queued_trx_isolation = FALSE;
  conn->queued_semi_trx_isolation = FALSE;
  conn->queued_autocommit = FALSE;
  conn->queued_sql_log_off = FALSE;
  conn->queued_time_zone = FALSE;
  conn->queued_trx_start = FALSE;
  conn->queued_xa_start = FALSE;
  DBUG_VOID_RETURN;
}

void spider_conn_clear_queue_at_commit(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_conn_clear_queue_at_commit");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  if (conn->queued_trx_start) {
    conn->queued_trx_start = FALSE;
    conn->trx_start = FALSE;
  }
  conn->queued_xa_start = FALSE;
  conn->is_xa_commit_one_phase = FALSE;
  DBUG_VOID_RETURN;
}

void spider_conn_set_timeout(SPIDER_CONN *conn, uint net_read_timeout,
                             uint net_write_timeout) {
  DBUG_ENTER("spider_conn_set_timeout");
  DBUG_PRINT("info", ("spider conn=%p", conn));
  if (net_read_timeout != conn->net_read_timeout) {
    DBUG_PRINT("info", ("spider net_read_timeout set from %u to %u",
                        conn->net_read_timeout, net_read_timeout));
    conn->queued_net_timeout = TRUE;
    conn->net_read_timeout = net_read_timeout;
  }
  if (net_write_timeout != conn->net_write_timeout) {
    DBUG_PRINT("info", ("spider net_write_timeout set from %u to %u",
                        conn->net_write_timeout, net_write_timeout));
    conn->queued_net_timeout = TRUE;
    conn->net_write_timeout = net_write_timeout;
  }
  DBUG_VOID_RETURN;
}

void spider_conn_set_timeout_from_share(SPIDER_CONN *conn, int link_idx,
                                        THD *thd, SPIDER_SHARE *share) {
  DBUG_ENTER("spider_conn_set_timeout_from_share");
  spider_conn_set_timeout(
      conn,
      spider_param_net_read_timeout(thd, share->net_read_timeouts[link_idx]),
      spider_param_net_write_timeout(thd, share->net_write_timeouts[link_idx]));
  DBUG_VOID_RETURN;
}

void spider_conn_set_timeout_from_direct_sql(SPIDER_CONN *conn, THD *thd,
                                             SPIDER_DIRECT_SQL *direct_sql) {
  DBUG_ENTER("spider_conn_set_timeout_from_direct_sql");
  spider_conn_set_timeout(
      conn, spider_param_net_read_timeout(thd, direct_sql->net_read_timeout),
      spider_param_net_write_timeout(thd, direct_sql->net_write_timeout));
  DBUG_VOID_RETURN;
}

void spider_tree_insert(SPIDER_CONN *top, SPIDER_CONN *conn) {
  SPIDER_CONN *current = top;
  longlong priority = conn->priority;
  DBUG_ENTER("spider_tree_insert");
  while (TRUE) {
    if (priority < current->priority) {
      if (current->c_small == NULL) {
        conn->p_small = NULL;
        conn->p_big = current;
        conn->c_small = NULL;
        conn->c_big = NULL;
        current->c_small = conn;
        break;
      } else
        current = current->c_small;
    } else {
      if (current->c_big == NULL) {
        conn->p_small = current;
        conn->p_big = NULL;
        conn->c_small = NULL;
        conn->c_big = NULL;
        current->c_big = conn;
        break;
      } else
        current = current->c_big;
    }
  }
  DBUG_VOID_RETURN;
}

SPIDER_CONN *spider_tree_first(SPIDER_CONN *top) {
  SPIDER_CONN *current = top;
  DBUG_ENTER("spider_tree_first");
  while (current) {
    if (current->c_small == NULL)
      break;
    else
      current = current->c_small;
  }
  DBUG_RETURN(current);
}

SPIDER_CONN *spider_tree_last(SPIDER_CONN *top) {
  SPIDER_CONN *current = top;
  DBUG_ENTER("spider_tree_last");
  while (TRUE) {
    if (current->c_big == NULL)
      break;
    else
      current = current->c_big;
  }
  DBUG_RETURN(current);
}

SPIDER_CONN *spider_tree_next(SPIDER_CONN *current) {
  DBUG_ENTER("spider_tree_next");
  if (current->c_big) DBUG_RETURN(spider_tree_first(current->c_big));
  while (TRUE) {
    if (current->p_big) DBUG_RETURN(current->p_big);
    if (!current->p_small) DBUG_RETURN(NULL);
    current = current->p_small;
  }
}

uint spider_tree_num(SPIDER_CONN *top) {
  DBUG_ENTER("spider_tree_num");
  uint num = 0;
  SPIDER_CONN *conn = spider_tree_first(top);

  while (conn) {
    num++;
    conn = spider_tree_next(conn);
  }
  DBUG_RETURN(num);
};

SPIDER_CONN *spider_tree_delete(SPIDER_CONN *conn, SPIDER_CONN *top) {
  DBUG_ENTER("spider_tree_delete");
  if (conn->p_small) {
    if (conn->c_small) {
      conn->c_small->p_big = NULL;
      conn->c_small->p_small = conn->p_small;
      conn->p_small->c_big = conn->c_small;
      if (conn->c_big) {
        SPIDER_CONN *last = spider_tree_last(conn->c_small);
        conn->c_big->p_small = last;
        last->c_big = conn->c_big;
      }
    } else if (conn->c_big) {
      conn->c_big->p_small = conn->p_small;
      conn->p_small->c_big = conn->c_big;
    } else
      conn->p_small->c_big = NULL;
  } else if (conn->p_big) {
    if (conn->c_small) {
      conn->c_small->p_big = conn->p_big;
      conn->p_big->c_small = conn->c_small;
      if (conn->c_big) {
        SPIDER_CONN *last = spider_tree_last(conn->c_small);
        conn->c_big->p_small = last;
        last->c_big = conn->c_big;
      }
    } else if (conn->c_big) {
      conn->c_big->p_big = conn->p_big;
      conn->c_big->p_small = NULL;
      conn->p_big->c_small = conn->c_big;
    } else
      conn->p_big->c_small = NULL;
  } else {
    if (conn->c_small) {
      conn->c_small->p_big = NULL;
      conn->c_small->p_small = NULL;
      if (conn->c_big) {
        SPIDER_CONN *last = spider_tree_last(conn->c_small);
        conn->c_big->p_small = last;
        last->c_big = conn->c_big;
      }
      DBUG_RETURN(conn->c_small);
    } else if (conn->c_big) {
      conn->c_big->p_small = NULL;
      DBUG_RETURN(conn->c_big);
    }
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN(top);
}

int spider_set_conn_bg_param(ha_spider *spider) {
  int error_num, roop_count, bgs_mode;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  THD *thd = spider->trx->thd;
  DBUG_ENTER("spider_set_conn_bg_param");
  DBUG_PRINT("info", ("spider spider=%p", spider));
  bgs_mode = spider_param_bgs_mode(thd, share->bgs_mode);

  if (!spider->use_pre_call) bgs_mode = 0;
  if (bgs_mode == 0)
    result_list->bgs_phase = 0;
  else if (bgs_mode <= 2 &&
           (result_list->lock_type == F_WRLCK || spider->lock_mode == 2))
    result_list->bgs_phase = 0;
  else if (bgs_mode <= 1 && spider->lock_mode == 1)
    result_list->bgs_phase = 0;
  else {
    result_list->bgs_phase = 1;

    result_list->bgs_split_read = spider_bg_split_read_param(spider);
    if (spider->use_pre_call) {
      DBUG_PRINT("info", ("spider use_pre_call=TRUE"));
      result_list->bgs_first_read = result_list->bgs_split_read;
      result_list->bgs_second_read = result_list->bgs_split_read;
    } else {
      DBUG_PRINT("info", ("spider use_pre_call=FALSE"));
      result_list->bgs_first_read =
          spider_param_bgs_first_read(thd, share->bgs_first_read);
      result_list->bgs_second_read =
          spider_param_bgs_second_read(thd, share->bgs_second_read);
    }
    DBUG_PRINT("info",
               ("spider bgs_split_read=%lld", result_list->bgs_split_read));
    DBUG_PRINT("info", ("spider bgs_first_read=%lld", share->bgs_first_read));
    DBUG_PRINT("info", ("spider bgs_second_read=%lld", share->bgs_second_read));

    result_list->split_read = result_list->bgs_first_read > 0
                                  ? result_list->bgs_first_read
                                  : result_list->bgs_split_read;
  }

  if (result_list->bgs_phase > 0) {
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
    if (spider->use_fields) {
      SPIDER_LINK_IDX_CHAIN *link_idx_chain;
      spider_fields *fields = spider->fields;
      fields->set_pos_to_first_link_idx_chain();
      while ((link_idx_chain = fields->get_next_link_idx_chain())) {
        if ((error_num = spider_create_conn_thread(link_idx_chain->conn)))
          DBUG_RETURN(error_num);
      }
    } else {
#endif
      for (roop_count = spider_conn_link_idx_next(
               share->link_statuses, spider->conn_link_idx, -1,
               share->link_count,
               spider->lock_mode ? SPIDER_LINK_STATUS_RECOVERY
                                 : SPIDER_LINK_STATUS_OK);
           roop_count < (int)share->link_count;
           roop_count = spider_conn_link_idx_next(
               share->link_statuses, spider->conn_link_idx, roop_count,
               share->link_count,
               spider->lock_mode ? SPIDER_LINK_STATUS_RECOVERY
                                 : SPIDER_LINK_STATUS_OK)) {
        if ((error_num = spider_create_conn_thread(
                 spider->spider_get_conn_by_idx(roop_count))))
          DBUG_RETURN(error_num);
      }
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
    }
#endif
  }
  DBUG_RETURN(0);
}

int spider_set_conn_bg_param_for_dml(ha_spider *spider) {
  int error_num, roop_count, dml_bgs_mode;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  THD *thd = spider->trx->thd;
  DBUG_ENTER("spider_set_conn_bg_param");
  st_select_lex *select_lex;
  select_lex = spider_get_select_lex(spider);
  /* dml_bgs_mode is determed by:
  1. sql is INSERT (current only support insert)
  2. spider_bgs_mode is 1;
  3. spider_bgs_dml is 1;
  4. total_inserted_rows must grater than 1
  5. not in transaction
  6. spider_rone_shard_flag==FALSE
  7. just one table included
  8. don't support limit*/
  /* TODO  set dml_bgs_mode = 1 when involving multiple partitions */
  if (thd && select_lex &&
      (!(select_lex->explicit_limit || select_lex->offset_limit ||
         select_lex->select_limit)) &&
      (thd->lex->sql_command == SQLCOM_INSERT ||
       thd->lex->sql_command == SQLCOM_UPDATE ||
       thd->lex->sql_command == SQLCOM_DELETE ||
       thd->lex->sql_command == SQLCOM_LOAD) &&
      (!(thd_test_options(thd, OPTION_NOT_AUTOCOMMIT) ||
         thd_test_options(thd, OPTION_BEGIN))) &&
      (!(thd->lex->spider_rone_shard_flag)) && thd->lex->query_tables &&
      (thd->lex->query_tables->next_global == NULL) &&
      spider_param_bgs_mode(thd, share->bgs_mode) >
          0)  //  && spider->get_total_inserted_rows() > 1)
    dml_bgs_mode = spider_param_bgs_dml(thd);
  else
    dml_bgs_mode = 0;

  if (dml_bgs_mode) result_list->bgs_phase = 1;

  if (result_list->bgs_phase > 0) {
    for (roop_count = spider_conn_link_idx_next(
             share->link_statuses, spider->conn_link_idx, -1, share->link_count,
             spider->lock_mode ? SPIDER_LINK_STATUS_RECOVERY
                               : SPIDER_LINK_STATUS_OK);
         roop_count < (int)share->link_count;
         roop_count = spider_conn_link_idx_next(
             share->link_statuses, spider->conn_link_idx, roop_count,
             share->link_count,
             spider->lock_mode ? SPIDER_LINK_STATUS_RECOVERY
                               : SPIDER_LINK_STATUS_OK)) {
      if ((error_num = spider_create_conn_thread(
               spider->spider_get_conn_by_idx(roop_count))))
        DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_create_conn_thread(SPIDER_CONN *conn) {
  int error_num;
  DBUG_ENTER("spider_create_conn_thread");
  if (!conn) {
    DBUG_RETURN(ER_SPIDER_CON_COUNT_ERROR);
  }
  if (conn && !conn->bg_init) {
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&conn->bg_conn_chain_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_bg_conn_chain,
                         &conn->bg_conn_chain_mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_chain_mutex_init;
    }
    conn->bg_conn_chain_mutex_ptr = NULL;
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&conn->bg_conn_sync_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_bg_conn_sync, &conn->bg_conn_sync_mutex,
                         MY_MUTEX_INIT_FAST))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_mutex_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&conn->bg_conn_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_bg_conn, &conn->bg_conn_mutex,
                         MY_MUTEX_INIT_FAST))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_mutex_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&conn->bg_job_stack_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_bg_job_stack, &conn->bg_job_stack_mutex,
                         MY_MUTEX_INIT_FAST))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_job_stack_mutex_init;
    }
    if (SPD_INIT_DYNAMIC_ARRAY2(&conn->bg_job_stack, sizeof(void *), NULL, 16,
                                16, MYF(MY_WME))) {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_job_stack_init;
    }
    spider_alloc_calc_mem_init(conn->bg_job_stack, 163);
    spider_alloc_calc_mem(
        spider_current_trx, conn->bg_job_stack,
        conn->bg_job_stack.max_element * conn->bg_job_stack.size_of_element);
    conn->bg_job_stack_cur_pos = 0;
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&conn->bg_conn_sync_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_conn_sync, &conn->bg_conn_sync_cond,
                        NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&conn->bg_conn_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_conn, &conn->bg_conn_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_cond_init;
    }
    pthread_mutex_lock(&conn->bg_conn_mutex);
#if MYSQL_VERSION_ID < 50500
    if (pthread_create(&conn->bg_thread, &spider_pt_attr, spider_bg_conn_action,
                       (void *)conn))
#else
    if (mysql_thread_create(spd_key_thd_bg, &conn->bg_thread, &spider_pt_attr,
                            spider_bg_conn_action, (void *)conn))
#endif
    {
      pthread_mutex_unlock(&conn->bg_conn_mutex);
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_thread_create;
    }
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_mutex);
    pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    if (!conn->bg_init) {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_thread_create;
    }
  }
  DBUG_RETURN(0);

error_thread_create:
  pthread_cond_destroy(&conn->bg_conn_cond);
error_cond_init:
  pthread_cond_destroy(&conn->bg_conn_sync_cond);
error_sync_cond_init:
  spider_free_mem_calc(
      spider_current_trx, conn->bg_job_stack_id,
      conn->bg_job_stack.max_element * conn->bg_job_stack.size_of_element);
  delete_dynamic(&conn->bg_job_stack);
error_job_stack_init:
  pthread_mutex_destroy(&conn->bg_job_stack_mutex);
error_job_stack_mutex_init:
  pthread_mutex_destroy(&conn->bg_conn_mutex);
error_mutex_init:
  pthread_mutex_destroy(&conn->bg_conn_sync_mutex);
error_sync_mutex_init:
  pthread_mutex_destroy(&conn->bg_conn_chain_mutex);
error_chain_mutex_init:
  DBUG_RETURN(error_num);
}

void spider_free_conn_thread(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_free_conn_thread");
  if (conn->bg_init) {
    spider_bg_conn_break(conn, NULL);
    pthread_mutex_lock(&conn->bg_conn_mutex);
    conn->bg_kill = TRUE;
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
    pthread_cond_signal(&conn->bg_conn_cond);
    pthread_mutex_unlock(&conn->bg_conn_mutex);
    pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    pthread_join(conn->bg_thread, NULL);
    pthread_cond_destroy(&conn->bg_conn_cond);
    pthread_cond_destroy(&conn->bg_conn_sync_cond);
    spider_free_mem_calc(
        spider_current_trx, conn->bg_job_stack_id,
        conn->bg_job_stack.max_element * conn->bg_job_stack.size_of_element);
    delete_dynamic(&conn->bg_job_stack);
    pthread_mutex_destroy(&conn->bg_job_stack_mutex);
    pthread_mutex_destroy(&conn->bg_conn_mutex);
    pthread_mutex_destroy(&conn->bg_conn_sync_mutex);
    pthread_mutex_destroy(&conn->bg_conn_chain_mutex);
    conn->bg_kill = FALSE;
    conn->bg_init = FALSE;
  }
  DBUG_VOID_RETURN;
}

void spider_bg_conn_wait(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_bg_conn_wait");
  if (conn->bg_init) {
    pthread_mutex_lock(&conn->bg_conn_mutex);
    pthread_mutex_unlock(&conn->bg_conn_mutex);
  }
  DBUG_VOID_RETURN;
}

void spider_bg_all_conn_wait(ha_spider *spider) {
  int roop_count;
  SPIDER_CONN *conn;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_bg_all_conn_wait");
  for (roop_count = spider_conn_link_idx_next(
           share->link_statuses, spider->conn_link_idx, -1, share->link_count,
           SPIDER_LINK_STATUS_RECOVERY);
       roop_count < (int)share->link_count;
       roop_count = spider_conn_link_idx_next(
           share->link_statuses, spider->conn_link_idx, roop_count,
           share->link_count, SPIDER_LINK_STATUS_RECOVERY)) {
    conn = spider->conns[roop_count];
    if (conn && result_list->bgs_working) spider_bg_conn_wait(conn);
  }
  DBUG_VOID_RETURN;
}

int spider_bg_all_conn_pre_next(ha_spider *spider, int link_idx) {
  int roop_start, roop_end, roop_count, lock_mode, link_ok, error_num;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_SHARE *share = spider->share;
  THD *thd = current_thd;
  DBUG_ENTER("spider_bg_all_conn_pre_next");
  thd_proc_info(thd, "pre_next start");
  if (result_list->bgs_phase > 0) {
    lock_mode = spider_conn_lock_mode(spider);
    if (lock_mode) {
      /* "for update" or "lock in share mode" */
      link_ok = spider_conn_link_idx_next(
          share->link_statuses, spider->conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_OK);
      roop_start = spider_conn_link_idx_next(
          share->link_statuses, spider->conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY);
      roop_end = spider->share->link_count;
    } else {
      link_ok = link_idx;
      roop_start = link_idx;
      roop_end = link_idx + 1;
    }

    for (roop_count = roop_start; roop_count < roop_end;
         roop_count = spider_conn_link_idx_next(
             share->link_statuses, spider->conn_link_idx, roop_count,
             share->link_count, SPIDER_LINK_STATUS_RECOVERY)) {
      if ((error_num =
               spider_bg_conn_search(spider, roop_count, roop_start, TRUE, TRUE,
                                     (roop_count != link_ok)))) {
        thd_proc_info(thd, "pre_next end");
        DBUG_RETURN(error_num);
      }
    }
  }
  thd_proc_info(thd, "pre_next end");
  DBUG_RETURN(0);
}

void spider_bg_conn_break(SPIDER_CONN *conn, ha_spider *spider) {
  DBUG_ENTER("spider_bg_conn_break");
  if (conn->bg_init && conn->bg_thd != current_thd &&
      (!spider ||
       (spider->result_list.bgs_working && conn->bg_target == spider))) {
    conn->bg_break = TRUE;
    pthread_mutex_lock(&conn->bg_conn_mutex);
    pthread_mutex_unlock(&conn->bg_conn_mutex);
    conn->bg_break = FALSE;
  }
  DBUG_VOID_RETURN;
}

void spider_bg_all_conn_break(ha_spider *spider) {
  int roop_count;
  SPIDER_CONN *conn;
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_bg_all_conn_break");
  for (roop_count = spider_conn_link_idx_next(
           share->link_statuses, spider->conn_link_idx, -1, share->link_count,
           SPIDER_LINK_STATUS_RECOVERY);
       roop_count < (int)share->link_count;
       roop_count = spider_conn_link_idx_next(
           share->link_statuses, spider->conn_link_idx, roop_count,
           share->link_count, SPIDER_LINK_STATUS_RECOVERY)) {
    conn = spider->conns[roop_count];
    if (conn && result_list->bgs_working) spider_bg_conn_break(conn, spider);
    if (conn && spider->quick_targets[roop_count]) {
      spider_db_free_one_quick_result((SPIDER_RESULT *)result_list->current);
      DBUG_ASSERT(spider->quick_targets[roop_count] == conn->quick_target);
      DBUG_PRINT("info", ("spider conn[%p]->quick_target=NULL", conn));
      conn->quick_target = NULL;
      spider->quick_targets[roop_count] = NULL;
    }
  }
  DBUG_VOID_RETURN;
}

bool spider_bg_conn_get_job(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_bg_conn_get_job");
  pthread_mutex_lock(&conn->bg_job_stack_mutex);
  if (conn->bg_job_stack_cur_pos >= conn->bg_job_stack.elements) {
    DBUG_PRINT("info", ("spider bg all jobs are completed"));
    conn->bg_get_job_stack_off = FALSE;
    pthread_mutex_unlock(&conn->bg_job_stack_mutex);
    DBUG_RETURN(FALSE);
  }
  DBUG_PRINT("info", ("spider bg get job %u", conn->bg_job_stack_cur_pos));
  conn->bg_target = ((void **)(conn->bg_job_stack.buffer +
                               conn->bg_job_stack.size_of_element *
                                   conn->bg_job_stack_cur_pos))[0];
  conn->bg_job_stack_cur_pos++;
  if (conn->bg_job_stack_cur_pos == conn->bg_job_stack.elements) {
    DBUG_PRINT("info", ("spider bg shift job stack"));
    conn->bg_job_stack_cur_pos = 0;
    conn->bg_job_stack.elements = 0;
  }
  pthread_mutex_unlock(&conn->bg_job_stack_mutex);
  DBUG_RETURN(TRUE);
}

int spider_bg_conn_search(ha_spider *spider, int link_idx, int first_link_idx,
                          bool first, bool pre_next, bool discard_result,
                          ulong sql_type) {
  int error_num;
  SPIDER_CONN *conn, *first_conn = NULL;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  bool with_lock = FALSE;
  THD *thd = current_thd;
  DBUG_ENTER("spider_bg_conn_search");
  DBUG_PRINT("info", ("spider spider=%p", spider));
  conn = spider->spider_get_conn_by_idx(link_idx);
  if (!conn) {
    error_num = ER_SPIDER_CON_COUNT_ERROR;
    DBUG_RETURN(error_num);
  }
  with_lock = (spider_conn_lock_mode(spider) != SPIDER_LOCK_MODE_NO_LOCK);
  first_conn = spider->spider_get_conn_by_idx(first_link_idx);
  if (!first_conn) {
    error_num = ER_SPIDER_CON_COUNT_ERROR;
    DBUG_RETURN(error_num);
  }
  if (first) {
    if (spider->use_pre_call) {
      DBUG_PRINT("info", ("spider skip bg first search"));
    } else {
      DBUG_PRINT("info", ("spider bg first search"));
      pthread_mutex_lock(&conn->bg_conn_mutex);
      result_list->sql_type = sql_type;
      result_list->bgs_working = TRUE;
      conn->bg_search = TRUE;
      conn->bg_caller_wait = TRUE;
      conn->bg_target = spider;
      conn->link_idx = link_idx;
      conn->bg_discard_result = discard_result;
      if (sql_type != SPIDER_SQL_TYPE_SELECT_SQL)
        conn->bg_caller_sync_wait = TRUE;  // wait for sql ready only

      thd_proc_info(thd, "Waking up bg thread ");
      pthread_mutex_lock(
          &conn->bg_conn_sync_mutex);  // must ensure: before signal backend is wait
      pthread_cond_signal(&conn->bg_conn_cond);
      pthread_mutex_unlock(&conn->bg_conn_mutex);
      pthread_cond_wait(&conn->bg_conn_sync_cond,
                        &conn->bg_conn_sync_mutex);  // also ok if don't wait
                                                     // ? handshake ?
      pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
      conn->bg_caller_wait = FALSE;
      if (sql_type != SPIDER_SQL_TYPE_SELECT_SQL)
        conn->bg_caller_sync_wait = FALSE;  // reset bg_caller_sync_wait,in
                                            // case of spider_conn_recycle
      if (result_list->bgs_error &&
          result_list->bgs_error != HA_ERR_END_OF_FILE) {
        if (result_list->bgs_error_with_message)
          my_message(result_list->bgs_error, result_list->bgs_error_msg,
                     MYF(0));
        DBUG_RETURN(result_list->bgs_error);
      }
    }
    if (sql_type == SPIDER_SQL_TYPE_SELECT_SQL &&
        (result_list->bgs_working || !result_list->finish_flg ||
         conn->bg_conn_working)) {
      thd_proc_info(thd, "Waiting bg action");
      pthread_mutex_lock(
          &conn->bg_conn_mutex); /* ֻonly spider_bg_action at pthread_cond_wait can success 1. wait query; 2. process again
                                  */
      assert(!conn->bg_conn_working);
      result_list->sql_type = sql_type;
      if (!result_list->finish_flg) {
        DBUG_PRINT("info", ("spider bg second search"));
        if (!spider->use_pre_call || pre_next) {
          if (result_list->bgs_error) {
            pthread_mutex_unlock(&conn->bg_conn_mutex);
            DBUG_PRINT("info", ("spider bg error"));
            if (result_list->bgs_error == HA_ERR_END_OF_FILE) {
              DBUG_PRINT(
                  "info",
                  ("spider bg current->finish_flg=%s",
                   result_list->current
                       ? (result_list->current->finish_flg ? "TRUE" : "FALSE")
                       : "NULL"));
              DBUG_RETURN(0);
            }
            if (result_list->bgs_error_with_message)
              my_message(result_list->bgs_error, result_list->bgs_error_msg,
                         MYF(0));
            DBUG_RETURN(result_list->bgs_error);
          }
          DBUG_PRINT("info", ("spider result_list->quick_mode=%d",
                              result_list->quick_mode));
          DBUG_PRINT("info", ("spider result_list->bgs_current->result=%p",
                              result_list->bgs_current->result));
          if (result_list->quick_mode == 0 ||
              !result_list->bgs_current->result) {
            DBUG_PRINT("info", ("spider result_list->bgs_second_read=%lld",
                                result_list->bgs_second_read));
            DBUG_PRINT("info", ("spider result_list->bgs_split_read=%lld",
                                result_list->bgs_split_read));
            result_list->split_read = result_list->bgs_second_read > 0
                                          ? result_list->bgs_second_read
                                          : result_list->bgs_split_read;
            result_list->limit_num =
                result_list->internal_limit - result_list->record_num >=
                        result_list->split_read
                    ? result_list->split_read
                    : result_list->internal_limit - result_list->record_num;
            DBUG_PRINT("info", ("spider sql_kinds=%u", spider->sql_kinds));
            if (spider->sql_kinds & SPIDER_SQL_KIND_SQL) {
              if ((error_num = spider->reappend_limit_sql_part(
                       result_list->internal_offset + result_list->record_num,
                       result_list->limit_num, SPIDER_SQL_TYPE_SELECT_SQL))) {
                pthread_mutex_unlock(&conn->bg_conn_mutex);
                DBUG_RETURN(error_num);
              }
              if (!result_list->use_union &&
                  (error_num = spider->append_select_lock_sql_part(
                       SPIDER_SQL_TYPE_SELECT_SQL))) {
                pthread_mutex_unlock(&conn->bg_conn_mutex);
                DBUG_RETURN(error_num);
              }
            }
            if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER) {
              spider_db_append_handler_next(spider);
              if ((error_num = spider->reappend_limit_sql_part(
                       0, result_list->limit_num, SPIDER_SQL_TYPE_HANDLER))) {
                pthread_mutex_unlock(&conn->bg_conn_mutex);
                DBUG_RETURN(error_num);
              }
            }
          }
          result_list->bgs_phase = 2;
        }
        result_list->bgs_working = TRUE;
        conn->bg_search = TRUE;
        if (with_lock)
          conn->bg_conn_chain_mutex_ptr = &first_conn->bg_conn_chain_mutex;
        conn->bg_caller_sync_wait = TRUE;
        conn->bg_target = spider;
        conn->link_idx = link_idx;
        conn->bg_discard_result = discard_result;
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
        conn->link_idx_chain = spider->link_idx_chain;
#endif
        thd_proc_info(thd, "Starting bg action");
        pthread_mutex_lock(&conn->bg_conn_sync_mutex);
        pthread_cond_signal(&conn->bg_conn_cond);  // send signal => backend to execute sql
        pthread_mutex_unlock(&conn->bg_conn_mutex);
        pthread_cond_wait(
            &conn->bg_conn_sync_cond,
            &conn->bg_conn_sync_mutex);  // make sure that backend has executed sql, not at wait state
        pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
        conn->bg_caller_sync_wait = FALSE;
      } else {
        pthread_mutex_unlock(&conn->bg_conn_mutex);
        DBUG_PRINT("info",
                   ("spider bg current->finish_flg=%s",
                    result_list->current
                        ? (result_list->current->finish_flg ? "TRUE" : "FALSE")
                        : "NULL"));
        if (result_list->bgs_error) {
          DBUG_PRINT("info", ("spider bg error"));
          if (result_list->bgs_error != HA_ERR_END_OF_FILE) {
            if (result_list->bgs_error_with_message)
              my_message(result_list->bgs_error, result_list->bgs_error_msg,
                         MYF(0));
            DBUG_RETURN(result_list->bgs_error);
          }
        }
      }
    } else {
      DBUG_PRINT("info",
                 ("spider bg current->finish_flg=%s",
                  result_list->current
                      ? (result_list->current->finish_flg ? "TRUE" : "FALSE")
                      : "NULL"));
      if (result_list->bgs_error) {
        DBUG_PRINT("info", ("spider bg error"));
        if (result_list->bgs_error != HA_ERR_END_OF_FILE) {
          if (result_list->bgs_error_with_message)
            my_message(result_list->bgs_error, result_list->bgs_error_msg,
                       MYF(0));
          DBUG_RETURN(result_list->bgs_error);
        }
      }
    }
  } else {
    DBUG_PRINT("info", ("spider bg search"));
    if (result_list->current->finish_flg) {
      DBUG_PRINT("info", ("spider bg end of file"));
      result_list->table->status = STATUS_NOT_FOUND;
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
    if (result_list->bgs_working) {
      /* wait */
      DBUG_PRINT("info", ("spider bg working wait"));
      thd_proc_info(thd, "Waiting bg action done");
      pthread_mutex_lock(&conn->bg_conn_mutex);
      result_list->sql_type = sql_type;
      pthread_mutex_unlock(&conn->bg_conn_mutex);
    }
    if (result_list->bgs_error) {
      DBUG_PRINT("info", ("spider bg error"));
      if (result_list->bgs_error == HA_ERR_END_OF_FILE) {
        result_list->current = result_list->current->next;
        result_list->current_row_num = 0;
        result_list->table->status = STATUS_NOT_FOUND;
      }
      if (result_list->bgs_error_with_message)
        my_message(result_list->bgs_error, result_list->bgs_error_msg, MYF(0));
      DBUG_RETURN(result_list->bgs_error);
    }
    result_list->current = result_list->current->next;
    result_list->current_row_num = 0;
    if (result_list->current == result_list->bgs_current) {
      assert(sql_type == SPIDER_SQL_TYPE_SELECT_SQL);
      DBUG_PRINT("info", ("spider bg next search"));
      if (!result_list->current->finish_flg) {
        DBUG_PRINT("info", ("spider result_list->quick_mode=%d",
                            result_list->quick_mode));
        DBUG_PRINT("info", ("spider result_list->bgs_current->result=%p",
                            result_list->bgs_current->result));
        pthread_mutex_lock(&conn->bg_conn_mutex);
        result_list->bgs_phase = 3;
        if (result_list->quick_mode == 0 || !result_list->bgs_current->result) {
          result_list->split_read = result_list->bgs_split_read;
          result_list->limit_num =
              result_list->internal_limit - result_list->record_num >=
                      result_list->split_read
                  ? result_list->split_read
                  : result_list->internal_limit - result_list->record_num;
          DBUG_PRINT("info", ("spider sql_kinds=%u", spider->sql_kinds));
          if (spider->sql_kinds & SPIDER_SQL_KIND_SQL) {
            if ((error_num = spider->reappend_limit_sql_part(
                     result_list->internal_offset + result_list->record_num,
                     result_list->limit_num, SPIDER_SQL_TYPE_SELECT_SQL))) {
              pthread_mutex_unlock(&conn->bg_conn_mutex);
              DBUG_RETURN(error_num);
            }
            if (!result_list->use_union &&
                (error_num = spider->append_select_lock_sql_part(
                     SPIDER_SQL_TYPE_SELECT_SQL))) {
              pthread_mutex_unlock(&conn->bg_conn_mutex);
              DBUG_RETURN(error_num);
            }
          }
          if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER) {
            spider_db_append_handler_next(spider);
            if ((error_num = spider->reappend_limit_sql_part(
                     0, result_list->limit_num, SPIDER_SQL_TYPE_HANDLER))) {
              pthread_mutex_unlock(&conn->bg_conn_mutex);
              DBUG_RETURN(error_num);
            }
          }
        }
        conn->bg_target = spider;
        conn->link_idx = link_idx;
        conn->bg_discard_result = discard_result;
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
        conn->link_idx_chain = spider->link_idx_chain;
#endif
        result_list->bgs_working = TRUE;
        conn->bg_search = TRUE;
        if (with_lock)
          conn->bg_conn_chain_mutex_ptr = &first_conn->bg_conn_chain_mutex;
        conn->bg_caller_sync_wait = TRUE;
        thd_proc_info(thd, "Waiting bg store result");
        pthread_mutex_lock(&conn->bg_conn_sync_mutex);
        pthread_cond_signal(&conn->bg_conn_cond);
        pthread_mutex_unlock(&conn->bg_conn_mutex);
        pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
        pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
        conn->bg_caller_sync_wait = FALSE;
      }
    }
  }
  DBUG_RETURN(0);
}

void spider_bg_conn_simple_action(SPIDER_CONN *conn, uint simple_action,
                                  bool caller_wait, void *target, uint link_idx,
                                  int *error_num) {
  DBUG_ENTER("spider_bg_conn_simple_action");
  pthread_mutex_lock(&conn->bg_conn_mutex);
  conn->bg_target = target;
  conn->link_idx = link_idx;
  conn->bg_simple_action = simple_action;
  conn->bg_error_num = error_num;
  if (caller_wait) {
    conn->bg_caller_wait = TRUE;
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
  } else {
    conn->bg_caller_sync_wait = TRUE;
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
  }
  pthread_cond_signal(&conn->bg_conn_cond);
  pthread_mutex_unlock(&conn->bg_conn_mutex);
  if (caller_wait) {
    pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    conn->bg_caller_wait = FALSE;
  } else {
    pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    conn->bg_caller_sync_wait = FALSE;
  }
  DBUG_VOID_RETURN;
}

void *spider_bg_conn_action(void *arg) {
  int error_num;
  SPIDER_CONN *conn = (SPIDER_CONN *)arg;
  SPIDER_TRX *trx;
  ha_spider *spider;
  SPIDER_RESULT_LIST *result_list;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_bg_conn_action");
  /* init start */
  if (!(thd = SPIDER_new_THD(next_thread_id()))) {
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
    pthread_cond_signal(&conn->bg_conn_sync_cond);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd->thread_stack = (char *)&thd;
  thd->store_globals();
  if (!(trx = spider_get_trx(thd, FALSE, &error_num))) {
    delete thd;
    pthread_mutex_lock(&conn->bg_conn_sync_mutex);
    pthread_cond_signal(&conn->bg_conn_sync_cond);
    pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32) || defined(_MSC_VER)
    my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  thread_safe_decrement32(
      &thread_count); /* for shutdonw, don't wait this thread */
  /* lex_start(thd); */
  conn->bg_thd = thd;
  pthread_mutex_lock(&conn->bg_conn_mutex);
  pthread_mutex_lock(&conn->bg_conn_sync_mutex);
  pthread_cond_signal(&conn->bg_conn_sync_cond);
  conn->bg_init = TRUE;
  pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
  /* init end */

  while (TRUE) {
    bool set_sql = false;
    if (conn->bg_conn_chain_mutex_ptr) {
      pthread_mutex_unlock(conn->bg_conn_chain_mutex_ptr);
      conn->bg_conn_chain_mutex_ptr = NULL;
    }
    thd->clear_error();
    conn->bg_conn_working = false;
    pthread_cond_wait(
        &conn->bg_conn_cond,
        &conn->bg_conn_mutex); /* wait main thread processing the query */
    conn->bg_conn_working = true;
    DBUG_PRINT("info", ("spider bg roop start"));
#ifndef DBUG_OFF
    DBUG_PRINT("info", ("spider conn->thd=%p", conn->thd));
    if (conn->thd) {
      DBUG_PRINT("info", ("spider query_id=%lld", conn->thd->query_id));
    }
#endif
    if (conn->bg_caller_sync_wait) {
      spider_db_handler *dbton_handler;
      spider = (ha_spider *)conn->bg_target;
      dbton_handler = spider->dbton_handler[conn->dbton_id];
      result_list = &spider->result_list;

      if (result_list->sql_type == SPIDER_SQL_TYPE_INSERT_SQL) {
        result_list->bgs_error = 0;
        result_list->bgs_error_with_message = FALSE;
        // sql should be set before signal
        if ((error_num = dbton_handler->set_sql_for_exec(
                 result_list->sql_type, conn->link_idx, true))) {
          result_list->bgs_error = error_num;
          if ((result_list->bgs_error_with_message = thd->is_error()))
            strmov(result_list->bgs_error_msg, spider_stmt_da_message(thd));
        }
        set_sql = true;
      }
      /* after bg_search sending signal, tell main/sub threads to work */
      pthread_mutex_lock(&conn->bg_conn_sync_mutex);
      if (conn->bg_direct_sql) conn->bg_get_job_stack_off = TRUE;
      pthread_cond_signal(
          &conn->bg_conn_sync_cond); /* send signal of responding */
      pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
      if (conn->bg_conn_chain_mutex_ptr) { /* normaly it's 0
                                              in TSpider, one shard => one conn
                                            */
        pthread_mutex_lock(conn->bg_conn_chain_mutex_ptr);
        if ((&conn->bg_conn_chain_mutex) != conn->bg_conn_chain_mutex_ptr) {
          pthread_mutex_unlock(conn->bg_conn_chain_mutex_ptr);
          conn->bg_conn_chain_mutex_ptr = NULL;
        }
      }
    }
    if (conn->bg_kill) { /* true when free conn */
      DBUG_PRINT("info", ("spider bg kill start"));
      if (conn->bg_conn_chain_mutex_ptr) {
        pthread_mutex_unlock(conn->bg_conn_chain_mutex_ptr);
        conn->bg_conn_chain_mutex_ptr = NULL;
      }
      spider_free_trx(trx, TRUE);
      thread_safe_increment32(&thread_count);
      /* lex_end(thd->lex); */
      delete thd;
      pthread_mutex_lock(&conn->bg_conn_sync_mutex);
      pthread_cond_signal(&conn->bg_conn_sync_cond);
      pthread_mutex_unlock(&conn->bg_conn_mutex);
      pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32) || defined(_MSC_VER)
      my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
      my_thread_end();
      DBUG_RETURN(NULL);
    }
    if (conn->bg_get_job_stack) { /* only udf reach here */
      conn->bg_get_job_stack = FALSE;
      if (!spider_bg_conn_get_job(conn)) {
        conn->bg_direct_sql = FALSE;
      }
    }
    if (conn->bg_search) {
      SPIDER_SHARE *share;
      spider_db_handler *dbton_handler;
      DBUG_PRINT("info", ("spider bg search start"));
      spider = (ha_spider *)conn->bg_target;
      share = spider->share;
      dbton_handler = spider->dbton_handler[conn->dbton_id];
      result_list = &spider->result_list;
      result_list->bgs_error = 0;
      result_list->bgs_error_with_message = FALSE;
      if (result_list->quick_mode == 0 || result_list->bgs_phase == 1 ||
          !result_list->bgs_current->result) {
        ulong sql_type;
        if (spider->sql_kind[conn->link_idx] == SPIDER_SQL_KIND_SQL) {
          sql_type = result_list->sql_type | SPIDER_SQL_TYPE_TMP_SQL;
        } else {
          sql_type = SPIDER_SQL_TYPE_HANDLER;
        }
        if (dbton_handler->need_lock_before_set_sql_for_exec(sql_type)) {
          spider_mta_conn_mutex_lock(conn);
        }
        if (spider->use_fields) {
          if (!set_sql &&
              (error_num = dbton_handler->set_sql_for_exec(
                   sql_type, conn->link_idx, conn->link_idx_chain))) {
            result_list->bgs_error = error_num;
            if ((result_list->bgs_error_with_message = thd->is_error()))
              strmov(result_list->bgs_error_msg, spider_stmt_da_message(thd));
          }
        } else {
          if (!set_sql && (error_num = dbton_handler->set_sql_for_exec(
                               sql_type, conn->link_idx))) {
            result_list->bgs_error = error_num;
            if ((result_list->bgs_error_with_message = thd->is_error()))
              strmov(result_list->bgs_error_msg, spider_stmt_da_message(thd));
          }
        }
        if (!dbton_handler->need_lock_before_set_sql_for_exec(sql_type)) {
          spider_mta_conn_mutex_lock(conn);
        }
        sql_type &= ~SPIDER_SQL_TYPE_TMP_SQL;
        DBUG_PRINT("info", ("spider sql_type=%lu", sql_type));
#ifdef HA_CAN_BULK_ACCESS
        if (spider->is_bulk_access_clone) {
          spider->connection_ids[conn->link_idx] = conn->connection_id;
          spider_trx_add_bulk_access_conn(spider->trx, conn);
        }
#endif
        if (!result_list->bgs_error) {
          conn->need_mon = &spider->need_mons[conn->link_idx];
          conn->mta_conn_mutex_lock_already = TRUE;
          conn->mta_conn_mutex_unlock_later = TRUE;
#ifdef HA_CAN_BULK_ACCESS
          if (!spider->is_bulk_access_clone) {
#endif
            if (!(result_list->bgs_error =
                      spider_db_set_names(spider, conn, conn->link_idx))) {
              if (result_list->tmp_table_join && spider->bka_mode != 2 &&
                  spider_bit_is_set(result_list->tmp_table_join_first,
                                    conn->link_idx)) {
                spider_clear_bit(result_list->tmp_table_join_first,
                                 conn->link_idx);
                spider_set_bit(result_list->tmp_table_created, conn->link_idx);
                result_list->tmp_tables_created = TRUE;
                spider_conn_set_timeout_from_share(conn, conn->link_idx,
                                                   spider->trx->thd, share);
                if (dbton_handler->execute_sql(
                        SPIDER_SQL_TYPE_TMP_SQL, conn, -1,
                        &spider->need_mons[conn->link_idx])) {
                  result_list->bgs_error = spider_db_errorno(conn);
                  if ((result_list->bgs_error_with_message = thd->is_error()))
                    strmov(result_list->bgs_error_msg,
                           spider_stmt_da_message(thd));
                } else
                  spider_db_discard_multiple_result(spider, conn->link_idx,
                                                    conn);
              }
              if (!result_list->bgs_error) {
                spider_conn_set_timeout_from_share(conn, conn->link_idx,
                                                   spider->trx->thd, share);
                if (dbton_handler->execute_sql(
                        sql_type, conn, result_list->quick_mode,
                        &spider->need_mons[conn->link_idx])) {
                  result_list->bgs_error = spider_db_errorno(conn);
                  if ((result_list->bgs_error_with_message = thd->is_error()))
                    strmov(result_list->bgs_error_msg,
                           spider_stmt_da_message(thd));
                } else {
                  spider->connection_ids[conn->link_idx] = conn->connection_id;
                  if (!conn->bg_discard_result) {
                    if (!(result_list->bgs_error = spider_db_store_result(
                              spider, conn->link_idx, result_list->table)))
                      spider->result_link_idx = conn->link_idx;
                    else {
                      if ((result_list->bgs_error_with_message =
                               thd->is_error()))
                        strmov(result_list->bgs_error_msg,
                               spider_stmt_da_message(thd));
                    }
                  } else {
                    result_list->bgs_error = 0;
                    spider_db_discard_result(spider, conn->link_idx, conn);
                  }
                }
              }
            } else {
              if ((result_list->bgs_error_with_message = thd->is_error()))
                strmov(result_list->bgs_error_msg, spider_stmt_da_message(thd));
            }
#ifdef HA_CAN_BULK_ACCESS
          }
#endif
          conn->mta_conn_mutex_lock_already = FALSE;
          conn->mta_conn_mutex_unlock_later = FALSE;
          spider_mta_conn_mutex_unlock(conn);
        } else {
          spider_mta_conn_mutex_unlock(conn);
        }
      } else { /* fetch result next time */
        spider->connection_ids[conn->link_idx] = conn->connection_id;
        conn->mta_conn_mutex_unlock_later = TRUE;
        result_list->bgs_error =
            spider_db_store_result(spider, conn->link_idx, result_list->table);
        if ((result_list->bgs_error_with_message = thd->is_error()))
          strmov(result_list->bgs_error_msg, spider_stmt_da_message(thd));
        conn->mta_conn_mutex_unlock_later = FALSE;
      }
      conn->bg_search = FALSE;
      result_list->bgs_working = FALSE;
      if (conn->bg_caller_wait) {
        pthread_mutex_lock(&conn->bg_conn_sync_mutex);
        pthread_cond_signal(&conn->bg_conn_sync_cond);
        pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
      }
      continue;
    }
    if (conn->bg_direct_sql) {  // if not udf
      bool is_error = FALSE;
      DBUG_PRINT("info", ("spider bg direct sql start"));
      do {
        SPIDER_DIRECT_SQL *direct_sql = (SPIDER_DIRECT_SQL *)conn->bg_target;
        if ((error_num = spider_db_udf_direct_sql(direct_sql))) {
          if (thd->is_error()) {
            if (direct_sql->error_rw_mode &&
                spider_db_conn_is_network_error(error_num)) {
              thd->clear_error();
            } else {
              SPIDER_BG_DIRECT_SQL *bg_direct_sql =
                  (SPIDER_BG_DIRECT_SQL *)direct_sql->parent;
              pthread_mutex_lock(direct_sql->bg_mutex);
              bg_direct_sql->bg_error = spider_stmt_da_sql_errno(thd);
              strmov((char *)bg_direct_sql->bg_error_msg,
                     spider_stmt_da_message(thd));
              pthread_mutex_unlock(direct_sql->bg_mutex);
              is_error = TRUE;
            }
          }
        }
        if (direct_sql->modified_non_trans_table) {
          SPIDER_BG_DIRECT_SQL *bg_direct_sql =
              (SPIDER_BG_DIRECT_SQL *)direct_sql->parent;
          pthread_mutex_lock(direct_sql->bg_mutex);
          bg_direct_sql->modified_non_trans_table = TRUE;
          pthread_mutex_unlock(direct_sql->bg_mutex);
        }
        spider_udf_free_direct_sql_alloc(direct_sql, TRUE);
      } while (!is_error && spider_bg_conn_get_job(conn));
      if (is_error) {
        while (spider_bg_conn_get_job(conn))
          spider_udf_free_direct_sql_alloc((SPIDER_DIRECT_SQL *)conn->bg_target,
                                           TRUE);
      }
      conn->bg_direct_sql = FALSE;
      continue;
    }
    if (conn->bg_exec_sql) {  // if not udf
      DBUG_PRINT("info", ("spider bg exec sql start"));
      spider = (ha_spider *)conn->bg_target;
      *conn->bg_error_num = spider_db_query_with_set_names(
          conn->bg_sql_type, spider, conn, conn->link_idx);
      conn->bg_exec_sql = FALSE;
      continue;
    }
    if (conn->bg_simple_action) {  // if oracle
      switch (conn->bg_simple_action) {
        case SPIDER_BG_SIMPLE_CONNECT:
          conn->db_conn->bg_connect();
          break;
        case SPIDER_BG_SIMPLE_DISCONNECT:
          conn->db_conn->bg_disconnect();
          break;
        case SPIDER_BG_SIMPLE_RECORDS:
          DBUG_PRINT("info", ("spider bg simple records"));
          spider = (ha_spider *)conn->bg_target;
          *conn->bg_error_num =
              spider->dbton_handler[conn->dbton_id]->show_records(
                  conn->link_idx);
          break;
        default:
          break;
      }
      conn->bg_simple_action = SPIDER_BG_SIMPLE_NO_ACTION;
      if (conn->bg_caller_wait) {
        pthread_mutex_lock(&conn->bg_conn_sync_mutex);
        pthread_cond_signal(&conn->bg_conn_sync_cond);
        pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
      }
      continue;
    }
    if (conn->bg_break) {
      DBUG_PRINT("info", ("spider bg break start"));
      spider = (ha_spider *)conn->bg_target;
      result_list = &spider->result_list;
      result_list->bgs_working = FALSE;
      continue;
    }
  }
}

int spider_create_sts_thread(SPIDER_SHARE *share) {
  int error_num;
  DBUG_ENTER("spider_create_sts_thread");
  if (!share->bg_sts_init) {
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&share->bg_sts_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_sts, &share->bg_sts_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&share->bg_sts_sync_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_sts_sync, &share->bg_sts_sync_cond,
                        NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_create(&share->bg_sts_thread, &spider_pt_attr,
                       spider_bg_sts_action, (void *)share))
#else
    if (mysql_thread_create(spd_key_thd_bg_sts, &share->bg_sts_thread,
                            &spider_pt_attr, spider_bg_sts_action,
                            (void *)share))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_thread_create;
    }
    share->bg_sts_init = TRUE;
  }
  DBUG_RETURN(0);

error_thread_create:
  pthread_cond_destroy(&share->bg_sts_sync_cond);
error_sync_cond_init:
  pthread_cond_destroy(&share->bg_sts_cond);
error_cond_init:
  DBUG_RETURN(error_num);
}

void spider_free_sts_thread(SPIDER_SHARE *share) {
  DBUG_ENTER("spider_free_sts_thread");
  if (share->bg_sts_init) {
    pthread_mutex_lock(&share->sts_mutex);
    share->bg_sts_kill = TRUE;
    pthread_cond_signal(&share->bg_sts_cond);
    pthread_cond_wait(&share->bg_sts_sync_cond, &share->sts_mutex);
    pthread_mutex_unlock(&share->sts_mutex);
    pthread_join(share->bg_sts_thread, NULL);
    pthread_cond_destroy(&share->bg_sts_sync_cond);
    pthread_cond_destroy(&share->bg_sts_cond);
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
  }
  DBUG_VOID_RETURN;
}

void *spider_bg_sts_action(void *arg) {
  SPIDER_SHARE *share = (SPIDER_SHARE *)arg;
  SPIDER_TRX *trx;
  int error_num = 0, roop_count;
  ha_spider spider;
  int *need_mons;
  SPIDER_CONN **conns;
  uint *conn_link_idx;
  uchar *conn_can_fo;
  char **conn_keys;
  spider_db_handler **dbton_hdl;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_bg_sts_action");
  /* init start */
  char *ptr;
  ptr = (char *)my_alloca((sizeof(int) * share->link_count) +
                          (sizeof(SPIDER_CONN *) * share->link_count) +
                          (sizeof(uint) * share->link_count) +
                          (sizeof(uchar) * share->link_bitmap_size) +
                          (sizeof(char *) * share->link_count) +
                          (sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE));
  if (!ptr) {
    pthread_mutex_lock(&share->sts_mutex);
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
    pthread_mutex_unlock(&share->sts_mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  need_mons = (int *)ptr;
  ptr += (sizeof(int) * share->link_count);
  conns = (SPIDER_CONN **)ptr;
  ptr += (sizeof(SPIDER_CONN *) * share->link_count);
  conn_link_idx = (uint *)ptr;
  ptr += (sizeof(uint) * share->link_count);
  conn_can_fo = (uchar *)ptr;
  ptr += (sizeof(uchar) * share->link_bitmap_size);
  conn_keys = (char **)ptr;
  ptr += (sizeof(char *) * share->link_count);
  dbton_hdl = (spider_db_handler **)ptr;
  pthread_mutex_lock(&share->sts_mutex);
  if (!(thd = SPIDER_new_THD(next_thread_id()))) {
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
    pthread_mutex_unlock(&share->sts_mutex);
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd->thread_stack = (char *)&thd;
  thd->store_globals();
  if (!(trx = spider_get_trx(thd, FALSE, &error_num))) {
    delete thd;
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
    pthread_mutex_unlock(&share->sts_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32) || defined(_MSC_VER)
    my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  share->bg_sts_thd = thd;
  spider.trx = trx;
  spider.share = share;
  spider.conns = conns;
  spider.conn_link_idx = conn_link_idx;
  spider.conn_can_fo = conn_can_fo;
  spider.need_mons = need_mons;
  spider.dbton_handler = dbton_hdl;
  memset(conns, 0, sizeof(SPIDER_CONN *) * share->link_count);
  memset(need_mons, 0, sizeof(int) * share->link_count);
  memset(dbton_hdl, 0, sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE);
  spider_trx_set_link_idx_for_all(&spider);
  spider.search_link_idx = spider_conn_first_link_idx(
      thd, share->link_statuses, share->access_balances, spider.conn_link_idx,
      share->link_count, SPIDER_LINK_STATUS_OK);
  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; roop_count++) {
    if (spider_bit_is_set(share->dbton_bitmap, roop_count) &&
        spider_dbton[roop_count].create_db_handler) {
      if (!(dbton_hdl[roop_count] = spider_dbton[roop_count].create_db_handler(
                &spider, share->dbton_share[roop_count])))
        break;
      if (dbton_hdl[roop_count]->init()) break;
    }
  }
  if (roop_count < SPIDER_DBTON_SIZE) {
    DBUG_PRINT("info", ("spider handler init error"));
    for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; --roop_count) {
      if (spider_bit_is_set(share->dbton_bitmap, roop_count) &&
          dbton_hdl[roop_count]) {
        delete dbton_hdl[roop_count];
        dbton_hdl[roop_count] = NULL;
      }
    }
    spider_free_trx(trx, TRUE);
    delete thd;
    share->bg_sts_thd_wait = FALSE;
    share->bg_sts_kill = FALSE;
    share->bg_sts_init = FALSE;
    pthread_mutex_unlock(&share->sts_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32) || defined(_MSC_VER)
    my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  /* init end */

  while (TRUE) {
    DBUG_PRINT("info", ("spider bg sts roop start"));
    if (share->bg_sts_kill) {
      DBUG_PRINT("info", ("spider bg sts kill start"));
      for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; --roop_count) {
        if (spider_bit_is_set(share->dbton_bitmap, roop_count) &&
            dbton_hdl[roop_count]) {
          delete dbton_hdl[roop_count];
          dbton_hdl[roop_count] = NULL;
        }
      }
      spider_free_trx(trx, TRUE);
      delete thd;
      pthread_cond_signal(&share->bg_sts_sync_cond);
      pthread_mutex_unlock(&share->sts_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32) || defined(_MSC_VER)
      my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
      my_thread_end();
      my_afree(need_mons);
      DBUG_RETURN(NULL);
    }
    if (spider.search_link_idx < 0) {
      spider_trx_set_link_idx_for_all(&spider);
      /*
            spider.search_link_idx = spider_conn_next_link_idx(
              thd, share->link_statuses, share->access_balances,
              spider.conn_link_idx, spider.search_link_idx, share->link_count,
              SPIDER_LINK_STATUS_OK);
      */
      spider.search_link_idx = spider_conn_first_link_idx(
          thd, share->link_statuses, share->access_balances,
          spider.conn_link_idx, share->link_count, SPIDER_LINK_STATUS_OK);
    }
    if (spider.search_link_idx >= 0) {
      if (difftime(share->bg_sts_try_time, share->sts_get_time) >=
          share->bg_sts_interval) {
        if (!conns[spider.search_link_idx]) {
          spider_get_conn(share, spider.search_link_idx, trx, &spider, FALSE,
                          FALSE, SPIDER_CONN_KIND_MYSQL, &error_num);
          conns[spider.search_link_idx]->error_mode = 0;
          /*
                    if (
                      error_num &&
                      share->monitoring_kind[spider.search_link_idx] &&
                      need_mons[spider.search_link_idx]
                    ) {
                      lex_start(thd);
                      error_num = spider_ping_table_mon_from_table(
                          trx,
                          thd,
                          share,
                          spider.search_link_idx,
                          (uint32)
             share->monitoring_sid[spider.search_link_idx], share->table_name,
                          share->table_name_length,
                          spider.conn_link_idx[spider.search_link_idx],
                          NULL,
                          0,
                          share->monitoring_kind[spider.search_link_idx],
                          share->monitoring_limit[spider.search_link_idx],
                          share->monitoring_flag[spider.search_link_idx],
                          TRUE
                        );
                      lex_end(thd->lex);
                    }
          */
          spider.search_link_idx = -1;
        }
        if (spider.search_link_idx != -1 && conns[spider.search_link_idx]) {
#ifdef WITH_PARTITION_STORAGE_ENGINE
          if (spider_get_sts(
                  share, spider.search_link_idx, share->bg_sts_try_time,
                  &spider, share->bg_sts_interval, share->bg_sts_mode,
                  share->bg_sts_sync, 2, HA_STATUS_CONST | HA_STATUS_VARIABLE))
#else
          if (spider_get_sts(share, spider.search_link_idx,
                             share->bg_sts_try_time, &spider,
                             share->bg_sts_interval, share->bg_sts_mode, 2,
                             HA_STATUS_CONST | HA_STATUS_VARIABLE))
#endif
          {
            /*
                        if (
                          share->monitoring_kind[spider.search_link_idx] &&
                          need_mons[spider.search_link_idx]
                        ) {
                          lex_start(thd);
                          error_num = spider_ping_table_mon_from_table(
                              trx,
                              thd,
                              share,
                              spider.search_link_idx,
                              (uint32)
               share->monitoring_sid[spider.search_link_idx],
                              share->table_name,
                              share->table_name_length,
                              spider.conn_link_idx[spider.search_link_idx],
                              NULL,
                              0,
                              share->monitoring_kind[spider.search_link_idx],
                              share->monitoring_limit[spider.search_link_idx],
                              share->monitoring_flag[spider.search_link_idx],
                              TRUE
                            );
                          lex_end(thd->lex);
                        }
            */
            spider.search_link_idx = -1;
          }
        }
      }
    }
    memset(need_mons, 0, sizeof(int) * share->link_count);
    share->bg_sts_thd_wait = TRUE;
    pthread_cond_wait(&share->bg_sts_cond, &share->sts_mutex);
  }
}

int spider_create_crd_thread(SPIDER_SHARE *share) {
  int error_num;
  DBUG_ENTER("spider_create_crd_thread");
  if (!share->bg_crd_init) {
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&share->bg_crd_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_crd, &share->bg_crd_cond, NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&share->bg_crd_sync_cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_bg_crd_sync, &share->bg_crd_sync_cond,
                        NULL))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_sync_cond_init;
    }
#if MYSQL_VERSION_ID < 50500
    if (pthread_create(&share->bg_crd_thread, &spider_pt_attr,
                       spider_bg_crd_action, (void *)share))
#else
    if (mysql_thread_create(spd_key_thd_bg_crd, &share->bg_crd_thread,
                            &spider_pt_attr, spider_bg_crd_action,
                            (void *)share))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_thread_create;
    }
    share->bg_crd_init = TRUE;
  }
  DBUG_RETURN(0);

error_thread_create:
  pthread_cond_destroy(&share->bg_crd_sync_cond);
error_sync_cond_init:
  pthread_cond_destroy(&share->bg_crd_cond);
error_cond_init:
  DBUG_RETURN(error_num);
}

void spider_free_crd_thread(SPIDER_SHARE *share) {
  DBUG_ENTER("spider_free_crd_thread");
  if (share->bg_crd_init) {
    pthread_mutex_lock(&share->crd_mutex);
    share->bg_crd_kill = TRUE;
    pthread_cond_signal(&share->bg_crd_cond);
    pthread_cond_wait(&share->bg_crd_sync_cond, &share->crd_mutex);
    pthread_mutex_unlock(&share->crd_mutex);
    pthread_join(share->bg_crd_thread, NULL);
    pthread_cond_destroy(&share->bg_crd_sync_cond);
    pthread_cond_destroy(&share->bg_crd_cond);
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
  }
  DBUG_VOID_RETURN;
}

void *spider_bg_crd_action(void *arg) {
  SPIDER_SHARE *share = (SPIDER_SHARE *)arg;
  SPIDER_TRX *trx;
  int error_num = 0, roop_count;
  ha_spider spider;
  TABLE table;
  int *need_mons;
  SPIDER_CONN **conns;
  uint *conn_link_idx;
  uchar *conn_can_fo;
  char **conn_keys;
  spider_db_handler **dbton_hdl;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_bg_crd_action");
  /* init start */
  char *ptr;
  ptr = (char *)my_alloca((sizeof(int) * share->link_count) +
                          (sizeof(SPIDER_CONN *) * share->link_count) +
                          (sizeof(uint) * share->link_count) +
                          (sizeof(uchar) * share->link_bitmap_size) +
                          (sizeof(char *) * share->link_count) +
                          (sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE));
  if (!ptr) {
    pthread_mutex_lock(&share->crd_mutex);
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
    pthread_mutex_unlock(&share->crd_mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  need_mons = (int *)ptr;
  ptr += (sizeof(int) * share->link_count);
  conns = (SPIDER_CONN **)ptr;
  ptr += (sizeof(SPIDER_CONN *) * share->link_count);
  conn_link_idx = (uint *)ptr;
  ptr += (sizeof(uint) * share->link_count);
  conn_can_fo = (uchar *)ptr;
  ptr += (sizeof(uchar) * share->link_bitmap_size);
  conn_keys = (char **)ptr;
  ptr += (sizeof(char *) * share->link_count);
  dbton_hdl = (spider_db_handler **)ptr;
  pthread_mutex_lock(&share->crd_mutex);
  if (!(thd = SPIDER_new_THD(next_thread_id()))) {
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
    pthread_mutex_unlock(&share->crd_mutex);
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd->thread_stack = (char *)&thd;
  thd->store_globals();
  if (!(trx = spider_get_trx(thd, FALSE, &error_num))) {
    delete thd;
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
    pthread_mutex_unlock(&share->crd_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32) || defined(_MSC_VER)
    my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  share->bg_crd_thd = thd;
  table.s = share->table_share;
  table.field = share->table_share->field;
  table.key_info = share->table_share->key_info;
  spider.trx = trx;
  spider.change_table_ptr(&table, share->table_share);
  spider.share = share;
  spider.conns = conns;
  spider.conn_link_idx = conn_link_idx;
  spider.conn_can_fo = conn_can_fo;
  spider.need_mons = need_mons;
  spider.dbton_handler = dbton_hdl;
  memset(conns, 0, sizeof(SPIDER_CONN *) * share->link_count);
  memset(need_mons, 0, sizeof(int) * share->link_count);
  memset(dbton_hdl, 0, sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE);
  spider_trx_set_link_idx_for_all(&spider);
  spider.search_link_idx = spider_conn_first_link_idx(
      thd, share->link_statuses, share->access_balances, spider.conn_link_idx,
      share->link_count, SPIDER_LINK_STATUS_OK);
  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; roop_count++) {
    if (spider_bit_is_set(share->dbton_bitmap, roop_count) &&
        spider_dbton[roop_count].create_db_handler) {
      if (!(dbton_hdl[roop_count] = spider_dbton[roop_count].create_db_handler(
                &spider, share->dbton_share[roop_count])))
        break;
      if (dbton_hdl[roop_count]->init()) break;
    }
  }
  if (roop_count < SPIDER_DBTON_SIZE) {
    DBUG_PRINT("info", ("spider handler init error"));
    for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; --roop_count) {
      if (spider_bit_is_set(share->dbton_bitmap, roop_count) &&
          dbton_hdl[roop_count]) {
        delete dbton_hdl[roop_count];
        dbton_hdl[roop_count] = NULL;
      }
    }
    spider_free_trx(trx, TRUE);
    delete thd;
    share->bg_crd_thd_wait = FALSE;
    share->bg_crd_kill = FALSE;
    share->bg_crd_init = FALSE;
    pthread_mutex_unlock(&share->crd_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32) || defined(_MSC_VER)
    my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
    my_thread_end();
    my_afree(need_mons);
    DBUG_RETURN(NULL);
  }
  /* init end */

  while (TRUE) {
    DBUG_PRINT("info", ("spider bg crd roop start"));
    if (share->bg_crd_kill) {
      DBUG_PRINT("info", ("spider bg crd kill start"));
      for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; --roop_count) {
        if (spider_bit_is_set(share->dbton_bitmap, roop_count) &&
            dbton_hdl[roop_count]) {
          delete dbton_hdl[roop_count];
          dbton_hdl[roop_count] = NULL;
        }
      }
      spider_free_trx(trx, TRUE);
      delete thd;
      pthread_cond_signal(&share->bg_crd_sync_cond);
      pthread_mutex_unlock(&share->crd_mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32) || defined(_MSC_VER)
      my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
      my_thread_end();
      my_afree(need_mons);
      DBUG_RETURN(NULL);
    }
    if (spider.search_link_idx < 0) {
      spider_trx_set_link_idx_for_all(&spider);
      /*
            spider.search_link_idx = spider_conn_next_link_idx(
              thd, share->link_statuses, share->access_balances,
              spider.conn_link_idx, spider.search_link_idx, share->link_count,
              SPIDER_LINK_STATUS_OK);
      */
      spider.search_link_idx = spider_conn_first_link_idx(
          thd, share->link_statuses, share->access_balances,
          spider.conn_link_idx, share->link_count, SPIDER_LINK_STATUS_OK);
    }
    if (spider.search_link_idx >= 0) {
      if (difftime(share->bg_crd_try_time, share->crd_get_time) >=
          share->bg_crd_interval) {
        if (!conns[spider.search_link_idx]) {
          spider_get_conn(share, spider.search_link_idx, trx, &spider, FALSE,
                          FALSE, SPIDER_CONN_KIND_MYSQL, &error_num);
          conns[spider.search_link_idx]->error_mode = 0;
          /*
                    if (
                      error_num &&
                      share->monitoring_kind[spider.search_link_idx] &&
                      need_mons[spider.search_link_idx]
                    ) {
                      lex_start(thd);
                      error_num = spider_ping_table_mon_from_table(
                          trx,
                          thd,
                          share,
                          spider.search_link_idx,
                          (uint32)
             share->monitoring_sid[spider.search_link_idx], share->table_name,
                          share->table_name_length,
                          spider.conn_link_idx[spider.search_link_idx],
                          NULL,
                          0,
                          share->monitoring_kind[spider.search_link_idx],
                          share->monitoring_limit[spider.search_link_idx],
                          share->monitoring_flag[spider.search_link_idx],
                          TRUE
                        );
                      lex_end(thd->lex);
                    }
          */
          spider.search_link_idx = -1;
        }
        if (spider.search_link_idx != -1 && conns[spider.search_link_idx]) {
#ifdef WITH_PARTITION_STORAGE_ENGINE
          if (spider_get_crd(share, spider.search_link_idx,
                             share->bg_crd_try_time, &spider, &table,
                             share->bg_crd_interval, share->bg_crd_mode,
                             share->bg_crd_sync, 2))
#else
          if (spider_get_crd(share, spider.search_link_idx,
                             share->bg_crd_try_time, &spider, &table,
                             share->bg_crd_interval, share->bg_crd_mode, 2))
#endif
          {
            /*
                        if (
                          share->monitoring_kind[spider.search_link_idx] &&
                          need_mons[spider.search_link_idx]
                        ) {
                          lex_start(thd);
                          error_num = spider_ping_table_mon_from_table(
                              trx,
                              thd,
                              share,
                              spider.search_link_idx,
                              (uint32)
               share->monitoring_sid[spider.search_link_idx],
                              share->table_name,
                              share->table_name_length,
                              spider.conn_link_idx[spider.search_link_idx],
                              NULL,
                              0,
                              share->monitoring_kind[spider.search_link_idx],
                              share->monitoring_limit[spider.search_link_idx],
                              share->monitoring_flag[spider.search_link_idx],
                              TRUE
                            );
                          lex_end(thd->lex);
                        }
            */
            spider.search_link_idx = -1;
          }
        }
      }
    }
    memset(need_mons, 0, sizeof(int) * share->link_count);
    share->bg_crd_thd_wait = TRUE;
    pthread_cond_wait(&share->bg_crd_cond, &share->crd_mutex);
  }
}

int spider_create_mon_threads(SPIDER_TRX *trx, SPIDER_SHARE *share) {
  bool create_bg_mons = FALSE;
  int error_num, roop_count, roop_count2;
  SPIDER_LINK_PACK link_pack;
  SPIDER_TABLE_MON_LIST *table_mon_list;
  DBUG_ENTER("spider_create_mon_threads");
  if (!share->bg_mon_init) {
    for (roop_count = 0; roop_count < (int)share->all_link_count;
         roop_count++) {
      if (share->monitoring_bg_kind[roop_count]) {
        create_bg_mons = TRUE;
        break;
      }
    }
    if (create_bg_mons) {
      char link_idx_str[SPIDER_SQL_INT_LEN];
      int link_idx_str_length;
      char *buf =
          (char *)my_alloca(share->table_name_length + SPIDER_SQL_INT_LEN + 1);
      spider_string conv_name_str(
          buf, share->table_name_length + SPIDER_SQL_INT_LEN + 1,
          system_charset_info);
      conv_name_str.init_calc_mem(105);
      conv_name_str.length(0);
      conv_name_str.q_append(share->table_name, share->table_name_length);
      for (roop_count = 0; roop_count < (int)share->all_link_count;
           roop_count++) {
        if (share->monitoring_bg_kind[roop_count]) {
          conv_name_str.length(share->table_name_length);
          if (share->static_link_ids[roop_count]) {
            memcpy(link_idx_str, share->static_link_ids[roop_count],
                   share->static_link_ids_lengths[roop_count] + 1);
            link_idx_str_length = share->static_link_ids_lengths[roop_count];
          } else {
            link_idx_str_length =
                my_sprintf(link_idx_str, (link_idx_str, "%010d", roop_count));
          }
          conv_name_str.q_append(link_idx_str, link_idx_str_length + 1);
          conv_name_str.length(conv_name_str.length() - 1);
          if (!(table_mon_list = spider_get_ping_table_mon_list(
                    trx, trx->thd, &conv_name_str, share->table_name_length,
                    roop_count, share->static_link_ids[roop_count],
                    share->static_link_ids_lengths[roop_count],
                    (uint32)share->monitoring_sid[roop_count], FALSE,
                    &error_num))) {
            my_afree(buf);
            goto error_get_ping_table_mon_list;
          }
          spider_free_ping_table_mon_list(table_mon_list);
        }
      }
      if (!(share->bg_mon_thds = (THD **)spider_bulk_malloc(
                spider_current_trx, 23, MYF(MY_WME | MY_ZEROFILL),
                &share->bg_mon_thds, sizeof(THD *) * share->all_link_count,
                &share->bg_mon_threads,
                sizeof(pthread_t) * share->all_link_count,
                &share->bg_mon_mutexes,
                sizeof(pthread_mutex_t) * share->all_link_count,
                &share->bg_mon_conds,
                sizeof(pthread_cond_t) * share->all_link_count,
                &share->bg_mon_sleep_conds,
                sizeof(pthread_cond_t) * share->all_link_count, NullS))) {
        error_num = HA_ERR_OUT_OF_MEM;
        my_afree(buf);
        goto error_alloc_base;
      }
      for (roop_count = 0; roop_count < (int)share->all_link_count;
           roop_count++) {
        if (share->monitoring_bg_kind[roop_count] &&
#if MYSQL_VERSION_ID < 50500
            pthread_mutex_init(&share->bg_mon_mutexes[roop_count],
                               MY_MUTEX_INIT_FAST)
#else
            mysql_mutex_init(spd_key_mutex_bg_mon,
                             &share->bg_mon_mutexes[roop_count],
                             MY_MUTEX_INIT_FAST)
#endif
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          my_afree(buf);
          goto error_mutex_init;
        }
      }
      for (roop_count = 0; roop_count < (int)share->all_link_count;
           roop_count++) {
        if (share->monitoring_bg_kind[roop_count] &&
#if MYSQL_VERSION_ID < 50500
            pthread_cond_init(&share->bg_mon_conds[roop_count], NULL)
#else
            mysql_cond_init(spd_key_cond_bg_mon,
                            &share->bg_mon_conds[roop_count], NULL)
#endif
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          my_afree(buf);
          goto error_cond_init;
        }
      }
      for (roop_count = 0; roop_count < (int)share->all_link_count;
           roop_count++) {
        if (share->monitoring_bg_kind[roop_count] &&
#if MYSQL_VERSION_ID < 50500
            pthread_cond_init(&share->bg_mon_sleep_conds[roop_count], NULL)
#else
            mysql_cond_init(spd_key_cond_bg_mon_sleep,
                            &share->bg_mon_sleep_conds[roop_count], NULL)
#endif
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          my_afree(buf);
          goto error_sleep_cond_init;
        }
      }
      link_pack.share = share;
      for (roop_count = 0; roop_count < (int)share->all_link_count;
           roop_count++) {
        if (share->monitoring_bg_kind[roop_count]) {
          link_pack.link_idx = roop_count;
          pthread_mutex_lock(&share->bg_mon_mutexes[roop_count]);
#if MYSQL_VERSION_ID < 50500
          if (pthread_create(&share->bg_mon_threads[roop_count],
                             &spider_pt_attr, spider_bg_mon_action,
                             (void *)&link_pack))
#else
          if (mysql_thread_create(
                  spd_key_thd_bg_mon, &share->bg_mon_threads[roop_count],
                  &spider_pt_attr, spider_bg_mon_action, (void *)&link_pack))
#endif
          {
            error_num = HA_ERR_OUT_OF_MEM;
            my_afree(buf);
            goto error_thread_create;
          }
          pthread_cond_wait(&share->bg_mon_conds[roop_count],
                            &share->bg_mon_mutexes[roop_count]);
          pthread_mutex_unlock(&share->bg_mon_mutexes[roop_count]);
        }
      }
      share->bg_mon_init = TRUE;
      my_afree(buf);
    }
  }
  DBUG_RETURN(0);

error_thread_create:
  roop_count2 = roop_count;
  for (roop_count--; roop_count >= 0; roop_count--) {
    if (share->monitoring_bg_kind[roop_count])
      pthread_mutex_lock(&share->bg_mon_mutexes[roop_count]);
  }
  share->bg_mon_kill = TRUE;
  for (roop_count = roop_count2 - 1; roop_count >= 0; roop_count--) {
    if (share->monitoring_bg_kind[roop_count]) {
      pthread_cond_wait(&share->bg_mon_conds[roop_count],
                        &share->bg_mon_mutexes[roop_count]);
      pthread_mutex_unlock(&share->bg_mon_mutexes[roop_count]);
    }
  }
  share->bg_mon_kill = FALSE;
  roop_count = share->all_link_count;
error_sleep_cond_init:
  for (roop_count--; roop_count >= 0; roop_count--) {
    if (share->monitoring_bg_kind[roop_count])
      pthread_cond_destroy(&share->bg_mon_sleep_conds[roop_count]);
  }
  roop_count = share->all_link_count;
error_cond_init:
  for (roop_count--; roop_count >= 0; roop_count--) {
    if (share->monitoring_bg_kind[roop_count])
      pthread_cond_destroy(&share->bg_mon_conds[roop_count]);
  }
  roop_count = share->all_link_count;
error_mutex_init:
  for (roop_count--; roop_count >= 0; roop_count--) {
    if (share->monitoring_bg_kind[roop_count])
      pthread_mutex_destroy(&share->bg_mon_mutexes[roop_count]);
  }
  spider_free(spider_current_trx, share->bg_mon_thds, MYF(0));
error_alloc_base:
error_get_ping_table_mon_list:
  DBUG_RETURN(error_num);
}

void spider_free_mon_threads(SPIDER_SHARE *share) {
  int roop_count;
  DBUG_ENTER("spider_free_mon_threads");
  if (share->bg_mon_init) {
    for (roop_count = 0; roop_count < (int)share->all_link_count;
         roop_count++) {
      if (share->monitoring_bg_kind[roop_count] &&
          share->bg_mon_thds[roop_count]) {
        share->bg_mon_thds[roop_count]->killed = SPIDER_THD_KILL_CONNECTION;
      }
    }
    for (roop_count = 0; roop_count < (int)share->all_link_count;
         roop_count++) {
      if (share->monitoring_bg_kind[roop_count])
        pthread_mutex_lock(&share->bg_mon_mutexes[roop_count]);
    }
    share->bg_mon_kill = TRUE;
    for (roop_count = 0; roop_count < (int)share->all_link_count;
         roop_count++) {
      if (share->monitoring_bg_kind[roop_count]) {
        pthread_cond_signal(&share->bg_mon_sleep_conds[roop_count]);
        pthread_cond_wait(&share->bg_mon_conds[roop_count],
                          &share->bg_mon_mutexes[roop_count]);
        pthread_mutex_unlock(&share->bg_mon_mutexes[roop_count]);
        pthread_join(share->bg_mon_threads[roop_count], NULL);
        pthread_cond_destroy(&share->bg_mon_conds[roop_count]);
        pthread_cond_destroy(&share->bg_mon_sleep_conds[roop_count]);
        pthread_mutex_destroy(&share->bg_mon_mutexes[roop_count]);
      }
    }
    spider_free(spider_current_trx, share->bg_mon_thds, MYF(0));
    share->bg_mon_kill = FALSE;
    share->bg_mon_init = FALSE;
  }
  DBUG_VOID_RETURN;
}

void *spider_bg_mon_action(void *arg) {
  SPIDER_LINK_PACK *link_pack = (SPIDER_LINK_PACK *)arg;
  SPIDER_SHARE *share = link_pack->share;
  SPIDER_TRX *trx;
  int error_num, link_idx = link_pack->link_idx;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_bg_mon_action");
  /* init start */
  pthread_mutex_lock(&share->bg_mon_mutexes[link_idx]);
  if (!(thd = SPIDER_new_THD(next_thread_id()))) {
    share->bg_mon_kill = FALSE;
    share->bg_mon_init = FALSE;
    pthread_cond_signal(&share->bg_mon_conds[link_idx]);
    pthread_mutex_unlock(&share->bg_mon_mutexes[link_idx]);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd->thread_stack = (char *)&thd;
  thd->store_globals();
  if (!(trx = spider_get_trx(thd, FALSE, &error_num))) {
    delete thd;
    share->bg_mon_kill = FALSE;
    share->bg_mon_init = FALSE;
    pthread_cond_signal(&share->bg_mon_conds[link_idx]);
    pthread_mutex_unlock(&share->bg_mon_mutexes[link_idx]);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32) || defined(_MSC_VER)
    my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  share->bg_mon_thds[link_idx] = thd;
  pthread_cond_signal(&share->bg_mon_conds[link_idx]);
  /*
    pthread_mutex_unlock(&share->bg_mon_mutexes[link_idx]);
  */
  /* init end */

  while (TRUE) {
    DBUG_PRINT("info", ("spider bg mon sleep %lld",
                        share->monitoring_bg_interval[link_idx]));
    if (!share->bg_mon_kill) {
      struct timespec abstime;
      set_timespec_nsec(abstime,
                        share->monitoring_bg_interval[link_idx] * 1000);
      pthread_cond_timedwait(&share->bg_mon_sleep_conds[link_idx],
                             &share->bg_mon_mutexes[link_idx], &abstime);
      /*
            my_sleep((ulong) share->monitoring_bg_interval[link_idx]);
      */
    }
    DBUG_PRINT("info", ("spider bg mon roop start"));
    if (share->bg_mon_kill) {
      DBUG_PRINT("info", ("spider bg mon kill start"));
      /*
            pthread_mutex_lock(&share->bg_mon_mutexes[link_idx]);
      */
      pthread_cond_signal(&share->bg_mon_conds[link_idx]);
      pthread_mutex_unlock(&share->bg_mon_mutexes[link_idx]);
      spider_free_trx(trx, TRUE);
      delete thd;
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32) || defined(_MSC_VER)
      my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
      my_thread_end();
      DBUG_RETURN(NULL);
    }
    if (share->monitoring_bg_kind[link_idx]) {
      lex_start(thd);
      error_num = spider_ping_table_mon_from_table(
          trx, thd, share, link_idx, (uint32)share->monitoring_sid[link_idx],
          share->table_name, share->table_name_length, link_idx, NULL, 0,
          share->monitoring_bg_kind[link_idx],
          share->monitoring_limit[link_idx],
          share->monitoring_bg_flag[link_idx], TRUE);
      lex_end(thd->lex);
    }
  }
}

#ifndef SPIDER_DISABLE_LINK
int spider_conn_first_link_idx(THD *thd, long *link_statuses,
                               long *access_balances, uint *conn_link_idx,
                               int link_count, int link_status) {
  int roop_count, active_links = 0;
  longlong balance_total = 0, balance_val;
  double rand_val;
  int *link_idxs, link_idx;
  long *balances;
  DBUG_ENTER("spider_conn_first_link_idx");
  char *ptr;
  ptr = (char *)my_alloca((sizeof(int) * link_count) +
                          (sizeof(long) * link_count));
  if (!ptr) {
    DBUG_PRINT("info", ("spider out of memory"));
    DBUG_RETURN(-2);
  }
  link_idxs = (int *)ptr;
  ptr += sizeof(int) * link_count;
  balances = (long *)ptr;
  for (roop_count = 0; roop_count < link_count; roop_count++) {
    DBUG_ASSERT((conn_link_idx[roop_count] - roop_count) % link_count == 0);
    if (link_statuses[conn_link_idx[roop_count]] <= link_status) {
      link_idxs[active_links] = roop_count;
      balances[active_links] = access_balances[roop_count];
      balance_total += access_balances[roop_count];
      active_links++;
    }
  }

  if (active_links == 0) {
    DBUG_PRINT("info", ("spider all links are failed"));
    my_afree(link_idxs);
    DBUG_RETURN(-1);
  }
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100002
  DBUG_PRINT("info", ("spider server_id=%lu", thd->variables.server_id));
#else
  DBUG_PRINT("info", ("spider server_id=%u", thd->server_id));
#endif
  DBUG_PRINT("info", ("spider thread_id=%lu", thd_get_thread_id(thd)));
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100002
  rand_val = spider_rand(thd->variables.server_id + thd_get_thread_id(thd));
#else
  rand_val = spider_rand(thd->server_id + thd_get_thread_id(thd));
#endif
  DBUG_PRINT("info", ("spider rand_val=%f", rand_val));
  balance_val = (longlong)(rand_val * balance_total);
  DBUG_PRINT("info", ("spider balance_val=%lld", balance_val));
  for (roop_count = 0; roop_count < active_links - 1; roop_count++) {
    DBUG_PRINT("info",
               ("spider balances[%d]=%ld", roop_count, balances[roop_count]));
    if (balance_val < balances[roop_count]) break;
    balance_val -= balances[roop_count];
  }

  DBUG_PRINT("info", ("spider first link_idx=%d", link_idxs[roop_count]));
  link_idx = link_idxs[roop_count];
  my_afree(link_idxs);
  DBUG_RETURN(link_idx);
}

int spider_conn_next_link_idx(THD *thd, long *link_statuses,
                              long *access_balances, uint *conn_link_idx,
                              int link_idx, int link_count, int link_status) {
  int tmp_link_idx;
  DBUG_ENTER("spider_conn_next_link_idx");
  DBUG_ASSERT((conn_link_idx[link_idx] - link_idx) % link_count == 0);
  tmp_link_idx =
      spider_conn_first_link_idx(thd, link_statuses, access_balances,
                                 conn_link_idx, link_count, link_status);
  if (tmp_link_idx >= 0 && tmp_link_idx == link_idx) {
    do {
      tmp_link_idx++;
      if (tmp_link_idx >= link_count) tmp_link_idx = 0;
      if (tmp_link_idx == link_idx) break;
    } while (link_statuses[conn_link_idx[tmp_link_idx]] > link_status);
    DBUG_PRINT("info", ("spider next link_idx=%d", tmp_link_idx));
    DBUG_RETURN(tmp_link_idx);
  }
  DBUG_PRINT("info", ("spider next link_idx=%d", tmp_link_idx));
  DBUG_RETURN(tmp_link_idx);
}

int spider_conn_link_idx_next(long *link_statuses, uint *conn_link_idx,
                              int link_idx, int link_count, int link_status) {
  DBUG_ENTER("spider_conn_link_idx_next");
  do {
    link_idx++;
    if (link_idx >= link_count) break;
    DBUG_ASSERT((conn_link_idx[link_idx] - link_idx) % link_count == 0);
  } while (link_statuses[conn_link_idx[link_idx]] > link_status);
  DBUG_PRINT("info", ("spider link_idx=%d", link_idx));
  DBUG_RETURN(link_idx);
}
#endif

int spider_conn_get_link_status(long *link_statuses, uint *conn_link_idx,
                                int link_idx) {
  DBUG_ENTER("spider_conn_get_link_status");
  DBUG_PRINT("info", ("spider link_status=%d",
                      (int)link_statuses[conn_link_idx[link_idx]]));
  DBUG_RETURN((int)link_statuses[conn_link_idx[link_idx]]);
}

int spider_conn_lock_mode(ha_spider *spider) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_conn_lock_mode");
  if (result_list->lock_type == F_WRLCK || spider->lock_mode == 2)
    DBUG_RETURN(SPIDER_LOCK_MODE_EXCLUSIVE);
  else if (spider->lock_mode == 1)
    DBUG_RETURN(SPIDER_LOCK_MODE_SHARED);
  DBUG_RETURN(SPIDER_LOCK_MODE_NO_LOCK);
}

bool spider_conn_check_recovery_link(SPIDER_SHARE *share) {
  int roop_count;
  DBUG_ENTER("spider_check_recovery_link");
  for (roop_count = 0; roop_count < (int)share->link_count; roop_count++) {
    if (share->link_statuses[roop_count] == SPIDER_LINK_STATUS_RECOVERY)
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

bool spider_conn_use_handler(ha_spider *spider, int lock_mode, int link_idx) {
  THD *thd = spider->trx->thd;
  int use_handler =
      spider_param_use_handler(thd, spider->share->use_handlers[link_idx]);
  DBUG_ENTER("spider_conn_use_handler");
  DBUG_PRINT("info", ("spider use_handler=%d", use_handler));
  DBUG_PRINT("info", ("spider spider->conn_kind[link_idx]=%u",
                      spider->conn_kind[link_idx]));
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  if (spider->do_direct_update) {
    spider->sql_kinds |= SPIDER_SQL_KIND_SQL;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_SQL;
    spider->direct_update_kinds |= SPIDER_SQL_KIND_SQL;
    DBUG_PRINT("info", ("spider FALSE by using direct_update"));
    DBUG_RETURN(FALSE);
  }
#endif
  if (spider->use_spatial_index) {
    DBUG_PRINT("info", ("spider FALSE by use_spatial_index"));
    spider->sql_kinds |= SPIDER_SQL_KIND_SQL;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_SQL;
    DBUG_RETURN(FALSE);
  }
  uint dbton_id;
  spider_db_handler *dbton_hdl;
  dbton_id = spider->share->sql_dbton_ids[spider->conn_link_idx[link_idx]];
  dbton_hdl = spider->dbton_handler[dbton_id];
  if (!dbton_hdl->support_use_handler(use_handler)) {
    DBUG_PRINT("info", ("spider FALSE by dbton"));
    spider->sql_kinds |= SPIDER_SQL_KIND_SQL;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_SQL;
    DBUG_RETURN(FALSE);
  }
  if (spider->sql_command == SQLCOM_HA_READ &&
      (!(use_handler & 2) || (spider_param_sync_trx_isolation(thd) &&
                              thd_tx_isolation(thd) == ISO_SERIALIZABLE))) {
    DBUG_PRINT("info", ("spider TRUE by HA"));
    spider->sql_kinds |= SPIDER_SQL_KIND_HANDLER;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_HANDLER;
    DBUG_RETURN(TRUE);
  }
  if (spider->sql_command != SQLCOM_HA_READ &&
      lock_mode == SPIDER_LOCK_MODE_NO_LOCK &&
      spider_param_sync_trx_isolation(thd) &&
      thd_tx_isolation(thd) != ISO_SERIALIZABLE && (use_handler & 1)) {
    DBUG_PRINT("info", ("spider TRUE by PARAM"));
    spider->sql_kinds |= SPIDER_SQL_KIND_HANDLER;
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_HANDLER;
    DBUG_RETURN(TRUE);
  }
  spider->sql_kinds |= SPIDER_SQL_KIND_SQL;
  spider->sql_kind[link_idx] = SPIDER_SQL_KIND_SQL;
  DBUG_RETURN(FALSE);
}

bool spider_conn_need_open_handler(ha_spider *spider, uint idx, int link_idx) {
  DBUG_ENTER("spider_conn_need_open_handler");
  DBUG_PRINT("info", ("spider spider=%p", spider));
  if (spider->handler_opened(link_idx, spider->conn_kind[link_idx])) {
    DBUG_PRINT("info", ("spider HA already opened"));
    DBUG_RETURN(FALSE);
  }
  DBUG_RETURN(TRUE);
}

SPIDER_CONN *spider_get_conn_from_idle_connection(
    SPIDER_SHARE *share, int link_idx, char *conn_key, ha_spider *spider,
    uint conn_kind, int base_link_idx, int *error_num) {
  DBUG_ENTER("spider_get_conn_from_idle_connection");
  THD *thd = current_thd;
  SPIDER_IP_PORT_CONN *ip_port_conn;
  SPIDER_CONN *conn = NULL;
  uint spider_max_connections = spider_param_max_connections();
  struct timespec abstime;
  ulonglong start, inter_val = 0;
  longlong last_ntime = 0;
  ulonglong wait_time = (ulonglong)spider_param_conn_wait_timeout() * 1000 *
                        1000 * 1000;  // default 20s

  unsigned long ip_port_count = 0;  // init 0

  set_timespec(abstime, 0);

  pthread_mutex_lock(&spider_ipport_conn_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if ((ip_port_conn = (SPIDER_IP_PORT_CONN *)my_hash_search_using_hash_value(
           &spider_ipport_conns, share->conn_keys_hash_value[link_idx],
           (uchar *)share->conn_keys[link_idx],
           share->conn_keys_lengths[link_idx])))
#else
  if ((ip_port_conn = (SPIDER_IP_PORT_CONN *)my_hash_search(
           &spider_ipport_conns, (uchar *)share->conn_keys[link_idx],
           share->conn_keys_lengths[link_idx])))
#endif
  { /* exists */
    pthread_mutex_unlock(&spider_ipport_conn_mutex);
    pthread_mutex_lock(&ip_port_conn->mutex);
    ip_port_count = ip_port_conn->ip_port_count;
  } else {
    pthread_mutex_unlock(&spider_ipport_conn_mutex);
  }

  if (ip_port_conn && ip_port_count >= spider_max_connections &&
      spider_max_connections >
          0) { /* no idle conn && enable connection pool, wait */
    pthread_mutex_unlock(&ip_port_conn->mutex);
    start = my_hrtime().val;
    while (1) {
      int error;
      inter_val = my_hrtime().val - start;        // us
      last_ntime = wait_time - inter_val * 1000;  // *1000, to ns
      if (last_ntime <= 0) {                      /* wait timeout */
        *error_num = ER_SPIDER_CON_COUNT_ERROR;
        DBUG_RETURN(NULL);
      }
      set_timespec_nsec(abstime, last_ntime);
      pthread_mutex_lock(&ip_port_conn->mutex);
      ++ip_port_conn->waiting_count;
      error = pthread_cond_timedwait(&ip_port_conn->cond, &ip_port_conn->mutex,
                                     &abstime);
      --ip_port_conn->waiting_count;
      pthread_mutex_unlock(&ip_port_conn->mutex);
      if (error == ETIMEDOUT || error == ETIME || error != 0) {
        *error_num = ER_SPIDER_CON_COUNT_ERROR;
        DBUG_RETURN(NULL);
      }

      // pthread_mutex_lock(&spider_conn_mutex);
      if ((conn = spd_connect_pools.get_conn(
               share->conn_keys_hash_value[link_idx],
               (uchar *)share->conn_keys[link_idx],
               share->conn_keys_lengths[link_idx])))
      {
        /* get conn from spider_open_connections, then delete conn in
         * spider_open_connections */
        // my_hash_delete(&spider_open_connections, (uchar *)conn);
        // pthread_mutex_unlock(&spider_conn_mutex);
        DBUG_PRINT("info", ("spider get global conn"));
        if (spider) {
          spider->conns[base_link_idx] = conn;
          if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
            conn->use_for_active_standby = TRUE;
        }
        DBUG_RETURN(conn);
      } else {
        // pthread_mutex_unlock(&spider_conn_mutex);
      }
    }
  } else { /* create conn */
    if (ip_port_conn) pthread_mutex_unlock(&ip_port_conn->mutex);
    DBUG_PRINT("info", ("spider create new conn"));
    if (!(conn = spider_create_conn(share, spider, link_idx, base_link_idx,
                                    conn_kind, error_num)))
      DBUG_RETURN(conn);
    *conn->conn_key = *conn_key;
    if (spider) {
      spider->conns[base_link_idx] = conn;
      if (spider_bit_is_set(spider->conn_can_fo, base_link_idx))
        conn->use_for_active_standby = TRUE;
    }
  }

  DBUG_RETURN(conn);
}

SPIDER_IP_PORT_CONN *spider_create_ipport_conn(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_create_ipport_conn");
  if (conn) {
    SPIDER_IP_PORT_CONN *ret =
        (SPIDER_IP_PORT_CONN *)my_malloc(sizeof(*ret), MY_ZEROFILL | MY_WME);
    if (!ret) {
      goto err_return_direct;
    }

#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&ret->mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_conn_i, &ret->mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      // error
      goto err_malloc_key;
    }

#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&ret->cond, NULL))
#else
    if (mysql_cond_init(spd_key_cond_conn_i, &ret->cond, NULL))
#endif
    {
      pthread_mutex_destroy(&ret->mutex);
      goto err_malloc_key;
      // error
    }

    ret->key_len = conn->conn_key_length;
    if (ret->key_len <= 0) {
      pthread_cond_destroy(&ret->cond);
      pthread_mutex_destroy(&ret->mutex);
      goto err_malloc_key;
    }

    ret->key = (char *)my_malloc(ret->key_len, MY_ZEROFILL | MY_WME);
    if (!ret->key) {
      pthread_cond_destroy(&ret->cond);
      pthread_mutex_destroy(&ret->mutex);
      goto err_malloc_key;
    }

    memcpy(ret->key, conn->conn_key, ret->key_len);

    strncpy(ret->remote_ip_str, conn->tgt_host, sizeof(ret->remote_ip_str));
    ret->remote_port = conn->tgt_port;
    ret->conn_id = conn->conn_id;
    ret->ip_port_count = 1;  // init
    ret->waiting_count = 0;

#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    ret->key_hash_value = conn->conn_key_hash_value;
#endif
    DBUG_RETURN(ret);
  err_malloc_key:
    spider_my_free(ret, MYF(0));
  err_return_direct:
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN(NULL);
}

void spider_free_ipport_conn(void *info) {
  DBUG_ENTER("spider_free_ipport_conn");
  if (info) {
    SPIDER_IP_PORT_CONN *p = (SPIDER_IP_PORT_CONN *)info;
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->mutex);
    spider_my_free(p->key, MYF(0));
    spider_my_free(p, MYF(0));
  }
  DBUG_VOID_RETURN;
}

static my_bool my_polling_last_visited(uchar *entry, void *data) {
  DBUG_ENTER("my_polling_last_visited");
  SPIDER_CONN *conn = (SPIDER_CONN *)entry;
  delegate_param *param = (delegate_param *)data;
  DYNAMIC_STRING_ARRAY **arr_info = param->arr_info;
  if (conn) {
    time_t time_now = time((time_t *)0);
    if (time_now > 0 && time_now > conn->last_visited &&
        time_now - conn->last_visited >=
            spider_param_idle_conn_recycle_interval()) {
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
      append_dynamic_string_array(arr_info[0],
                                  (char *)&conn->conn_key_hash_value,
                                  sizeof(conn->conn_key_hash_value));
#else
      SPD_OPEN_CONN *soc = param->hash_info;
      my_hash_value_type hash_value = soc->calc_hash(
          conn->conn_key, conn->conn_key_length);
      append_dynamic_string_array(arr_info[0], (char *)&hash_value,
                                  sizeof(hash_value));
#endif
      append_dynamic_string_array(arr_info[1], (char *)conn->conn_key,
                                  conn->conn_key_length);
    }

  }
  return 0;
}

/*
  new my_polling_last_visited() used by SPIDER_CONN_POOL::iterate()
*/
static my_bool poll_last_visited(uchar *entry, void *data) {
  DBUG_ENTER("my_polling_last_visited");
  conn_queue *cq = (conn_queue *)entry;
  void *buffer = NULL;
  bool buffer_malloced = false;
  if (!cq || !cq->q_ptr) return FALSE; /* queue is empty */
  assert(cq->mtx_inited);
  pthread_mutex_lock(&cq->mtx);
  /* this may be the bottleneck, since we do a mutex lock and walk
     through the whole array, memcpy (used by get_dynamic) might also
     account for the overhead
  */
  for (uint i = 0; i < cq->q_ptr->elements; i++) {
    if (!buffer_malloced) {
      buffer = my_malloc(sizeof(SPIDER_CONN **), MY_WME);
      if (!buffer) return TRUE; /* OOM */
      buffer_malloced = true;
    }
    get_dynamic(cq->q_ptr, buffer, i);
    SPIDER_CONN *conn = *(SPIDER_CONN **)buffer;
    delegate_param *param = (delegate_param *)data;
    DYNAMIC_STRING_ARRAY **arr_info = param->arr_info;
    if (conn) {
      time_t time_now = time((time_t *)0);
      if (time_now > 0 && time_now > conn->last_visited &&
          time_now - conn->last_visited >=
              spider_param_idle_conn_recycle_interval()) {
        append_dynamic_string_array(arr_info[0],
                                    (char *)&conn->conn_key_hash_value,
                                    sizeof(conn->conn_key_hash_value));
        append_dynamic_string_array(arr_info[1], (char *)conn->conn_key,
                                    conn->conn_key_length);
      }
    } 
  }
  pthread_mutex_unlock(&cq->mtx);
  if (buffer_malloced) {
    my_free(buffer);
    buffer = NULL;
  }
  return FALSE;
}

static void *spider_conn_recycle_action(void *arg) {
  THD *thd = current_thd;
  DBUG_ENTER("spider_conn_recycle_action");
  DYNAMIC_STRING_ARRAY idle_conn_key_hash_value_arr;
  DYNAMIC_STRING_ARRAY idle_conn_key_arr;

  if (init_dynamic_string_array(&idle_conn_key_hash_value_arr, 64, 64) ||
      init_dynamic_string_array(&idle_conn_key_arr, 64, 64)) {
    DBUG_RETURN(NULL);
  }

  delegate_param param;
  param.hash_info = &spd_connect_pools;
  param.arr_info[0] = &idle_conn_key_hash_value_arr;
  param.arr_info[1] = &idle_conn_key_arr;
  while (conn_rcyc_init) {
    clear_dynamic_string_array(&idle_conn_key_hash_value_arr);
    clear_dynamic_string_array(&idle_conn_key_arr);

    // pthread_mutex_lock(&spider_conn_mutex);
    // my_hash_delegate(&spider_open_connections, my_polling_last_visited, &param);
    // pthread_mutex_unlock(&spider_conn_mutex);
    spd_connect_pools.iterate((my_hash_delegate_func)poll_last_visited, &param);

    for (size_t i = 0; i < idle_conn_key_hash_value_arr.cur_idx; ++i) {
      my_hash_value_type *tmp_ptr = NULL;
      if (get_dynamic_string_array(&idle_conn_key_hash_value_arr,
                                   (char **)&tmp_ptr, NULL, i)) {
        spider_my_err_logging(
            "[ERROR] fill tmp_ptr error by idle_conn_key_hash_value_arr!\n");
        break;
      }

      char *conn_key = NULL;
      size_t conn_key_len;
      my_hash_value_type conn_key_hash_value = *tmp_ptr;
      if (get_dynamic_string_array(&idle_conn_key_arr, &conn_key, &conn_key_len,
                                   i)) {
        spider_my_err_logging(
            "[ERROR] fill conn_key error from idle_conn_key_arr!\n");
        break;
      }
      // pthread_mutex_lock(&spider_conn_mutex);
      SPIDER_CONN *conn = spd_connect_pools.get_conn(conn_key_hash_value, 
          (uchar *)conn_key, conn_key_len); 
      // pthread_mutex_unlock(&spider_conn_mutex);
      if (conn) {
        spider_update_conn_meta_info(conn, SPIDER_CONN_INVALID_STATUS);
        spider_free_conn(conn);
      }
    }

    /* NOTE: In worst case, idle connection would be freed in 1.25 *
     * spider_param_idle_conn_recycle_interval */
    /* sleep(spider_param_idle_conn_recycle_interval() >> 2);  */
    int sleep_sec = (spider_param_idle_conn_recycle_interval() >> 2);
    while (sleep_sec > 0) {
      sleep(2);
      sleep_sec -= 2;
      if (sleep_sec > (spider_param_idle_conn_recycle_interval() >> 2)) {
        sleep_sec = (spider_param_idle_conn_recycle_interval() >> 2);
      }
      if (!conn_rcyc_init) break;
    }
  }

  free_dynamic_string_array(&idle_conn_key_hash_value_arr);
  free_dynamic_string_array(&idle_conn_key_arr);
  DBUG_RETURN(NULL);
}

int spider_create_conn_recycle_thread() {
  int error_num;
  DBUG_ENTER("spider_create_conn_recycle_thread");
  if (conn_rcyc_init) {
    DBUG_RETURN(0);
  }

#if MYSQL_VERSION_ID < 50500
  if (pthread_create(&conn_rcyc_thread, NULL, spider_conn_recycle_action, NULL))
#else
  if (mysql_thread_create(spd_key_thd_conn_rcyc, &conn_rcyc_thread, NULL,
                          spider_conn_recycle_action, NULL))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_thread_create;
  }
  conn_rcyc_init = TRUE;
  DBUG_RETURN(0);

error_thread_create:
  DBUG_RETURN(error_num);
}

void spider_free_conn_recycle_thread() {
  DBUG_ENTER("spider_free_conn_recycle_thread");
  if (conn_rcyc_init) {
    conn_rcyc_init = FALSE;
    pthread_cancel(conn_rcyc_thread);
    pthread_join(conn_rcyc_thread, NULL);
  }
  DBUG_VOID_RETURN;
}

void spider_free_conn_meta(void *meta) {
  DBUG_ENTER("free_spider_conn_meta");
  if (meta) {
    SPIDER_CONN_META_INFO *p = (SPIDER_CONN_META_INFO *)meta;
    my_free(p->key);
    my_free(p);
  }
  DBUG_VOID_RETURN;
}

SPIDER_CONN_META_INFO *spider_create_conn_meta(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_create_conn_meta");
  if (conn) {
    SPIDER_CONN_META_INFO *ret =
        (SPIDER_CONN_META_INFO *)my_malloc(sizeof(*ret), MY_ZEROFILL | MY_WME);
    if (!ret) {
      goto err_return_direct;
    }

    ret->key_len = conn->conn_key_length;
    if (ret->key_len <= 0) {
      goto err_return_direct;
    }

    ret->key = (char *)my_malloc(ret->key_len, MY_ZEROFILL | MY_WME);
    if (!ret->key) {
      goto err_malloc_key;
    }

    memcpy(ret->key, conn->conn_key, ret->key_len);

    strncpy(ret->remote_user_str, conn->tgt_username,
            sizeof(ret->remote_user_str));
    strncpy(ret->remote_ip_str, conn->tgt_host, sizeof(ret->remote_ip_str));
    ret->remote_port = conn->tgt_port;

#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    ret->key_hash_value = conn->conn_key_hash_value;
#endif

    ret->status = SPIDER_CONN_INIT_STATUS;
    ret->conn_id = conn->conn_id;
    ret->alloc_tm = time(NULL);
    ret->reusage_counter = 0;
    DBUG_RETURN(ret);
  err_malloc_key:
    my_free(ret);
  err_return_direct:
    DBUG_RETURN(NULL);
  }

  DBUG_RETURN(NULL);
}

/*
static my_bool
update_alloc_time(SPIDER_CONN_META_INFO *meta)
{
DBUG_ENTER("update_alloc_time");
if (meta && SPIDER_CONN_IS_INVALID(meta)) {
meta->status_str = SPIDER_CONN_META_INIT2_STATUS;
spider_gettime_str(meta->alloc_time_str, SPIDER_CONN_META_BUF_LEN);
DBUG_RETURN(TRUE);
}

DBUG_RETURN(FALSE);
}

my_bool
update_visit_time(SPIDER_CONN_META_INFO *meta)
{
DBUG_ENTER("update_visit_time");
if (meta && !SPIDER_CONN_IS_INVALID(meta)) {
meta->status_str = SPIDER_CONN_META_ACTIVE_STATUS;
spider_gettime_str(meta->last_visit_time_str, SPIDER_CONN_META_BUF_LEN);
DBUG_RETURN(TRUE);
}

DBUG_RETURN(FALSE);
}
*/

my_bool spider_add_conn_meta_info(SPIDER_CONN *conn) {
  DBUG_ENTER("spider_add_conn_meta_info");
  SPIDER_CONN_META_INFO *meta_info;
  pthread_mutex_lock(&spider_conn_meta_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (!(meta_info = (SPIDER_CONN_META_INFO *)my_hash_search_using_hash_value(
            &spider_conn_meta_info, conn->conn_key_hash_value,
            (uchar *)conn->conn_key, conn->conn_key_length)))
#else
  if (!(meta_info = (SPIDER_CONN_META_INFO *)my_hash_search(
            &spider_conn_meta_info, (uchar *)conn->conn_key,
            conn->conn_key_length)))
#endif
  {
    pthread_mutex_unlock(&spider_conn_meta_mutex);
    meta_info = spider_create_conn_meta(conn);
    if (!meta_info) {
      DBUG_RETURN(FALSE);
    }
    pthread_mutex_lock(&spider_conn_meta_mutex);
    if (my_hash_insert(&spider_conn_meta_info, (uchar *)meta_info)) {
      /* insert failed */
      pthread_mutex_unlock(&spider_conn_meta_mutex);
      DBUG_RETURN(FALSE);
    }
    pthread_mutex_unlock(&spider_conn_meta_mutex);
  } else {
    pthread_mutex_unlock(&spider_conn_meta_mutex);
    /* exist already */
    if (SPIDER_CONN_IS_INVALID(meta_info)) {
      meta_info->alloc_tm = time(NULL);
      meta_info->status = SPIDER_CONN_INIT2_STATUS;
      meta_info->reusage_counter = 0;
    } else {
      /* NOTE: deleted from spider_open_connections for in-place usage, not be
       * freed. */
      ++meta_info->reusage_counter;
    }
  }

  DBUG_RETURN(TRUE);
}

void spider_update_conn_meta_info(SPIDER_CONN *conn, uint new_status) {
  DBUG_ENTER("spider_update_conn_meta_info");
  SPIDER_CONN_META_INFO *meta_info;
  pthread_mutex_lock(&spider_conn_meta_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (!(meta_info = (SPIDER_CONN_META_INFO *)my_hash_search_using_hash_value(
            &spider_conn_meta_info, conn->conn_key_hash_value,
            (uchar *)conn->conn_key, conn->conn_key_length)))
#else
  if (!(meta_info = (SPIDER_CONN_META_INFO *)my_hash_search(
            &spider_conn_meta_info, (uchar *)conn->conn_key,
            conn->conn_key_length)))
#endif
  {
    pthread_mutex_unlock(&spider_conn_meta_mutex);
    DBUG_VOID_RETURN;
  } else {
    pthread_mutex_unlock(&spider_conn_meta_mutex);
    /* exist already */
    if (!SPIDER_CONN_IS_INVALID(meta_info)) {
      if (new_status == SPIDER_CONN_ACTIVE_STATUS) {
        meta_info->last_visit_tm = time(NULL);
      } else if (new_status == SPIDER_CONN_INVALID_STATUS) {
        meta_info->free_tm = time(NULL);
      }
      meta_info->status = new_status;
    }
  }

  DBUG_VOID_RETURN;
}

MYSQL *spider_mysql_connect(char *tgt_host, char *tgt_username,
                            char *tgt_password, long tgt_port,
                            char *tgt_socket) {
  MYSQL *db_conn = NULL;
  uint real_connect_option = 0;
  DBUG_ENTER("spider_mysql_connect");

  if (!db_conn) {
    if (!(db_conn = mysql_init(NULL))) DBUG_RETURN(NULL);
  }

  real_connect_option =
      CLIENT_INTERACTIVE | CLIENT_MULTI_STATEMENTS | CLIENT_FOUND_ROWS;

  /* tgt_db not use */
  if (!mysql_real_connect(db_conn, tgt_host, tgt_username, tgt_password, NULL,
                          tgt_port, tgt_socket, real_connect_option)) {
    if (db_conn) {
      mysql_close(db_conn);
      db_conn = NULL;
    }
  }
  DBUG_RETURN(db_conn);
}

static void *spider_get_status_action(void *arg) {
  DBUG_ENTER("spider_get_status_action");

  SPIDER_SHARE *share;
  MYSQL *conn;
  THD *thd;
  my_thread_init();
  if (!(thd = SPIDER_new_THD(next_thread_id()))) {
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
  thd->thread_stack = (char *)&thd;
  thd->store_globals();
  thread_safe_decrement32(
      &thread_count); /* for shutdonw, don't wait this thread */

  while (get_status_init) {
    double modify_interval = opt_spider_modify_status_interval;
    double interval_least = opt_spider_status_least;
    time_t pre_modify_time;
    time_t cur_time;
    ulong share_records;
    ulong sleep_time = 60;
    char host[65] = {0};
    char username[65] = {0};
    char password[65] = {0};
    long port;
    char socket[256] = {0};
    char db_tb[256] = {0};
    uint db_tb_len;
    char tgt_db[65] = {0};
    char tgt_tb[65] = {0};
    time_t modify_time;
    char query_head[] = "show table status from ";
    char query[256] = {0};
    char key[256] = {0};
    ulong key_len;

    /* result */
    MYSQL_RES *res;
    MYSQL_ROW mysql_row;
    ha_rows records;
    ulong mean_rec_length;
    ulonglong data_file_length;
    ulonglong max_data_file_length;
    ulonglong index_file_length;
    ulonglong auto_increment_value;
    time_t create_time;
    time_t update_time;
    time_t check_time;
    MYSQL_TIME mysql_time;
    MYSQL_TIME_STATUS time_status;
    uint not_used_my_bool;
    long not_used_long;
    SPIDER_FOR_STS_CONN *sts_conn = NULL;
    time_t to_tm_time = (time_t)time((time_t *)0);
    struct tm lt;
    struct tm *l_time = localtime_r(&to_tm_time, &lt);
    my_hrtime_t current_time = my_hrtime();
    long usec = hrtime_sec_part(current_time);
    pthread_mutex_lock(&spider_tbl_mutex);
    share_records = spider_open_tables.records;
    pthread_mutex_unlock(&spider_tbl_mutex);

    for (ulong i = 0; (i < share_records) && get_status_init;
         i++) { /* foreach share */

      if (!spider_param_get_sts_or_crd()) {
        break;
      }
      pthread_mutex_lock(&spider_tbl_mutex);
      share = (SPIDER_SHARE *)my_hash_element(&spider_open_tables, i);
      if (!share) {
        pthread_mutex_unlock(&spider_tbl_mutex);
        continue;
      }

      if (!share->tgt_hosts[0] || !share->tgt_usernames[0] ||
          !share->tgt_passwords[0] || !share->table_name ||
          !share->tgt_dbs[0] || !share->tgt_table_names[0]) {
        pthread_mutex_unlock(&spider_tbl_mutex);
        continue;
      }
      cur_time = (time_t)time((time_t *)0);
      memcpy(host, share->tgt_hosts[0], strlen(share->tgt_hosts[0]) + 1);
      memcpy(username, share->tgt_usernames[0],
             strlen(share->tgt_usernames[0]) + 1);
      memcpy(password, share->tgt_passwords[0],
             strlen(share->tgt_passwords[0]) + 1);
      if (share->tgt_sockets[0])
        memcpy(socket, share->tgt_sockets[0],
               strlen(share->tgt_sockets[0]) + 1);
      memcpy(db_tb, share->table_name, strlen(share->table_name) + 1);
      memcpy(tgt_db, share->tgt_dbs[0], strlen(share->tgt_dbs[0]) + 1);
      memcpy(tgt_tb, share->tgt_table_names[0],
             strlen(share->tgt_table_names[0]) + 1);
      port = share->tgt_ports[0];
      pre_modify_time = share->pre_modify_time;
      share->pre_modify_time = cur_time;
      db_tb_len = share->table_name_length;
      modify_time = share->modify_time;
      pthread_mutex_unlock(&spider_tbl_mutex);

      sts_conn = NULL;
      if (difftime(cur_time, modify_time) >= modify_interval &&
          difftime(cur_time, pre_modify_time) >
              interval_least) { /* 1. need to modify table status
                                2. modify table status at least per 60s
                                */
        key_len =
            spider_create_sts_conn_key(key, host, port, username, password);
        sts_conn = (SPIDER_FOR_STS_CONN *)my_hash_search(&spider_for_sts_conns,
                                                         (uchar *)key, key_len);
        if (!sts_conn) { /* not exits, then new and add into hash */
          conn = spider_mysql_connect(host, username, password, port, socket);
          if (conn) {
            sts_conn = spider_create_sts_conn(key, key_len, (char *)conn);
            my_hash_insert(&spider_for_sts_conns, (uchar *)sts_conn);
          } else { /* ignore this conn */
            continue;
          }
        } else { /* can get from hash */
          conn = (MYSQL *)sts_conn->conn;
        }

        if (conn) {
          snprintf(query, 256, "%s %s like '%s'", query_head, tgt_db, tgt_tb);
          if (!mysql_real_query(conn, query, strlen(query))) {
            res = mysql_store_result(conn);
            if (res) {
              int error_num;
              mysql_row = mysql_fetch_row(res);
              if (mysql_row) {
                if (mysql_row[4])
                  records = (ha_rows)my_strtoll10(mysql_row[4], (char **)NULL,
                                                  &error_num);
                else
                  records = (ha_rows)0;
                if (mysql_row[5])
                  mean_rec_length = (ulong)my_strtoll10(
                      mysql_row[5], (char **)NULL, &error_num);
                else
                  mean_rec_length = 0;
                if (mysql_row[6])
                  data_file_length = (ulonglong)my_strtoll10(
                      mysql_row[6], (char **)NULL, &error_num);
                else
                  data_file_length = 0;
                if (mysql_row[7])
                  max_data_file_length = (ulonglong)my_strtoll10(
                      mysql_row[7], (char **)NULL, &error_num);
                else
                  max_data_file_length = 0;
                if (mysql_row[8])
                  index_file_length = (ulonglong)my_strtoll10(
                      mysql_row[8], (char **)NULL, &error_num);
                else
                  index_file_length = 0;
                if (mysql_row[10])
                  auto_increment_value = (ulonglong)my_strtoll10(
                      mysql_row[10], (char **)NULL, &error_num);
                else
                  auto_increment_value = 1;
                if (mysql_row[11]) {
                  str_to_datetime(mysql_row[11], strlen(mysql_row[11]),
                                  &mysql_time, 0, &time_status);
                  create_time = (time_t)my_system_gmt_sec(
                      &mysql_time, &not_used_long, &not_used_my_bool);
                } else
                  create_time = (time_t)0;
                if (mysql_row[12]) {
                  str_to_datetime(mysql_row[12], strlen(mysql_row[12]),
                                  &mysql_time, 0, &time_status);
                  update_time = (time_t)my_system_gmt_sec(
                      &mysql_time, &not_used_long, &not_used_my_bool);
                } else
                  update_time = (time_t)0;
                if (mysql_row[13]) {
                  str_to_datetime(mysql_row[13], strlen(mysql_row[13]),
                                  &mysql_time, 0, &time_status);
                  check_time = (time_t)my_system_gmt_sec(
                      &mysql_time, &not_used_long, &not_used_my_bool);
                } else
                  check_time = (time_t)0;
                spider_replace_table_status_up(
                    db_tb, db_tb_len, tgt_tb, tgt_db, data_file_length,
                    max_data_file_length, index_file_length, records,
                    mean_rec_length, check_time, create_time, update_time);

              } else {
                fprintf(stderr,
                        "%04d%02d%02d %02d:%02d:%02d.%ld [WARN SPIDER RESULT] "
                        "record = %lu, i = %lu, tb_name = %s,  failed to fetch "
                        "row\n",
                        l_time->tm_year + 1900, l_time->tm_mon + 1,
                        l_time->tm_mday, l_time->tm_hour, l_time->tm_min,
                        l_time->tm_sec, usec, share_records, i, db_tb);
              }
              mysql_free_result(res);
            }
          } else {
            fprintf(stderr,
                    "%04d%02d%02d %02d:%02d:%02d.%ld  [WARN SPIDER RESULT] "
                    "record = %lu, i = %lu, tb_name = %s,  failed to do real "
                    "query\n",
                    l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
                    l_time->tm_hour, l_time->tm_min, l_time->tm_sec, usec,
                    share_records, i, db_tb);
            my_hash_delete(&spider_for_sts_conns, (uchar *)sts_conn);
          }
        } else {
          fprintf(stderr,
                  "%04d%02d%02d %02d:%02d:%02d.%ld [WARN SPIDER RESULT] "
                  "record = %lu, i = %lu,  tb_name = %s, failed to get conn\n",
                  l_time->tm_year + 1900, l_time->tm_mon + 1, l_time->tm_mday,
                  l_time->tm_hour, l_time->tm_min, l_time->tm_sec, usec,
                  share_records, i, db_tb);
          if (sts_conn)
            my_hash_delete(&spider_for_sts_conns, (uchar *)sts_conn);
        }
      }
    } /* end foreach */
    /* 60s */
    for (ulong i = 0; (i < sleep_time) && get_status_init; i++) {
      sleep(1);
    }
  } /* end while */
  //   delete thd;
  my_thread_end();
  DBUG_RETURN(NULL);
}

int spider_create_get_status_thread() {
  int error_num;
  DBUG_ENTER("spider_create_get_status_thread");
  if (get_status_init) {
    DBUG_RETURN(0);
  }

#if MYSQL_VERSION_ID < 50500
  if (pthread_create(&get_status_thread, NULL, spider_get_status_action, NULL))
#else
  if (mysql_thread_create(spd_key_thd_get_status, &get_status_thread, NULL,
                          spider_get_status_action, NULL))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_thread_create;
  }
  get_status_init = TRUE;
  DBUG_RETURN(0);

error_thread_create:
  DBUG_RETURN(error_num);
}

void spider_free_get_status_thread() {
  DBUG_ENTER("spider_free_get_status_thread");
  if (get_status_init) {
    get_status_init = FALSE;
    //       pthread_cancel(get_status_thread);
    pthread_join(get_status_thread, NULL);
  }
  DBUG_VOID_RETURN;
}

void spider_free_for_sts_conn(void *info) {
  DBUG_ENTER("spider_free_for_sts_conn");
  if (info) {
    SPIDER_FOR_STS_CONN *p = (SPIDER_FOR_STS_CONN *)info;
    my_free(p->key);
    my_free(p);
  }
  DBUG_VOID_RETURN;
}

uint spider_create_sts_conn_key(char *key, char *host, ulong port, char *user,
                                char *passwd) {
  char port_str[10];
  uint key_length;

  ullstr(port, port_str);
  key_length =
      (uint)(
          strmov(strmov(strmov(strmov(key, host) + 1, port_str) + 1, user) + 1,
                 passwd) -
          key) +
      1;

  return key_length;
}

SPIDER_FOR_STS_CONN *spider_create_sts_conn(char *key, ulong key_len,
                                            char *conn) {
  SPIDER_FOR_STS_CONN *sts_conn = NULL;
  DBUG_ENTER("spider_create_sts_conn");
  sts_conn =
      (SPIDER_FOR_STS_CONN *)my_malloc(sizeof(*sts_conn), MY_ZEROFILL | MY_WME);
  if (!sts_conn) {
    DBUG_RETURN(NULL);
  }
  sts_conn->key_len = key_len;
  sts_conn->key = (char *)my_malloc(key_len, MY_ZEROFILL | MY_WME);
  if (!sts_conn->key) {
    DBUG_RETURN(NULL);
  }
  memcpy(sts_conn->key, key, key_len);
  sts_conn->conn = conn;
  DBUG_RETURN(sts_conn);
}

void spider_free_conn_by_servername(char *servername) {
  THD *thd = current_thd;
  SPIDER_CONN *conn = NULL;
  char conn_key[SPIDER_CONN_META_BUF_LEN + 2] = {0};
  uint conn_key_len = strlen(servername) + 1;
  conn_key[0] = '0';
  strmov(conn_key + 1, servername);

  while (conn = spd_connect_pools.get_conn_by_key((uchar *)conn_key,
      conn_key_len)) {
    spider_free_conn(conn);
  }
}
