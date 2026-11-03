#include <cstddef>
#include <expected>
#include <kanariv/media/source.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace kanariv::media {

namespace {

void closeDescriptor(int fd) noexcept {
#if defined(_WIN32)
    (void)_close(fd);
#else
    (void)::close(fd);
#endif
}

} // namespace

PathSource::PathSource(std::filesystem::path path)
    : path_(std::move(path)), displayName_(path_.string()) {}

auto PathSource::kind() const noexcept -> SourceKind {
    return SourceKind::path;
}

auto PathSource::displayName() const noexcept -> std::string_view {
    return displayName_;
}

auto PathSource::seekable() const noexcept -> bool {
    return true;
}

auto PathSource::open() -> kanariv::utils::Expected<std::unique_ptr<Reader>> {
    return FileReader::open(path_);
}

auto PathSource::path() const noexcept -> const std::filesystem::path& {
    return path_;
}

DescriptorSource::DescriptorSource(FileDescriptor descriptor, std::string displayName)
    : descriptor_(descriptor),
      displayName_(displayName.empty() ? "descriptor:" + std::to_string(descriptor.value)
                                       : std::move(displayName)) {}

DescriptorSource::DescriptorSource(DescriptorSource&& other) noexcept
    : descriptor_(other.descriptor_), displayName_(std::move(other.displayName_)),
      consumed_(other.consumed_) {
    other.descriptor_.value = -1;
    other.descriptor_.closeOnDestroy = false;
    other.consumed_ = true;
}

auto DescriptorSource::operator=(DescriptorSource&& other) noexcept -> DescriptorSource& {
    if (this != &other) {
        if (!consumed_ && descriptor_.closeOnDestroy && descriptor_.value >= 0) {
            closeDescriptor(descriptor_.value);
        }
        descriptor_ = other.descriptor_;
        displayName_ = std::move(other.displayName_);
        consumed_ = other.consumed_;
        other.descriptor_.value = -1;
        other.descriptor_.closeOnDestroy = false;
        other.consumed_ = true;
    }
    return *this;
}

DescriptorSource::~DescriptorSource() {
    if (!consumed_ && descriptor_.closeOnDestroy && descriptor_.value >= 0) {
        closeDescriptor(descriptor_.value);
    }
}

auto DescriptorSource::kind() const noexcept -> SourceKind {
    return SourceKind::descriptor;
}

auto DescriptorSource::displayName() const noexcept -> std::string_view {
    return displayName_;
}

auto DescriptorSource::seekable() const noexcept -> bool {
    return descriptor_.seekable;
}

auto DescriptorSource::open() -> kanariv::utils::Expected<std::unique_ptr<Reader>> {
    if (consumed_) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::State,
                                                     std::string_view("source already opened")});
    }
    auto reader = DescriptorReader::open(descriptor_);
    if (!reader) {
        return std::unexpected(reader.error());
    }
    consumed_ = true;
    descriptor_.value = -1;
    descriptor_.closeOnDestroy = false;
    return reader;
}

auto DescriptorSource::consumed() const noexcept -> bool {
    return consumed_;
}

} // namespace kanariv::media
