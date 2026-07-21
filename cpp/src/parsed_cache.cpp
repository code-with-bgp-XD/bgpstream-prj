#include "bgpstream_runner/parsed_cache.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "bgpstream_runner/common.h"

namespace bgpstream_runner {

namespace {

constexpr std::array<char, 8> kCacheMagic{{'B', 'G', 'P', 'C', 'A', 'C', 'H', '1'}};
constexpr std::uint32_t kCodecNone = 0;
constexpr std::uint32_t kCodecZstd = 1;
constexpr std::uint32_t kOrderingTimestampStable = 1;
constexpr std::uint32_t kBlockMarker = 0x314b4c42U;   // BLK1 in little endian.
constexpr std::uint32_t kFooterMarker = 0x31544f46U;  // FOT1 in little endian.
constexpr std::uint64_t kMaximumBlockBytes = std::uint64_t{1} << 30;
constexpr std::uint32_t kMaximumStringBytes = std::uint32_t{1} << 26;
constexpr std::uint32_t kMaximumVectorElements = std::uint32_t{1} << 24;
constexpr int kZstdCompressionLevel = 1;

std::string normalized_path_text(const std::filesystem::path &path) {
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::int64_t file_mtime_ticks(const std::filesystem::path &path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) {
        throw ParsedCacheFailure("Failed to read source modification time " + path.string() + ": " +
                                 error.message());
    }
    const auto count = time.time_since_epoch().count();
    using Count = std::remove_cv_t<decltype(count)>;
    if constexpr (std::is_signed_v<Count>) {
        if constexpr (std::numeric_limits<Count>::digits >
                      std::numeric_limits<std::int64_t>::digits) {
            if (count < static_cast<Count>(std::numeric_limits<std::int64_t>::min()) ||
                count > static_cast<Count>(std::numeric_limits<std::int64_t>::max())) {
                throw ParsedCacheFailure("Source modification time is outside the cache format range: " +
                                         path.string());
            }
        }
    } else if (count > static_cast<Count>(std::numeric_limits<std::int64_t>::max())) {
        throw ParsedCacheFailure("Source modification time is outside the cache format range: " +
                                 path.string());
    }
    return static_cast<std::int64_t>(count);
}

std::uint64_t require_source_size(const std::filesystem::path &path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        throw ParsedCacheFailure("Required source MRT file is missing: " + path.string());
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ParsedCacheFailure("Failed to read source size " + path.string() + ": " + error.message());
    }
    return static_cast<std::uint64_t>(size);
}

void require_output(std::ostream &output, const std::filesystem::path &path) {
    if (!output) {
        throw ParsedCacheFailure("Failed to write parsed cache: " + path.string());
    }
}

void write_bytes(std::ostream &output, const void *data, std::size_t size,
                 const std::filesystem::path &path) {
    output.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    require_output(output, path);
}

void write_u32(std::ostream &output, std::uint32_t value, const std::filesystem::path &path) {
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU);
    }
    write_bytes(output, bytes.data(), bytes.size(), path);
}

void write_u64(std::ostream &output, std::uint64_t value, const std::filesystem::path &path) {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU);
    }
    write_bytes(output, bytes.data(), bytes.size(), path);
}

void write_i64(std::ostream &output, std::int64_t value, const std::filesystem::path &path) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    write_u64(output, bits, path);
}

void write_string(std::ostream &output, const std::string &value, const std::filesystem::path &path) {
    if (value.size() > kMaximumStringBytes) {
        throw ParsedCacheFailure("Cache header string is too large for " + path.string());
    }
    write_u32(output, static_cast<std::uint32_t>(value.size()), path);
    write_bytes(output, value.data(), value.size(), path);
}

void read_exact(std::istream &input, void *data, std::size_t size, const std::filesystem::path &path) {
    input.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        throw ParsedCacheFailure("Parsed cache is truncated: " + path.string());
    }
}

std::uint32_t read_u32(std::istream &input, const std::filesystem::path &path) {
    std::array<std::uint8_t, 4> bytes{};
    read_exact(input, bytes.data(), bytes.size(), path);
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
    }
    return value;
}

std::uint64_t read_u64(std::istream &input, const std::filesystem::path &path) {
    std::array<std::uint8_t, 8> bytes{};
    read_exact(input, bytes.data(), bytes.size(), path);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    }
    return value;
}

std::int64_t read_i64(std::istream &input, const std::filesystem::path &path) {
    const std::uint64_t bits = read_u64(input, path);
    std::int64_t value = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string read_string(std::istream &input, const std::filesystem::path &path) {
    const std::uint32_t size = read_u32(input, path);
    if (size > kMaximumStringBytes) {
        throw ParsedCacheFailure("Parsed cache contains an oversized string: " + path.string());
    }
    std::string value(size, '\0');
    read_exact(input, value.data(), value.size(), path);
    return value;
}

class ByteWriter {
   public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }

    void u16(std::uint16_t value) {
        for (std::size_t index = 0; index < 2; ++index) {
            u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU));
        }
    }

    void u32(std::uint32_t value) {
        for (std::size_t index = 0; index < 4; ++index) {
            u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU));
        }
    }

    void u64(std::uint64_t value) {
        for (std::size_t index = 0; index < 8; ++index) {
            u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU));
        }
    }

    void i64(std::int64_t value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }

    void boolean(bool value) { u8(value ? 1 : 0); }

    void string(const std::string &value) {
        if (value.size() > kMaximumStringBytes) {
            throw ParsedCacheFailure("BGP message string exceeds parsed-cache format limit");
        }
        u32(static_cast<std::uint32_t>(value.size()));
        append(value.data(), value.size());
    }

    void append(const void *data, std::size_t size) {
        const auto *begin = static_cast<const std::uint8_t *>(data);
        bytes_.insert(bytes_.end(), begin, begin + size);
    }

    const std::vector<std::uint8_t> &bytes() const noexcept { return bytes_; }
    std::size_t size() const noexcept { return bytes_.size(); }
    void clear() noexcept { bytes_.clear(); }

   private:
    std::vector<std::uint8_t> bytes_;
};

class ByteReader {
   public:
    ByteReader(const std::uint8_t *data, std::size_t size, std::string context)
        : data_(data), size_(size), context_(std::move(context)) {}

    std::uint8_t u8() {
        require(1);
        return data_[position_++];
    }

    std::uint16_t u16() {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 2; ++index) {
            value |= static_cast<std::uint32_t>(u8()) << (index * 8);
        }
        return static_cast<std::uint16_t>(value);
    }

    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            value |= static_cast<std::uint32_t>(u8()) << (index * 8);
        }
        return value;
    }

    std::uint64_t u64() {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 8; ++index) {
            value |= static_cast<std::uint64_t>(u8()) << (index * 8);
        }
        return value;
    }

    std::int64_t i64() {
        const std::uint64_t bits = u64();
        std::int64_t value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool boolean() {
        const std::uint8_t value = u8();
        if (value > 1) {
            fail("invalid boolean");
        }
        return value != 0;
    }

    std::string string(bool materialize) {
        const std::uint32_t size = u32();
        if (size > kMaximumStringBytes) {
            fail("oversized string");
        }
        require(size);
        std::string value;
        if (materialize) {
            value.assign(reinterpret_cast<const char *>(data_ + position_), size);
        }
        position_ += size;
        return value;
    }

    ByteReader subreader(std::size_t size, std::string suffix) {
        require(size);
        ByteReader result(data_ + position_, size, context_ + std::move(suffix));
        position_ += size;
        return result;
    }

    std::size_t remaining() const noexcept { return size_ - position_; }
    bool empty() const noexcept { return remaining() == 0; }

    void require_empty() const {
        if (!empty()) {
            fail("unexpected trailing bytes");
        }
    }

   private:
    void require(std::size_t size) const {
        if (size > remaining()) {
            fail("truncated payload");
        }
    }

    [[noreturn]] void fail(const std::string &reason) const {
        throw ParsedCacheFailure("Invalid parsed cache " + context_ + ": " + reason);
    }

    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
    std::string context_;
};

std::uint64_t checksum64(const std::vector<std::uint8_t> &bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

class ZstdApi {
   public:
    static const ZstdApi &instance() {
        static const ZstdApi api;
        return api;
    }

    bool available() const noexcept { return handle_ != nullptr; }

    std::vector<std::uint8_t> compress(const std::vector<std::uint8_t> &input) const {
        if (!available()) {
            throw ParsedCacheFailure("Zstd runtime is unavailable");
        }
        std::vector<std::uint8_t> output(compress_bound_(input.size()));
        const std::size_t result =
            compress_(output.data(), output.size(), input.data(), input.size(), kZstdCompressionLevel);
        check(result, "compress");
        output.resize(result);
        return output;
    }

    std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t> &input,
                                         std::size_t output_size) const {
        if (!available()) {
            throw ParsedCacheFailure("This parsed cache uses Zstd, but libzstd.so.1 is unavailable");
        }
        std::vector<std::uint8_t> output(output_size);
        const std::size_t result = decompress_(output.data(), output.size(), input.data(), input.size());
        check(result, "decompress");
        if (result != output_size) {
            throw ParsedCacheFailure("Zstd block decompressed to an unexpected size");
        }
        return output;
    }

   private:
    using CompressBound = std::size_t (*)(std::size_t);
    using Compress = std::size_t (*)(void *, std::size_t, const void *, std::size_t, int);
    using Decompress = std::size_t (*)(void *, std::size_t, const void *, std::size_t);
    using IsError = unsigned (*)(std::size_t);
    using ErrorName = const char *(*)(std::size_t);

    template <typename Function>
    bool load(Function *function, const char *name) {
        void *symbol = dlsym(handle_, name);
        if (symbol == nullptr) {
            return false;
        }
        static_assert(sizeof(symbol) == sizeof(*function));
        std::memcpy(function, &symbol, sizeof(symbol));
        return true;
    }

    ZstdApi() {
        handle_ = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
        if (handle_ == nullptr) {
            handle_ = dlopen("libzstd.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (handle_ == nullptr) {
            return;
        }
        if (!load(&compress_bound_, "ZSTD_compressBound") || !load(&compress_, "ZSTD_compress") ||
            !load(&decompress_, "ZSTD_decompress") || !load(&is_error_, "ZSTD_isError") ||
            !load(&error_name_, "ZSTD_getErrorName")) {
            dlclose(handle_);
            handle_ = nullptr;
        }
    }

    ~ZstdApi() {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    void check(std::size_t result, const char *operation) const {
        if (is_error_(result) != 0U) {
            throw ParsedCacheFailure(std::string("Failed to ") + operation + " parsed-cache block: " +
                                     error_name_(result));
        }
    }

    void *handle_ = nullptr;
    CompressBound compress_bound_ = nullptr;
    Compress compress_ = nullptr;
    Decompress decompress_ = nullptr;
    IsError is_error_ = nullptr;
    ErrorName error_name_ = nullptr;
};

void encode_message(const BGPMessage &message, ByteWriter *output) {
    output->u8(static_cast<std::uint8_t>(message.type));
    output->u8(static_cast<std::uint8_t>(message.record_type));
    output->u8(static_cast<std::uint8_t>(message.record_status));
    output->i64(static_cast<std::int64_t>(message.timestamp));
    output->u32(message.timestamp_microseconds);
    output->string(message.project_name);
    output->string(message.collector_name);
    output->string(message.router_name);
    output->string(message.router_ip);
    output->u8(static_cast<std::uint8_t>(message.dump_position));
    output->i64(static_cast<std::int64_t>(message.dump_timestamp));
    // source_file belongs to the cache header and is restored from the current source path.
    output->u64(message.record_index);
    output->u64(message.element_index);
    output->i64(static_cast<std::int64_t>(message.originated_timestamp));
    output->u32(message.originated_timestamp_microseconds);
    output->string(message.peer_ip);
    output->u32(message.peer_asn);
    output->string(message.prefix);
    output->string(message.next_hop);
    output->boolean(message.has_as_path);
    output->string(message.as_path);

    if (message.as_path_segments.size() > kMaximumVectorElements) {
        throw ParsedCacheFailure("AS path contains too many segments");
    }
    output->u32(static_cast<std::uint32_t>(message.as_path_segments.size()));
    for (const ASPathSegment &segment : message.as_path_segments) {
        output->u8(static_cast<std::uint8_t>(segment.type));
        if (segment.asns.size() > kMaximumVectorElements) {
            throw ParsedCacheFailure("AS path segment contains too many ASNs");
        }
        output->u32(static_cast<std::uint32_t>(segment.asns.size()));
        for (const std::uint32_t asn : segment.asns) {
            output->u32(asn);
        }
    }

    if (message.asns.size() > kMaximumVectorElements) {
        throw ParsedCacheFailure("Flattened AS path contains too many ASNs");
    }
    output->u32(static_cast<std::uint32_t>(message.asns.size()));
    for (const std::uint32_t asn : message.asns) {
        output->u32(asn);
    }
    output->boolean(message.origin_asn.has_value());
    if (message.origin_asn.has_value()) {
        output->u32(*message.origin_asn);
    }

    output->boolean(message.has_communities);
    if (message.communities.size() > kMaximumVectorElements) {
        throw ParsedCacheFailure("BGP message contains too many communities");
    }
    output->u32(static_cast<std::uint32_t>(message.communities.size()));
    for (const BGPCommunity &community : message.communities) {
        output->u16(community.asn);
        output->u16(community.value);
    }

    output->boolean(message.origin.has_value());
    if (message.origin.has_value()) {
        output->u8(static_cast<std::uint8_t>(*message.origin));
    }
    output->boolean(message.med.has_value());
    if (message.med.has_value()) {
        output->u32(*message.med);
    }
    output->boolean(message.local_pref.has_value());
    if (message.local_pref.has_value()) {
        output->u32(*message.local_pref);
    }
    output->boolean(message.atomic_aggregate);
    output->boolean(message.aggregator.has_value());
    if (message.aggregator.has_value()) {
        output->u32(message.aggregator->asn);
        output->string(message.aggregator->address);
    }
    output->boolean(message.old_peer_state.has_value());
    if (message.old_peer_state.has_value()) {
        output->u8(static_cast<std::uint8_t>(*message.old_peer_state));
    }
    output->boolean(message.new_peer_state.has_value());
    if (message.new_peer_state.has_value()) {
        output->u8(static_cast<std::uint8_t>(*message.new_peer_state));
    }
    output->boolean(message.annotations.rpki_active);
    output->boolean(message.annotations.has_rpki_config);
    output->u32(message.annotations.timestamp);
}

template <typename Enum>
Enum read_enum(ByteReader *input, std::uint8_t maximum, const char *name) {
    const std::uint8_t value = input->u8();
    if (value > maximum) {
        throw ParsedCacheFailure(std::string("Invalid ") + name + " enum in parsed cache");
    }
    return static_cast<Enum>(value);
}

struct MessageTimestampKey {
    std::int64_t seconds = 0;
    std::uint32_t microseconds = 0;
};

bool timestamp_less(const MessageTimestampKey &left, const MessageTimestampKey &right) noexcept {
    return left.seconds < right.seconds ||
           (left.seconds == right.seconds && left.microseconds < right.microseconds);
}

struct EncodedMessageMetadata {
    BGPMessageType type = BGPMessageType::Unknown;
    MessageTimestampKey timestamp;
};

EncodedMessageMetadata inspect_encoded_message(ByteReader input) {
    EncodedMessageMetadata metadata;
    metadata.type = read_enum<BGPMessageType>(
        &input, static_cast<std::uint8_t>(BGPMessageType::Unknown), "message type");
    (void)read_enum<BGPRecordType>(&input, static_cast<std::uint8_t>(BGPRecordType::Unknown),
                                   "record type");
    (void)read_enum<BGPRecordStatus>(&input, static_cast<std::uint8_t>(BGPRecordStatus::Unknown),
                                     "record status");
    metadata.timestamp.seconds = input.i64();
    metadata.timestamp.microseconds = input.u32();
    return metadata;
}

struct SortEntry {
    std::int64_t timestamp = 0;
    std::uint64_t offset = 0;
    std::uint32_t encoded_size = 0;
    std::uint32_t timestamp_microseconds = 0;
};

class ReadOnlyFileMapping {
   public:
    ReadOnlyFileMapping(const std::filesystem::path &path, std::uint64_t expected_size)
        : path_(path) {
        if (expected_size > std::numeric_limits<std::size_t>::max()) {
            throw ParsedCacheFailure("Sort spool is too large to map: " + path.string());
        }
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) {
            throw ParsedCacheFailure("Failed to open sort spool " + path.string() + ": " +
                                     std::strerror(errno));
        }

        struct stat file_stat {};
        if (::fstat(fd_, &file_stat) != 0) {
            const int error = errno;
            close_fd();
            throw ParsedCacheFailure("Failed to stat sort spool " + path.string() + ": " +
                                     std::strerror(error));
        }
        if (file_stat.st_size < 0 || static_cast<std::uint64_t>(file_stat.st_size) != expected_size) {
            close_fd();
            throw ParsedCacheFailure("Sort spool size changed unexpectedly: " + path.string());
        }

        size_ = static_cast<std::size_t>(expected_size);
        if (size_ == 0) {
            return;
        }
        mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapping_ == MAP_FAILED) {
            const int error = errno;
            close_fd();
            throw ParsedCacheFailure("Failed to map sort spool " + path.string() + ": " +
                                     std::strerror(error));
        }
    }

    ~ReadOnlyFileMapping() {
        if (mapping_ != MAP_FAILED) {
            ::munmap(mapping_, size_);
        }
        close_fd();
    }

    ReadOnlyFileMapping(const ReadOnlyFileMapping &) = delete;
    ReadOnlyFileMapping &operator=(const ReadOnlyFileMapping &) = delete;

    const std::uint8_t *data() const noexcept {
        return mapping_ == MAP_FAILED ? nullptr : static_cast<const std::uint8_t *>(mapping_);
    }
    std::size_t size() const noexcept { return size_; }

   private:
    void close_fd() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::filesystem::path path_;
    int fd_ = -1;
    void *mapping_ = MAP_FAILED;
    std::size_t size_ = 0;
};

class SortedMessageSpool {
   public:
    explicit SortedMessageSpool(const std::filesystem::path &cache_path)
        : path_(cache_path.string() + ".sort.part") {
        output_.open(path_, std::ios::binary | std::ios::trunc);
        if (!output_) {
            throw ParsedCacheFailure("Failed to create message sort spool: " + path_.string());
        }
    }

    ~SortedMessageSpool() {
        output_.close();
        std::error_code remove_error;
        std::filesystem::remove(path_, remove_error);
    }

    SortedMessageSpool(const SortedMessageSpool &) = delete;
    SortedMessageSpool &operator=(const SortedMessageSpool &) = delete;

    void append(const std::vector<BGPMessage> &messages) {
        if (closed_) {
            throw ParsedCacheFailure("Cannot append to a closed message sort spool");
        }
        if (messages.empty()) {
            return;
        }

        ByteWriter encoded_batch;
        for (const BGPMessage &message : messages) {
            ByteWriter encoded_message;
            encode_message(message, &encoded_message);
            if (encoded_message.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw ParsedCacheFailure("One BGP message exceeds the parsed-cache format limit");
            }
            if (encoded_batch.size() > std::numeric_limits<std::uint64_t>::max() - spool_size_) {
                throw ParsedCacheFailure("Message sort spool exceeds the supported size");
            }
            const std::uint64_t offset = spool_size_ + encoded_batch.size();
            if (encoded_message.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
                throw ParsedCacheFailure("Message sort spool exceeds the supported size");
            }
            entries_.push_back(SortEntry{
                static_cast<std::int64_t>(message.timestamp),
                offset,
                static_cast<std::uint32_t>(encoded_message.size()),
                message.timestamp_microseconds,
            });
            encoded_batch.append(encoded_message.bytes().data(), encoded_message.size());
        }
        write_bytes(output_, encoded_batch.bytes().data(), encoded_batch.size(), path_);
        spool_size_ += encoded_batch.size();
    }

    void close_and_sort() {
        if (closed_) {
            return;
        }
        output_.flush();
        require_output(output_, path_);
        output_.close();
        if (!output_) {
            throw ParsedCacheFailure("Failed to close message sort spool: " + path_.string());
        }
        closed_ = true;
        std::sort(entries_.begin(), entries_.end(), [](const SortEntry &left, const SortEntry &right) {
            if (left.timestamp != right.timestamp) {
                return left.timestamp < right.timestamp;
            }
            if (left.timestamp_microseconds != right.timestamp_microseconds) {
                return left.timestamp_microseconds < right.timestamp_microseconds;
            }
            return left.offset < right.offset;
        });
    }

    const std::filesystem::path &path() const noexcept { return path_; }
    const std::vector<SortEntry> &entries() const noexcept { return entries_; }
    std::uint64_t size() const noexcept { return spool_size_; }

   private:
    std::filesystem::path path_;
    std::ofstream output_;
    std::vector<SortEntry> entries_;
    std::uint64_t spool_size_ = 0;
    bool closed_ = false;
};

BGPMessage decode_message(ByteReader *input, BGPMessageFields fields, const std::string &source_file) {
    BGPMessage message;
    const BGPMessageType type = read_enum<BGPMessageType>(
        input, static_cast<std::uint8_t>(BGPMessageType::Unknown), "message type");
    if (has_message_field(fields, BGPMessageFields::Type)) {
        message.type = type;
    }
    const BGPRecordType record_type = read_enum<BGPRecordType>(
        input, static_cast<std::uint8_t>(BGPRecordType::Unknown), "record type");
    if (has_message_field(fields, BGPMessageFields::RecordType)) {
        message.record_type = record_type;
    }
    const BGPRecordStatus record_status = read_enum<BGPRecordStatus>(
        input, static_cast<std::uint8_t>(BGPRecordStatus::Unknown), "record status");
    if (has_message_field(fields, BGPMessageFields::RecordStatus)) {
        message.record_status = record_status;
    }
    const std::int64_t timestamp = input->i64();
    const std::uint32_t timestamp_microseconds = input->u32();
    if (has_message_field(fields, BGPMessageFields::Timestamp)) {
        message.timestamp = static_cast<std::time_t>(timestamp);
        message.timestamp_microseconds = timestamp_microseconds;
    }
    message.project_name = input->string(has_message_field(fields, BGPMessageFields::ProjectName));
    message.collector_name = input->string(has_message_field(fields, BGPMessageFields::CollectorName));
    message.router_name = input->string(has_message_field(fields, BGPMessageFields::RouterName));
    message.router_ip = input->string(has_message_field(fields, BGPMessageFields::RouterIp));
    const BGPDumpPosition dump_position = read_enum<BGPDumpPosition>(
        input, static_cast<std::uint8_t>(BGPDumpPosition::Unknown), "dump position");
    if (has_message_field(fields, BGPMessageFields::DumpPosition)) {
        message.dump_position = dump_position;
    }
    const std::int64_t dump_timestamp = input->i64();
    if (has_message_field(fields, BGPMessageFields::DumpTimestamp)) {
        message.dump_timestamp = static_cast<std::time_t>(dump_timestamp);
    }
    if (has_message_field(fields, BGPMessageFields::SourceFile)) {
        message.source_file = source_file;
    }
    const std::uint64_t record_index = input->u64();
    const std::uint64_t element_index = input->u64();
    if (has_message_field(fields, BGPMessageFields::RecordIndex)) {
        message.record_index = record_index;
    }
    if (has_message_field(fields, BGPMessageFields::ElementIndex)) {
        message.element_index = element_index;
    }
    const std::int64_t originated_timestamp = input->i64();
    const std::uint32_t originated_microseconds = input->u32();
    if (has_message_field(fields, BGPMessageFields::OriginatedTimestamp)) {
        message.originated_timestamp = static_cast<std::time_t>(originated_timestamp);
        message.originated_timestamp_microseconds = originated_microseconds;
    }
    message.peer_ip = input->string(has_message_field(fields, BGPMessageFields::PeerIp));
    const std::uint32_t peer_asn = input->u32();
    if (has_message_field(fields, BGPMessageFields::PeerAsn)) {
        message.peer_asn = peer_asn;
    }
    message.prefix = input->string(has_message_field(fields, BGPMessageFields::Prefix));
    message.next_hop = input->string(has_message_field(fields, BGPMessageFields::NextHop));
    const bool has_as_path = input->boolean();
    if (has_message_field(fields, BGPMessageFields::HasASPath)) {
        message.has_as_path = has_as_path;
    }
    message.as_path = input->string(has_message_field(fields, BGPMessageFields::ASPathString));

    const std::uint32_t segment_count = input->u32();
    if (segment_count > kMaximumVectorElements) {
        throw ParsedCacheFailure("Parsed cache contains too many AS path segments");
    }
    const bool materialize_segments = has_message_field(fields, BGPMessageFields::ASPathSegments);
    if (materialize_segments) {
        message.as_path_segments.reserve(segment_count);
    }
    for (std::uint32_t segment_index = 0; segment_index < segment_count; ++segment_index) {
        const ASPathSegmentType segment_type = read_enum<ASPathSegmentType>(
            input, static_cast<std::uint8_t>(ASPathSegmentType::Unknown), "AS path segment type");
        const std::uint32_t asn_count = input->u32();
        if (asn_count > kMaximumVectorElements) {
            throw ParsedCacheFailure("Parsed cache contains an oversized AS path segment");
        }
        ASPathSegment segment;
        if (materialize_segments) {
            segment.type = segment_type;
            segment.asns.reserve(asn_count);
        }
        for (std::uint32_t asn_index = 0; asn_index < asn_count; ++asn_index) {
            const std::uint32_t asn = input->u32();
            if (materialize_segments) {
                segment.asns.push_back(asn);
            }
        }
        if (materialize_segments) {
            message.as_path_segments.push_back(std::move(segment));
        }
    }

    const std::uint32_t flattened_count = input->u32();
    if (flattened_count > kMaximumVectorElements) {
        throw ParsedCacheFailure("Parsed cache contains an oversized flattened AS path");
    }
    const bool materialize_flattened = has_message_field(fields, BGPMessageFields::FlattenedAsns);
    if (materialize_flattened) {
        message.asns.reserve(flattened_count);
    }
    for (std::uint32_t index = 0; index < flattened_count; ++index) {
        const std::uint32_t asn = input->u32();
        if (materialize_flattened) {
            message.asns.push_back(asn);
        }
    }
    const bool has_origin_asn = input->boolean();
    if (has_origin_asn) {
        const std::uint32_t origin_asn = input->u32();
        if (has_message_field(fields, BGPMessageFields::OriginAsn)) {
            message.origin_asn = origin_asn;
        }
    }

    const bool has_communities = input->boolean();
    if (has_message_field(fields, BGPMessageFields::HasCommunities)) {
        message.has_communities = has_communities;
    }
    const std::uint32_t community_count = input->u32();
    if (community_count > kMaximumVectorElements) {
        throw ParsedCacheFailure("Parsed cache contains too many communities");
    }
    const bool materialize_communities = has_message_field(fields, BGPMessageFields::Communities);
    if (materialize_communities) {
        message.communities.reserve(community_count);
    }
    for (std::uint32_t index = 0; index < community_count; ++index) {
        const BGPCommunity community{input->u16(), input->u16()};
        if (materialize_communities) {
            message.communities.push_back(community);
        }
    }

    const bool has_origin = input->boolean();
    if (has_origin) {
        const BGPOrigin origin =
            read_enum<BGPOrigin>(input, static_cast<std::uint8_t>(BGPOrigin::Unknown), "origin");
        if (has_message_field(fields, BGPMessageFields::Origin)) {
            message.origin = origin;
        }
    }
    const bool has_med = input->boolean();
    if (has_med) {
        const std::uint32_t med = input->u32();
        if (has_message_field(fields, BGPMessageFields::Med)) {
            message.med = med;
        }
    }
    const bool has_local_pref = input->boolean();
    if (has_local_pref) {
        const std::uint32_t local_pref = input->u32();
        if (has_message_field(fields, BGPMessageFields::LocalPref)) {
            message.local_pref = local_pref;
        }
    }
    const bool atomic_aggregate = input->boolean();
    if (has_message_field(fields, BGPMessageFields::AtomicAggregate)) {
        message.atomic_aggregate = atomic_aggregate;
    }
    const bool has_aggregator = input->boolean();
    if (has_aggregator) {
        BGPAggregator aggregator;
        aggregator.asn = input->u32();
        aggregator.address = input->string(has_message_field(fields, BGPMessageFields::Aggregator));
        if (has_message_field(fields, BGPMessageFields::Aggregator)) {
            message.aggregator = std::move(aggregator);
        }
    }
    const bool has_old_state = input->boolean();
    if (has_old_state) {
        const BGPPeerState state = read_enum<BGPPeerState>(
            input, static_cast<std::uint8_t>(BGPPeerState::Deleted), "old peer state");
        if (has_message_field(fields, BGPMessageFields::PeerStates)) {
            message.old_peer_state = state;
        }
    }
    const bool has_new_state = input->boolean();
    if (has_new_state) {
        const BGPPeerState state = read_enum<BGPPeerState>(
            input, static_cast<std::uint8_t>(BGPPeerState::Deleted), "new peer state");
        if (has_message_field(fields, BGPMessageFields::PeerStates)) {
            message.new_peer_state = state;
        }
    }
    const bool rpki_active = input->boolean();
    const bool has_rpki_config = input->boolean();
    const std::uint32_t annotation_timestamp = input->u32();
    if (has_message_field(fields, BGPMessageFields::Annotations)) {
        message.annotations.rpki_active = rpki_active;
        message.annotations.has_rpki_config = has_rpki_config;
        message.annotations.timestamp = annotation_timestamp;
    }
    input->require_empty();
    return message;
}

void record_message_type(BGPMessageType type, MessageTraversalStats *stats) {
    switch (type) {
        case BGPMessageType::RIB:
            ++stats->rib_messages;
            break;
        case BGPMessageType::Announcement:
            ++stats->announcement_messages;
            break;
        case BGPMessageType::Withdrawal:
            ++stats->withdrawal_messages;
            break;
        case BGPMessageType::PeerState:
            ++stats->peer_state_messages;
            break;
        case BGPMessageType::EndOfRib:
            ++stats->end_of_rib_messages;
            break;
        case BGPMessageType::Unknown:
            break;
    }
    ++stats->visited_messages;
}

bool same_stats(const MessageTraversalStats &left, const MessageTraversalStats &right) {
    return left.visited_messages == right.visited_messages && left.rib_messages == right.rib_messages &&
           left.announcement_messages == right.announcement_messages &&
           left.withdrawal_messages == right.withdrawal_messages &&
           left.peer_state_messages == right.peer_state_messages &&
           left.end_of_rib_messages == right.end_of_rib_messages;
}

void add_stats(MessageTraversalStats *destination, const MessageTraversalStats &source) {
    destination->visited_messages += source.visited_messages;
    destination->rib_messages += source.rib_messages;
    destination->announcement_messages += source.announcement_messages;
    destination->withdrawal_messages += source.withdrawal_messages;
    destination->peer_state_messages += source.peer_state_messages;
    destination->end_of_rib_messages += source.end_of_rib_messages;
}

struct CacheHeader {
    std::uint32_t codec = kCodecNone;
    std::uint32_t ordering = kOrderingTimestampStable;
    std::uint64_t fields = 0;
    std::uint64_t source_size = 0;
    std::int64_t source_mtime = 0;
    std::string source_path;
};

void write_header(std::ostream &output, const std::filesystem::path &output_path,
                  const CacheHeader &header) {
    write_bytes(output, kCacheMagic.data(), kCacheMagic.size(), output_path);
    write_u32(output, kParsedCacheSchemaVersion, output_path);
    write_u32(output, header.codec, output_path);
    write_u32(output, header.ordering, output_path);
    write_u64(output, header.fields, output_path);
    write_u64(output, header.source_size, output_path);
    write_i64(output, header.source_mtime, output_path);
    write_string(output, header.source_path, output_path);
}

CacheHeader read_header(std::istream &input, const std::filesystem::path &cache_path) {
    std::array<char, kCacheMagic.size()> magic{};
    read_exact(input, magic.data(), magic.size(), cache_path);
    if (magic != kCacheMagic) {
        throw ParsedCacheFailure("File is not a supported parsed cache: " + cache_path.string());
    }
    const std::uint32_t version = read_u32(input, cache_path);
    if (version != kParsedCacheSchemaVersion) {
        throw ParsedCacheFailure("Parsed cache schema version " + std::to_string(version) +
                                 " is not supported (expected " +
                                 std::to_string(kParsedCacheSchemaVersion) + "): " + cache_path.string());
    }
    CacheHeader header;
    header.codec = read_u32(input, cache_path);
    if (header.codec != kCodecNone && header.codec != kCodecZstd) {
        throw ParsedCacheFailure("Parsed cache uses an unknown compression codec: " + cache_path.string());
    }
    if (header.codec == kCodecZstd && !ZstdApi::instance().available()) {
        throw ParsedCacheFailure("Parsed cache requires libzstd.so.1: " + cache_path.string());
    }
    header.ordering = read_u32(input, cache_path);
    if (header.ordering != kOrderingTimestampStable) {
        throw ParsedCacheFailure("Parsed cache does not guarantee stable timestamp ordering: " +
                                 cache_path.string());
    }
    header.fields = read_u64(input, cache_path);
    header.source_size = read_u64(input, cache_path);
    header.source_mtime = read_i64(input, cache_path);
    header.source_path = read_string(input, cache_path);
    return header;
}

MessageTraversalStats read_stats(std::istream &input, const std::filesystem::path &path);

void validate_cache_envelope(std::istream &input, const std::filesystem::path &cache_path,
                             std::uint32_t codec) {
    std::uint64_t message_count = 0;
    while (true) {
        const std::uint32_t marker = read_u32(input, cache_path);
        if (marker == kFooterMarker) {
            const MessageTraversalStats footer_stats = read_stats(input, cache_path);
            if (footer_stats.visited_messages != message_count) {
                throw ParsedCacheFailure("Parsed-cache footer count does not match its blocks: " +
                                         cache_path.string());
            }
            if (input.peek() != std::char_traits<char>::eof()) {
                throw ParsedCacheFailure("Parsed cache has trailing data after its footer: " +
                                         cache_path.string());
            }
            return;
        }
        if (marker != kBlockMarker) {
            throw ParsedCacheFailure("Parsed cache contains an invalid block marker: " + cache_path.string());
        }
        const std::uint32_t block_messages = read_u32(input, cache_path);
        const std::uint64_t raw_size = read_u64(input, cache_path);
        const std::uint64_t stored_size = read_u64(input, cache_path);
        (void)read_u64(input, cache_path);  // checksum
        if (block_messages == 0 || raw_size == 0 || stored_size == 0 || raw_size > kMaximumBlockBytes ||
            stored_size > kMaximumBlockBytes || (codec == kCodecNone && stored_size != raw_size) ||
            message_count > std::numeric_limits<std::uint64_t>::max() - block_messages) {
            throw ParsedCacheFailure("Parsed cache contains invalid block dimensions: " + cache_path.string());
        }
        input.seekg(static_cast<std::streamoff>(stored_size), std::ios::cur);
        if (!input) {
            throw ParsedCacheFailure("Parsed cache is truncated: " + cache_path.string());
        }
        message_count += block_messages;
    }
}

void verify_header_for_source(const CacheHeader &header, const std::filesystem::path &source_file,
                              const std::filesystem::path &cache_path) {
    if (header.fields != static_cast<std::uint64_t>(kAllBGPMessageFields)) {
        throw ParsedCacheFailure("Parsed cache does not contain the complete BGPMessage schema: " +
                                 cache_path.string());
    }
    const std::uint64_t current_size = require_source_size(source_file);
    if (header.source_size != current_size) {
        throw ParsedCacheFailure("Source MRT size changed after cache generation: " + source_file.string());
    }
    const std::int64_t current_mtime = file_mtime_ticks(source_file);
    if (header.source_mtime != current_mtime) {
        throw ParsedCacheFailure("Source MRT modification time changed after cache generation: " +
                                 source_file.string());
    }
}

MessageTraversalStats read_stats(std::istream &input, const std::filesystem::path &path) {
    MessageTraversalStats stats;
    stats.visited_messages = read_u64(input, path);
    stats.rib_messages = read_u64(input, path);
    stats.announcement_messages = read_u64(input, path);
    stats.withdrawal_messages = read_u64(input, path);
    stats.peer_state_messages = read_u64(input, path);
    stats.end_of_rib_messages = read_u64(input, path);
    return stats;
}

void write_stats(std::ostream &output, const std::filesystem::path &path,
                 const MessageTraversalStats &stats) {
    write_u64(output, stats.visited_messages, path);
    write_u64(output, stats.rib_messages, path);
    write_u64(output, stats.announcement_messages, path);
    write_u64(output, stats.withdrawal_messages, path);
    write_u64(output, stats.peer_state_messages, path);
    write_u64(output, stats.end_of_rib_messages, path);
}

std::string summarize_failures(const std::vector<std::string> &failures) {
    std::ostringstream message;
    message << "Failed to generate " << failures.size() << " parsed cache file(s)";
    const std::size_t display_count = std::min<std::size_t>(failures.size(), 8);
    for (std::size_t index = 0; index < display_count; ++index) {
        message << "\n  " << failures[index];
    }
    if (failures.size() > display_count) {
        message << "\n  ... and " << failures.size() - display_count << " more";
    }
    return message.str();
}

}  // namespace

std::filesystem::path parsed_cache_path(const std::filesystem::path &source_file) {
    return source_file.parent_path() / "parsed-cache" / (source_file.filename().string() + ".bgpcache");
}

ParsedCacheInspection inspect_parsed_cache(const std::filesystem::path &source_file) {
    ParsedCacheInspection inspection;
    inspection.cache_path = parsed_cache_path(source_file);
    if (!std::filesystem::exists(inspection.cache_path)) {
        inspection.state = ParsedCacheState::Missing;
        inspection.reason = "parsed cache is missing";
        return inspection;
    }

    try {
        std::ifstream input(inspection.cache_path, std::ios::binary);
        if (!input) {
            throw ParsedCacheFailure("Failed to open parsed cache: " + inspection.cache_path.string());
        }
        const CacheHeader header = read_header(input, inspection.cache_path);
        verify_header_for_source(header, source_file, inspection.cache_path);
        validate_cache_envelope(input, inspection.cache_path, header.codec);
        inspection.state = ParsedCacheState::Valid;
    } catch (const std::exception &error) {
        inspection.state = ParsedCacheState::Invalid;
        inspection.reason = error.what();
    }
    return inspection;
}

struct ParsedCacheWriter::Impl {
    explicit Impl(std::filesystem::path source)
        : source_file(std::move(source)), output_path(parsed_cache_path(source_file)),
          partial_path(output_path.parent_path() / (output_path.filename().string() + ".part")) {
        const CacheHeader header{
            ZstdApi::instance().available() ? kCodecZstd : kCodecNone,
            kOrderingTimestampStable,
            static_cast<std::uint64_t>(kAllBGPMessageFields),
            require_source_size(source_file),
            file_mtime_ticks(source_file),
            normalized_path_text(source_file),
        };
        codec = header.codec;

        std::error_code directory_error;
        std::filesystem::create_directories(output_path.parent_path(), directory_error);
        if (directory_error) {
            throw ParsedCacheFailure("Failed to create parsed-cache directory " +
                                     output_path.parent_path().string() + ": " + directory_error.message());
        }
        output.open(partial_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ParsedCacheFailure("Failed to create parsed cache: " + partial_path.string());
        }
        write_header(output, partial_path, header);
    }

    void append(const std::vector<BGPMessage> &messages) {
        if (finalized) {
            throw ParsedCacheFailure("Cannot append to a finalized parsed cache");
        }
        if (messages.empty()) {
            return;
        }
        if (messages.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw ParsedCacheFailure("Parsed-cache block contains too many messages");
        }

        ByteWriter payload;
        for (const BGPMessage &message : messages) {
            ByteWriter encoded_message;
            encode_message(message, &encoded_message);
            if (encoded_message.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw ParsedCacheFailure("One BGP message exceeds the parsed-cache format limit");
            }
            payload.u32(static_cast<std::uint32_t>(encoded_message.size()));
            payload.append(encoded_message.bytes().data(), encoded_message.size());
        }
        append_serialized_block(payload.bytes(), static_cast<std::uint32_t>(messages.size()));
    }

    void append_serialized_block(const std::vector<std::uint8_t> &payload,
                                 std::uint32_t message_count) {
        if (finalized) {
            throw ParsedCacheFailure("Cannot append to a finalized parsed cache");
        }
        if (message_count == 0) {
            if (!payload.empty()) {
                throw ParsedCacheFailure("Parsed-cache block has data but no messages");
            }
            return;
        }
        if (payload.size() > kMaximumBlockBytes) {
            throw ParsedCacheFailure("Parsed-cache block exceeds the maximum supported size");
        }

        MessageTraversalStats block_stats;
        std::optional<MessageTimestampKey> block_last_timestamp = last_timestamp;
        ByteReader framed(payload.data(), payload.size(), partial_path.string() + " pending block");
        for (std::uint32_t index = 0; index < message_count; ++index) {
            const std::uint32_t encoded_size = framed.u32();
            ByteReader encoded = framed.subreader(encoded_size, " message " + std::to_string(index));
            const EncodedMessageMetadata metadata = inspect_encoded_message(encoded);
            if (block_last_timestamp.has_value() &&
                timestamp_less(metadata.timestamp, *block_last_timestamp)) {
                throw ParsedCacheFailure("Parsed-cache messages are not ordered by timestamp: " +
                                         source_file.string());
            }
            block_last_timestamp = metadata.timestamp;
            record_message_type(metadata.type, &block_stats);
        }
        framed.require_empty();

        const std::vector<std::uint8_t> stored =
            codec == kCodecZstd ? ZstdApi::instance().compress(payload) : payload;
        if (stored.size() > kMaximumBlockBytes) {
            throw ParsedCacheFailure("Stored parsed-cache block exceeds the maximum supported size");
        }
        write_u32(output, kBlockMarker, partial_path);
        write_u32(output, message_count, partial_path);
        write_u64(output, payload.size(), partial_path);
        write_u64(output, stored.size(), partial_path);
        write_u64(output, checksum64(payload), partial_path);
        write_bytes(output, stored.data(), stored.size(), partial_path);
        add_stats(&stats, block_stats);
        last_timestamp = block_last_timestamp;
    }

    void finalize() {
        if (finalized) {
            return;
        }
        write_u32(output, kFooterMarker, partial_path);
        write_stats(output, partial_path, stats);
        output.flush();
        require_output(output, partial_path);
        output.close();
        if (!output) {
            throw ParsedCacheFailure("Failed to close parsed cache: " + partial_path.string());
        }

        std::error_code rename_error;
        std::filesystem::rename(partial_path, output_path, rename_error);
        if (rename_error) {
            throw ParsedCacheFailure("Failed to atomically publish parsed cache " + output_path.string() + ": " +
                                     rename_error.message());
        }
        finalized = true;
    }

    std::filesystem::path source_file;
    std::filesystem::path output_path;
    std::filesystem::path partial_path;
    std::ofstream output;
    std::uint32_t codec = kCodecNone;
    MessageTraversalStats stats;
    std::optional<MessageTimestampKey> last_timestamp;
    bool finalized = false;
};

ParsedCacheWriter::ParsedCacheWriter(std::filesystem::path source_file)
    : impl_(std::make_unique<Impl>(std::move(source_file))) {}

ParsedCacheWriter::~ParsedCacheWriter() = default;

void ParsedCacheWriter::append(const std::vector<BGPMessage> &messages) { impl_->append(messages); }

void ParsedCacheWriter::finalize() { impl_->finalize(); }

const MessageTraversalStats &ParsedCacheWriter::stats() const noexcept { return impl_->stats; }

const std::filesystem::path &ParsedCacheWriter::output_path() const noexcept { return impl_->output_path; }

MessageTraversalStats generate_parsed_cache(const Config &config, const std::filesystem::path &source_file,
                                            std::size_t message_batch_size) {
    ParsedCacheWriter writer(source_file);
    SortedMessageSpool spool(writer.output_path());
    const MessageTraversalStats parsed_stats =
        traverse_mrt_file(config, source_file, kAllBGPMessageFields, message_batch_size, std::nullopt,
                          [&](std::vector<BGPMessage> &messages) { spool.append(messages); });
    spool.close_and_sort();
    if (spool.entries().size() != parsed_stats.visited_messages) {
        throw ParsedCacheFailure("Message sort index count does not match MRT parser count for " +
                                 source_file.string());
    }

    const ReadOnlyFileMapping mapping(spool.path(), spool.size());
    ByteWriter payload;
    std::uint32_t block_message_count = 0;
    auto flush_block = [&]() {
        if (block_message_count == 0) {
            return;
        }
        writer.impl_->append_serialized_block(payload.bytes(), block_message_count);
        payload.clear();
        block_message_count = 0;
    };

    for (const SortEntry &entry : spool.entries()) {
        if (entry.offset > mapping.size() || entry.encoded_size > mapping.size() - entry.offset) {
            throw ParsedCacheFailure("Message sort index points outside its spool for " +
                                     source_file.string());
        }
        const std::uint64_t framed_size = sizeof(std::uint32_t) + entry.encoded_size;
        if (framed_size > kMaximumBlockBytes) {
            throw ParsedCacheFailure("One BGP message exceeds the parsed-cache block size limit");
        }
        if (block_message_count > 0 &&
            (block_message_count == std::numeric_limits<std::uint32_t>::max() ||
             block_message_count >= message_batch_size ||
             payload.size() > kMaximumBlockBytes - framed_size)) {
            flush_block();
        }
        payload.u32(entry.encoded_size);
        payload.append(mapping.data() + static_cast<std::size_t>(entry.offset), entry.encoded_size);
        ++block_message_count;
    }
    flush_block();

    if (!same_stats(parsed_stats, writer.stats())) {
        throw ParsedCacheFailure("Parsed-cache writer message counts do not match MRT parser counts for " +
                                 source_file.string());
    }
    writer.finalize();
    const ParsedCacheInspection inspection = inspect_parsed_cache(source_file);
    if (inspection.state != ParsedCacheState::Valid) {
        throw ParsedCacheFailure("Generated parsed cache failed validation: " + inspection.cache_path.string() +
                                 " | " + inspection.reason);
    }
    return parsed_stats;
}

MessageTraversalStats read_parsed_cache(const std::filesystem::path &source_file, BGPMessageFields fields,
                                        std::size_t message_batch_size,
                                        const std::optional<ClosedDateRange> &range,
                                        const MessageBatchHandler &handle_batch) {
    if (message_batch_size == 0) {
        throw std::invalid_argument("message_batch_size must be positive");
    }
    const std::filesystem::path cache_path = parsed_cache_path(source_file);
    std::ifstream input(cache_path, std::ios::binary);
    if (!input) {
        throw ParsedCacheFailure("Required parsed cache is missing: " + cache_path.string());
    }
    const CacheHeader header = read_header(input, cache_path);
    verify_header_for_source(header, source_file, cache_path);

    MessageTraversalStats delivered_stats;
    MessageTraversalStats complete_stats;
    std::vector<BGPMessage> batch;
    batch.reserve(message_batch_size);
    const std::string current_source_file = normalized_path_text(source_file);
    auto flush_batch = [&]() {
        if (batch.empty()) {
            return;
        }
        handle_batch(batch);
        batch.clear();
    };

    std::uint64_t block_index = 0;
    std::optional<MessageTimestampKey> previous_cache_timestamp;
    while (true) {
        const std::uint32_t marker = read_u32(input, cache_path);
        if (marker == kFooterMarker) {
            const MessageTraversalStats footer_stats = read_stats(input, cache_path);
            if (!same_stats(footer_stats, complete_stats)) {
                throw ParsedCacheFailure("Parsed-cache footer counts do not match its blocks: " +
                                         cache_path.string());
            }
            if (input.peek() != std::char_traits<char>::eof()) {
                throw ParsedCacheFailure("Parsed cache has trailing data after its footer: " + cache_path.string());
            }
            break;
        }
        if (marker != kBlockMarker) {
            throw ParsedCacheFailure("Parsed cache contains an invalid block marker: " + cache_path.string());
        }

        const std::uint32_t message_count = read_u32(input, cache_path);
        const std::uint64_t raw_size = read_u64(input, cache_path);
        const std::uint64_t stored_size = read_u64(input, cache_path);
        const std::uint64_t expected_checksum = read_u64(input, cache_path);
        if (message_count == 0 || raw_size == 0 || stored_size == 0 || raw_size > kMaximumBlockBytes ||
            stored_size > kMaximumBlockBytes ||
            (header.codec == kCodecNone && stored_size != raw_size)) {
            throw ParsedCacheFailure("Parsed cache contains invalid block dimensions: " + cache_path.string());
        }
        std::vector<std::uint8_t> stored(static_cast<std::size_t>(stored_size));
        read_exact(input, stored.data(), stored.size(), cache_path);
        const std::vector<std::uint8_t> raw =
            header.codec == kCodecZstd
                ? ZstdApi::instance().decompress(stored, static_cast<std::size_t>(raw_size))
                : std::move(stored);
        if (raw.size() != raw_size || checksum64(raw) != expected_checksum) {
            throw ParsedCacheFailure("Parsed-cache block checksum mismatch: " + cache_path.string());
        }

        ByteReader payload(raw.data(), raw.size(), cache_path.string() + " block " +
                                                       std::to_string(block_index));
        for (std::uint32_t message_index = 0; message_index < message_count; ++message_index) {
            const std::uint32_t encoded_size = payload.u32();
            ByteReader encoded = payload.subreader(encoded_size, " message " + std::to_string(message_index));
            const EncodedMessageMetadata metadata = inspect_encoded_message(encoded);
            if (previous_cache_timestamp.has_value() &&
                timestamp_less(metadata.timestamp, *previous_cache_timestamp)) {
                throw ParsedCacheFailure("Parsed cache is not ordered by timestamp; regenerate it: " +
                                         cache_path.string());
            }
            previous_cache_timestamp = metadata.timestamp;
            record_message_type(metadata.type, &complete_stats);

            const bool in_range = !range.has_value() ||
                                  (static_cast<std::int64_t>(range->start_epoch) <=
                                       metadata.timestamp.seconds &&
                                   metadata.timestamp.seconds <
                                       static_cast<std::int64_t>(range->end_exclusive_epoch));
            if (!in_range) {
                continue;
            }
            batch.push_back(decode_message(&encoded, fields, current_source_file));
            record_message_type(metadata.type, &delivered_stats);
            if (batch.size() >= message_batch_size) {
                flush_batch();
            }
        }
        payload.require_empty();
        ++block_index;
    }
    flush_batch();
    return delivered_stats;
}

AnalysisInputTraversal traverse_analysis_input(const Config &config,
                                                const std::filesystem::path &source_file,
                                                BGPMessageFields fields,
                                                std::size_t message_batch_size,
                                                const std::optional<ClosedDateRange> &range,
                                                const MessageBatchHandler &handle_batch) {
    if (!std::filesystem::exists(source_file)) {
        throw MrtParseFailure("Required source MRT is missing: " + source_file.string() +
                              ". Analysis mode never downloads data.");
    }

    const std::filesystem::path cache_path = parsed_cache_path(source_file);
    if (std::filesystem::exists(cache_path)) {
        return AnalysisInputTraversal{
            read_parsed_cache(source_file, fields, message_batch_size, range, handle_batch),
            false,
        };
    }
    if (!config.parse_on_cache_miss) {
        throw ParsedCacheFailure("Required parsed cache is missing: " + cache_path.string() +
                                 ". Set analysis.parse_on_cache_miss to true to parse the downloaded MRT "
                                 "during analysis.");
    }

    return AnalysisInputTraversal{
        traverse_mrt_file(config, source_file, fields, message_batch_size, range, handle_batch),
        true,
    };
}

ParsedCacheBuildSummary ensure_parsed_caches(const Config &config,
                                             const std::vector<std::filesystem::path> &source_files,
                                             bool show_progress) {
    if (config.parser_workers < 1) {
        throw std::invalid_argument("parser_workers must be positive");
    }
    if (config.message_batch_size < 1) {
        throw std::invalid_argument("message_batch_size must be positive");
    }

    std::vector<std::filesystem::path> unique_files;
    unique_files.reserve(source_files.size());
    std::unordered_set<std::string> seen;
    for (const std::filesystem::path &source_file : source_files) {
        const std::string key = normalized_path_text(source_file);
        if (seen.insert(key).second) {
            unique_files.push_back(source_file);
        }
    }

    ParsedCacheBuildSummary summary;
    summary.source_files = unique_files.size();
    std::vector<std::filesystem::path> pending;
    std::size_t reusable_count = 0;
    std::uint64_t reusable_source_bytes = 0;

    std::unique_ptr<FileProgressDisplay> cache_check_progress;
    if (show_progress && !unique_files.empty()) {
        cache_check_progress =
            std::make_unique<FileProgressDisplay>(unique_files.size(), 0, "cache-check");
    }
    const std::size_t cache_check_update_interval =
        std::max<std::size_t>(unique_files.size() / 1000, 1);
    std::size_t checked_batch_files = 0;
    std::uint64_t checked_batch_bytes = 0;
    for (const std::filesystem::path &source_file : unique_files) {
        const std::uint64_t source_size = require_source_size(source_file);
        summary.source_bytes += source_size;
        const ParsedCacheInspection inspection = inspect_parsed_cache(source_file);
        if (inspection.state == ParsedCacheState::Valid) {
            ++reusable_count;
            reusable_source_bytes += source_size;
            summary.cache_bytes += safe_file_size(inspection.cache_path);
        } else {
            pending.push_back(source_file);
        }

        ++checked_batch_files;
        checked_batch_bytes += source_size;
        if (cache_check_progress != nullptr &&
            checked_batch_files >= cache_check_update_interval) {
            cache_check_progress->mark_batch_completed(checked_batch_files, checked_batch_bytes);
            checked_batch_files = 0;
            checked_batch_bytes = 0;
        }
    }
    if (cache_check_progress != nullptr) {
        if (checked_batch_files > 0) {
            cache_check_progress->mark_batch_completed(checked_batch_files, checked_batch_bytes);
        }
        cache_check_progress->finish();
    }
    summary.reused_files = reusable_count;

    std::unique_ptr<FileProgressDisplay> progress;
    if (show_progress && !unique_files.empty()) {
        progress = std::make_unique<FileProgressDisplay>(unique_files.size(), summary.source_bytes, "preparse");
        if (reusable_count > 0) {
            progress->mark_batch_completed(reusable_count, reusable_source_bytes);
        }
    }
    if (pending.empty()) {
        if (progress != nullptr) {
            progress->finish();
        }
        return summary;
    }

    std::atomic<std::size_t> next_index{0};
    std::mutex result_mutex;
    std::vector<std::string> failures;
    const std::size_t worker_count = std::min<std::size_t>(
        pending.size(), static_cast<std::size_t>(std::max(config.parser_workers, 1)));
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        workers.emplace_back([&]() {
            while (true) {
                const std::size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
                if (index >= pending.size()) {
                    break;
                }
                const std::filesystem::path &source_file = pending[index];
                try {
                    const MessageTraversalStats stats = generate_parsed_cache(
                        config, source_file, static_cast<std::size_t>(config.message_batch_size));
                    const std::uint64_t cache_size = safe_file_size(parsed_cache_path(source_file));
                    std::lock_guard<std::mutex> lock(result_mutex);
                    ++summary.generated_files;
                    summary.generated_messages += stats.visited_messages;
                    summary.cache_bytes += cache_size;
                } catch (const std::exception &error) {
                    std::lock_guard<std::mutex> lock(result_mutex);
                    failures.push_back(source_file.string() + " | " + error.what());
                }
                if (progress != nullptr) {
                    progress->mark_batch_completed(1, safe_file_size(source_file));
                }
            }
        });
    }
    for (std::thread &worker : workers) {
        worker.join();
    }
    if (progress != nullptr) {
        progress->finish();
    }
    if (!failures.empty()) {
        throw ParsedCacheFailure(summarize_failures(failures));
    }
    return summary;
}

}  // namespace bgpstream_runner
