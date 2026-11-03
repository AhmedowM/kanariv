#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <string_view>

namespace kanariv::utils {

Error::Error(StatusCode c, std::string_view msg) : code(c), message(msg) {}

auto makeError(StatusCode c, std::string_view msg) -> Error {
    return Error{c, msg};
}

} // namespace kanariv::utils
