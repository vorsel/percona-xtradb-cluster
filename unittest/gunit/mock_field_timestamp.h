/* Copyright (c) 2011, 2021, Oracle and/or its affiliates.

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

#ifndef MOCK_FIELD_TIMESTAMP_H
#define MOCK_FIELD_TIMESTAMP_H

#include "fake_table.h"
#include "field.h"

/*
  Strictly speaking not a mock class. Does not expect to be used in a
  certain way.

  Beware that the class creates manages its own TABLE instance.
*/
class Mock_field_timestamp : public Field_timestamp
{
  uchar null_byte;
  void initialize()
  {
    table = new Fake_TABLE(this);
    EXPECT_FALSE(table == NULL) << "Out of memory";
    ptr= buffer;
    memset(buffer, 0, PACK_LENGTH);
    set_null_ptr(&null_byte, 1);
  }

public:
  uchar buffer[PACK_LENGTH];
  bool store_timestamp_called;

  Mock_field_timestamp(Field::utype utype) :
    Field_timestamp(NULL, // ptr_arg
                    0,    // len_arg
                    NULL, // null_ptr_arg
                    '\0', // null_bit_arg
                    utype,// unireg_check_arg
                    ""),  // field_name_arg
    null_byte(0),
    store_timestamp_called(false)
  {
    initialize();
  }

  Mock_field_timestamp() :
    Field_timestamp(NULL, 0, NULL, '\0', NONE, ""),
    null_byte(0),
    store_timestamp_called(false)
  {
    initialize();
  }

  timeval to_timeval()
  {
    timeval tm;
    int warnings= 0;
    get_timestamp(&tm, &warnings);
    EXPECT_EQ(0, warnings);
    return tm;
  }

  /* Averts ASSERT_COLUMN_MARKED_FOR_WRITE assertion. */
  void make_writable() { bitmap_set_bit(table->write_set, field_index); }
  void make_readable() { bitmap_set_bit(table->read_set, field_index); }

  void store_timestamp(const timeval *tm)
  {
    make_writable();
    Field_temporal_with_date_and_time::store_timestamp(tm);
    store_timestamp_called= true;
  }

  ~Mock_field_timestamp() { delete static_cast<Fake_TABLE *>(table); }
};

#endif // MOCK_FIELD_TIMESTAMP_H
