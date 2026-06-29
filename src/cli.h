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

    // Pagination (PDF only). When --page-size is given, the PDF is split into
    // pages of that paper size; otherwise output stays a single page (default).
    std::string page_size;          // --page-size: preset or WxH[unit]; empty = single page
    bool        landscape  = false; // --landscape (only meaningful with --page-size)
    double      margin_mm  = 10.0;  // --margin in millimetres (only with --page-size)
    // Resolved geometry, filled by parse_args when page_size is non-empty.
    // page_w_pt > 0 signals "paginate"; all values are in PDF points (1/72 in).
    double      page_w_pt  = 0.0;
    double      page_h_pt  = 0.0;
    double      margin_pt  = 0.0;
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
