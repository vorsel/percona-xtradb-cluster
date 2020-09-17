#ifndef MYSQLPP_UDF_WRAPPERS_HPP
#define MYSQLPP_UDF_WRAPPERS_HPP

#include <cassert>
#include <cstring>
#include <string>
#include <type_traits>

#include <mysql/udf_registration_types.h>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/utility/string_view.hpp>
// #include <boost/type_traits/make_void.hpp>

namespace ext {

// TODO: the following two 'using' directives can be changed to
//   using std::optional;
//   using std::string_view;
// when MySQL source code switches to c++17
using boost::optional;
using boost::string_view;

}  // namespace ext

#ifdef WIN32
#define MYSQLPP_UDF_EXPORT extern "C" __declspec(dllexport)
#else
#define MYSQLPP_UDF_EXPORT extern "C"
#endif

namespace mysqlpp {

using item_result_type = Item_result;

using optional_string_t = ext::optional<std::string>;
using optional_double_t = ext::optional<double>;
using optional_long_long_t = ext::optional<long long>;

class udf_context {
  template <typename CapsuleType>
  friend class generic_udf_base;

  template <typename CapsuleType, item_result_type ItemResult>
  friend class generic_udf;

 public:
  std::size_t get_number_of_args() const noexcept {
    return static_cast<std::size_t>(args_->arg_count);
  }
  item_result_type get_arg_type(std::size_t index) const noexcept {
    return args_->arg_type[index];
  }

  template <item_result_type ItemResult>
  auto get_arg(std::size_t index) const noexcept {
    // TODO: this function can be converted to 'if constexpr' chain instead of
    // tag dispatch when MySQL source code switches to c++17
    return get_arg_impl(index, item_result_tag<ItemResult>{});
  }

  ext::string_view get_attribute(std::size_t index) const noexcept {
    return {args_->attributes[index], args_->attribute_lengths[index]};
  }

  bool is_arg_nullable(std::size_t index) const noexcept {
    return args_->maybe_null[index] != 0;
  }

  bool is_result_nullabale() const noexcept { return initid_->maybe_null; }

  bool is_result_const() const noexcept { return initid_->const_item; }

  void set_arg_type(std::size_t index, item_result_type type) noexcept {
    args_->arg_type[index] = type;
  }

  void mark_arg_nullable(std::size_t index, bool nullable) noexcept {
    args_->maybe_null[index] = (nullable ? 1 : 0);
  }

  void mark_result_nullable(bool nullable) noexcept {
    initid_->maybe_null = nullable;
  }

  void mark_result_const(bool constant) noexcept {
    initid_->const_item = constant;
  }

 private:
  UDF_INIT *initid_;
  UDF_ARGS *args_;

  template <item_result_type ItemResult>
  using item_result_tag = std::integral_constant<item_result_type, ItemResult>;

  using string_result_tag = item_result_tag<STRING_RESULT>;
  using real_result_tag = item_result_tag<REAL_RESULT>;
  using int_result_tag = item_result_tag<INT_RESULT>;
  using decimal_result_tag = item_result_tag<DECIMAL_RESULT>;

  udf_context(UDF_INIT *initid, UDF_ARGS *args) noexcept
      : initid_{initid}, args_{args} {}

  auto get_arg_impl(std::size_t index, string_result_tag) const noexcept {
    assert(get_arg_type(index) == STRING_RESULT);
    return ext::string_view{args_->args[index], args_->lengths[index]};
  }
  auto get_arg_impl(std::size_t index, real_result_tag) const noexcept {
    assert(get_arg_type(index) == REAL_RESULT);
    if (args_->args[index] == nullptr) return optional_double_t{};
    double res;
    std::memcpy(&res, args_->args[index], sizeof res);
    return optional_double_t{res};
  }
  auto get_arg_impl(std::size_t index, int_result_tag) const noexcept {
    assert(get_arg_type(index) == INT_RESULT);
    if (args_->args[index] == nullptr) return optional_long_long_t{};
    long long res;
    std::memcpy(&res, args_->args[index], sizeof res);
    return optional_long_long_t{res};
  }
  auto get_arg_impl(std::size_t index, decimal_result_tag) const noexcept {
    assert(get_arg_type(index) == DECIMAL_RESULT);
    return ext::string_view{args_->args[index], args_->lengths[index]};
  }
};

template <typename CapsuleType>
class generic_udf_base {
  // static_assert that CapsuleType has a constructor that accepts udf_context&
  // CapsuleType{std::devlval<udf_context&>()}
 public:
  static bool init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
    udf_context udf_ctx{initid, args};
    CapsuleType *capsule = nullptr;
    try {
      capsule = new CapsuleType{udf_ctx};
    } catch (const std::exception &e) {
      // TODO: change to strncpy(MYSQL_ERRMSG_SIZE, ...)
      std::strcpy(message, e.what());
      return true;
    } catch (...) {
      // TODO: change to strncpy(MYSQL_ERRMSG_SIZE, ...)
      std::strcpy(message, "unexpected exception");
      return true;
    }

    initid->maybe_null = udf_ctx.is_result_nullabale();
    initid->const_item = udf_ctx.is_result_const();
    initid->ptr = reinterpret_cast<char *>(capsule);

    return false;
  }

  static void deinit(UDF_INIT *initid) {
    delete get_capsule_from_udf_initid(initid);
  }

 protected:
  static CapsuleType *get_capsule_from_udf_initid(UDF_INIT *initid) noexcept {
    return reinterpret_cast<CapsuleType *>(initid->ptr);
  }
};

template <typename CapsuleType, item_result_type ItemResult>
class generic_udf;

template <typename CapsuleType>
class generic_udf<CapsuleType, STRING_RESULT>
    : public generic_udf_base<CapsuleType> {
  // static_assert that CapsuleType has a method calculate() that accepts const
  // udf_context& std::devlval<CapsuleType&>().calculate(std::devlval<const
  // udf_context&>())
 public:
  static char *func(UDF_INIT *initid, UDF_ARGS *args, char * /* result */,
                    unsigned long *length, unsigned char *is_null,
                    unsigned char *error) {
    auto &capsule =
        *generic_udf_base<CapsuleType>::get_capsule_from_udf_initid(initid);
    ext::string_view res;
    const udf_context udf_ctx{initid, args};
    try {
      res = capsule.calculate(udf_ctx);
      //} catch (const udf_exception &e) {
      //  my_error(e.get_code(), MYF(0), e.what());
      //  *error = 1;
      //  return nullptr;
    } catch (const std::exception &e) {
      // TODO: set error message and code via my_error(ER_CODE, MYF(0),
      // "message");
      *error = 1;
      return nullptr;
    } catch (...) {
      // TODO: set error message and code via my_error(ER_CODE, MYF(0),
      // "message");
      *error = 1;
      return nullptr;
    }

    *error = 0;
    if (res.empty() && res.data() == nullptr) {
      assert(udf_ctx.is_result_nullabale());
      *is_null = 1;
      return nullptr;
    } else {
      *is_null = 0;
      *length = res.size();
      return const_cast<char *>(res.data());
    }
  }
};

}  // namespace mysqlpp

#endif

#define DECLARE_UDF_INIT(CAPSULE, RESULT_TYPE, NAME)                          \
  MYSQLPP_UDF_EXPORT                                                          \
  bool NAME##_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {         \
    static_assert(std::is_same<decltype(&NAME##_init), Udf_func_init>::value, \
                  "Invalid UDF init function signature");                     \
    return mysqlpp::generic_udf<CAPSULE, RESULT_TYPE>::init(initid, args,     \
                                                            message);         \
  }

#define DECLARE_UDF_DEINIT(CAPSULE, RESULT_TYPE, NAME)                  \
  MYSQLPP_UDF_EXPORT                                                    \
  void NAME##_deinit(UDF_INIT *initid) {                                \
    static_assert(                                                      \
        std::is_same<decltype(&NAME##_deinit), Udf_func_deinit>::value, \
        "Invalid UDF deinit function signature");                       \
    mysqlpp::generic_udf<CAPSULE, RESULT_TYPE>::deinit(initid);         \
  }

#define DECLARE_UDF_STRING_FUNC(CAPSULE, NAME)                           \
  MYSQLPP_UDF_EXPORT                                                     \
  char *NAME(UDF_INIT *initid, UDF_ARGS *args, char *result,             \
             unsigned long *length, unsigned char *is_null,              \
             unsigned char *error) {                                     \
    static_assert(std::is_same<decltype(&NAME), Udf_func_string>::value, \
                  "Invalid string UDF function signature");              \
    return mysqlpp::generic_udf<CAPSULE, STRING_RESULT>::func(           \
        initid, args, result, length, is_null, error);                   \
  }

#define DECLARE_STRING_UDF(CAPSULE, NAME)        \
  DECLARE_UDF_INIT(CAPSULE, STRING_RESULT, NAME) \
  DECLARE_UDF_STRING_FUNC(CAPSULE, NAME)         \
  DECLARE_UDF_DEINIT(CAPSULE, STRING_RESULT, NAME)
