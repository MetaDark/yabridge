// yabridge: a Wine VST bridge
// Copyright (C) 2020-2021 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "utils.h"

#include <unistd.h>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/process/io.hpp>
#include <boost/process/pipe.hpp>
#include <boost/process/search_path.hpp>
#include <boost/process/system.hpp>
#include <sstream>

// Generated inside of the build directory
#include <src/common/config/config.h>

#include "../common/configuration.h"
#include "../common/utils.h"

namespace bp = boost::process;
namespace fs = boost::filesystem;

// These functions are used to populate the fields in `PluginInfo`. See the
// docstrings for the corresponding fields for more information on what we're
// actually doing here.
fs::path find_plugin_library(const fs::path& this_plugin_path,
                             PluginType plugin_type,
                             bool prefer_32bit_vst3);
fs::path normalize_plugin_path(const fs::path& windows_library_path,
                               PluginType plugin_type);
std::variant<OverridenWinePrefix, fs::path, DefaultWinePrefix> find_wine_prefix(
    fs::path windows_plugin_path);

PluginInfo::PluginInfo(PluginType plugin_type, bool prefer_32bit_vst3)
    : plugin_type(plugin_type),
      native_library_path(get_this_file_location()),
      // As explained in the docstring, this is the actual Windows library. For
      // VST3 plugins that come in a module we should be loading that module
      // instead of the `.vst3` file within in, which is where
      // `windows_plugin_path` comes in.
      windows_library_path(find_plugin_library(native_library_path,
                                               plugin_type,
                                               prefer_32bit_vst3)),
      plugin_arch(find_dll_architecture(windows_library_path)),
      windows_plugin_path(
          normalize_plugin_path(windows_library_path, plugin_type)),
      wine_prefix(find_wine_prefix(windows_plugin_path)) {}

bp::environment PluginInfo::create_host_env() const {
    bp::environment env = boost::this_process::environment();

    // Only set the prefix when could auto detect it and it's not being
    // overridden (this entire `std::visit` instead of `std::has_alternative` is
    // just for clarity's sake)
    std::visit(overload{
                   [](const OverridenWinePrefix&) {},
                   [&](const boost::filesystem::path& prefix) {
                       env["WINEPREFIX"] = prefix.string();
                   },
                   [](const DefaultWinePrefix&) {},
               },
               wine_prefix);

    return env;
}

boost::filesystem::path PluginInfo::normalize_wine_prefix() const {
    return std::visit(
        overload{
            [](const OverridenWinePrefix& prefix) { return prefix.value; },
            [](const boost::filesystem::path& prefix) { return prefix; },
            [](const DefaultWinePrefix&) {
                const bp::environment env = boost::this_process::environment();
                return fs::path(env.at("HOME").to_string()) / ".wine";
            },
        },
        wine_prefix);
}

fs::path find_plugin_library(const fs::path& this_plugin_path,
                             PluginType plugin_type,
                             bool prefer_32bit_vst3) {
    switch (plugin_type) {
        case PluginType::vst2: {
            fs::path plugin_path(this_plugin_path);
            plugin_path.replace_extension(".dll");
            if (fs::exists(plugin_path)) {
                // Also resolve symlinks here, to support symlinked .dll files
                return fs::canonical(plugin_path);
            }

            // In case this files does not exist and our `.so` file is a
            // symlink, we'll also repeat this check after resolving that
            // symlink to support links to copies of `libyabridge-vst2.so` as
            // described in issue #3
            fs::path alternative_plugin_path = fs::canonical(this_plugin_path);
            alternative_plugin_path.replace_extension(".dll");
            if (fs::exists(alternative_plugin_path)) {
                return fs::canonical(alternative_plugin_path);
            }

            // This function is used in the constructor's initializer list so we
            // have to throw when the path could not be found
            throw std::runtime_error("'" + plugin_path.string() +
                                     "' does not exist, make sure to rename "
                                     "'libyabridge-vst2.so' to match a "
                                     "VST plugin .dll file.");
        } break;
        case PluginType::vst3: {
            // A VST3 plugin in Linux always has to be inside of a bundle (=
            // directory) named `X.vst3` that contains a static object
            // `X.vst3/Contents/x86_64-linux/X.so`. On Linux `X.so` is not
            // allowed to be standalone, so for yabridge this should also always
            // be installed this way.
            // https://developer.steinberg.help/pages/viewpage.action?pageId=9798275
            const fs::path bundle_home =
                this_plugin_path.parent_path().parent_path().parent_path();
            const fs::path win_module_name =
                this_plugin_path.filename().replace_extension(".vst3");

            // Quick check in case the plugin was set up without yabridgectl,
            // since the format is very specific and any deviations from that
            // will be incorrect.
            if (bundle_home.extension() != ".vst3") {
                throw std::runtime_error(
                    "'" + this_plugin_path.string() +
                    "' is not inside of a VST3 bundle. Use yabridgectl to "
                    "set up yabridge for VST3 plugins or check the readme "
                    "for the correct format.");
            }

            // Finding the Windows plugin consists of two steps because
            // Steinberg changed the format around:
            // - First we'll find the plugin in the VST3 bundle created by
            //   yabridgectl in `~/.vst3/yabridge`. The plugin can be either
            //   32-bit or 64-bit. If both exist, then we'll take the 64-bit
            //   version, unless the `vst3_prefer_32bit` yabridge.toml option
            //   has been enabled for this plugin.
            // - After that we'll resolve the symlink to the module in the Wine
            //   prefix, and then we'll have to figure out if this module is an
            //   old style standalone module (< 3.6.10) or if it's inside of
            //   a bundle (>= 3.6.10)
            const fs::path candidate_path_64bit =
                bundle_home / "Contents" / "x86_64-win" / win_module_name;
            const fs::path candidate_path_32bit =
                bundle_home / "Contents" / "x86-win" / win_module_name;

            // After this we'll have to use `normalize_plugin_path()` to get the
            // actual module entry point in case the plugin is using a VST
            // 3.6.10 style bundle, because we need to inspect that for the
            // _actual_ (with yabridgectl `x86_64-win` should only contain a
            // 64-bit plugin and `x86-win` should only contain a 32-bit plugin,
            // but you never know!)
            // NOLINTNEXTLINE(bugprone-branch-clone)
            if (prefer_32bit_vst3 && fs::exists(candidate_path_32bit)) {
                return fs::canonical(candidate_path_32bit);
            } else if (fs::exists(candidate_path_64bit)) {
                return fs::canonical(candidate_path_64bit);
            } else if (fs::exists(candidate_path_32bit)) {
                return fs::canonical(candidate_path_32bit);
            }

            throw std::runtime_error(
                "'" + bundle_home.string() +
                "' does not contain a Windows VST3 module. Use yabridgectl to "
                "set up yabridge for VST3 plugins or check the readme "
                "for the correct format.");
        } break;
        default:
            throw std::runtime_error("How did you manage to get this?");
            break;
    }
}

fs::path normalize_plugin_path(const fs::path& windows_library_path,
                               PluginType plugin_type) {
    switch (plugin_type) {
        case PluginType::vst2:
            return windows_library_path;
            break;
        case PluginType::vst3: {
            // Now we'll have to figure out if this is a new-style bundle or
            // an old standalone module
            const fs::path win_module_name =
                windows_library_path.filename().replace_extension(".vst3");
            const fs::path windows_bundle_home =
                windows_library_path.parent_path().parent_path().parent_path();
            if (equals_case_insensitive(windows_bundle_home.filename().string(),
                                        win_module_name.string())) {
                return windows_bundle_home;
            } else {
                return windows_library_path;
            }
        } break;
        default:
            throw std::runtime_error("How did you manage to get this?");
            break;
    }
}

std::variant<OverridenWinePrefix, fs::path, DefaultWinePrefix> find_wine_prefix(
    fs::path windows_plugin_path) {
    bp::environment env = boost::this_process::environment();
    if (!env["WINEPREFIX"].empty()) {
        return OverridenWinePrefix{env["WINEPREFIX"].to_string()};
    }

    std::optional<fs::path> dosdevices_dir = find_dominating_file(
        "dosdevices", windows_plugin_path, fs::is_directory);
    if (!dosdevices_dir) {
        return DefaultWinePrefix{};
    }

    return dosdevices_dir->parent_path();
}

fs::path get_this_file_location() {
    // HACK: Not sure why, but `boost::dll::this_line_location()` returns a path
    //       starting with a double slash on some systems. I've seen this happen
    //       on both Ubuntu 18.04 and 20.04, but not on Arch based distros.
    //       Under Linux a path starting with two slashes is treated the same as
    //       a path starting with only a single slash, but Wine will refuse to
    //       load any files when the path starts with two slashes. The easiest
    //       way to work around this if this happens is to just add another
    //       leading slash and then normalize the path, since three or more
    //       slashes will be coerced into a single slash.
    fs::path this_file = boost::dll::this_line_location();
    if (this_file.string().starts_with("//")) {
        this_file = ("/" / this_file).lexically_normal();
    }

    return this_file;
}

bool equals_case_insensitive(const std::string& a, const std::string& b) {
    return std::equal(a.begin(), a.end(), b.begin(),
                      [](const char& a_char, const char& b_char) {
                          return std::tolower(a_char) == std::tolower(b_char);
                      });
}

std::string join_quoted_strings(std::vector<std::string>& strings) {
    bool is_first = true;
    std::ostringstream joined_strings{};
    for (const auto& option : strings) {
        joined_strings << (is_first ? "'" : ", '") << option << "'";
        is_first = false;
    }

    return joined_strings.str();
}

std::string create_logger_prefix(const fs::path& endpoint_base_dir) {
    // Use the name of the base directory used for our sockets as the logger
    // prefix, but strip the `yabridge-` part since that's redundant
    std::string endpoint_name = endpoint_base_dir.filename().string();

    constexpr std::string_view socket_prefix("yabridge-");
    assert(endpoint_name.starts_with(socket_prefix));
    endpoint_name = endpoint_name.substr(socket_prefix.size());

    return "[" + endpoint_name + "] ";
}

fs::path find_vst_host(const boost::filesystem::path& this_plugin_path,
                       LibArchitecture plugin_arch,
                       bool use_plugin_groups) {
    auto host_name = use_plugin_groups ? yabridge_group_host_name
                                       : yabridge_individual_host_name;
    if (plugin_arch == LibArchitecture::dll_32) {
        host_name = use_plugin_groups ? yabridge_group_host_name_32bit
                                      : yabridge_individual_host_name_32bit;
    }

    // If our `.so` file is a symlink, then search for the host in the directory
    // of the file that symlink points to
    fs::path host_path =
        fs::canonical(this_plugin_path).remove_filename() / host_name;
    if (fs::exists(host_path)) {
        return host_path;
    }

    // Boost will return an empty path if the file could not be found in the
    // search path
    const fs::path vst_host_path =
        bp::search_path(host_name, get_augmented_search_path());
    if (vst_host_path == "") {
        throw std::runtime_error("Could not locate '" + std::string(host_name) +
                                 "'");
    }

    return vst_host_path;
}

boost::filesystem::path generate_group_endpoint(
    const std::string& group_name,
    const boost::filesystem::path& wine_prefix,
    const LibArchitecture architecture) {
    std::ostringstream socket_name;
    socket_name << "yabridge-group-" << group_name << "-"
                << std::to_string(
                       std::hash<std::string>{}(wine_prefix.string()))
                << "-";
    switch (architecture) {
        case LibArchitecture::dll_32:
            socket_name << "x32";
            break;
        case LibArchitecture::dll_64:
            socket_name << "x64";
            break;
    }
    socket_name << ".sock";

    return get_temporary_directory() / socket_name.str();
}

std::vector<boost::filesystem::path> get_augmented_search_path() {
    std::vector<boost::filesystem::path> search_path =
        boost::this_process::path();

    const bp::environment environment = boost::this_process::environment();
    if (auto home_directory = environment.find("HOME");
        home_directory != environment.end()) {
        search_path.push_back(fs::path(home_directory->to_string()) / ".local" /
                              "share" / "yabridge");
    }

    return search_path;
}

std::string get_wine_version() {
    // The '*.exe' scripts generated by winegcc allow you to override the binary
    // used to run Wine, so will will respect this as well
    std::string wine_path;
    bp::environment env = boost::this_process::environment();
    const std::string wineloader_path = env["WINELOADER"].to_string();
    if (access(wineloader_path.c_str(), X_OK) == 0) {
        wine_path = wineloader_path;
    } else {
        wine_path = bp::search_path("wine");
    }

    bp::ipstream output;
    try {
        bp::system(wine_path, "--version", bp::std_out = output);
    } catch (const std::system_error&) {
        return "<NOT FOUND>";
    }

    // `wine --version` might contain additional output in certain custom Wine
    // builds, so we only want to look at the first line
    std::string version_string;
    std::getline(output, version_string);

    // Strip the `wine-` prefix from the output, could potentially be absent in
    // custom Wine builds
    constexpr std::string_view version_prefix("wine-");
    if (version_string.starts_with(version_prefix)) {
        version_string = version_string.substr(version_prefix.size());
    }

    return version_string;
}

Configuration load_config_for(const fs::path& yabridge_path) {
    // First find the closest `yabridge.tmol` file for the plugin, falling back
    // to default configuration settings if it doesn't exist
    const std::optional<fs::path> config_file =
        find_dominating_file("yabridge.toml", yabridge_path);
    if (!config_file) {
        return Configuration();
    }

    return Configuration(*config_file, yabridge_path);
}
