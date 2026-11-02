#include <kanariv/media/source.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <cstddef>
#include <expected>
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

SourceKind PathSource::kind() const noexcept {
    return SourceKind::path;
}

std::string_view PathSource::displayName() const noexcept {
    return displayName_;
}

bool PathSource::seekable() const noexcept {
    return true;
}

kanariv::utils::Expected<std::unique_ptr<Reader>> PathSource::open() {
    return FileReader::open(path_);
}

const std::filesystem::path& PathSource::path() const noexcept {
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

DescriptorSource& DescriptorSource::operator=(DescriptorSource&& other) noexcept {
    if (this != &other) {
        if (!consumed_ && descriptor_.closeOnDestroy && descriptor_.value >= 0)
            closeDescriptor(descriptor_.value);
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
    if (!consumed_ && descriptor_.closeOnDestroy && descriptor_.value >= 0)
        closeDescriptor(descriptor_.value);
}

SourceKind DescriptorSource::kind() const noexcept {
    return SourceKind::descriptor;
}

std::string_view DescriptorSource::displayName() const noexcept {
    return displayName_;
}

bool DescriptorSource::seekable() const noexcept {
    return descriptor_.seekable;
}

kanariv::utils::Expected<std::unique_ptr<Reader>> DescriptorSource::open() {
    if (consumed_) {
        return std::unexpected(kanariv::utils::Error{kanariv::utils::StatusCode::State,
                                                    std::string_view("source already opened")});
    }
    auto reader = DescriptorReader::open(descriptor_);
    if (!reader)
        return std::unexpected(reader.error());
    consumed_ = true;
    descriptor_.value = -1;
    descriptor_.closeOnDestroy = false;
    return reader;
}

bool DescriptorSource::consumed() const noexcept {
    return consumed_;
}

} // namespace kanariv::media
