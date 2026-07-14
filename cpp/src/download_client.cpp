#include "bgpstream_runner/download_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "bgpstream_runner/common.h"
#include "routeviews_archive.h"

namespace bgpstream_runner {

namespace {

constexpr char kBrokerUrl[] = "https://broker.bgpstream.caida.org/v2/data";
constexpr char kRouteViewsArchiveBaseUrl[] = "https://archive.routeviews.org";
constexpr char kRecordType[] = "updates";
constexpr long kConnectTimeoutSeconds = 30;
constexpr long kLowSpeedTimeoutSeconds = 60;
constexpr int kDefaultRetries = 3;
constexpr std::chrono::seconds kInitialRetryDelay{1};

struct Resource {
    std::string url;
    std::string project;
    std::string collector;
    std::string record_type;
    std::int64_t initial_time = 0;
    std::int64_t duration = 0;
    std::uint64_t remote_size = 0;
};

class CurlGlobal {
   public:
    CurlGlobal() {
        const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK) {
            throw std::runtime_error(std::string("Failed to initialize libcurl: ") + curl_easy_strerror(code));
        }
    }

    CurlGlobal(const CurlGlobal &) = delete;
    CurlGlobal &operator=(const CurlGlobal &) = delete;

    ~CurlGlobal() { curl_global_cleanup(); }
};

struct CurlEasyDeleter {
    void operator()(CURL *handle) const {
        if (handle != nullptr) {
            curl_easy_cleanup(handle);
        }
    }
};

using CurlEasy = std::unique_ptr<CURL, CurlEasyDeleter>;

void ensure_curl_initialized() {
    static const CurlGlobal curl_global;
    (void)curl_global;
}

CurlEasy make_curl_handle() {
    ensure_curl_initialized();
    CurlEasy handle(curl_easy_init());
    if (handle == nullptr) {
        throw std::runtime_error("Failed to create a libcurl easy handle");
    }
    return handle;
}

std::chrono::seconds retry_delay(int retry_round) {
    const int exponent = std::max(retry_round - 1, 0);
    const int multiplier = 1 << std::min(exponent, 3);
    return std::min(kInitialRetryDelay * multiplier, std::chrono::seconds(10));
}

void configure_common_curl_options(CURL *handle, const std::string &url, char *error_buffer) {
    error_buffer[0] = '\0';
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, kLowSpeedTimeoutSeconds);
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "bgpstream-chunk-runner/1.0");
}

std::string curl_failure_text(CURLcode code, const char *error_buffer) {
    if (error_buffer != nullptr && *error_buffer != '\0') {
        return error_buffer;
    }
    return curl_easy_strerror(code);
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::optional<std::uint64_t> parse_unsigned(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

struct HeaderInfo {
    long status_code = 0;
    std::optional<std::uint64_t> content_length;
    std::optional<std::uint64_t> content_range_total;
};

std::size_t capture_headers(char *data, std::size_t size, std::size_t count, void *userdata) {
    const std::size_t byte_count = size * count;
    auto *headers = static_cast<HeaderInfo *>(userdata);
    std::string line(data, byte_count);

    if (line.rfind("HTTP/", 0) == 0) {
        std::istringstream input(line);
        std::string protocol;
        long status_code = 0;
        input >> protocol >> status_code;
        headers->status_code = status_code;
        headers->content_length.reset();
        headers->content_range_total.reset();
        return byte_count;
    }

    const std::size_t separator = line.find(':');
    if (separator == std::string::npos) {
        return byte_count;
    }

    const std::string name = lowercase(trim(line.substr(0, separator)));
    const std::string value = trim(line.substr(separator + 1));
    if (name == "content-length") {
        headers->content_length = parse_unsigned(value);
    } else if (name == "content-range") {
        const std::size_t slash = value.rfind('/');
        if (slash != std::string::npos && slash + 1 < value.size() && value[slash + 1] != '*') {
            headers->content_range_total = parse_unsigned(std::string_view(value).substr(slash + 1));
        }
    }
    return byte_count;
}

struct StringSink {
    std::string text;
    bool failed = false;
};

std::size_t append_to_string(char *data, std::size_t size, std::size_t count, void *userdata) {
    const std::size_t byte_count = size * count;
    auto *sink = static_cast<StringSink *>(userdata);
    try {
        sink->text.append(data, byte_count);
        return byte_count;
    } catch (...) {
        sink->failed = true;
        return 0;
    }
}

std::string fetch_text(const std::string &url) {
    std::string last_error;

    for (int attempt = 1; attempt <= kDefaultRetries; ++attempt) {
        CurlEasy handle = make_curl_handle();
        char error_buffer[CURL_ERROR_SIZE]{};
        HeaderInfo headers;
        StringSink sink;

        configure_common_curl_options(handle.get(), url, error_buffer);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, capture_headers);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &headers);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append_to_string);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &sink);

        const CURLcode code = curl_easy_perform(handle.get());
        long status_code = headers.status_code;
        curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);

        if (code == CURLE_OK && !sink.failed && status_code >= 200 && status_code < 300) {
            return sink.text;
        }

        if (sink.failed) {
            last_error = "Not enough memory while receiving " + url;
        } else if (code != CURLE_OK) {
            last_error = "Failed to open " + url + ": " + curl_failure_text(code, error_buffer);
        } else {
            last_error = "HTTP " + std::to_string(status_code) + " while opening " + url;
        }

        if (attempt < kDefaultRetries) {
            std::this_thread::sleep_for(retry_delay(attempt));
        }
    }

    throw std::runtime_error(last_error);
}

class JsonValue {
   public:
    enum class Kind {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    const JsonValue &member(const std::string &name) const {
        if (kind != Kind::Object) {
            throw std::runtime_error("Expected a JSON object while reading member '" + name + "'");
        }
        const auto iter = object.find(name);
        if (iter == object.end()) {
            throw std::runtime_error("Broker response is missing JSON member '" + name + "'");
        }
        return iter->second;
    }

    const std::string &require_string(const std::string &name) const {
        if (kind != Kind::String) {
            throw std::runtime_error("Broker JSON member '" + name + "' must be a string");
        }
        return string;
    }

    std::int64_t require_integer(const std::string &name) const {
        double integral = 0;
        if (kind != Kind::Number || !std::isfinite(number) || std::modf(number, &integral) != 0.0 ||
            integral < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            integral > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("Broker JSON member '" + name + "' must be an integer");
        }
        return static_cast<std::int64_t>(integral);
    }
};

class JsonParser {
   public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skip_whitespace();
        JsonValue value = parse_value();
        skip_whitespace();
        if (position_ != text_.size()) {
            fail("unexpected trailing content");
        }
        return value;
    }

   private:
    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error("Invalid broker JSON at byte " + std::to_string(position_) + ": " +
                                 std::string(message));
    }

    char peek() const { return position_ < text_.size() ? text_[position_] : '\0'; }

    char consume() {
        if (position_ >= text_.size()) {
            fail("unexpected end of input");
        }
        return text_[position_++];
    }

    void skip_whitespace() {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
    }

    void expect(char expected) {
        if (consume() != expected) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    bool consume_literal(std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    static void append_utf8(std::string *output, std::uint32_t codepoint) {
        if (codepoint <= 0x7f) {
            output->push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            output->push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            output->push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            output->push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    std::uint32_t parse_hex_quad() {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char ch = consume();
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value |= static_cast<std::uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value |= static_cast<std::uint32_t>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                value |= static_cast<std::uint32_t>(ch - 'A' + 10);
            } else {
                fail("invalid Unicode escape");
            }
        }
        return value;
    }

    std::string parse_string() {
        expect('"');
        std::string value;
        while (true) {
            const char ch = consume();
            if (ch == '"') {
                return value;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                fail("control character in string");
            }
            if (ch != '\\') {
                value.push_back(ch);
                continue;
            }

            const char escaped = consume();
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(escaped);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t codepoint = parse_hex_quad();
                    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                        if (consume() != '\\' || consume() != 'u') {
                            fail("high surrogate without low surrogate");
                        }
                        const std::uint32_t low = parse_hex_quad();
                        if (low < 0xdc00 || low > 0xdfff) {
                            fail("invalid low surrogate");
                        }
                        codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                        fail("low surrogate without high surrogate");
                    }
                    append_utf8(&value, codepoint);
                    break;
                }
                default:
                    fail("invalid string escape");
            }
        }
    }

    double parse_number() {
        const std::size_t start = position_;
        if (peek() == '-') {
            ++position_;
        }
        if (peek() == '0') {
            ++position_;
        } else {
            if (peek() < '1' || peek() > '9') {
                fail("invalid number");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }
        if (peek() == '.') {
            ++position_;
            if (peek() < '0' || peek() > '9') {
                fail("invalid fractional number");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++position_;
            if (peek() == '+' || peek() == '-') {
                ++position_;
            }
            if (peek() < '0' || peek() > '9') {
                fail("invalid exponent");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }

        try {
            return std::stod(std::string(text_.substr(start, position_ - start)));
        } catch (const std::exception &) {
            fail("invalid number");
        }
    }

    JsonValue parse_array() {
        JsonValue value;
        value.kind = JsonValue::Kind::Array;
        expect('[');
        skip_whitespace();
        if (peek() == ']') {
            ++position_;
            return value;
        }
        while (true) {
            skip_whitespace();
            value.array.push_back(parse_value());
            skip_whitespace();
            const char delimiter = consume();
            if (delimiter == ']') {
                return value;
            }
            if (delimiter != ',') {
                fail("expected ',' or ']' in array");
            }
        }
    }

    JsonValue parse_object() {
        JsonValue value;
        value.kind = JsonValue::Kind::Object;
        expect('{');
        skip_whitespace();
        if (peek() == '}') {
            ++position_;
            return value;
        }
        while (true) {
            skip_whitespace();
            if (peek() != '"') {
                fail("expected object member name");
            }
            std::string name = parse_string();
            skip_whitespace();
            expect(':');
            skip_whitespace();
            value.object.insert_or_assign(std::move(name), parse_value());
            skip_whitespace();
            const char delimiter = consume();
            if (delimiter == '}') {
                return value;
            }
            if (delimiter != ',') {
                fail("expected ',' or '}' in object");
            }
        }
    }

    JsonValue parse_value() {
        JsonValue value;
        switch (peek()) {
            case 'n':
                if (!consume_literal("null")) {
                    fail("invalid literal");
                }
                return value;
            case 't':
                if (!consume_literal("true")) {
                    fail("invalid literal");
                }
                value.kind = JsonValue::Kind::Boolean;
                value.boolean = true;
                return value;
            case 'f':
                if (!consume_literal("false")) {
                    fail("invalid literal");
                }
                value.kind = JsonValue::Kind::Boolean;
                return value;
            case '"':
                value.kind = JsonValue::Kind::String;
                value.string = parse_string();
                return value;
            case '[':
                return parse_array();
            case '{':
                return parse_object();
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                    value.kind = JsonValue::Kind::Number;
                    value.number = parse_number();
                    return value;
                }
                fail("unsupported value");
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

std::string url_encode(std::string_view value) {
    CurlEasy handle = make_curl_handle();
    char *escaped = curl_easy_escape(handle.get(), value.data(), static_cast<int>(value.size()));
    if (escaped == nullptr) {
        throw std::runtime_error("Failed to URL-encode broker query parameter");
    }
    std::string result(escaped);
    curl_free(escaped);
    return result;
}

std::vector<Resource> fetch_resources_via_broker(const ClosedDateRange &range, const Config &config,
                                                 FileProgressDisplay *progress) {
    std::ostringstream url;
    url << kBrokerUrl << "?collectors%5B%5D=" << url_encode(config.collector) << "&types%5B%5D="
        << url_encode(kRecordType) << "&intervals%5B%5D="
        << url_encode(std::to_string(range.start_epoch) + "," + std::to_string(range.end_exclusive_epoch));
    if (!config.project.empty()) {
        url << "&projects%5B%5D=" << url_encode(config.project);
    }

    const JsonValue root = JsonParser(fetch_text(url.str())).parse();
    const JsonValue &resources_value = root.member("data").member("resources");
    if (resources_value.kind != JsonValue::Kind::Array) {
        throw std::runtime_error("Broker JSON member 'resources' must be an array");
    }

    std::vector<Resource> resources;
    resources.reserve(resources_value.array.size());
    for (const JsonValue &item : resources_value.array) {
        Resource resource;
        resource.url = item.member("url").require_string("url");
        resource.project = item.member("project").require_string("project");
        resource.collector = item.member("collector").require_string("collector");
        resource.record_type = item.member("type").require_string("type");
        resource.initial_time = item.member("initialTime").require_integer("initialTime");
        resource.duration = item.member("duration").require_integer("duration");
        resources.push_back(std::move(resource));
    }

    std::sort(resources.begin(), resources.end(), [](const Resource &lhs, const Resource &rhs) {
        if (lhs.initial_time != rhs.initial_time) {
            return lhs.initial_time < rhs.initial_time;
        }
        if (lhs.collector != rhs.collector) {
            return lhs.collector < rhs.collector;
        }
        return lhs.url < rhs.url;
    });
    if (progress != nullptr) {
        progress->mark_batch_completed(1, 0);
    }
    return resources;
}

std::vector<routeviews_archive::UpdateEntry> fetch_routeviews_month_index(const std::string &index_url) {
    static std::mutex cache_mutex;
    static std::map<std::string, std::vector<routeviews_archive::UpdateEntry>> cache;

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const auto cached = cache.find(index_url);
        if (cached != cache.end()) {
            return cached->second;
        }
    }

    std::vector<routeviews_archive::UpdateEntry> entries =
        routeviews_archive::parse_updates_index(fetch_text(index_url));
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const auto [cached, inserted] = cache.emplace(index_url, entries);
        if (!inserted) {
            return cached->second;
        }
    }
    return entries;
}

std::vector<Resource> fetch_resources_via_routeviews_direct(const ClosedDateRange &range, const Config &config,
                                                            FileProgressDisplay *progress) {
    std::vector<routeviews_archive::UpdateEntry> entries;
    std::map<std::string, std::string> entry_urls;
    for (const std::string &month : routeviews_archive::months_for_range(range)) {
        const std::string index_url = std::string(kRouteViewsArchiveBaseUrl) + "/" + config.collector +
                                      "/bgpdata/" + month + "/UPDATES/";
        const std::vector<routeviews_archive::UpdateEntry> month_entries =
            fetch_routeviews_month_index(index_url);
        entries.insert(entries.end(), month_entries.begin(), month_entries.end());
        for (const routeviews_archive::UpdateEntry &entry : month_entries) {
            entry_urls.try_emplace(entry.filename, index_url + entry.filename);
        }
        if (progress != nullptr) {
            progress->mark_batch_completed(1, 0);
        }
    }

    entries = routeviews_archive::select_updates_for_range(std::move(entries), range);

    std::vector<Resource> resources;
    resources.reserve(entries.size());
    for (const routeviews_archive::UpdateEntry &entry : entries) {
        resources.push_back(Resource{
            entry_urls.at(entry.filename),
            "routeviews",
            config.collector,
            kRecordType,
            static_cast<std::int64_t>(entry.initial_time),
            static_cast<std::int64_t>(routeviews_archive::kUpdateDurationSeconds),
            0,
        });
    }
    return resources;
}

std::vector<Resource> resolve_resources(const ClosedDateRange &range, const Config &config,
                                        FileProgressDisplay *progress) {
    if (config.collector.rfind("route-views", 0) == 0) {
        return fetch_resources_via_routeviews_direct(range, config, progress);
    }
    return fetch_resources_via_broker(range, config, progress);
}

std::string filename_from_url(const std::string &url) {
    const std::size_t path_end = url.find_first_of("?#");
    const std::string_view path(url.data(), path_end == std::string::npos ? url.size() : path_end);
    const std::size_t slash = path.rfind('/');
    const std::string filename(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
    if (filename.empty() || filename == "." || filename == "..") {
        throw std::runtime_error("Remote resource URL has no usable filename: " + url);
    }
    return filename;
}

std::filesystem::path destination_path(const std::filesystem::path &base_dir, const Resource &resource) {
    return base_dir / resource.project / resource.collector / resource.record_type /
           utc_year_month_path(static_cast<std::time_t>(resource.initial_time)) /
           filename_from_url(resource.url);
}

std::filesystem::path partial_path(const std::filesystem::path &destination) {
    return destination.parent_path() / (destination.filename().string() + ".part");
}

std::filesystem::path cache_artifact_path(const std::filesystem::path &base_dir, const Resource &resource) {
    std::ostringstream filename;
    filename << resource.project << '.' << resource.collector << '.' << resource.record_type << '.'
             << resource.initial_time << '.' << resource.duration << ".cache";
    return base_dir / resource.project / resource.collector / resource.record_type /
           utc_year_month_path(static_cast<std::time_t>(resource.initial_time)) / filename.str();
}

std::filesystem::path preferred_local_path(const std::filesystem::path &base_dir, const Resource &resource) {
    const std::filesystem::path cache_path = cache_artifact_path(base_dir, resource);
    if (std::filesystem::exists(cache_path)) {
        return cache_path;
    }
    return destination_path(base_dir, resource);
}

std::optional<std::filesystem::path> available_local_path(const std::filesystem::path &base_dir,
                                                          const Resource &resource) {
    const std::filesystem::path cache_path = cache_artifact_path(base_dir, resource);
    if (std::filesystem::exists(cache_path)) {
        return cache_path;
    }
    const std::filesystem::path destination = destination_path(base_dir, resource);
    if (std::filesystem::exists(destination)) {
        return destination;
    }
    return std::nullopt;
}

std::vector<Resource> limited_resources(const ClosedDateRange &range, const Config &config, int limit,
                                        FileProgressDisplay *progress = nullptr) {
    std::vector<Resource> resources = resolve_resources(range, config, progress);
    if (limit > 0 && resources.size() > static_cast<std::size_t>(limit)) {
        resources.resize(static_cast<std::size_t>(limit));
    }
    return resources;
}

std::size_t abort_probe_body(char *, std::size_t, std::size_t, void *) { return 0; }

std::uint64_t probe_remote_size(const std::string &url) {
    std::string last_error = "Could not determine remote size for " + url;

    for (int attempt = 1; attempt <= kDefaultRetries; ++attempt) {
        {
            CurlEasy handle = make_curl_handle();
            char error_buffer[CURL_ERROR_SIZE]{};
            HeaderInfo headers;
            configure_common_curl_options(handle.get(), url, error_buffer);
            curl_easy_setopt(handle.get(), CURLOPT_NOBODY, 1L);
            curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, capture_headers);
            curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &headers);

            const CURLcode code = curl_easy_perform(handle.get());
            long status_code = headers.status_code;
            curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);
            if (code == CURLE_OK && status_code >= 200 && status_code < 300 && headers.content_length.has_value()) {
                return *headers.content_length;
            }
            if (code != CURLE_OK) {
                last_error = curl_failure_text(code, error_buffer);
            } else {
                last_error = "HTTP " + std::to_string(status_code);
            }
        }

        {
            CurlEasy handle = make_curl_handle();
            char error_buffer[CURL_ERROR_SIZE]{};
            HeaderInfo headers;
            configure_common_curl_options(handle.get(), url, error_buffer);
            curl_easy_setopt(handle.get(), CURLOPT_RANGE, "0-0");
            curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, capture_headers);
            curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &headers);
            curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, abort_probe_body);

            const CURLcode code = curl_easy_perform(handle.get());
            long status_code = headers.status_code;
            curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);
            if ((code == CURLE_OK || code == CURLE_WRITE_ERROR) && status_code >= 200 && status_code < 300) {
                if (headers.content_range_total.has_value()) {
                    return *headers.content_range_total;
                }
                if (headers.content_length.has_value()) {
                    return *headers.content_length;
                }
            }
            if (code != CURLE_OK) {
                last_error = curl_failure_text(code, error_buffer);
            } else {
                last_error = "HTTP " + std::to_string(status_code);
            }
        }

        if (attempt < kDefaultRetries) {
            std::this_thread::sleep_for(retry_delay(attempt));
        }
    }

    throw std::runtime_error("Could not determine remote size for " + url + ": " + last_error);
}

class DownloadFailure : public std::runtime_error {
   public:
    DownloadFailure(std::string message, long failure_http_status = 0, CURLcode failure_curl_code = CURLE_OK,
                    bool should_discard_partial = false)
        : std::runtime_error(std::move(message)),
          http_status(failure_http_status),
          curl_code(failure_curl_code),
          discard_partial(should_discard_partial) {}

    long http_status;
    CURLcode curl_code;
    bool discard_partial;
};

struct FileSink {
    std::filesystem::path path;
    HeaderInfo *headers = nullptr;
    std::uint64_t existing_size = 0;
    std::uint64_t current_size = 0;
    std::FILE *file = nullptr;
    std::string error;

    FileSink(std::filesystem::path output_path, HeaderInfo *response_headers, std::uint64_t partial_size)
        : path(std::move(output_path)), headers(response_headers), existing_size(partial_size) {}

    FileSink(const FileSink &) = delete;
    FileSink &operator=(const FileSink &) = delete;

    ~FileSink() { close(); }

    bool open_if_needed() {
        if (file != nullptr) {
            return true;
        }
        const bool append = existing_size > 0 && headers->status_code == 206;
        file = std::fopen(path.c_str(), append ? "ab" : "wb");
        if (file == nullptr) {
            error = "Failed to open partial download " + path.string() + ": " + std::strerror(errno);
            return false;
        }
        current_size = append ? existing_size : 0;
        return true;
    }

    void close() {
        if (file != nullptr) {
            if (std::fclose(file) != 0 && error.empty()) {
                error = "Failed to close partial download " + path.string() + ": " + std::strerror(errno);
            }
            file = nullptr;
        }
    }
};

std::size_t write_download_file(char *data, std::size_t size, std::size_t count, void *userdata) {
    const std::size_t byte_count = size * count;
    auto *sink = static_cast<FileSink *>(userdata);

    if (sink->headers->status_code != 200 && sink->headers->status_code != 206) {
        return byte_count;
    }
    if (!sink->open_if_needed()) {
        return 0;
    }

    const std::size_t written = std::fwrite(data, 1, byte_count, sink->file);
    sink->current_size += static_cast<std::uint64_t>(written);
    if (written != byte_count && sink->error.empty()) {
        sink->error = "Failed to write partial download " + sink->path.string() + ": " + std::strerror(errno);
    }
    return written;
}

std::uint64_t download_resource(Resource *resource, const std::filesystem::path &base_dir) {
    const std::filesystem::path cache_path = cache_artifact_path(base_dir, *resource);
    if (std::filesystem::exists(cache_path)) {
        return safe_file_size(cache_path);
    }

    const std::filesystem::path destination = destination_path(base_dir, *resource);
    if (std::filesystem::exists(destination)) {
        return safe_file_size(destination);
    }

    std::error_code directory_error;
    std::filesystem::create_directories(destination.parent_path(), directory_error);
    if (directory_error) {
        throw DownloadFailure("Failed to create download directory " + destination.parent_path().string() + ": " +
                              directory_error.message());
    }

    const std::filesystem::path partial = partial_path(destination);
    std::uint64_t existing_size = safe_file_size(partial);
    if (resource->remote_size == 0 && existing_size > 0) {
        resource->remote_size = probe_remote_size(resource->url);
    }
    if (resource->remote_size > 0 && existing_size > resource->remote_size) {
        std::filesystem::remove(partial);
        throw DownloadFailure("Local partial file is larger than remote file: " + partial.string(), 0, CURLE_OK,
                              true);
    }
    if (resource->remote_size > 0 && existing_size == resource->remote_size) {
        std::error_code rename_error;
        std::filesystem::rename(partial, destination, rename_error);
        if (rename_error) {
            throw DownloadFailure("Failed to finalize download " + destination.string() + ": " +
                                  rename_error.message());
        }
        return resource->remote_size;
    }

    CurlEasy handle = make_curl_handle();
    char error_buffer[CURL_ERROR_SIZE]{};
    HeaderInfo headers;
    FileSink sink(partial, &headers, existing_size);
    configure_common_curl_options(handle.get(), resource->url, error_buffer);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, capture_headers);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_download_file);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &sink);
    if (existing_size > 0) {
        const std::string range = std::to_string(existing_size) + "-";
        curl_easy_setopt(handle.get(), CURLOPT_RANGE, range.c_str());
    }

    const CURLcode code = curl_easy_perform(handle.get());
    sink.close();
    long status_code = headers.status_code;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);

    if (!sink.error.empty()) {
        throw DownloadFailure(sink.error, status_code, code);
    }
    if (code != CURLE_OK) {
        throw DownloadFailure(curl_failure_text(code, error_buffer), status_code, code);
    }
    if (status_code != 200 && status_code != 206) {
        throw DownloadFailure("HTTP " + std::to_string(status_code) + " while opening " + resource->url,
                              status_code);
    }
    if (!std::filesystem::exists(partial) && !sink.open_if_needed()) {
        throw DownloadFailure(sink.error, status_code);
    }
    sink.close();
    if (!sink.error.empty()) {
        throw DownloadFailure(sink.error, status_code);
    }

    if (headers.content_range_total.has_value()) {
        resource->remote_size = *headers.content_range_total;
    } else if (headers.content_length.has_value()) {
        resource->remote_size = headers.status_code == 206 ? existing_size + *headers.content_length
                                                           : *headers.content_length;
    }

    const std::uint64_t final_size = safe_file_size(partial);
    if (resource->remote_size > 0 && final_size != resource->remote_size) {
        std::filesystem::remove(partial);
        throw DownloadFailure("Downloaded size mismatch for " + partial.string() + ": got " +
                                  std::to_string(final_size) + ", expected " +
                                  std::to_string(resource->remote_size),
                              status_code, CURLE_OK, true);
    }

    std::error_code rename_error;
    std::filesystem::rename(partial, destination, rename_error);
    if (rename_error) {
        throw DownloadFailure("Failed to finalize download " + destination.string() + ": " +
                              rename_error.message());
    }
    return final_size;
}

struct FailedDownload {
    std::size_t resource_index = 0;
    std::string reason;
    long http_status = 0;
    CURLcode curl_code = CURLE_OK;
    bool discard_partial = false;
};

std::vector<FailedDownload> run_download_round(std::vector<Resource> *resources,
                                               const std::vector<std::size_t> &pending,
                                               const std::filesystem::path &base_dir, std::size_t worker_count,
                                               FileProgressDisplay *progress,
                                               std::vector<bool> *completed_resources) {
    std::atomic<std::size_t> next_index{0};
    std::mutex failures_mutex;
    std::mutex completed_mutex;
    std::vector<FailedDownload> failures;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        workers.emplace_back([&]() {
            while (true) {
                const std::size_t pending_index = next_index.fetch_add(1, std::memory_order_relaxed);
                if (pending_index >= pending.size()) {
                    return;
                }

                const std::size_t resource_index = pending[pending_index];
                try {
                    const std::uint64_t downloaded_size =
                        download_resource(&resources->at(resource_index), base_dir);
                    {
                        std::lock_guard<std::mutex> lock(completed_mutex);
                        if (!completed_resources->at(resource_index)) {
                            completed_resources->at(resource_index) = true;
                            if (progress != nullptr) {
                                progress->mark_batch_completed(1, downloaded_size);
                            }
                        }
                    }
                } catch (const DownloadFailure &exc) {
                    std::lock_guard<std::mutex> lock(failures_mutex);
                    failures.push_back(FailedDownload{resource_index, exc.what(), exc.http_status, exc.curl_code,
                                                       exc.discard_partial});
                } catch (const std::exception &exc) {
                    std::lock_guard<std::mutex> lock(failures_mutex);
                    failures.push_back(FailedDownload{resource_index, exc.what()});
                }
            }
        });
    }

    for (std::thread &worker : workers) {
        worker.join();
    }
    std::sort(failures.begin(), failures.end(), [](const FailedDownload &lhs, const FailedDownload &rhs) {
        return lhs.resource_index < rhs.resource_index;
    });
    return failures;
}

std::string solution_hint(const FailedDownload &failure) {
    if (failure.http_status == 404) {
        return "建议: 资源清单列出了该文件，但服务器返回 404；上游目录或镜像可能暂时不一致，请稍后重试。";
    }
    if (failure.http_status == 429 || failure.http_status == 500 || failure.http_status == 502 ||
        failure.http_status == 503 || failure.http_status == 504) {
        return "建议: 上游暂时不可用或限流，稍后重试，或降低 download_workers。";
    }
    if (failure.curl_code == CURLE_OPERATION_TIMEDOUT || failure.curl_code == CURLE_COULDNT_CONNECT ||
        failure.curl_code == CURLE_COULDNT_RESOLVE_HOST || failure.curl_code == CURLE_SSL_CONNECT_ERROR ||
        failure.curl_code == CURLE_PEER_FAILED_VERIFICATION) {
        return "建议: 检查网络、代理、防火墙、CA 证书和系统时间，必要时降低 download_workers。";
    }
    if (failure.discard_partial) {
        return "建议: 损坏的分片已删除，下次会从头下载该文件。";
    }
    return "建议: 检查网络、磁盘空间和目录权限；下次运行会重新尝试该文件。";
}

}  // namespace

DownloadClient::DownloadClient(Config config) : config_(std::move(config)) { ensure_curl_initialized(); }

std::vector<DownloadTarget> DownloadClient::collect_targets(const ClosedDateRange &range,
                                                            int limit_override,
                                                            bool show_progress) const {
    const bool uses_routeviews_direct = config_.collector.rfind("route-views", 0) == 0;
    const std::size_t discovery_steps =
        uses_routeviews_direct ? routeviews_archive::months_for_range(range).size() : 1;
    std::unique_ptr<FileProgressDisplay> progress;
    if (show_progress && discovery_steps > 0) {
        progress = std::make_unique<FileProgressDisplay>(
            discovery_steps, 0, "discover", uses_routeviews_direct ? "months" : "requests", false);
    }

    const std::vector<Resource> resources =
        limited_resources(range, config_, resolve_limit(limit_override), progress.get());
    if (progress != nullptr) {
        progress->finish();
    }
    std::vector<DownloadTarget> targets;
    targets.reserve(resources.size());
    for (const Resource &resource : resources) {
        const std::filesystem::path destination = destination_path(config_.output_dir, resource);
        targets.push_back(DownloadTarget{
            destination,
            preferred_local_path(config_.output_dir, resource),
            resource.remote_size,
        });
    }
    return targets;
}

void DownloadClient::download_range(const ClosedDateRange &range, int limit_override, bool show_progress) const {
    std::vector<Resource> resources = limited_resources(range, config_, resolve_limit(limit_override));
    if (resources.empty()) {
        return;
    }

    std::vector<bool> completed_resources(resources.size(), false);
    std::vector<std::size_t> pending;
    std::size_t completed_count = 0;
    std::uint64_t completed_bytes = 0;
    std::uint64_t known_total_bytes = 0;
    bool all_sizes_known = true;

    for (std::size_t index = 0; index < resources.size(); ++index) {
        const Resource &resource = resources[index];
        if (resource.remote_size == 0) {
            all_sizes_known = false;
        } else {
            known_total_bytes += resource.remote_size;
        }
        const std::optional<std::filesystem::path> local_path = available_local_path(config_.output_dir, resource);
        if (local_path.has_value()) {
            completed_resources[index] = true;
            ++completed_count;
            completed_bytes += safe_file_size(*local_path);
        } else {
            pending.push_back(index);
        }
    }

    if (pending.empty()) {
        std::cout << "all matched files already cached locally" << std::endl;
        return;
    }

    std::unique_ptr<FileProgressDisplay> progress;
    if (show_progress) {
        progress = std::make_unique<FileProgressDisplay>(resources.size(),
                                                         all_sizes_known ? known_total_bytes : 0, "download");
    }
    if (progress != nullptr && completed_count > 0) {
        progress->mark_batch_completed(completed_count, completed_bytes);
    }

    std::vector<FailedDownload> failures;
    try {
        const std::size_t worker_count =
            std::min<std::size_t>(pending.size(), static_cast<std::size_t>(config_.download_workers));
        failures = run_download_round(&resources, pending, config_.output_dir, worker_count, progress.get(),
                                      &completed_resources);

        for (int retry_round = 1; retry_round <= kDefaultRetries && !failures.empty(); ++retry_round) {
            std::this_thread::sleep_for(retry_delay(retry_round));
            pending.clear();
            pending.reserve(failures.size());
            for (const FailedDownload &failure : failures) {
                pending.push_back(failure.resource_index);
            }
            const std::size_t retry_workers =
                std::min<std::size_t>(pending.size(), static_cast<std::size_t>(config_.download_workers));
            failures = run_download_round(&resources, pending, config_.output_dir, retry_workers, progress.get(),
                                          &completed_resources);
        }
        if (progress != nullptr) {
            progress->finish();
        }
    } catch (...) {
        if (progress != nullptr) {
            progress->close();
        }
        throw;
    }

    for (const FailedDownload &failure : failures) {
        const Resource &resource = resources[failure.resource_index];
        const std::filesystem::path destination = destination_path(config_.output_dir, resource);
        const std::filesystem::path partial = partial_path(destination);
        const std::uint64_t partial_bytes = safe_file_size(partial);
        if (failure.discard_partial) {
            std::error_code remove_error;
            std::filesystem::remove(partial, remove_error);
            if (partial_bytes > 0 && !remove_error) {
                std::cerr << "已删除无法续传的损坏分片: " << partial.filename().string() << " ("
                          << format_bytes(partial_bytes) << ")\n";
            }
        } else if (partial_bytes > 0) {
            std::cerr << "已保留下载分片以便下次断点续传: " << partial.filename().string() << " ("
                      << format_bytes(partial_bytes) << ")\n";
        }
        std::cerr << "下载失败: " << destination.filename().string() << " | " << failure.reason << " | "
                  << solution_hint(failure) << '\n';
    }

    const std::size_t available_count = static_cast<std::size_t>(
        std::count(completed_resources.begin(), completed_resources.end(), true));
    std::cout << "download complete: " << available_count << '/' << resources.size() << " files available"
              << std::endl;
}

int DownloadClient::resolve_limit(int limit_override) const {
    if (limit_override >= 0) {
        return limit_override;
    }
    return config_.limit;
}

}  // namespace bgpstream_runner
