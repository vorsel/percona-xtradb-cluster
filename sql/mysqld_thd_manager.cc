/* Copyright (c) 2000, 2021, Oracle and/or its affiliates.

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

#include "mysqld_thd_manager.h"

#include "mysql/thread_pool_priv.h"  // inc_thread_created
#include "mutex_lock.h"              // Mutex_lock
#include "debug_sync.h"              // DEBUG_SYNC_C
#include "sql_class.h"               // THD
#ifdef WITH_WSREP
#include "wsrep_sst.h"
#endif /* WITH_WSREP */

#include <functional>
#include <algorithm>

int Global_THD_manager::global_thd_count= 0;
Global_THD_manager *Global_THD_manager::thd_manager = NULL;

/**
  Internal class used in do_for_all_thd() and do_for_all_thd_copy()
  implementation.
*/

class Do_THD : public std::unary_function<THD*, void>
{
public:
  explicit Do_THD(Do_THD_Impl *impl) : m_impl(impl) {}

  /**
    Users of this class will override operator() in the _Impl class.

    @param thd THD of one element in global thread list
  */
  void operator()(THD* thd)
  {
    m_impl->operator()(thd);
  }
private:
  Do_THD_Impl *m_impl;
};


/**
  Internal class used in find_thd() implementation.
*/

class Find_THD : public std::unary_function<THD*, bool>
{
public:
  explicit Find_THD(Find_THD_Impl *impl) : m_impl(impl) {}

  bool operator()(THD* thd)
  {
    return m_impl->operator()(thd);
  }
private:
  Find_THD_Impl *m_impl;
};

#ifdef WITH_WSREP
Do_THD_Impl::Do_THD_Impl()
{
  /* Empty constructor */
}
#endif /* WITH_WSREP */


#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_thd_list;
static PSI_mutex_key key_LOCK_thd_remove;
static PSI_mutex_key key_LOCK_thread_ids;

static PSI_mutex_info all_thd_manager_mutexes[]=
{
  { &key_LOCK_thd_list, "LOCK_thd_list", PSI_FLAG_GLOBAL},
  { &key_LOCK_thd_remove, "LOCK_thd_remove", PSI_FLAG_GLOBAL},
  { &key_LOCK_thread_ids, "LOCK_thread_ids", PSI_FLAG_GLOBAL }
};

static PSI_cond_key key_COND_thd_list;

static PSI_cond_info all_thd_manager_conds[]=
{
  { &key_COND_thd_list, "COND_thd_list", PSI_FLAG_GLOBAL}
};
#endif // HAVE_PSI_INTERFACE


const my_thread_id Global_THD_manager::reserved_thread_id= 0;

Global_THD_manager::Global_THD_manager()
  : thd_list(PSI_INSTRUMENT_ME),
    thread_ids(PSI_INSTRUMENT_ME),
    num_thread_running(0),
    thread_created(0),
    thread_id_counter(reserved_thread_id + 1),
    unit_test(false)
{
#ifdef HAVE_PSI_INTERFACE
  int count= array_elements(all_thd_manager_mutexes);
  mysql_mutex_register("sql", all_thd_manager_mutexes, count);

  count= array_elements(all_thd_manager_conds);
  mysql_cond_register("sql", all_thd_manager_conds, count);
#endif

  mysql_mutex_init(key_LOCK_thd_list, &LOCK_thd_list,
                   MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_thd_remove,
                   &LOCK_thd_remove, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_thread_ids,
                   &LOCK_thread_ids, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_thd_list, &COND_thd_list);

  // The reserved thread ID should never be used by normal threads,
  // so mark it as in-use. This ID is used by temporary THDs never
  // added to the list of THDs.
  thread_ids.push_back(reserved_thread_id);
}


#ifdef WITH_WSREP
class Print_conn : public Do_THD_Impl
{
public:
  Print_conn() { }

  virtual void operator()(THD *thd)
  {
    WSREP_INFO("THD %u applier %s exec_mode %s killed %s",
               thd->thread_id(),
               thd->wsrep_applier ? "true" : "false",
               wsrep_get_exec_mode(thd->wsrep_exec_mode),
               thd->killed ? "true" : "false");
  }
};
#endif /* WITH_WSREP */

Global_THD_manager::~Global_THD_manager()
{
  thread_ids.erase_unique(reserved_thread_id);
#ifdef WITH_WSREP
  if (!(thd_list.empty()))
  {
    Print_conn print_conn;
    do_for_all_thd(&print_conn);
  }
#endif /* WITH_WSREP */
  assert(thd_list.empty());
  assert(thread_ids.empty());
  mysql_mutex_destroy(&LOCK_thd_list);
  mysql_mutex_destroy(&LOCK_thd_remove);
  mysql_mutex_destroy(&LOCK_thread_ids);
  mysql_cond_destroy(&COND_thd_list);
}


/*
  Singleton Instance creation
  This method do not require mutex guard as it is called only from main thread.
*/
bool Global_THD_manager::create_instance()
{
  if (thd_manager == NULL)
    thd_manager= new (std::nothrow) Global_THD_manager();
  return (thd_manager == NULL);
}


void Global_THD_manager::destroy_instance()
{
  delete thd_manager;
  thd_manager= NULL;
}


void Global_THD_manager::add_thd(THD *thd)
{
  DBUG_PRINT("info", ("Global_THD_manager::add_thd %p", thd));
  // Should have an assigned ID before adding to the list.
  assert(thd->thread_id() != reserved_thread_id);
  mysql_mutex_lock(&LOCK_thd_list);
  // Technically it is not supported to compare pointers, but it works.
  std::pair<THD_array::iterator, bool> insert_result=
    thd_list.insert_unique(thd);
  if (insert_result.second)
  {
    ++global_thd_count;
  }
#ifdef WITH_WSREP
  if (WSREP_ON && thd->wsrep_applier)
  {
    wsrep_running_threads++;
    WSREP_DEBUG("wsrep running threads now: %lu", wsrep_running_threads);
  }
#endif /* WITH_WSREP */
  // Adding the same THD twice is an error.
  assert(insert_result.second);
  mysql_mutex_unlock(&LOCK_thd_list);
}


void Global_THD_manager::remove_thd(THD *thd)
{
  DBUG_PRINT("info", ("Global_THD_manager::remove_thd %p", thd));
  mysql_mutex_lock(&LOCK_thd_remove);
  mysql_mutex_lock(&LOCK_thd_list);

  if (!unit_test)
    assert(thd->release_resources_done());

  /*
    Used by binlog_reset_master.  It would be cleaner to use
    DEBUG_SYNC here, but that's not possible because the THD's debug
    sync feature has been shut down at this point.
  */
  DBUG_EXECUTE_IF("sleep_after_lock_thread_count_before_delete_thd", sleep(5););

  const size_t num_erased= thd_list.erase_unique(thd);
  if (num_erased == 1)
    --global_thd_count;
  // Removing a THD that was never added is an error.
  assert(1 == num_erased);
#ifdef WITH_WSREP
  if (WSREP_ON && thd->wsrep_applier)
  {
    wsrep_running_threads--;
    WSREP_DEBUG("wsrep running threads now: %lu", wsrep_running_threads);
  }
#endif /* WITH_WSREP */
  mysql_mutex_unlock(&LOCK_thd_remove);
  mysql_cond_broadcast(&COND_thd_list);
  mysql_mutex_unlock(&LOCK_thd_list);
}


my_thread_id Global_THD_manager::get_new_thread_id()
{
  my_thread_id new_id;
  Mutex_lock lock(&LOCK_thread_ids);
  do {
    new_id= thread_id_counter++;
  } while (!thread_ids.insert_unique(new_id).second);
  return new_id;
}


void Global_THD_manager::release_thread_id(my_thread_id thread_id)
{
  if (thread_id == reserved_thread_id)
    return; // Some temporary THDs are never given a proper ID.
  Mutex_lock lock(&LOCK_thread_ids);
  const size_t num_erased MY_ATTRIBUTE((unused))=
    thread_ids.erase_unique(thread_id);
  // Assert if the ID was not found in the list.
  assert(1 == num_erased);
}


void Global_THD_manager::set_thread_id_counter(my_thread_id new_id)
{
  assert(unit_test == true);
  Mutex_lock lock(&LOCK_thread_ids);
  thread_id_counter= new_id;
}


void Global_THD_manager::wait_till_no_thd()
{
  mysql_mutex_lock(&LOCK_thd_list);
  while (get_thd_count() > 0)
  {
    mysql_cond_wait(&COND_thd_list, &LOCK_thd_list);
    DBUG_PRINT("quit", ("One thread died (count=%u)", get_thd_count()));
  }
  mysql_mutex_unlock(&LOCK_thd_list);
}

#ifdef WITH_WSREP
void Global_THD_manager::wait_till_wsrep_thd_eq(Do_THD_Impl* func,
                                                int threshold_count)
{
  Do_THD doit(func);

  mysql_mutex_lock(&LOCK_thd_list);
  while (true)
  {
    func->reset();

    std::for_each(thd_list.begin(), thd_list.end(), doit);

    /* Check if the exit condition is true based on evaluator execution. */
    if (func->done(threshold_count))
      break;

    mysql_cond_wait(&COND_thd_list, &LOCK_thd_list);
    DBUG_PRINT("quit", ("One thread died (count=%u)", get_thd_count()));
  }
  mysql_mutex_unlock(&LOCK_thd_list);
}
#endif /* WITH_WSREP */

void Global_THD_manager::do_for_all_thd_copy(Do_THD_Impl *func)
{
  Do_THD doit(func);

  mysql_mutex_lock(&LOCK_thd_remove);
  mysql_mutex_lock(&LOCK_thd_list);

  /* Take copy of global_thread_list. */
  THD_array thd_list_copy(thd_list);

  /*
    Allow inserts to global_thread_list. Newly added thd
    will not be accounted for when executing func.
  */
  mysql_mutex_unlock(&LOCK_thd_list);

  /* Execute func for all existing threads. */
  std::for_each(thd_list_copy.begin(), thd_list_copy.end(), doit);

  DEBUG_SYNC_C("inside_do_for_all_thd_copy");
  mysql_mutex_unlock(&LOCK_thd_remove);
}


void Global_THD_manager::do_for_all_thd(Do_THD_Impl *func)
{
  Do_THD doit(func);
  mysql_mutex_lock(&LOCK_thd_list);
  std::for_each(thd_list.begin(), thd_list.end(), doit);
  mysql_mutex_unlock(&LOCK_thd_list);
}


THD* Global_THD_manager::find_thd(Find_THD_Impl *func)
{
  Find_THD find_thd(func);
  mysql_mutex_lock(&LOCK_thd_list);
  THD_array::const_iterator it=
    std::find_if(thd_list.begin(), thd_list.end(), find_thd);
  THD* ret= NULL;
  if (it != thd_list.end())
    ret= *it;
  mysql_mutex_unlock(&LOCK_thd_list);
  return ret;
}


void inc_thread_created()
{
  Global_THD_manager::get_instance()->inc_thread_created();
}


void thd_lock_thread_count(THD *)
{
  mysql_mutex_lock(&Global_THD_manager::get_instance()->LOCK_thd_list);
}


void thd_unlock_thread_count(THD *)
{
  Global_THD_manager *thd_manager= Global_THD_manager::get_instance();
  mysql_cond_broadcast(&thd_manager->COND_thd_list);
  mysql_mutex_unlock(&thd_manager->LOCK_thd_list);
}


template <typename T>
class Run_free_function : public Do_THD_Impl
{
public:
  typedef void (do_thd_impl)(THD*, T);

  Run_free_function(do_thd_impl *f, T arg) : m_func(f), m_arg(arg) {}

  virtual void operator()(THD *thd)
  {
    (*m_func)(thd, m_arg);
  }
private:
  do_thd_impl *m_func;
  T m_arg;
};


void do_for_all_thd(do_thd_impl_uint64 f, uint64 v)
{
  Run_free_function<uint64> runner(f, v);
  Global_THD_manager::get_instance()->do_for_all_thd(&runner);
}
