#ifndef MYSQLPP_UDF_WRAPPERS_HPP
#define MYSQLPP_UDF_WRAPPERS_HPP

#include <cassert>
#include <cstring>
#include <string>
#include <type_traits>

#include <my_sys.h>
#include <mysql_com.h>
#include <mysqld_error.h>

#include <mysqlpp/common_types.hpp>
#include <mysqlpp/udf_context.hpp>
#include <mysqlpp/udf_exception.hpp>

// #include <boost/type_traits/make_void.hpp>

#ifdef WIN32
#define MYSQLPP_UDF_EXPORT extern "C" __declspec(dllexport)
#else
#define MYSQLPP_UDF_EXPORT extern "C"
#endif

namespace mysqlpp {

template <typename ImplType>
class generic_udf_base {
  // static_assert that ImplType has a constructor that accepts udf_context&
  // ImplType{std::devlval<udf_context&>()}
 public:
  static bool init(UDF_INIT *initid, UDF_ARGS *args, char *message) noexcept {
    udf_context udf_ctx{initid, args};
    ImplType *impl = nullptr;
    try {
      impl = new ImplType{udf_ctx};
    } catch (const std::exception &e) {
      std::strncpy(message, e.what(), MYSQL_ERRMSG_SIZE);
      message[MYSQL_ERRMSG_SIZE - 1] = '\0';
      return true;
    } catch (...) {
      std::strncpy(message, "unexpected exception", MYSQL_ERRMSG_SIZE);
      message[MYSQL_ERRMSG_SIZE - 1] = '\0';
      return true;
    }

    initid->ptr = reinterpret_cast<char *>(impl);

    return false;
  }

  static void deinit(UDF_INIT *initid) noexcept {
    delete get_impl_from_udf_initid(initid);
  }

 protected:
  static ImplType *get_impl_from_udf_initid(UDF_INIT *initid) noexcept {
    return reinterpret_cast<ImplType *>(initid->ptr);
  }
  static void handle_exception() noexcept {
    // TODO: change "<function_name>" to udf_traits<ImplType>::name()
    static constexpr const char *function_name = "<function_name>";
    try {
      throw;
    } catch (const udf_exception &e) {
      if (e.has_error_code())
        my_error(e.get_error_code(), MYF(0), function_name, e.what());
    } catch (const std::exception &e) {
      my_error(ER_UDF_ERROR, MYF(0), function_name, e.what());
    } catch (...) {
      my_error(ER_UDF_ERROR, MYF(0), function_name, "unexpected exception");
    }
  }
};

template <typename ImplType, item_result_type ItemResult>
class generic_udf;

template <typename ImplType>
class generic_udf<ImplType, STRING_RESULT> : public generic_udf_base<ImplType> {
  // static_assert that ImplType has a method calculate() that accepts const
  // udf_context& std::devlval<ImplType&>().calculate(std::devlval<const
  // udf_context&>())
 public:
  static char *func(UDF_INIT *initid, UDF_ARGS *args, char * /* result */,
                    unsigned long *length, unsigned char *is_null,
                    unsigned char *error) noexcept {
    auto &impl = *generic_udf_base<ImplType>::get_impl_from_udf_initid(initid);
    ext::string_view res;
    const udf_context udf_ctx{initid, args};
    try {
      res = impl.calculate(udf_ctx);
    } catch (...) {
      generic_udf_base<ImplType>::handle_exception();
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

template <typename ImplType>
class generic_udf<ImplType, REAL_RESULT> : public generic_udf_base<ImplType> {
  // static_assert that ImplType has a method calculate() that accepts const
  // udf_context& std::devlval<ImplType&>().calculate(std::devlval<const
  // udf_context&>())
 public:
  static double func(UDF_INIT *initid, UDF_ARGS *args, unsigned char *is_null,
                     unsigned char *error) noexcept {
    auto &impl = *generic_udf_base<ImplType>::get_impl_from_udf_initid(initid);
    optional_double_t res;
    const udf_context udf_ctx{initid, args};
    try {
      res = impl.calculate(udf_ctx);
    } catch (...) {
      generic_udf_base<ImplType>::handle_exception();
      *error = 1;
      return 0.0;
    }

    *error = 0;
    if (!res) {
      assert(udf_ctx.is_result_nullabale());
      *is_null = 1;
      return 0.0;
    } else {
      *is_null = 0;
      return res.get();
    }
  }
};

}  // namespace mysqlpp

#define DECLARE_UDF_INIT(IMPL, RESULT_TYPE, NAME)                             \
  MYSQLPP_UDF_EXPORT                                                          \
  bool NAME##_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {         \
    static_assert(std::is_same<decltype(&NAME##_init), Udf_func_init>::value, \
                  "Invalid UDF init function signature");                     \
    return mysqlpp::generic_udf<IMPL, RESULT_TYPE>::init(initid, args,        \
                                                         message);            \
  }

#define DECLARE_UDF_DEINIT(IMPL, RESULT_TYPE, NAME)                     \
  MYSQLPP_UDF_EXPORT                                                    \
  void NAME##_deinit(UDF_INIT *initid) {                                \
    static_assert(                                                      \
        std::is_same<decltype(&NAME##_deinit), Udf_func_deinit>::value, \
        "Invalid UDF deinit function signature");                       \
    mysqlpp::generic_udf<IMPL, RESULT_TYPE>::deinit(initid);            \
  }

#define DECLARE_UDF_STRING_FUNC(IMPL, NAME)                              \
  MYSQLPP_UDF_EXPORT                                                     \
  char *NAME(UDF_INIT *initid, UDF_ARGS *args, char *result,             \
             unsigned long *length, unsigned char *is_null,              \
             unsigned char *error) {                                     \
    static_assert(std::is_same<decltype(&NAME), Udf_func_string>::value, \
                  "Invalid string UDF function signature");              \
    return mysqlpp::generic_udf<IMPL, STRING_RESULT>::func(              \
        initid, args, result, length, is_null, error);                   \
  }

#define DECLARE_UDF_REAL_FUNC(IMPL, NAME)                                 \
  MYSQLPP_UDF_EXPORT                                                      \
  double NAME(UDF_INIT *initid, UDF_ARGS *args, unsigned char *is_null,   \
              unsigned char *error) {                                     \
    static_assert(std::is_same<decltype(&NAME), Udf_func_double>::value,  \
                  "Invalid real UDF function signature");                 \
    return mysqlpp::generic_udf<IMPL, REAL_RESULT>::func(initid, args,    \
                                                         is_null, error); \
  }

#define DECLARE_STRING_UDF(IMPL, NAME)        \
  DECLARE_UDF_INIT(IMPL, STRING_RESULT, NAME) \
  DECLARE_UDF_STRING_FUNC(IMPL, NAME)         \
  DECLARE_UDF_DEINIT(IMPL, STRING_RESULT, NAME)

#define DECLARE_REAL_UDF(IMPL, NAME)          \
  DECLARE_UDF_INIT(IMPL, STRING_RESULT, NAME) \
  DECLARE_UDF_REAL_FUNC(IMPL, NAME)           \
  DECLARE_UDF_DEINIT(IMPL, STRING_RESULT, NAME)

#endif
