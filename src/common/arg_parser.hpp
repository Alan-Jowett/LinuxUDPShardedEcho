/**
 * @file arg_parser.hpp
 * @brief Lightweight command-line argument parser used by client/server examples.
 *
 * The ArgParser class provides a minimal API to register long and short
 * command-line options, parse `argc/argv` and query option values. It is
 * intentionally small and forgiving: unknown options are ignored and
 * positional arguments are not collected.
 *
 * @copyright Copyright (c) 2025 LinuxUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @class ArgParser
 * @brief Simple command-line option registrar and parser.
 *
 * Usage example:
 * @code
 * ArgParser p;
 * p.add_option("server", 's', "localhost", true, "server address");
 * p.parse(argc, argv);
 * auto host = p.get("server");
 * @endcode
 */
class ArgParser {
   public:
    /**
     * Register a command-line option.
     *
     * @param long_name The long option name (without leading dashes), e.g. "server".
     * @param short_name A single-character short name (e.g. 's'), or '\0' if none.
     * @param default_value Default string value returned by `get()` when option is not set.
     * @param takes_value If true the option expects a value (e.g. `--key value` or `--key=value`).
     *                    If false the option is treated as a boolean flag (set => "1").
     * @param description Optional human-readable description shown by `print_help()`.
     */
    void add_option(const std::string& long_name, char short_name, const std::string& default_value,
                    bool takes_value = true, const std::string& description = "") {
        Option opt;
        opt.short_name = short_name;
        opt.default_value = default_value;
        opt.value = default_value;
        opt.takes_value = takes_value;
        opt.set = false;
        opt.description = description;
        opts_.emplace(long_name, std::move(opt));
        if (short_name != '\0') short_to_long_.emplace(short_name, long_name);
    }

    /**
     * Parse command-line arguments.
     *
     * Unknown options are silently ignored. Positional arguments are not
     * stored by this parser. Supports GNU-style `--name=value`, `--name value`
     * and single-character short flags `-s value` or `-f` (for non-value flags).
     *
     * @param argc Program `argc`.
     * @param argv Program `argv`.
     */
    void parse(int argc, const char* const argv[]) {
        for (int i = 1; i < argc; ++i) {
            const char* a = argv[i];
            if (std::strncmp(a, "--", 2) == 0) {
                const char* name = a + 2;
                const char* eq = std::strchr(name, '=');
                std::string key;
                std::string val;
                if (eq) {
                    key.assign(name, eq - name);
                    val = std::string(eq + 1);
                } else {
                    key = name;
                }
                auto it = opts_.find(key);
                if (it == opts_.end()) continue;
                if (it->second.takes_value) {
                    if (!val.empty()) {
                        it->second.value = val;
                        it->second.set = true;
                    } else if (i + 1 < argc) {
                        it->second.value = argv[++i];
                        it->second.set = true;
                    }
                } else {
                    // flag
                    it->second.value = "1";
                    it->second.set = true;
                }
            } else if (a[0] == '-' && a[1] != '\0') {
                // short flags (only single-character supported)
                char shortname = a[1];
                auto itmap = short_to_long_.find(shortname);
                if (itmap == short_to_long_.end()) continue;
                auto it = opts_.find(itmap->second);
                if (it == opts_.end()) continue;
                if (it->second.takes_value) {
                    if (i + 1 < argc) {
                        it->second.value = argv[++i];
                        it->second.set = true;
                    }
                } else {
                    it->second.value = "1";
                    it->second.set = true;
                }
            } else {
                // Positional arguments are ignored by the parser
            }
        }
    }

    /**
     * Retrieve the string value for a registered option.
     *
     * Returns the parsed value if the option was supplied on the command-line,
     * otherwise returns the option's registered `default_value`.
     *
     * @param long_name The long option name to query.
     * @throws std::invalid_argument if the option was not registered via `add_option`.
     * @return The option value as a std::string.
     */
    std::string get(const std::string& long_name) const {
        auto it = opts_.find(long_name);
        if (it == opts_.end()) throw std::invalid_argument("Unknown option: " + long_name);
        return it->second.value;
    }

    /**
     * Print a simple usage/help message to `std::cout`.
     *
     * Iterates over registered options and prints the long and short
     * names, whether a value is expected, the default value, and the
     * optional description.
     *
     * @param program_name Name of the program (typically `argv[0]`).
     */
    void print_help(const char* program_name) const {
        std::cout << "Usage: " << program_name << " [options]\n";
        std::cout << "Options:\n";
        for (const auto& kv : opts_) {
            const std::string& long_name = kv.first;
            const Option& opt = kv.second;
            std::cout << "  --" << long_name;
            if (opt.short_name != '\0') std::cout << ", -" << opt.short_name;
            if (opt.takes_value) std::cout << " <value>";
            std::cout << "\tDefault: " << opt.default_value;
            if (!opt.description.empty()) std::cout << "\t" << opt.description;
            std::cout << "\n";
        }
    }

    /**
     * Check whether an option was explicitly set on the command-line.
     *
     * @param long_name The long option name to check.
     * @return true if the option was provided by the user, false otherwise.
     */
    bool is_set(const std::string& long_name) const {
        auto it = opts_.find(long_name);
        if (it == opts_.end()) return false;
        return it->second.set;
    }

   private:
    /**
     * Internal representation for a registered option.
     */
    struct Option {
        /// Short single-character name (or '\0' if none).
        char short_name;
        /// Value returned when option is not set.
        std::string default_value;
        /// Current value (either parsed value or `default_value`).
        std::string value;
        /// Whether this option expects a value.
        bool takes_value;
        /// True if the option was explicitly provided on the command-line.
        bool set;
        /// Optional description used by `print_help()`.
        std::string description;
    };

    /// Map of long option name -> Option metadata and values.
    std::unordered_map<std::string, Option> opts_;
    /// Map of short name -> long option name for quick lookup during parsing.
    std::unordered_map<char, std::string> short_to_long_;
};
