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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA */

/* Test if a record has changed since last read */
/* In heap this is only used when debugging */

#include "heapdef.h"

int hp_rectest(HP_INFO *info, const uchar *old)
{
  DBUG_ENTER("hp_rectest");

  if (hp_process_record_data_to_chunkset(info->s, old,
                                         info->current_ptr,
                                         1))
  {
    set_my_errno(HA_ERR_RECORD_CHANGED);
    DBUG_RETURN(HA_ERR_RECORD_CHANGED); /* Record have changed */
  }
  DBUG_RETURN(0);
} /* _heap_rectest */
