#ifndef MYSQLPP_UDF_EXCEPTION_HPP
#define MYSQLPP_UDF_EXCEPTION_HPP

#include <stdexcept>
#include <string>

namespace mysqlpp {

class udf_exception : public std::runtime_error {
 private:
  using error_code_t = int;
  static constexpr error_code_t error_code_sentinel = ~error_code_t{};

 public:
  explicit udf_exception(const std::string &what,
                         error_code_t error_code = error_code_sentinel)
      : std::runtime_error{what}, error_code_{error_code} {}

  bool has_error_code() const noexcept {
    return error_code_ != error_code_sentinel;
  }
  error_code_t get_error_code() const noexcept { return error_code_; }

 private:
  error_code_t error_code_;
};

}  // namespace mysqlpp

#endif
