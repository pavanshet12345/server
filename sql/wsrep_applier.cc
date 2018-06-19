/* Copyright (C) 2013-2015 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "mariadb.h"
#include "mysql/service_wsrep.h"
#include "wsrep_applier.h"

#include "wsrep_priv.h"
#include "wsrep_binlog.h" // wsrep_dump_rbr_buf()
#include "wsrep_xid.h"
#include "wsrep_thd.h"
#include "wsrep_trans_observer.h"

#include "slave.h" // opt_log_slave_updates
#include "log_event.h" // class THD, EVENT_LEN_OFFSET, etc.
#include "debug_sync.h"

/*
  read the first event from (*buf). The size of the (*buf) is (*buf_len).
  At the end (*buf) is shitfed to point to the following event or NULL and
  (*buf_len) will be changed to account just being read bytes of the 1st event.
*/
static Log_event* wsrep_read_log_event(
    char **arg_buf, size_t *arg_buf_len,
    const Format_description_log_event *description_event)
{
  DBUG_ENTER("wsrep_read_log_event");
  char *head= (*arg_buf);

  uint data_len = uint4korr(head + EVENT_LEN_OFFSET);
  char *buf= (*arg_buf);
  const char *error= 0;
  Log_event *res=  0;

  res= Log_event::read_log_event(buf, data_len, &error, description_event,
                                 true);

  if (!res)
  {
    DBUG_ASSERT(error != 0);
    sql_print_error("Error in Log_event::read_log_event(): "
                    "'%s', data_len: %d, event_type: %d",
                    error,data_len,head[EVENT_TYPE_OFFSET]);
  }
  (*arg_buf)+= data_len;
  (*arg_buf_len)-= data_len;
  DBUG_RETURN(res);
}

#include "transaction.h" // trans_commit(), trans_rollback()
#include "rpl_rli.h"     // class Relay_log_info;

void wsrep_set_apply_format(THD* thd, Format_description_log_event* ev)
{
  if (thd->wsrep_apply_format)
  {
    delete (Format_description_log_event*)thd->wsrep_apply_format;
  }
  thd->wsrep_apply_format= ev;
}

static inline Format_description_log_event*
wsrep_get_apply_format(THD* thd, Relay_log_info* rli)
{
  if (thd->wsrep_apply_format)
  {
    return (Format_description_log_event*) thd->wsrep_apply_format;
  return rli->get_rli_description_event();
}

void wsrep_apply_error::store(const THD* const thd)
{
  Diagnostics_area::Sql_condition_iterator it=
    thd->get_stmt_da()->sql_conditions();
  const Sql_condition* cond;

  static size_t const max_len= 2*MAX_SLAVE_ERRMSG; // 2x so that we have enough

  if (NULL == str_)
  {
    // this must be freeable by standard free()
    str_= static_cast<char*>(malloc(max_len));
    if (NULL == str_)
    {
      WSREP_ERROR("Failed to allocate %zu bytes for error buffer.", max_len);
      len_ = 0;
      return;
    }
  }
  else
  {
    /* This is possible when we invoke rollback after failed applying.
     * In this situation DA should not be reset yet and should contain
     * all previous errors from applying and new ones from rollbacking,
     * so we just overwrite is from scratch */
  }

  char* slider= str_;
  const char* const buf_end= str_ + max_len - 1; // -1: leave space for \0

  for (cond= it++; cond && slider < buf_end; cond= it++)
  {
    uint const err_code= cond->get_sql_errno();
    const char* const err_str= cond->get_message_text();

    slider+= my_snprintf(slider, buf_end - slider, " %s, Error_code: %d;",
                         err_str, err_code);
  }

  *slider= '\0';
  len_= slider - str_ + 1; // +1: add \0

  WSREP_DEBUG("Error buffer for thd %llu seqno %lld, %zu bytes: %s",
              thd->thread_id, (long long)wsrep_thd_trx_seqno(thd),
              len_, str_ ? str_ : "(null)");
}

int wsrep_apply_events(THD*        thd,
                       Relay_log_info* rli,
                       const void* events_buf,
                       size_t      buf_len)
{
  char *buf= (char *)events_buf;
  int rcode= 0;
  int event= 1;
  Log_event_type typ;

  DBUG_ENTER("wsrep_apply_events");
  if (!buf_len) WSREP_DEBUG("empty rbr buffer to apply: %lld",
                            (long long) wsrep_thd_trx_seqno(thd));

  while (buf_len)
  {
    int exec_res;
    Log_event* ev= wsrep_read_log_event(&buf, &buf_len,
                                          wsrep_get_apply_format(thd, rli));
    if (!ev)
    {
      WSREP_ERROR("applier could not read binlog event, seqno: %lld, len: %zu",
                  (long long)wsrep_thd_trx_seqno(thd), buf_len);
      rcode= WSREP_ERR_BAD_EVENT;
      goto error;
    }

    typ= ev->get_type_code();

    switch (typ) {
    case FORMAT_DESCRIPTION_EVENT:
      wsrep_set_apply_format(thd, (Format_description_log_event*)ev);
      continue;
#ifdef GTID_SUPPORT
    case GTID_LOG_EVENT:
    {
      Gtid_log_event* gev= (Gtid_log_event*)ev;
      if (gev->get_gno() == 0)
      {
        /* Skip GTID log event to make binlog to generate LTID on commit */
        delete ev;
        continue;
      }
    }
#endif /* GTID_SUPPORT */
    default:
      break;
    }

    /* Use the original server id for logging. */
    thd->set_server_id(ev->server_id);
    thd->set_time();                            // time the query
    thd->lex->current_select= 0;
    if (!ev->when)
    {
      my_hrtime_t hrtime= my_hrtime();
      ev->when= hrtime_to_my_time(hrtime);
      ev->when_sec_part= hrtime_sec_part(hrtime);
    }

    thd->variables.option_bits=
      (thd->variables.option_bits & ~OPTION_SKIP_REPLICATION) |
      (ev->flags & LOG_EVENT_SKIP_REPLICATION_F ?  OPTION_SKIP_REPLICATION : 0);

    ev->thd = thd;
    exec_res = ev->apply_event(rli);
    DBUG_PRINT("info", ("exec_event result: %d", exec_res));

    if (exec_res)
    {
      WSREP_WARN("Event %d %s apply failed: %d, seqno %lld",
                 event, ev->get_type_str(), exec_res,
                 (long long) wsrep_thd_trx_seqno(thd));
      rcode= exec_res;
      /* stop processing for the first error */
      delete ev;
      goto error;
    }
    event++;

    delete ev;
  }

error:
  if (thd->killed == THD::KILL_CONNECTION)
    WSREP_INFO("applier aborted: %lld", (long long)wsrep_thd_trx_seqno(thd));

  DBUG_RETURN(rcode);
}

#if 0
static void wsrep_apply_non_blocking(THD* thd, void* args)
{
  /* Go to BF mode */
  wsrep_thd_shadow shadow;
  wsrep_prepare_bf_thd(thd, &shadow);

  Wsrep_nbo_ctx* nbo_ctx= (Wsrep_nbo_ctx*) args;

  DBUG_ASSERT(nbo_ctx);

  /*
    Record context for synchronizing with applier thread once
    MDL locks have been aquired.
  */
  thd->wsrep_nbo_ctx= nbo_ctx;

  /* Must be applier thread */
  assert(thd->wsrep_exec_mode == REPL_RECV);

  /* Set OSU method for non blocking */
  thd->variables.wsrep_OSU_method= WSREP_OSU_NBO;

  /*
    First apply TOI action as usual, wsrep_ptr->to_execute_start() will
    be called at stmt commit. Applying is always assumed to succeed here.
    Non-blocking operation end is supposed to sync the end result with
    group and abort if the result does not match.
  */
  wsrep_apply_error err;
  if (wsrep_apply_toi(thd, nbo_ctx->buf(), nbo_ctx->buf_len(), nbo_ctx->flags(),
                      &nbo_ctx->meta(), err) != WSREP_CB_SUCCESS)
  {
    WSREP_DEBUG("Applying NBO failed");
  }

  /*
    Applying did not cause action to signal slave thread. Wake up
    the slave before exit.
  */
  if (!thd->wsrep_nbo_ctx->ready()) thd->wsrep_nbo_ctx->signal();

  /* Non-blocking operation end. No need to take action on error here,
     it is handled by provider. */
  wsrep_buf_t const err_buf= err.get_buf();
  if (thd->wsrep_trx_meta.gtid.seqno != WSREP_SEQNO_UNDEFINED &&
      wsrep_ptr->to_execute_end(wsrep_ptr,
                                thd->thread_id, &err_buf) != WSREP_OK)
  {
    WSREP_WARN("Non-blocking operation end failed");
  }

  if (wsrep_ptr->free_connection(wsrep_ptr, thd->thread_id) != WSREP_OK)
  {
    WSREP_WARN("Failed to free connection: %llu",
               (long long unsigned)thd->thread_id);
  }

  thd->wsrep_nbo_ctx= NULL;
  delete nbo_ctx;

  /* Return from BF mode before syncing with group */
  wsrep_return_from_bf_mode(thd, &shadow);
}

static int start_nbo_thd(const void*             const buf,
                         size_t                  const buf_len,
                         uint32_t                const flags,
                         const wsrep_trx_meta_t* const meta)
{
  int rcode= WSREP_RET_SUCCESS;

  // Non-blocking operation start
  Wsrep_nbo_ctx* nbo_ctx= 0;
  Wsrep_thd_args* args= 0;
  try
  {
    nbo_ctx= new Wsrep_nbo_ctx(buf, buf_len, flags, *meta);
    args= new Wsrep_thd_args(wsrep_apply_non_blocking, nbo_ctx);
    pthread_t th;
    int err;
    int max_tries= 1000;
    while ((err= pthread_create(&th, &connection_attrib, start_wsrep_THD,
                                args)) == EAGAIN)
    {
      --max_tries;
      if (max_tries == 0)
      {
        delete nbo_ctx;
        delete args;
        WSREP_ERROR("Failed to create thread for non-blocking operation: %d",
                    err);
        return WSREP_ERR_FAILED;
      }
      else
      {
        usleep(1000);
      }
    }

    // Detach thread and wait until worker signals that it has locked
    // required resources.
    pthread_detach(th);
    nbo_ctx->wait_sync();
    // nbo_ctx will be deleted by worker thread
  }
  catch (...)
  {
    delete nbo_ctx;
    delete args;
    WSREP_ERROR("Caught exception while trying to create thread for "
                "non-blocking operation");
    rcode= WSREP_ERR_FAILED;
  }

  return rcode;
}

#endif
