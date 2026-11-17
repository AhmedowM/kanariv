#include <kanariv/utils/error.hpp>
#include <kanariv/utils/status_format.hpp>
#include <cassert>
#include <string_view>

namespace kanariv::utils {

Error::Error(StatusCode c, std::string_view msg) : code(c), message(msg) {
    assert(c != StatusCode::Ok);
}

Error makeError(StatusCode c, std::string_view msg) {
    return Error{c, msg};
}

} // namespace kanariv::utils
