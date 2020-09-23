#ifndef EXT_STRING_VIEW_HPP
#define EXT_STRING_VIEW_HPP

#include <boost/utility/string_view.hpp>

namespace ext {

// TODO: the following 'using' directives can be changed to
//   using std::string_view;
// once MySQL source code switches to c++17
using boost::string_view;

}  // namespace ext

#endif
