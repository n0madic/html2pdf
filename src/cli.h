#pragma once

#include <string>

namespace html2pdf {

// Output format inferred from the --output file extension.
enum class Format { PNG, PDF };

// Parsed command-line options.
struct Options {
    std::string input;       // --input  (mutually exclusive with url)
    std::string url;         // --url     (mutually exclusive with input)
    std::string output;      // --output  (required)
    int width = 1024;        // --width
    int height = 0;          // --height  (0 = auto, derived from content height)
    std::string user_agent = "html2pdf/1.0";  // --user-agent
    long timeout_sec = 30;   // --timeout (seconds)
    bool allow_local_network = false;  // --allow-local-network (disables SSRF guard)
    Format format = Format::PNG;  // derived from output extension
};

// Result of parsing the command line.
struct ParseResult {
    enum class Status {
        Ok,        // options are valid, proceed
        HelpShown, // --help/-h printed, exit 0
        Error      // invalid arguments, exit 1 (message already on stderr)
    };
    Status status = Status::Error;
    Options options;
};

// Parse argv into Options. On --help prints usage to stdout and returns
// HelpShown. On any validation error prints a message to stderr and returns
// Error. Otherwise returns Ok with a fully populated Options.
ParseResult parse_args(int argc, char** argv);

// Print usage text to the given stream.
void print_usage(const char* prog);

}  // namespace html2pdf
