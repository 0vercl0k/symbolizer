// Axel '0vercl0k' Souchet - September 11 2020
// #define SYMBOLIZER_DEBUG
#define _CRT_SECURE_NO_WARNINGS

#include "dbgeng_t.h"
#include <CLI/CLI.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fmt/os.h>
#include <fmt/printf.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;
namespace chrono = std::chrono;

//
// Utility to call a cleaner on scope exit.
//

template <typename F_t> struct Finally_t {
  F_t f_;
  Finally_t(F_t &&f) noexcept : f_(f) {}
  ~Finally_t() noexcept { f_(); }
};

template <typename F_t> [[nodiscard]] auto finally(F_t &&f) noexcept {
  return Finally_t(std::move(f));
}

//
// Utilities made to display seconds / numbers in a 'cleaner' way (w/ a unit,
// etc.).
//

struct SecondsHuman_t {
  double Value;
  const char *Unit;
};

struct NumberHuman_t {
  double Value;
  const char *Unit;
};

template <> struct fmt::formatter<NumberHuman_t> : fmt::formatter<std::string> {
  template <typename FormatContext>
  auto format(const NumberHuman_t &Number, FormatContext &Ctx) const
      -> decltype(Ctx.out()) {
    return fmt::format_to(Ctx.out(), "{:.1f}{}", Number.Value, Number.Unit);
  }
};

template <>
struct fmt::formatter<SecondsHuman_t> : fmt::formatter<std::string> {
  template <typename FormatContext>
  auto format(const SecondsHuman_t &Micro, FormatContext &Ctx) const
      -> decltype(Ctx.out()) {
    return fmt::format_to(Ctx.out(), "{:.1f}{}", Micro.Value, Micro.Unit);
  }
};

[[nodiscard]] constexpr NumberHuman_t NumberToHuman(const uint64_t N_) {
  const char *Unit = "";
  double N = double(N_);
  const uint64_t K = 1'000;
  const uint64_t M = K * K;
  if (N > M) {
    Unit = "m";
    N /= M;
  } else if (N > K) {
    Unit = "k";
    N /= K;
  }

  return {N, Unit};
}

[[nodiscard]] constexpr SecondsHuman_t
SecondsToHuman(const chrono::seconds &Seconds) {
  const char *Unit = "s";
  double SecondNumber = double(Seconds.count());
  const double M = 60;
  const double H = M * 60;
  const double D = H * 24;
  if (SecondNumber >= D) {
    Unit = "d";
    SecondNumber /= D;
  } else if (SecondNumber >= H) {
    Unit = "hr";
    SecondNumber /= H;
  } else if (SecondNumber >= M) {
    Unit = "min";
    SecondNumber /= M;
  }

  return {SecondNumber, Unit};
}

//
// Utility to calculate how many seconds since a past time point.
//

[[nodiscard]] chrono::seconds
SecondsSince(const chrono::high_resolution_clock::time_point &Since) {
  const auto &Now = chrono::high_resolution_clock::now();
  return chrono::duration_cast<chrono::seconds>(Now - Since);
}

//
// The various commad line options that Symbolizer supports.
//

struct Opts_t {

  //
  // The input path can be:
  //   - A path to a directory full of traces to symbolize,
  //   - A path to an input trace to symbolize.
  //

  fs::path Input;

  //
  // The output path can be:
  //   - A path to a directory where the output trace(s) are going to be written
  //   into,
  //   - A path to an output file where the output trace is going to be written
  //   into,
  //   - Empty if the output is to be dumped on stdout.
  //

  fs::path Output;

  //
  // This is the path to the crash-dump to load.
  //

  fs::path CrashdumpPath;

  //
  // Skip a number of lines.
  //

  uint64_t Skip = 0;

  //
  // The maximum amount of lines to process per file.
  //

  uint64_t Max = 0;

  //
  // This is the style used to output traces.
  //

  TraceStyle_t Style = TraceStyle_t::FullSymbol;

  //
  // Allow symbolizer to overwrite output traces.
  //

  bool Overwrite = false;

  //
  // Include line numbers in the output traces.
  //

  bool LineNumbers = false;
};

//
// Various stats we keep track of.
//

struct Stats_t {
  uint64_t NumberSymbolizedLines = 0;
  uint64_t NumberFailedSymbolization = 0;
  uint64_t NumberFiles = 0;
};

//
// The globals.
//

Opts_t Opts;
Stats_t Stats;

//
// Symbolize the |Input| into |Output|.
//

bool SymbolizeFile(DbgEng_t &Dbg, const fs::path &Input,
                   const fs::path &Output) {
  //
  // Open the input trace file.
  //

  HANDLE TraceFile =
      CreateFileA(Input.string().c_str(), GENERIC_READ, FILE_SHARE_READ,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  if (TraceFile == INVALID_HANDLE_VALUE) {
    fmt::print("Could not open input {}\n", Input.string());
    return false;
  }

  auto CloseTraceFile = finally([&] { CloseHandle(TraceFile); });

  HANDLE Mapping =
      CreateFileMappingA(TraceFile, nullptr, PAGE_READONLY, 0, 0, nullptr);

  if (Mapping == INVALID_HANDLE_VALUE) {
    fmt::print("Could not create a mapping\n");
    return false;
  }

  auto CloseMapping = finally([&] { CloseHandle(Mapping); });

  PVOID View = MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, 0);
  if (View == nullptr) {
    fmt::print("Could not map a view of the mapping\n");
    return false;
  }

  auto UnmapView = finally([&] { UnmapViewOfFile(View); });

  //
  // Open the output trace file; if we are not dumping data on stdout, then
  // let's actually open an output file.
  //

  const bool OutputIsStdout = Output.empty();
  std::optional<fmt::ostream> Out;
  if (!OutputIsStdout) {
    Out.emplace(fmt::output_file(Output.string()));
  }

  //
  // Read the trace file line by line.
  //

  uint64_t NumberSymbolizedLines = 0;
  uint64_t NumberFailedSymbolization = 0;
  char *Line = (char *)View;
  char *LineFeed = nullptr;
  for (uint64_t LineNumber = 0; (LineFeed = strchr(Line, '\n')) != nullptr;
       LineNumber++) {

    //
    // Do we have a max value, and if so have we hit it yet?
    //

    if (Opts.Max > 0 && NumberSymbolizedLines >= Opts.Max) {
      fmt::print("Hit the maximum number of symbolized lines {}, exiting\n",
                 NumberToHuman(Opts.Max));
      break;
    }

    //
    // Skipping a number of line.
    //

    if (LineNumber < Opts.Skip) {
      continue;
    }

    //
    // Convert the line into an address.
    //

    const uint64_t Address = std::strtoull(Line, nullptr, 16);
    const auto *LastLine = Line;
    Line = LineFeed + 1;

    //
    // Symbolize the address.
    //

    auto AddressSymbolized = Dbg.Symbolize(Address, Opts.Style);
    if (!AddressSymbolized.has_value()) {

      //
      // We're doing a best effort thing here. Basically, if there's a carriage
      // return right before the line feed, then the carriage return get
      // displayed below which kinda ruins the error output. So if there's a
      // carriage return, we'll just consider it the end of the line (vs the
      // line feed).
      //

      const auto *CarriageReturn = strchr(LastLine, '\r');
      const std::string_view FailedLine(
          LastLine, CarriageReturn ? CarriageReturn : LineFeed);

      fmt::print("{}:{}: Symbolization of {} failed ('{}'), skipping\n",
                 Input.filename().string(), LineNumber, Address, FailedLine);
      NumberFailedSymbolization++;
      continue;
    }

    //
    // Include the line numbers.
    //

    if (Opts.LineNumbers) {
      if (OutputIsStdout) {
        fmt::print("l{}: ", LineNumber);
      } else {
        Out->print("l{}: ", LineNumber);
      }
    }

    //
    // Write the symbolized address into the output trace.
    //

    if (OutputIsStdout) {
      fmt::print("{}\n", AddressSymbolized->get());
    } else {
      Out->print("{}\n", AddressSymbolized->get());
    }

    NumberSymbolizedLines++;
  }

  Stats.NumberSymbolizedLines += NumberSymbolizedLines;
  Stats.NumberFailedSymbolization += NumberFailedSymbolization;
  return true;
}

int main(int argc, char *argv[]) {

  //
  // Set up the argument parsing.
  //

  CLI::App Symbolizer(
      "Symbolizer - A fast execution trace symbolizer for Windows");

  Symbolizer.allow_windows_style_options();
  Symbolizer.set_help_all_flag("--help-all", "Expand all help");

  Symbolizer
      .add_option("-i,--input", Opts.Input, "Input trace file or directory")
      ->check(CLI::ExistingPath)
      ->required();
  Symbolizer
      .add_option("-c,--crash-dump", Opts.CrashdumpPath, "Crash-dump path")
      ->check(CLI::ExistingFile)
      ->required();
  Symbolizer.add_option("-o,--output", Opts.Output,
                        "Output trace (default: stdout)");
  Symbolizer.add_option("-s,--skip", Opts.Skip, "Skip a number of lines")
      ->default_val(0);
  Symbolizer.add_option("-m,--max", Opts.Max, "Stop after a number of lines")
      ->default_val(20'000'000);

  const std::unordered_map<std::string, TraceStyle_t> TraceStypeMap = {
      {"modoff", TraceStyle_t::Modoff}, {"fullsym", TraceStyle_t::FullSymbol}};

  Symbolizer.add_option("--style", Opts.Style, "Trace style")
      ->transform(CLI::CheckedTransformer(TraceStypeMap, CLI::ignore_case))
      ->default_val("fullsym");
  Symbolizer
      .add_flag("--overwrite", Opts.Overwrite,
                "Overwrite the output file if necessary")
      ->default_val(false);
  Symbolizer
      .add_flag("--line-numbers", Opts.LineNumbers, "Include line numbers")
      ->default_val(false);

  CLI11_PARSE(Symbolizer, argc, argv);

  //
  // Calculate a bunch of useful variables to take decisions later.
  //

  const bool InputIsDirectory = fs::is_directory(Opts.Input);
  const bool OutputIsDirectory = fs::is_directory(Opts.Output);
  const bool OutputDoesntExist = !fs::exists(Opts.Output);
  const bool OutputIsFile = fs::is_regular_file(Opts.Output);
  const bool OutputIsStdout = Opts.Output.empty();

  //
  // Initialize the debug engine APIs.
  //

  DbgEng_t DbgEng;
  if (!DbgEng.Init(Opts.CrashdumpPath)) {
    fmt::print("Failed to initialize the debugger api\n");
    return EXIT_FAILURE;
  }

  //
  // If the input flag is a folder, then we enumerate the files inside it.
  //

  std::vector<fs::path> Inputs;
  if (InputIsDirectory) {
    //
    // If the output is not a directory nor stdout, then it doesn't make any
    // sense, so bail.
    //

    if (!OutputIsDirectory && !OutputIsStdout) {
      fmt::print("When the input is a directory, the output can only be either "
                 "empty (for stdout) or a directory as well\n");
    }

    const fs::directory_iterator DirIt(Opts.Input);
    for (const auto &DirEntry : DirIt) {
      Inputs.emplace_back(DirEntry);
    }
  } else {
    Inputs.emplace_back(Opts.Input);
  }

  //
  // Symbolize each files.
  //

  fmt::print("Starting to process files..\n");
  const auto Before = chrono::high_resolution_clock::now();
  for (const auto &Input : Inputs) {

    //
    // If we run symbolizer from the same directory for both inputs and outputs,
    // we are going to see '.symbolizer' files into the input directory, so
    // let's just keep them instead of bailing.
    //

    if (Input.filename().string().ends_with(".symbolizer")) {
      fmt::print("Skipping %s..\n", Input.string().c_str());
      continue;
    }

    //
    // Calculate the output path.
    //

    fs::path Output;
    if (OutputIsDirectory) {

      //
      // If the output is a directory then generate an output file path.
      //

      Output =
          Opts.Output / fmt::format("{}.symbolizer", Input.filename().string());
    } else if (OutputDoesntExist || OutputIsFile) {

      //
      // There are two cases to consider here:
      //   - Either it is a path to a file that doesn't exist yet as we'll
      //   create it,
      //   - Or it points to an already existing file and the user might want to
      //   overwrite it.
      //

      Output = Opts.Output;
    } else {

      //
      // It is empty, data will be dumped on stdout.
      //
    }

    //
    // Verify that we are not about to overwrite an already generated trace
    // file. If the user specify specify --overwrite we will overwrite the files
    // that already exist.
    //

    if (!OutputIsStdout && fs::exists(Output)) {
      if (!Opts.Overwrite) {
        fmt::print("The output file {} already exists, continuing\n",
                   Output.string());
        continue;
      }

      fmt::print("The output file {} will be overwritten..\n", Output.string());
    }

    //
    // Process the file.
    //

    if (!SymbolizeFile(DbgEng, Input, Output)) {
      fmt::print("Parsing {} failed, exiting\n", Input.string());
      break;
    }

    Stats.NumberFiles++;
    fmt::print("[{} / {}] {} done\r", Stats.NumberFiles, Inputs.size(),
               Input.string());
  }

  fmt::print("\n");

  //
  // Yay we made it to the end! Let's dump a few stats out.
  //

  fmt::print("Completed symbolization of {} addresses ({} failed) in {} across "
             "{} files.\n",
             NumberToHuman(Stats.NumberSymbolizedLines),
             NumberToHuman(Stats.NumberFailedSymbolization),
             SecondsToHuman(SecondsSince(Before)),
             NumberToHuman(Stats.NumberFiles));

  return EXIT_SUCCESS;
}