#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <string_view>

namespace kanariv::utils {

Error::Error(StatusCode c, std::string_view msg) : code(c), message(msg) {}

Error makeError(StatusCode c, std::string_view msg) {
    return Error{c, msg};
}

} // namespace kanariv::utils
