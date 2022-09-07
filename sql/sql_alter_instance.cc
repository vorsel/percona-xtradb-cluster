/* Copyright (c) 2016, 2022, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is also distributed with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have included with MySQL.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License, version 2.0, for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/sql_alter_instance.h" /* Alter_instance class */

#include <utility>

#include "keyring_operations_helper.h"
#include "lex_string.h"
#include "m_string.h"
#include "mutex_lock.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h" /* my_error */
#include "mysqld_error.h"
#include "sql/auth/auth_acls.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/derror.h"  /* ER_THD */
#include "sql/handler.h" /* ha_resolve_by_legacy_type */
#include "sql/lock.h"    /* acquire_shared_global_read_lock */
#include "sql/mysqld.h"
#include "sql/rpl_log_encryption.h"
#include "sql/server_component/mysql_server_keyring_lockable_imp.h" /* Keyring */
#include "sql/sql_backup_lock.h" /* acquire_shared_backup_lock */
#include "sql/sql_class.h"       /* THD */
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/sql_plugin.h"
#include "sql/sql_plugin_ref.h"
#include "sql/sql_table.h" /* write_to_binlog */
#include "system_key.h"

/*
  @brief
  Log current command to binlog

  @returns false on success,
           true on error

  In case of failure, appropriate error is logged.
*/

bool Alter_instance::log_to_binlog() {
  bool res = false;
  if (!m_thd->lex->no_write_to_binlog)
    res =
        write_bin_log(m_thd, false, m_thd->query().str, m_thd->query().length);

  return res;
}

bool Rotate_innodb_key::check_security_context() {
  Security_context *sctx = m_thd->security_context();
  if (!sctx->check_access(SUPER_ACL) &&
      !sctx->has_global_grant(STRING_WITH_LEN("ENCRYPTION_KEY_ADMIN")).first) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
             "SUPER or ENCRYPTION_KEY_ADMIN");
    return true;
  }
  return false;
}

bool Rotate_innodb_key::acquire_backup_locks() {
  /*
    Acquire shared backup lock to block concurrent backup. Acquire exclusive
    backup lock to block any concurrent DDL. The fact that we acquire both
    these locks also ensures that concurrent KEY rotation requests are blocked.
  */
  if (acquire_exclusive_backup_lock(m_thd, m_thd->variables.lock_wait_timeout,
                                    true) ||
      acquire_shared_backup_lock(m_thd, m_thd->variables.lock_wait_timeout)) {
    // MDL subsystem has to set an error in Diagnostics Area
    assert(m_thd->get_stmt_da()->is_error());
    return true;
  }

  // Acquire Percona's LOCK TABLES FOR BACKUP lock
  if (m_thd->backup_tables_lock.abort_if_acquired() ||
      m_thd->backup_tables_lock.acquire_protection(
          m_thd, MDL_TRANSACTION, m_thd->variables.lock_wait_timeout)) {
    return true;
  }
  return false;
}

/*
  @brief
  Executes master key rotation by calling SE api.

  @returns false on success
           true on error

  In case of failure, appropriate error
  is logged by function.
*/

bool Rotate_innodb_master_key::execute() {
  const LEX_CSTRING storage_engine = {STRING_WITH_LEN("innodb")};
  plugin_ref se_plugin;
  handlerton *hton;

  if (check_security_context()) return true;

  if ((se_plugin = ha_resolve_by_name(m_thd, &storage_engine, false))) {
    hton = plugin_data<handlerton *>(se_plugin);
  } else {
    my_error(ER_MASTER_KEY_ROTATION_SE_UNAVAILABLE, MYF(0));
    return true;
  }

  if (!hton->rotate_encryption_master_key) {
    my_error(ER_MASTER_KEY_ROTATION_NOT_SUPPORTED_BY_SE, MYF(0));
    return true;
  }

  /*
    Acquire protection against GRL and check for concurrent change of read_only
    value since encryption key rotation is not allowed in read_only/
    super_read_only mode.
  */
  if (acquire_shared_global_read_lock(m_thd,
                                      m_thd->variables.lock_wait_timeout)) {
    // MDL subsystem has to set an error in Diagnostics Area
    assert(m_thd->get_stmt_da()->is_error());
    return true;
  }

  if (acquire_backup_locks()) return true;

  if (hton->rotate_encryption_master_key()) {
    /* SE should have raised error */
    assert(m_thd->get_stmt_da()->is_error());
    return true;
  }

  if (log_to_binlog()) {
    /*
      Though we failed to write to binlog,
      there is no way we can undo this operation.
      So, convert error to a warning and let user
      know that something went wrong while trying
      to make entry in binlog.
    */
    m_thd->clear_error();
    m_thd->get_stmt_da()->reset_diagnostics_area();

    push_warning(m_thd, Sql_condition::SL_WARNING,
                 ER_MASTER_KEY_ROTATION_BINLOG_FAILED,
                 ER_THD(m_thd, ER_MASTER_KEY_ROTATION_BINLOG_FAILED));
  }

  my_ok(m_thd);
  return false;
}

bool Rotate_percona_system_key::rotate() {
  size_t key_length{0};

  if (!is_valid_percona_system_key(system_key_name, &key_length)) {
    assert(false);
    return false;
  }

  assert(key_length != 0);

  std::ostringstream key_id_with_uuid_ss;
  key_id_with_uuid_ss << system_key_name;
  if (using_system_key_id) key_id_with_uuid_ss << '-' << system_key_id;
  key_id_with_uuid_ss << '-' << server_uuid;

  std::string key_id_with_uuid = key_id_with_uuid_ss.str();

  // It should only be possible to rotate already existing key.
  // First check that system key exists.
  char *key_type = nullptr;
  size_t key_len;
  unsigned char *key = nullptr;
  if (keyring_operations_helper::read_secret(
          srv_keyring_reader, key_id_with_uuid.c_str(), nullptr, &key, &key_len,
          &key_type, PSI_INSTRUMENT_ME) != 1 ||
      nullptr == key) {
    if (nullptr != key) {
      my_free(key);
    }
    if (nullptr != key_type) {
      my_free(key_type);
    }
    my_error(ER_SYSTEM_KEY_ROTATION_KEY_DOESNT_EXIST, MYF(0), system_key_id);
    return true;
  }
  assert(memcmp(key_type, "AES", 3) == 0);
  my_free(key_type);
  my_free(key);
  key = nullptr;
  key_type = nullptr;

  // rotate the key
  if (srv_keyring_generator->generate(key_id_with_uuid.c_str(), nullptr, "AES",
                                      key_length)) {
    my_error(ER_SYSTEM_KEY_ROTATION_CANT_GENERATE_NEW_VERSION, MYF(0),
             system_key_id);
    return true;
  }
  return false;
}

bool Rotate_innodb_system_key::execute() {
  assert(strlen(server_uuid) != 0);

  if (check_security_context() || acquire_backup_locks()) return true;

  if (rotate_percona_system_key.rotate()) return true;

  if (log_to_binlog()) {
    /*
      Though we failed to write to binlog,
      there is no way we can undo this operation.
      So, covert error to a warning and let user
      know that something went wrong while trying
      to make entry in binlog.
    */
    m_thd->clear_error();
    m_thd->get_stmt_da()->reset_diagnostics_area();

    push_warning(m_thd, Sql_condition::SL_WARNING,
                 ER_MASTER_KEY_ROTATION_BINLOG_FAILED,
                 ER_THD(m_thd, ER_MASTER_KEY_ROTATION_BINLOG_FAILED));
  }

  my_ok(m_thd);
  return false;
}

bool Innodb_redo_log::execute() {
  DBUG_TRACE;

  const LEX_CSTRING storage_engine = {STRING_WITH_LEN("innodb")};

  auto hton = plugin_data<handlerton *>(
      ha_resolve_by_name(m_thd, &storage_engine, false));

  if (hton == nullptr) {
    /* Innodb engine is not loaded. Should never happen. */
    my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), storage_engine.str);
  }

  Security_context *sctx = m_thd->security_context();
  if (!sctx->has_global_grant(STRING_WITH_LEN("INNODB_REDO_LOG_ENABLE"))
           .first) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "INNODB_REDO_LOG_ENABLE");
    return true;
  }

  /*
    Acquire shared backup lock to block concurrent backup. Acquire exclusive
    backup lock to block any concurrent DDL. This would also serialize any
    concurrent key rotation and other redo log enable/disable calls.
  */
  if (acquire_exclusive_backup_lock(m_thd, m_thd->variables.lock_wait_timeout,
                                    true) ||
      acquire_shared_backup_lock(m_thd, m_thd->variables.lock_wait_timeout)) {
    assert(m_thd->get_stmt_da()->is_error());
    return true;
  }

  // Acquire Percona's LOCK TABLES FOR BACKUP lock
  if (m_thd->backup_tables_lock.abort_if_acquired() ||
      m_thd->backup_tables_lock.acquire_protection(
          m_thd, MDL_TRANSACTION, m_thd->variables.lock_wait_timeout)) {
    return true;
  }

  if (hton->redo_log_set_state(m_thd, m_enable)) {
    /* SE should have raised error */
    assert(m_thd->get_stmt_da()->is_error());
    return true;
  }

  /* Right now, we don't log this command to binary log as redo logging
  options are low level physical attribute which is not needed to replicate
  to other instances. */

  my_ok(m_thd);
  return false;
}

bool Rotate_binlog_master_key::execute() {
  DBUG_TRACE;

  MUTEX_LOCK(lock, &LOCK_rotate_binlog_master_key);

  Security_context *sctx = m_thd->security_context();
  if (!sctx->check_access(SUPER_ACL) &&
      !sctx->has_global_grant(STRING_WITH_LEN("BINLOG_ENCRYPTION_ADMIN"))
           .first) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
             "SUPER or BINLOG_ENCRYPTION_ADMIN");
    return true;
  }

  if (!rpl_encryption.is_enabled()) {
    my_error(ER_RPL_ENCRYPTION_CANNOT_ROTATE_BINLOG_MASTER_KEY, MYF(0));
    return true;
  }

  if (rpl_encryption.remove_remaining_seqnos_from_keyring()) return true;

  if (rpl_encryption.rotate_master_key()) return true;

  my_ok(m_thd);
  return false;
}

bool Reload_keyring::execute() {
  DBUG_TRACE;

  /* Check privileges */
  Security_context *sctx = m_thd->security_context();
  if (sctx->has_global_grant(STRING_WITH_LEN("ENCRYPTION_KEY_ADMIN")).first ==
      false) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "ENCRYPTION_KEY_ADMIN");
    return true;
  }

  if (srv_keyring_load->load(opt_plugin_dir, mysql_real_data_home) != 0) {
    /* We encountered an error. Figure out what it is. */
    my_error(ER_RELOAD_KEYRING_FAILURE, MYF(0));
    return true;
  }

  /*
    Persisted variables require keyring support to
    persist SENSITIVE variables in a secure manner.
  */
  persisted_variables_refresh_keyring_support();

  my_ok(m_thd);
  return false;
}

bool Rotate_redo_system_key::execute() {
  DBUG_TRACE;

  Security_context *sctx = m_thd->security_context();
  if (!sctx->check_access(SUPER_ACL) &&
      !sctx->has_global_grant(STRING_WITH_LEN("ENCRYPTION_KEY_ADMIN")).first) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
             "SUPER or ENCRYPTION_KEY_ADMIN");
    return true;
  }

  if (rotate_percona_system_key.rotate()) return true;

  my_ok(m_thd);
  return false;
}
