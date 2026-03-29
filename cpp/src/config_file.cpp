#include "bgpstream_runner/config_file.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bgpstream_runner {

namespace {

enum class JsonScalarKind {
  String,
  Integer,
  Boolean,
  Null,
};

struct JsonScalar {
  JsonScalarKind kind = JsonScalarKind::Null;
  std::string string_value;
  long long integer_value = 0;
  bool bool_value = false;
};

class JsonConfigParser {
public:
  JsonConfigParser(std::string text, std::filesystem::path path, Config *config)
      : text_(std::move(text)), path_(std::move(path)), config_(config) {}

  void parse() {
    skip_whitespace();
    expect('{');
    skip_whitespace();

    if (peek() == '}') {
      ++position_;
      skip_whitespace();
      ensure_eof();
      return;
    }

    while (true) {
      skip_whitespace();
      const std::string key = parse_string();
      skip_whitespace();
      expect(':');
      skip_whitespace();
      const JsonScalar value = parse_scalar();
      apply_value(key, value);
      skip_whitespace();

      const char ch = peek();
      if (ch == ',') {
        ++position_;
        continue;
      }
      if (ch == '}') {
        ++position_;
        break;
      }
      error("Expected ',' or '}'");
    }

    skip_whitespace();
    ensure_eof();
  }

private:
  [[noreturn]] void error(std::string_view message) const {
    throw std::runtime_error("Invalid config JSON in " + path_.string() +
                             " at byte " + std::to_string(position_) + ": " +
                             std::string(message));
  }

  void ensure_eof() const {
    if (position_ != text_.size()) {
      error("Unexpected trailing content");
    }
  }

  char peek() const {
    if (position_ >= text_.size()) {
      return '\0';
    }
    return text_[position_];
  }

  char consume() {
    if (position_ >= text_.size()) {
      error("Unexpected end of file");
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
    const char ch = consume();
    if (ch != expected) {
      error(std::string("Expected '") + expected + "'");
    }
  }

  bool consume_literal(std::string_view literal) {
    if (text_.substr(position_, literal.size()) == literal) {
      position_ += literal.size();
      return true;
    }
    return false;
  }

  std::string parse_string() {
    expect('"');
    std::string value;

    while (true) {
      const char ch = consume();
      if (ch == '"') {
        return value;
      }
      if (ch == '\\') {
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
        default:
          error("Unsupported string escape");
        }
        continue;
      }
      if (static_cast<unsigned char>(ch) < 0x20) {
        error("Control characters are not allowed in JSON strings");
      }
      value.push_back(ch);
    }
  }

  long long parse_integer() {
    const std::size_t start = position_;
    if (peek() == '-') {
      ++position_;
    }
    if (!std::isdigit(static_cast<unsigned char>(peek()))) {
      error("Expected integer");
    }
    while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
      ++position_;
    }

    try {
      return std::stoll(text_.substr(start, position_ - start));
    } catch (const std::exception &) {
      error("Invalid integer value");
    }
  }

  JsonScalar parse_scalar() {
    JsonScalar value;
    const char ch = peek();

    if (ch == '"') {
      value.kind = JsonScalarKind::String;
      value.string_value = parse_string();
      return value;
    }
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
      value.kind = JsonScalarKind::Integer;
      value.integer_value = parse_integer();
      return value;
    }
    if (consume_literal("true")) {
      value.kind = JsonScalarKind::Boolean;
      value.bool_value = true;
      return value;
    }
    if (consume_literal("false")) {
      value.kind = JsonScalarKind::Boolean;
      value.bool_value = false;
      return value;
    }
    if (consume_literal("null")) {
      value.kind = JsonScalarKind::Null;
      return value;
    }

    error("Unsupported JSON value type");
  }

  int require_int(const std::string &key, const JsonScalar &value) const {
    if (value.kind != JsonScalarKind::Integer) {
      throw std::runtime_error("Config key '" + key + "' must be an integer");
    }
    return static_cast<int>(value.integer_value);
  }

  bool require_bool(const std::string &key, const JsonScalar &value) const {
    if (value.kind != JsonScalarKind::Boolean) {
      throw std::runtime_error("Config key '" + key + "' must be a boolean");
    }
    return value.bool_value;
  }

  std::string require_string(const std::string &key,
                             const JsonScalar &value) const {
    if (value.kind != JsonScalarKind::String) {
      throw std::runtime_error("Config key '" + key + "' must be a string");
    }
    return value.string_value;
  }

  ChunkUnit require_chunk_unit(const std::string &key,
                               const JsonScalar &value) const {
    const std::string text = require_string(key, value);
    if (text == "day" || text == "days") {
      return ChunkUnit::Day;
    }
    if (text == "month" || text == "months") {
      return ChunkUnit::Month;
    }
    throw std::runtime_error("Config key '" + key +
                             "' must be 'day' or 'month'");
  }

  void apply_value(const std::string &key, const JsonScalar &value) {
    if (key == "start_date") {
      config_->start_date = require_string(key, value);
    } else if (key == "end_date") {
      config_->end_date = require_string(key, value);
    } else if (key == "project") {
      config_->project = require_string(key, value);
    } else if (key == "collector") {
      config_->collector = require_string(key, value);
    } else if (key == "output_dir") {
      config_->output_dir = require_string(key, value);
    } else if (key == "download_workers") {
      config_->download_workers = require_int(key, value);
    } else if (key == "parser_workers") {
      config_->parser_workers = require_int(key, value);
    } else if (key == "message_batch_size") {
      config_->message_batch_size = require_int(key, value);
    } else if (key == "chunk_size") {
      config_->chunk_size = require_int(key, value);
    } else if (key == "chunk_unit") {
      config_->chunk_unit = require_chunk_unit(key, value);
    } else if (key == "limit") {
      if (value.kind == JsonScalarKind::Null) {
        config_->limit = -1;
      } else {
        config_->limit = require_int(key, value);
      }
    } else if (key == "log_phase_transitions") {
      config_->log_phase_transitions = require_bool(key, value);
    } else if (key == "log_chunk_summary") {
      config_->log_chunk_summary = require_bool(key, value);
    } else if (key == "log_final_summary") {
      config_->log_final_summary = require_bool(key, value);
    } else {
      throw std::runtime_error("Unknown config key '" + key + "' in " +
                               path_.string());
    }
  }

  const std::string text_;
  const std::filesystem::path path_;
  Config *config_ = nullptr;
  std::size_t position_ = 0;
};

} // namespace

void apply_json_config_file(const std::filesystem::path &path, Config *config) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open config file: " + path.string());
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  JsonConfigParser parser(buffer.str(), path, config);
  parser.parse();
}

} // namespace bgpstream_runner
