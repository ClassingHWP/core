#include "chwp/container/zip.hpp"

#include <cstring>

#include "miniz.h"

namespace chwp::zip {

struct Archive::Impl {
    mz_zip_archive zip{};
    bool opened = false;

    ~Impl() {
        if (opened) mz_zip_reader_end(&zip);
    }
};

Archive::Archive() : impl_(std::make_unique<Impl>()) {}
Archive::~Archive() = default;

bool Archive::isZip(ByteView data) {
    return data.size() >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 3 && data[3] == 4;
}

bool Archive::open(ByteView data, std::string* error) {
    impl_ = std::make_unique<Impl>();
    entries_.clear();
    if (!mz_zip_reader_init_mem(&impl_->zip, data.data(), data.size(), 0)) {
        if (error) *error = std::string("zip open failed: ") +
                            mz_zip_get_error_string(mz_zip_get_last_error(&impl_->zip));
        return false;
    }
    impl_->opened = true;

    const mz_uint count = mz_zip_reader_get_num_files(&impl_->zip);
    entries_.reserve(count);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&impl_->zip, i, &stat)) continue;
        Entry entry;
        entry.name = stat.m_filename;
        entry.size = stat.m_uncomp_size;
        entry.compressedSize = stat.m_comp_size;
        entry.isDirectory = stat.m_is_directory != 0;
        entries_.push_back(std::move(entry));
    }
    return true;
}

bool Archive::contains(std::string_view name) const {
    for (const auto& e : entries_) {
        if (e.name == name) return true;
    }
    return false;
}

bool Archive::read(std::string_view name, Bytes& out, std::string* error) const {
    out.clear();
    if (!impl_->opened) {
        if (error) *error = "zip archive not opened";
        return false;
    }
    const std::string key(name);
    const int index = mz_zip_reader_locate_file(&impl_->zip, key.c_str(), nullptr, 0);
    if (index < 0) {
        if (error) *error = "zip entry not found: " + key;
        return false;
    }
    mz_zip_archive_file_stat stat{};
    if (mz_zip_reader_file_stat(&impl_->zip, static_cast<mz_uint>(index), &stat) && stat.m_uncomp_size > 0) {
        out.resize(static_cast<std::size_t>(stat.m_uncomp_size));
        if (mz_zip_reader_extract_to_mem(&impl_->zip, static_cast<mz_uint>(index), out.data(), out.size(), 0)) {
            return true;
        }
        out.clear();
    }
    std::size_t size = 0;
    void* buffer = mz_zip_reader_extract_to_heap(&impl_->zip, static_cast<mz_uint>(index), &size, 0);
    if (!buffer) {
        if (error) *error = "zip extract failed: " + key;
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    out.assign(bytes, bytes + size);
    mz_free(buffer);
    return true;
}

bool inflateRaw(ByteView input, Bytes& out) {
    out.clear();
    if (input.empty()) return true;
    std::size_t size = 0;
    void* buffer = tinfl_decompress_mem_to_heap(input.data(), input.size(), &size, 0);
    if (!buffer) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    out.assign(bytes, bytes + size);
    mz_free(buffer);
    return true;
}

bool deflateRaw(ByteView input, Bytes& out) {
    out.clear();
    static const std::uint8_t kEmpty = 0;
    const void* src = input.empty() ? static_cast<const void*>(&kEmpty) : static_cast<const void*>(input.data());
    std::size_t size = 0;
    void* buffer = tdefl_compress_mem_to_heap(src, input.size(), &size, TDEFL_DEFAULT_MAX_PROBES);
    if (!buffer) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    out.assign(bytes, bytes + size);
    mz_free(buffer);
    return true;
}

std::uint32_t crc32(ByteView input) {
    return static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, input.data(), input.size()));
}

struct Writer::Impl {
    mz_zip_archive zip{};
    bool initialized = false;
    bool finalized = false;

    Impl() {
        std::memset(&zip, 0, sizeof(zip));
        initialized = mz_zip_writer_init_heap(&zip, 0, 64 * 1024) != 0;
    }

    ~Impl() {
        if (initialized) {
            mz_zip_writer_end(&zip);
        }
    }
};

Writer::Writer() : impl_(std::make_unique<Impl>()) {}
Writer::~Writer() = default;

bool Writer::addFile(std::string_view name, ByteView data, bool compress, std::string* error) {
    if (!impl_ || !impl_->initialized || impl_->finalized) {
        if (error) *error = "zip writer not active";
        return false;
    }
    const std::string key(name);
    static const std::uint8_t kEmpty = 0;
    const void* src = data.empty() ? static_cast<const void*>(&kEmpty) : static_cast<const void*>(data.data());
    const mz_uint flags = compress ? static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION) : static_cast<mz_uint>(MZ_NO_COMPRESSION);
    if (!mz_zip_writer_add_mem(&impl_->zip, key.c_str(), src, data.size(), flags)) {
        if (error) *error = "zip addFile failed: " + key;
        return false;
    }
    return true;
}

bool Writer::addText(std::string_view name, std::string_view text, bool compress, std::string* error) {
    return addFile(name, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), compress, error);
}

bool Writer::finalize(Bytes& out, std::string* error) {
    out.clear();
    if (!impl_ || !impl_->initialized || impl_->finalized) {
        if (error) *error = "zip writer not active";
        return false;
    }
    void* buf = nullptr;
    std::size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&impl_->zip, &buf, &size)) {
        if (error) *error = "zip finalize failed";
        return false;
    }
    impl_->finalized = true;
    const auto* bytes = static_cast<const std::uint8_t*>(buf);
    out.assign(bytes, bytes + size);
    mz_free(buf);
    mz_zip_writer_end(&impl_->zip);
    impl_->initialized = false;
    return true;
}

}  // namespace chwp::zip
