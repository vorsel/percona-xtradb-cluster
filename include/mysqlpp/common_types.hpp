#ifndef MYSQLPP_COMMON_TYPES_HPP
#define MYSQLPP_COMMON_TYPES_HPP

#include <mysql/udf_registration_types.h>

#include <ext/optional.hpp>
#include <ext/string_view.hpp>

namespace mysqlpp {

using item_result_type = Item_result;

using optional_string_t = ext::string_view;
using optional_double_t = ext::optional<double>;
using optional_long_long_t = ext::optional<long long>;

}  // namespace mysqlpp

#endif
