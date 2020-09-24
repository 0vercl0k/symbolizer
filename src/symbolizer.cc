// Axel '0vercl0k' Souchet - September 11 2020
//#define SYMBOLIZER_DEBUG
#define _CRT_SECURE_NO_WARNINGS

#include "dbgeng_t.h"
#include <CLI/CLI.hpp>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;
namespace chrono = std::chrono;

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

  std::ifstream TraceFile(Input);
  if (!TraceFile.good()) {
    printf("Could not open input %s\n", Input.string().c_str());
    return false;
  }

  //
  // Open the output trace file.
  //

  const bool OutputIsStdout = Output.empty();
  std::ofstream OutFile;

  //
  // If we are not dumping data on stdout, then let's actually open an output
  // file.
  //

  if (!OutputIsStdout) {
    OutFile.open(Output);
    if (!OutFile.good()) {
      printf("Could not open output file %s\n", Output.string().c_str());
      return false;
    }
  }

  //
  // We use this stream to be able to manipulate std::cout and the file the same
  // way.
  //

  std::basic_ostream<char> &OutStream = OutputIsStdout ? std::cout : OutFile;

  //
  // Read the trace file line by line.
  //

  uint64_t NumberSymbolizedLines = 0;
  uint64_t NumberFailedSymbolization = 0;
  std::string Line;
  for (uint64_t LineNumber = 0; std::getline(TraceFile, Line); LineNumber++) {

    //
    // Have we hit max yet?
    //

    if (NumberSymbolizedLines >= Opts.Max) {
      printf("Hit the maximum number %" PRIu64 ", breaking out\n", Opts.Max);
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

    const uint64_t Address = std::strtoull(Line.c_str(), nullptr, 0);

    //
    // Symbolize the address.
    //

    auto AddressSymbolized = Dbg.Symbolize(Address, Opts.Style);
    if (!AddressSymbolized.has_value()) {
      printf("%s:%" PRIu64 ": Symbolization of %" PRIx64
             " failed ('%s'), skipping\n",
             Input.filename().string().c_str(), LineNumber, Address,
             Line.c_str());
      NumberFailedSymbolization++;
      continue;
    }

    //
    // Include the line numbers.
    //

    if (Opts.LineNumbers) {
      OutStream << 'l' << LineNumber << ": ";
    }

    //
    // Write the symbolized address into the output trace.
    //

    OutStream << AddressSymbolized->c_str() << '\n';
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
    printf("Failed to initialize the debugger api.\n");
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
      printf("When the input is a directory, the output can only be either "
             "empty (for stdout) or a directory as well.\n");
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

  printf("Starting to process files..\n");
  const auto Before = chrono::high_resolution_clock::now();
  for (const auto &Input : Inputs) {

    //
    // If we run Symbolizer from the same directory for both inputs and outputs,
    // we are going to see '.symbolizer' files into the input directory, so
    // let's just keep them instead of bailing.
    //

    if (Input.filename().string().ends_with(".symbolizer")) {
      printf("Skipping %s..\n", Input.string().c_str());
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

      const std::string InputFilename(Input.filename().string());
      const std::string OutputFilename(InputFilename + ".symbolizer");
      Output = Opts.Output / OutputFilename;
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
        printf("The output file %s already exists, exiting.\n",
               Output.string().c_str());
        return EXIT_SUCCESS;
      }

      printf("The output file %s will be overwritten..\n",
             Output.string().c_str());
    }

    //
    // Process the file.
    //

    if (!SymbolizeFile(DbgEng, Input, Output)) {
      printf("Parsing %s failed, exiting\n", Input.string().c_str());
      break;
    }

    Stats.NumberFiles++;
    printf("[%" PRIu64 " / %zd] %s done\r", Stats.NumberFiles, Inputs.size(),
           Input.string().c_str());
  }

  printf("\n");

  //
  // Calculate the duration.
  //

  const auto After = chrono::high_resolution_clock::now();
  auto Duration = chrono::duration_cast<chrono::seconds>(After - Before);

  const char *Unit = "";
  if (Stats.NumberSymbolizedLines >= 1'000'000) {
    Stats.NumberSymbolizedLines /= 1'000'000;
    Unit = "m";
  } else if (Stats.NumberSymbolizedLines >= 1'000) {
    Stats.NumberSymbolizedLines /= 1'000;
    Unit = "k";
  }

  //
  // Yay we made it to the end! Let's dump a few stats out.
  //

  printf("Completed symbolization of %" PRIu64 "%s addresses (%" PRIu64
         " failed) in %" PRId64 "s across %" PRIu64 " files.\n",
         Stats.NumberSymbolizedLines, Unit, Stats.NumberFailedSymbolization,
         Duration.count(), Stats.NumberFiles);

  return EXIT_SUCCESS;
}