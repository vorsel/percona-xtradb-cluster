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

#include "heapdef.h"

	/* Read first record with the current key */


int heap_rlast(HP_INFO *info, uchar *record, int inx)
{
  HP_SHARE *share=    info->s;
  HP_KEYDEF *keyinfo= share->keydef + inx;

  DBUG_ENTER("heap_rlast");
  info->lastinx= inx;
  if (keyinfo->algorithm == HA_KEY_ALG_BTREE)
  {
    uchar *pos;

    if ((pos = tree_search_edge(&keyinfo->rb_tree, info->parents,
                                &info->last_pos, offsetof(TREE_ELEMENT, right))))
    {
      memcpy(&pos, pos + (*keyinfo->get_key_length)(keyinfo, pos), 
	     sizeof(uchar*));
      info->current_ptr = pos;
      if (hp_extract_record(info, record, pos))
      {
        DBUG_RETURN(my_errno());
      }
      info->update = HA_STATE_AKTIV;
    }
    else
    {
      set_my_errno(HA_ERR_END_OF_FILE);
      DBUG_RETURN(my_errno());
    }
    DBUG_RETURN(0);
  }
  else
  {
    info->current_ptr=0;
    info->current_hash_ptr=0;
    info->update=HA_STATE_NEXT_FOUND;
    DBUG_RETURN(heap_rprev(info,record));
  }
}
