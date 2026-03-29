#include "bgpstream_runner/plugin_loader.h"

#include <dlfcn.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bgpstream_runner {

namespace {

inline constexpr char kProcessorPluginManifestFilename[] = "bgpstream_processor_plugins.tsv";

struct RegisteredProcessorPlugin {
    std::string name;
    std::filesystem::path path;
};

std::string quote(std::string_view value) { return "'" + std::string(value) + "'"; }

std::string join_plugin_names(const std::vector<RegisteredProcessorPlugin> &plugins) {
    if (plugins.empty()) {
        return "(none)";
    }

    std::ostringstream output;
    for (std::size_t index = 0; index < plugins.size(); ++index) {
        if (index > 0) {
            output << ", ";
        }
        output << quote(plugins[index].name);
    }
    return output.str();
}

std::filesystem::path executable_dir(std::string_view argv0) {
    std::error_code error;
    const std::filesystem::path proc_self_exe = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !proc_self_exe.empty()) {
        return proc_self_exe.parent_path();
    }

    const std::filesystem::path executable_path(argv0.empty() ? "." : std::string(argv0));
    if (executable_path.has_parent_path()) {
        return std::filesystem::absolute(executable_path).parent_path();
    }
    return std::filesystem::current_path();
}

std::filesystem::path plugin_manifest_path(std::string_view argv0) {
    return executable_dir(argv0) / kProcessorPluginManifestFilename;
}

std::vector<RegisteredProcessorPlugin> read_registered_plugins(std::string_view argv0) {
    const std::filesystem::path manifest_path = plugin_manifest_path(argv0);
    std::ifstream input(manifest_path);
    if (!input) {
        return {};
    }

    std::vector<RegisteredProcessorPlugin> plugins;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }

        const std::size_t delimiter = line.find('\t');
        if (delimiter == std::string::npos || delimiter == 0 || delimiter + 1 >= line.size()) {
            throw std::runtime_error("Invalid processor plugin manifest entry in " + manifest_path.string() + " line " +
                                     std::to_string(line_number));
        }

        RegisteredProcessorPlugin plugin;
        plugin.name = line.substr(0, delimiter);
        plugin.path = line.substr(delimiter + 1);
        plugins.push_back(std::move(plugin));
    }

    return plugins;
}

bool is_same_path(const std::filesystem::path &left, const std::filesystem::path &right) {
    std::error_code error;
    const std::filesystem::path normalized_left = std::filesystem::weakly_canonical(left, error);
    if (error) {
        return false;
    }

    error.clear();
    const std::filesystem::path normalized_right = std::filesystem::weakly_canonical(right, error);
    if (error) {
        return false;
    }

    return normalized_left == normalized_right;
}

std::filesystem::path resolve_from_registered_plugins(const std::vector<RegisteredProcessorPlugin> &plugins,
                                                      std::string_view selector) {
    const std::string selector_text(selector);
    const std::filesystem::path selector_path(selector_text);
    std::vector<std::filesystem::path> matches;

    for (const RegisteredProcessorPlugin &plugin : plugins) {
        if (plugin.name == selector_text || plugin.path.filename() == selector_text || plugin.path.stem() == selector_text ||
            is_same_path(plugin.path, selector_path)) {
            matches.push_back(plugin.path);
        }
    }

    if (matches.empty()) {
        return {};
    }
    if (matches.size() > 1) {
        throw std::runtime_error("Processor plugin selector " + quote(selector) +
                                 " is ambiguous. Available plugins: " + join_plugin_names(plugins));
    }
    return matches.front();
}

}  // namespace

LoadedProcessorPlugin::LoadedProcessorPlugin(const Config &config, std::string_view argv0)
    : plugin_path_(resolve_plugin_path(config, argv0)) {
    handle_ = dlopen(plugin_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
        throw std::runtime_error("Failed to load processor plugin " + plugin_path_.string() + ": " + dlerror());
    }

    dlerror();
    auto *create_processor = reinterpret_cast<CreateProcessorFn>(dlsym(handle_, kCreateProcessorSymbol));
    const char *create_error = dlerror();
    if (create_error != nullptr || create_processor == nullptr) {
        close_handle();
        throw std::runtime_error("Failed to resolve symbol '" + std::string(kCreateProcessorSymbol) + "' in " +
                                 plugin_path_.string() + ": " + (create_error == nullptr ? "unknown error" : create_error));
    }

    dlerror();
    destroy_processor_ = reinterpret_cast<DestroyProcessorFn>(dlsym(handle_, kDestroyProcessorSymbol));
    const char *destroy_error = dlerror();
    if (destroy_error != nullptr || destroy_processor_ == nullptr) {
        close_handle();
        throw std::runtime_error("Failed to resolve symbol '" + std::string(kDestroyProcessorSymbol) + "' in " +
                                 plugin_path_.string() + ": " + (destroy_error == nullptr ? "unknown error" : destroy_error));
    }

    processor_ = std::unique_ptr<MessageProcessor, DestroyProcessorFn>(create_processor(), destroy_processor_);
    if (!processor_) {
        close_handle();
        throw std::runtime_error("Processor plugin returned a null processor: " + plugin_path_.string());
    }
}

LoadedProcessorPlugin::~LoadedProcessorPlugin() {
    processor_.reset();
    close_handle();
}

MessageProcessor &LoadedProcessorPlugin::processor() const { return *processor_; }

const std::filesystem::path &LoadedProcessorPlugin::plugin_path() const { return plugin_path_; }

std::filesystem::path LoadedProcessorPlugin::resolve_plugin_path(const Config &config, std::string_view argv0) const {
    const std::vector<RegisteredProcessorPlugin> registered_plugins = read_registered_plugins(argv0);

    if (!config.processor_plugin.empty()) {
        const std::filesystem::path configured_path(config.processor_plugin);
        if (std::filesystem::exists(configured_path)) {
            return std::filesystem::absolute(configured_path);
        }

        const std::filesystem::path executable_relative = executable_dir(argv0) / configured_path;
        if (!configured_path.is_absolute() && std::filesystem::exists(executable_relative)) {
            return std::filesystem::absolute(executable_relative);
        }

        const std::filesystem::path registered_match =
            resolve_from_registered_plugins(registered_plugins, config.processor_plugin);
        if (!registered_match.empty()) {
            return registered_match;
        }

        throw std::runtime_error("Processor plugin " + quote(config.processor_plugin) +
                                 " was not found. Available plugins: " + join_plugin_names(registered_plugins));
    }

    if (registered_plugins.empty()) {
        throw std::runtime_error("No processor plugins are registered. Add one under local/ and rebuild, or pass "
                                 "--processor-plugin PATH to load an external plugin.");
    }
    if (registered_plugins.size() > 1) {
        throw std::runtime_error("Multiple processor plugins are registered. Pass --processor-plugin NAME_OR_PATH. "
                                 "Available plugins: " +
                                 join_plugin_names(registered_plugins));
    }
    return registered_plugins.front().path;
}

void LoadedProcessorPlugin::close_handle() {
    if (handle_ != nullptr) {
        dlclose(handle_);
        handle_ = nullptr;
    }
}

}  // namespace bgpstream_runner
