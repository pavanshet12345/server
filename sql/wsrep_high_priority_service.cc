/* Copyright 2018 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "wsrep_high_priority_service.h"
#include "wsrep_applier.h"
#include "wsrep_binlog.h"
#include "wsrep_schema.h"
#include "wsrep_xid.h"
#include "wsrep_trans_observer.h"

#include "sql_class.h" /* THD */
#include "transaction.h"
#include "debug_sync.h"
/* RLI */
#include "rpl_rli.h"
#define NUMBER_OF_FIELDS_TO_IDENTIFY_COORDINATOR 1
#define NUMBER_OF_FIELDS_TO_IDENTIFY_WORKER 2
#include "rpl_info_factory.h"

namespace
{
/*
  Scoped mode for applying non-transactional write sets (TOI)
 */
class Wsrep_non_trans_mode
{
public:
  Wsrep_non_trans_mode(THD* thd, const wsrep::ws_meta& ws_meta)
    : m_thd(thd)
    , m_option_bits(thd->variables.option_bits)
    , m_server_status(thd->server_status)
  {
    m_thd->variables.option_bits&= ~OPTION_BEGIN;
    m_thd->server_status&= ~SERVER_STATUS_IN_TRANS;
    m_thd->wsrep_cs().enter_toi(ws_meta);
  }
  ~Wsrep_non_trans_mode()
  {
    m_thd->variables.option_bits= m_option_bits;
    m_thd->server_status= m_server_status;
    m_thd->wsrep_cs().leave_toi();
  }
private:
  Wsrep_non_trans_mode(const Wsrep_non_trans_mode&);
  Wsrep_non_trans_mode& operator=(const Wsrep_non_trans_mode&);
  THD* m_thd;
  ulonglong m_option_bits;
  uint m_server_status;
};
}

/* Create relay log info for applier context */
static Relay_log_info* wsrep_relay_log_init(const char* log_fname)
{
  uint rli_option = INFO_REPOSITORY_DUMMY;
  Relay_log_info *rli= NULL;
  rli = Rpl_info_factory::create_rli(rli_option, false);
  rli->set_rli_description_event(
    new Format_description_log_event(BINLOG_VERSION));
  return rli;
}

static void wsrep_setup_uk_and_fk_checks(THD* thd)
{
  /* Tune FK and UK checking policy. These are reset back to original
     in Wsrep_high_priority_service destructor. */
  if (wsrep_slave_UK_checks == FALSE)
    thd->variables.option_bits|= OPTION_RELAXED_UNIQUE_CHECKS;
  else
    thd->variables.option_bits&= ~OPTION_RELAXED_UNIQUE_CHECKS;

  if (wsrep_slave_FK_checks == FALSE)
    thd->variables.option_bits|= OPTION_NO_FOREIGN_KEY_CHECKS;
  else
    thd->variables.option_bits&= ~OPTION_NO_FOREIGN_KEY_CHECKS;
}

/****************************************************************************
                         High priority service
*****************************************************************************/

Wsrep_high_priority_service::Wsrep_high_priority_service(THD* thd)
  : wsrep::high_priority_service(Wsrep_server_state::instance())
  , wsrep::high_priority_context(thd->wsrep_cs())
  , m_thd(thd)
  , m_rli()
{
  m_shadow.option_bits   = thd->variables.option_bits;
  m_shadow.server_status = thd->server_status;
  m_shadow.vio           = thd->net.vio;
  m_shadow.tx_isolation  = thd->variables.tx_isolation;
  m_shadow.db            = thd->db;
  m_shadow.db_length     = thd->db_length;
  m_shadow.user_time     = thd->user_time;
  m_shadow.row_count_func= thd->get_row_count_func();
  m_shadow.wsrep_applier = thd->wsrep_applier;

  /* Disable general logging on applier threads */
  thd->variables.option_bits |= OPTION_LOG_OFF;
  /* Enable binlogging if opt_log_slave_updates is set */
  if (opt_log_slave_updates)
    thd->variables.option_bits|= OPTION_BIN_LOG;
  else
    thd->variables.option_bits&= ~(OPTION_BIN_LOG);

  thd->net.vio= 0;
  thd->reset_db(NULL, 0);
  thd->clear_error();
  thd->variables.tx_isolation = ISO_READ_COMMITTED;
  thd->tx_isolation           = ISO_READ_COMMITTED;

  /* From trans_begin() */
  thd->variables.option_bits|= OPTION_BEGIN;
  thd->server_status|= SERVER_STATUS_IN_TRANS;

  /* Make THD wsrep_applier so that it cannot be killed */
  thd->wsrep_applier= true;

  m_rli= wsrep_relay_log_init("wsrep_relay");
  m_rli->info_thd = thd;

  thd_proc_info(thd, "wsrep applier idle");
}

Wsrep_high_priority_service::~Wsrep_high_priority_service()
{
  THD* thd= m_thd;
  thd->variables.option_bits  = m_shadow.option_bits;
  thd->server_status          = m_shadow.server_status;
  thd->net.vio                = m_shadow.vio;
  thd->variables.tx_isolation = m_shadow.tx_isolation;
  thd->reset_db(m_shadow.db, m_shadow.db_length);
  thd->user_time              = m_shadow.user_time;
  thd->set_row_count_func(m_shadow.row_count_func);
  thd->wsrep_applier          = m_shadow.wsrep_applier;
  delete m_rli;
}

int Wsrep_high_priority_service::start_transaction(
  const wsrep::ws_handle& ws_handle, const wsrep::ws_meta& ws_meta)
{
  return m_thd->wsrep_cs().start_transaction(ws_handle, ws_meta);
}

const wsrep::transaction& Wsrep_high_priority_service::transaction() const
{
  return m_thd->wsrep_trx();
}

void Wsrep_high_priority_service::adopt_transaction(const wsrep::transaction& transaction)
{
  m_thd->wsrep_cs().adopt_transaction(transaction);
}


int Wsrep_high_priority_service::append_fragment_and_commit(
  const wsrep::ws_handle& ws_handle,
  const wsrep::ws_meta& ws_meta,
  const wsrep::const_buffer& data)
{
  DBUG_ENTER("Wsrep_high_priority_service::append_fragment");
  int ret= start_transaction(ws_handle, ws_meta);
  ret= ret || wsrep_schema->append_fragment(m_thd,
                                            ws_meta.server_id(),
                                            ws_meta.transaction_id(),
                                            ws_meta.seqno(),
                                            ws_meta.flags(),
                                            data);

  /*
    Note: The commit code below seems to be identical to
    Wsrep_storage_service::commit(). Consider implementing
    common utility function to deal with commit.
   */
  const bool do_binlog_commit= (opt_log_slave_updates && gtid_mode);
   /*
    Write skip event into binlog if gtid_mode is on. This is to
    maintain gtid continuity.
  */
  if (do_binlog_commit)
  {
    ret= wsrep_write_skip_event(m_thd);
  }

  if (!ret)
  {
    ret= m_thd->wsrep_cs().prepare_for_ordering(ws_handle,
                                                ws_meta, true);
  }

  if (!ret)
  {
    DBUG_ASSERT(wsrep_thd_trx_seqno(m_thd) > 0);
    if (!do_binlog_commit)
    {
      ret= wsrep_before_commit(m_thd, true);
    }
    ret= ret || trans_commit(m_thd);
    if (!do_binlog_commit)
    {
      if (opt_log_slave_updates)
      {
        ret= ret || wsrep_ordered_commit(m_thd, true, wsrep_apply_error());
      }
      ret= ret || wsrep_after_commit(m_thd, true);
    }
  }
  m_thd->wsrep_cs().after_applying();
  m_thd->mdl_context.release_transactional_locks();
  DBUG_RETURN(ret);
}

int Wsrep_high_priority_service::remove_fragments(const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_high_priority_service::remove_fragments");
  int ret= wsrep_schema->remove_fragments(m_thd,
                                          ws_meta.server_id(),
                                          ws_meta.transaction_id(),
                                          m_thd->wsrep_sr().fragments());
  DBUG_RETURN(ret);
}

int Wsrep_high_priority_service::commit(const wsrep::ws_handle& ws_handle,
                                        const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_high_priority_service::commit");
  THD* thd= m_thd;
  DBUG_ASSERT(thd->wsrep_trx().active());
  thd->wsrep_cs().prepare_for_ordering(ws_handle, ws_meta, true);
  thd_proc_info(thd, "committing");

  int ret= 0;
  const bool is_ordered= !ws_meta.seqno().is_undefined();
  /* If opt_log_slave_updates is not on, applier does not write
     anything to binlog cache and neither wsrep_before_commit()
     nor wsrep_after_commit() we be reached from binlog code
     path for applier. Therefore run wsrep_before_commit()
     and wsrep_after_commit() here. wsrep_ordered_commit()
     will be called from wsrep_ordered_commit_if_no_binlog(). */
  if (!opt_log_slave_updates && is_ordered)
  {
    ret= wsrep_before_commit(thd, true);
  }
  ret= ret || trans_commit(thd);

  if (ret == 0)
  {
    m_rli->cleanup_context(thd, 0);
    thd->variables.gtid_next.set_automatic();
  }

  if (ret == 0 && !opt_log_slave_updates && is_ordered)
  {
    ret= wsrep_after_commit(thd, true);
  }

  wsrep_set_apply_format(thd, 0);
  m_thd->mdl_context.release_transactional_locks();

  thd_proc_info(thd, "wsrep applier committed");

  if (!is_ordered)
  {
    /* Wsrep commit was not ordered so it does not go through commit time
       hooks and remains active. Roll it back to make cleanup happen
       in after_applying() call. */
    m_thd->wsrep_cs().before_rollback();
    m_thd->wsrep_cs().after_rollback();
  }

  must_exit_ = check_exit_status();
  DBUG_RETURN(ret);
}

int Wsrep_high_priority_service::rollback(const wsrep::ws_handle& ws_handle,
                                          const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_high_priority_service::rollback");
  m_thd->wsrep_cs().prepare_for_ordering(ws_handle, ws_meta, false);
  int ret= (trans_rollback_stmt(m_thd) || trans_rollback(m_thd));
  m_thd->mdl_context.release_transactional_locks();
  m_thd->mdl_context.release_explicit_locks();
  DBUG_RETURN(ret);
}

int Wsrep_high_priority_service::apply_toi(const wsrep::ws_meta& ws_meta,
                                           const wsrep::const_buffer& data)
{
  DBUG_ENTER("Wsrep_high_priority_service::apply_toi");
  THD* thd= m_thd;
  Wsrep_non_trans_mode non_trans_mode(thd, ws_meta);

  wsrep::client_state& client_state(thd->wsrep_cs());
  DBUG_ASSERT(client_state.in_toi());

  thd_proc_info(thd, "wsrep applier toi");

  WSREP_DEBUG("Wsrep_high_priority_service::apply_toi: %lld",
              client_state.toi_meta().seqno().get());

  int ret= wsrep_apply_events(thd, m_rli, data.data(), data.size());
  if (ret != 0 || thd->wsrep_has_ignored_error)
  {
    wsrep_dump_rbr_buf(thd, data.data(), data.size());
    thd->wsrep_has_ignored_error= false;
    /* todo: error voting */
  }
  trans_commit(thd);
  TABLE *tmp;
  while ((tmp = thd->temporary_tables))
  {
    WSREP_DEBUG("Applier %lu, has temporary tables: %s.%s",
                thd->thread_id,
                (tmp->s) ? tmp->s->db.str : "void",
                (tmp->s) ? tmp->s->table_name.str : "void");
    close_temporary_table(thd, tmp, 1, 1);
  }
  wsrep_set_SE_checkpoint(client_state.toi_meta().gtid());

  must_exit_ = check_exit_status();

  DBUG_RETURN(ret);
}

void Wsrep_high_priority_service::store_globals()
{
  DBUG_ENTER("Wsrep_high_priority_service::store_globals");
  /* In addition to calling THD::store_globals(), call
     wsrep::client_state::store_globals() to gain ownership of
     the client state */
  m_thd->store_globals();
  m_thd->wsrep_cs().store_globals();
  DBUG_VOID_RETURN;
}

void Wsrep_high_priority_service::reset_globals()
{
  DBUG_ENTER("Wsrep_high_priority_service::reset_globals");
  m_thd->restore_globals();
  DBUG_VOID_RETURN;
}

void Wsrep_high_priority_service::switch_execution_context(wsrep::high_priority_service& orig_high_priority_service)
{
  DBUG_ENTER("Wsrep_high_priority_service::switch_execution_context");
  Wsrep_high_priority_service&
    orig_hps= static_cast<Wsrep_high_priority_service&>(orig_high_priority_service);
  m_thd->thread_stack= orig_hps.m_thd->thread_stack;
  DBUG_VOID_RETURN;
}

int Wsrep_high_priority_service::log_dummy_write_set(const wsrep::ws_handle& ws_handle,
                                                     const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_high_priority_service::log_dummy_write_set");
  int ret= 0;
  DBUG_PRINT("info",
             ("Wsrep_high_priority_service::log_dummy_write_set: seqno=%lld",
              ws_meta.seqno().get()));
  m_thd->wsrep_cs().start_transaction(ws_handle, ws_meta);
  WSREP_DEBUG("Log dummy write set %lld", ws_meta.seqno().get());
  if (!(opt_log_slave_updates && gtid_mode))
  {
    m_thd->wsrep_cs().before_rollback();
    m_thd->wsrep_cs().after_rollback();
  }
  m_thd->wsrep_cs().after_applying();
  DBUG_RETURN(ret);
}

void Wsrep_high_priority_service::debug_crash(const char* crash_point)
{
  DBUG_ASSERT(m_thd == current_thd);
  DBUG_EXECUTE_IF(crash_point, DBUG_SUICIDE(););
}

/****************************************************************************
                           Applier service
*****************************************************************************/

Wsrep_applier_service::Wsrep_applier_service(THD* thd)
  : Wsrep_high_priority_service(thd)
{
  thd->wsrep_applier_service= this;
  thd->wsrep_cs().open(wsrep::client_id(thd->thread_id));
  thd->wsrep_cs().before_command();
  thd->wsrep_cs().debug_log_level(wsrep_debug);
}

Wsrep_applier_service::~Wsrep_applier_service()
{
  m_thd->wsrep_cs().after_command_before_result();
  m_thd->wsrep_cs().after_command_after_result();
  m_thd->wsrep_cs().close();
  m_thd->wsrep_cs().cleanup();
}

int Wsrep_applier_service::apply_write_set(const wsrep::ws_meta& ws_meta,
                                           const wsrep::const_buffer& data)
{
  DBUG_ENTER("Wsrep_applier_service::apply_write_set");
  THD* thd= m_thd;

  DBUG_ASSERT(thd->wsrep_trx().active());
  DBUG_ASSERT(thd->wsrep_trx().state() == wsrep::transaction::s_executing);

  thd_proc_info(thd, "applying write set");
  /* moved dbug sync point here, after possible THD switch for SR transactions
     has ben done
  */
  /* Allow tests to block the applier thread using the DBUG facilities */
  DBUG_EXECUTE_IF("sync.wsrep_apply_cb",
                 {
                   const char act[]=
                     "now "
                     "SIGNAL sync.wsrep_apply_cb_reached "
                     "WAIT_FOR signal.wsrep_apply_cb";
                   DBUG_ASSERT(!debug_sync_set_action(thd,
                                                      STRING_WITH_LEN(act)));
                 };);

  wsrep_setup_uk_and_fk_checks(thd);

  int ret= wsrep_apply_events(thd, m_rli, data.data(), data.size());

  if (ret || thd->wsrep_has_ignored_error)
  {
    wsrep_dump_rbr_buf(thd, data.data(), data.size());
  }

  TABLE *tmp;
  while ((tmp = thd->temporary_tables))
  {
    WSREP_DEBUG("Applier %lu, has temporary tables: %s.%s",
                m_thd->thread_id,
                (tmp->s) ? tmp->s->db.str : "void",
                (tmp->s) ? tmp->s->table_name.str : "void");
    close_temporary_table(m_thd, tmp, 1, 1);
  }

  if (!ret && !(ws_meta.flags() & wsrep::provider::flag::commit))
  {
    thd->wsrep_cs().fragment_applied(ws_meta.seqno());
  }
  thd_proc_info(thd, "wsrep applied write set");
  DBUG_RETURN(ret);
}

void Wsrep_applier_service::after_apply()
{
  DBUG_ENTER("Wsrep_applier_service::after_apply");
  wsrep_after_apply(m_thd);
  // thd_proc_info(m_thd, "wsrep applier idle");
  DBUG_VOID_RETURN;
}

bool Wsrep_applier_service::check_exit_status() const
{
  bool ret= false;
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);
  if (wsrep_slave_count_change < 0)
  {
    ++wsrep_slave_count_change;
    ret= true;
  }
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);
  return ret;
}

/****************************************************************************
                           Replayer service
*****************************************************************************/

Wsrep_replayer_service::Wsrep_replayer_service(THD* thd)
  : Wsrep_high_priority_service(thd)
  , m_da_shadow()
  , m_replay_status()
{
  /* Response must not have been sent to client */
  DBUG_ASSERT(!thd->get_stmt_da()->is_sent());
  /* PS reprepare observer should have been removed already
     open_table() will fail if we have dangling observer here */
  DBUG_ASSERT(!thd->get_reprepare_observer());
  /* Replaying should happen always from after_statement() hook
     after rollback, which should guarantee that there are no
     transactional locks */
  DBUG_ASSERT(!thd->mdl_context.has_transactional_locks());

  /* Make a shadow copy of diagnostics area and reset */
  m_da_shadow.status= thd->get_stmt_da()->status();
  if (m_da_shadow.status == Diagnostics_area::DA_OK)
  {
    m_da_shadow.affected_rows= thd->get_stmt_da()->affected_rows();
    m_da_shadow.last_insert_id= thd->get_stmt_da()->last_insert_id();
    strmake(m_da_shadow.message, thd->get_stmt_da()->message(),
            sizeof(m_da_shadow.message) - 1);
  }
  thd->get_stmt_da()->reset_diagnostics_area();

  /* Release explicit locks */
  if (thd->locked_tables_mode && thd->lock)
  {
    WSREP_WARN("releasing table lock for replaying (%ld)",
               thd->thread_id);
    thd->locked_tables_list.unlock_locked_tables(thd);
    thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
  }

  /*
    Replaying will call MYSQL_START_STATEMENT when handling
    BEGIN Query_log_event so end statement must be called before
    replaying.
  */
  MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
  thd->m_statement_psi= NULL;
  thd->m_digest= NULL;
  thd_proc_info(thd, "wsrep replaying trx");
}

Wsrep_replayer_service::~Wsrep_replayer_service()
{
  THD* thd= m_thd;
  DBUG_ASSERT(!thd->get_stmt_da()->is_sent());
  DBUG_ASSERT(!thd->get_stmt_da()->is_set());
  if (m_replay_status == wsrep::provider::success)
  {
    DBUG_ASSERT(thd->wsrep_cs().current_error() == wsrep::e_success);
    thd->killed= THD::NOT_KILLED;
    if (m_da_shadow.status == Diagnostics_area::DA_OK)
    {
      my_ok(thd,
            m_da_shadow.affected_rows,
            m_da_shadow.last_insert_id,
            m_da_shadow.message);
    }
    else
    {
      my_ok(thd);
    }
  }
  else if (m_replay_status == wsrep::provider::error_certification_failed)
  {
    DBUG_ASSERT(thd->wsrep_cs().current_error() == wsrep::e_deadlock_error);
  }
  else
  {
    DBUG_ASSERT(0);
    WSREP_ERROR("trx_replay failed for: %d, schema: %s, query: %s",
                m_replay_status,
                (thd->db ? thd->db : "(null)"), WSREP_QUERY(thd));
    unireg_abort(1);
  }
}

int Wsrep_replayer_service::apply_write_set(const wsrep::ws_meta& ws_meta,
                                                 const wsrep::const_buffer& data)
{
  DBUG_ENTER("Wsrep_replayer_service::apply_write_set");
  THD* thd= m_thd;

  DBUG_ASSERT(thd->wsrep_trx().active());
  DBUG_ASSERT(thd->wsrep_trx().state() == wsrep::transaction::s_replaying);

  wsrep_setup_uk_and_fk_checks(thd);

  int ret= 0;
  if (!wsrep::starts_transaction(ws_meta.flags()))
  {
    DBUG_ASSERT(thd->wsrep_trx().is_streaming());
    ret= wsrep_schema->replay_transaction(thd,
                                          m_rli,
                                          ws_meta,
                                          thd->wsrep_sr().fragments());
  }

  ret= ret || wsrep_apply_events(thd, m_rli, data.data(), data.size());

  if (ret || thd->wsrep_has_ignored_error)
  {
    wsrep_dump_rbr_buf(thd, data.data(), data.size());
  }

  TABLE *tmp;
  while ((tmp = thd->temporary_tables))
  {
    WSREP_DEBUG("Applier %lu, has temporary tables: %s.%s",
                m_thd->thread_id,
                (tmp->s) ? tmp->s->db.str : "void",
                (tmp->s) ? tmp->s->table_name.str : "void");
    close_temporary_table(m_thd, tmp, 1, 1);
  }

  if (!ret && !(ws_meta.flags() & wsrep::provider::flag::commit))
  {
    thd->wsrep_cs().fragment_applied(ws_meta.seqno());
  }

  thd_proc_info(thd, "wsrep replayed write set");
  DBUG_RETURN(ret);
}
