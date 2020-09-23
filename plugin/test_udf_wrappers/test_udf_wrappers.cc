#include <stdexcept>
#include <string>

#include <mysqlpp/udf_wrappers.hpp>

namespace {

//
// GET_BINLOG_BY_GTID()
// This MySQL function accepts a GTID and returns the name of the binlog file
// that contains this GTID.
//
class wrapped_udf_string_impl {
 public:
  wrapped_udf_string_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() == 2)
      throw mysqlpp::udf_exception("test udf_exception with sentinel");
    if (ctx.get_number_of_args() == 3)
      throw mysqlpp::udf_exception("test udf_exception without sentinel",
                                   ER_WRAPPED_UDF_EXCEPTION);
    if (ctx.get_number_of_args() == 4) throw 42;

    if (ctx.get_number_of_args() != 1)
      throw std::invalid_argument("function requires exactly one argument");
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    ctx.mark_arg_nullable(0, true);
    ctx.set_arg_type(0, STRING_RESULT);
  }
  ~wrapped_udf_string_impl() {}

  ext::string_view calculate(const mysqlpp::udf_context &ctx) {
    auto arg_sv = ctx.get_arg<STRING_RESULT>(0);
    if (arg_sv.data() == nullptr) return {};
    if (arg_sv == "100") {
      my_error(ER_DA_OOM, MYF(0));
      throw mysqlpp::udf_exception("test udf_exception with sentinel");
    }
    if (arg_sv == "101")
      throw mysqlpp::udf_exception("test udf_exception without sentinel",
                                   ER_WRAPPED_UDF_EXCEPTION);
    if (arg_sv == "102") throw std::runtime_error("test runtime_error");
    if (arg_sv == "103") throw 42;

    result_ = '[';
    result_.append(arg_sv.data(), arg_sv.size());
    result_ += ']';

    return {result_};
  }

 private:
  std::string result_;
};

class wrapped_udf_real_impl {
 public:
  wrapped_udf_real_impl(mysqlpp::udf_context &ctx) {
    if (ctx.get_number_of_args() != 1)
      throw std::invalid_argument("function requires exactly one argument");
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    ctx.set_result_decimals_not_fixed();
    ctx.mark_arg_nullable(0, true);
    ctx.set_arg_type(0, REAL_RESULT);
  }
  ~wrapped_udf_real_impl() {}

  mysqlpp::optional_double_t calculate(const mysqlpp::udf_context &ctx) {
    auto arg_opt = ctx.get_arg<REAL_RESULT>(0);
    if (!arg_opt) return {};

    if (arg_opt.get() == 100.0) {
      my_error(ER_DA_OOM, MYF(0));
      throw mysqlpp::udf_exception("test udf_exception with sentinel");
    }
    if (arg_opt.get() == 101.0)
      throw mysqlpp::udf_exception("test udf_exception without sentinel",
                                   ER_WRAPPED_UDF_EXCEPTION);
    if (arg_opt.get() == 102.0) throw std::runtime_error("test runtime_error");
    if (arg_opt.get() == 103.0) throw 42;

    return arg_opt.get() + 0.25;
  }
};

}  // end of anonymous namespace

DECLARE_STRING_UDF(wrapped_udf_string_impl, wrapped_udf_string)
DECLARE_REAL_UDF(wrapped_udf_real_impl, wrapped_udf_real)
