// Axel '0vercl0k' Souchet - September 11 2020
//#define SYMBOLIZER_DEBUG
#include <CLI/CLI.hpp>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <dbgeng.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <windows.h>

#pragma comment(lib, "dbgeng")

namespace fs = std::filesystem;
namespace chrono = std::chrono;

class Debugger {
  //
  // Highly inspired from:
  // C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\sdk\samples\dumpstk
  //

  class StdioOutputCallbacks_t : public IDebugOutputCallbacks {
  public:
    // IUnknown
    STDMETHODIMP
    QueryInterface(REFIID InterfaceId, PVOID *Interface) {
      *Interface = NULL;

      if (IsEqualIID(InterfaceId, __uuidof(IUnknown)) ||
          IsEqualIID(InterfaceId, __uuidof(IDebugOutputCallbacks))) {
        *Interface = (IDebugOutputCallbacks *)this;
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() {
      // This class is designed to be static so
      // there's no true refcount.
      return 1;
    }

    STDMETHODIMP_(ULONG) Release() {
      // This class is designed to be static so
      // there's no true refcount.
      return 0;
    }

    STDMETHODIMP Output(ULONG Mask, PCSTR Text) {
      printf("%s", Text);
      return S_OK;
    }
  };

  std::unordered_map<uint64_t, std::string> Cache_;
  IDebugClient *Client_ = nullptr;
  IDebugControl *Control_ = nullptr;
  IDebugSymbols3 *Symbols_ = nullptr;

#ifdef SYMBOLIZER_DEBUG
  StdioOutputCallbacks_t StdioOutputCallbacks_;
#endif

public:
  explicit Debugger() = default;

  ~Debugger() {
    if (Client_) {
      Client_->EndSession(DEBUG_END_ACTIVE_DETACH);
      Client_->Release();
    }

    if (Control_) {
      Control_->Release();
    }

    if (Symbols_) {
      Symbols_->Release();
    }
  }

  bool Init(const fs::path &DumpPath) {
    //
    // Ensure that we both have dbghelp.dll and symsrv.dll in the current
    // directory otherwise things don't work. cf
    // https://docs.microsoft.com/en-us/windows/win32/debug/using-symsrv
    // "Installation"
    //

    char ExePathBuffer[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, ExePathBuffer, sizeof(ExePathBuffer))) {
      printf("GetModuleFileNameA failed.\n");
      return false;
    }

    const fs::path ExePath(ExePathBuffer);
    const fs::path ParentDir(ExePath.parent_path());
    if (!fs::exists(ParentDir / "dbghelp.dll") ||
        !fs::exists(ParentDir / "symsrv.dll")) {
      const fs::path DefaultDbghelpLocation(
          R"(c:\windows\system32\dbghelp.dll)");
      const fs::path DefaultSymsrvLocation(
          R"(c:\program Files (x86)\microsoft visual studio\2019\Community\Common7\IDE\symsrv.dll)");
      const bool Dbghelp = fs::exists(DefaultDbghelpLocation);
      const bool Symsrv = fs::exists(DefaultSymsrvLocation);
      if (!Dbghelp || !Symsrv) {
        printf(
            "The debugger class expects dbghelp.dll / symsrv.dll in directory "
            "where the application is.\n");
        return false;
      }

      fs::copy(DefaultDbghelpLocation, ParentDir);
      fs::copy(DefaultSymsrvLocation, ParentDir);
      printf("Copied dbghelp and symsrv.dll from default location into the "
             "executable directory..\n");
    }

    printf("Initializing the debugger instance..\n");

    HRESULT Status = DebugCreate(__uuidof(IDebugClient), (void **)&Client_);
    if (FAILED(Status)) {
      printf("DebugCreate failed with hr=%lx\n", Status);
      return false;
    }

    Status =
        Client_->QueryInterface(__uuidof(IDebugControl), (void **)&Control_);
    if (FAILED(Status)) {
      printf("QueryInterface/IDebugControl failed with hr=%lx\n", Status);
      return false;
    }

    Status =
        Client_->QueryInterface(__uuidof(IDebugSymbols3), (void **)&Symbols_);
    if (FAILED(Status)) {
      printf("QueryInterface/IDebugSymbols failed with hr=%lx\n", Status);
      return false;
    }

    //
    // Turn the below on to debug issues.
    //

#ifdef SYMBOLIZER_DEBUG
    const uint32_t SYMOPT_DEBUG = 0x80000000;
    Status = Symbols_->SetSymbolOptions(SYMOPT_DEBUG);
    if (FAILED(Status)) {
      printf("IDebugSymbols::SetSymbolOptions failed with hr=%x\n", Status);
      return false;
    }

    Client_->SetOutputCallbacks(&StdioOutputCallbacks_);
#endif

    printf("Opening the dump file..\n");
    const std::string &DumpFileString = DumpPath.string();
    const char *DumpFileA = DumpFileString.c_str();
    Status = Client_->OpenDumpFile(DumpFileA);
    if (FAILED(Status)) {
      printf("OpenDumpFile(h%s) failed with hr=%lx\n", DumpFileA, Status);
      return false;
    }

    //
    // Note The engine doesn't completely attach to the dump file until the
    // WaitForEvent method has been called. When a dump file is created from a
    // process or kernel, information about the last event is stored in the
    // dump file. After the dump file is opened, the next time execution is
    // attempted, the engine will generate this event for the event callbacks.
    // Only then does the dump file become available in the debugging session.
    // https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/dbgeng/nf-dbgeng-idebugclient-opendumpfile
    //

    Status = WaitForEvent();
    if (FAILED(Status)) {
      printf("WaitForEvent for OpenDumpFile failed with hr=%lx\n", Status);
      return false;
    }

    return true;
  }

  HRESULT WaitForEvent() const {
    HRESULT Status = Control_->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
    if (FAILED(Status)) {
      printf("Execute::WaitForEvent failed with %lx\n", Status);
    }
    return Status;
  }

  std::optional<std::string> Symbolize(const uint64_t SymbolAddress) {
    //
    // Fast path for the addresses we have symbolized already.
    //

    if (Cache_.contains(SymbolAddress)) {
      return Cache_.at(SymbolAddress);
    }

    //
    // Slow path, we need to ask dbgeng..
    //

    const size_t NameSizeMax = MAX_PATH;
    char Buffer[NameSizeMax];

    uint64_t Displacement = 0;
    const HRESULT Status = Symbols_->GetNameByOffset(
        SymbolAddress, Buffer, NameSizeMax, nullptr, &Displacement);
    if (FAILED(Status)) {
      return std::nullopt;
    }

    //
    // Format the buffer.
    //

    std::snprintf(Buffer, NameSizeMax, "%s+0x%llx", Buffer, Displacement);

    //
    // Feed it into the cache.
    //

    Cache_.emplace(SymbolAddress, Buffer);
    return Cache_.at(SymbolAddress);
  }
};

struct Opts_t {
  fs::path Input;
  fs::path CrashdumpPath;
  fs::path Output;
  uint64_t Skip = 0;
  uint64_t Max = 1'000'000;
  bool Overwrite = false;
};

int main(int argc, char *argv[]) {
  //
  // Set up the argument parsing.
  //

  Opts_t Opts;
  CLI::App Symbolizer("symbolizer");

  Symbolizer.allow_windows_style_options();
  Symbolizer.set_help_all_flag("--help-all", "Expand all help");

  Symbolizer.add_option("-i,--input", Opts.Input, "Input trace path")
      ->required();
  Symbolizer
      .add_option("-c,--crash-dump", Opts.CrashdumpPath, "Crash-dump path")
      ->required();
  Symbolizer.add_option("-o,--output", Opts.Output,
                        "Output trace (default: stdout)");

  Symbolizer.add_option("-s,--skip", Opts.Skip, "Skip a number of lines");
  Symbolizer.add_option("-m,--max", Opts.Max, "Stop after a number of lines");
  Symbolizer.add_flag("--overwrite", Opts.Overwrite,
                      "Overwrite the output file if necessary");

  CLI11_PARSE(Symbolizer, argc, argv);

  //
  // Verify that we are not about to overwrite an already generated trace file.
  //

  const bool StdoutOutput = Opts.Output.empty();
  if (!StdoutOutput) {
    if (fs::exists(Opts.Output)) {
      if (!Opts.Overwrite) {
        printf("The output file %s already exists, exiting.\n",
               Opts.Output.string().c_str());
        return EXIT_SUCCESS;
      }

      printf("The output file %s will be overwritten..\n",
             Opts.Output.string().c_str());
    }
  }

  printf("Input trace file: %s\n", Opts.Input.string().c_str());
  printf("Crash-dump file: %s\n", Opts.CrashdumpPath.string().c_str());
  printf("Output trace file: %s\n",
         StdoutOutput ? "stdout" : Opts.Output.string().c_str());

  //
  // Initialize the debug engine APIs.
  //

  Debugger Dbg;
  if (!Dbg.Init(Opts.CrashdumpPath)) {
    printf("Failed to initialize the debugger api.\n");
    return EXIT_FAILURE;
  }

  //
  // Open the input trace file.
  //

  std::ifstream TraceFile(Opts.Input);
  if (!TraceFile.good()) {
    printf("Could not open %s\n", Opts.Input.string().c_str());
    return EXIT_FAILURE;
  }

  //
  // Open the output trace file.
  //

  std::ofstream OutFile(Opts.Output);
  std::basic_ostream<char> &OutStream = StdoutOutput ? std::cout : OutFile;

  //
  // Read the trace file line by line.
  //

  printf("Starting to read the input file..\n");
  auto Before = chrono::high_resolution_clock::now();
  uint64_t NumberSymbolizedLines = 0;
  uint64_t NumberFailedSymbolization = 0;
  std::string Line;
  for (uint64_t Idx = 0; std::getline(TraceFile, Line); Idx++) {

    //
    // Have we hit max yet?
    //

    if (Idx >= Opts.Max) {
      printf("Hit the maximum number %" PRId64 ", breaking out\n", Opts.Max);
      break;
    }

    //
    // Skipping a number of line.
    //

    if (Idx < Opts.Skip) {
      continue;
    }

    //
    // Convert the line into an address.
    //

    const uint64_t Address = std::strtoull(Line.c_str(), nullptr, 0);

    //
    // Symbolize the address.
    //

    auto AddressSymbolized = Dbg.Symbolize(Address);
    if (!AddressSymbolized.has_value()) {
      printf("Symbolization of %" PRIx64 " failed, skipping\n", Address);
      OutStream << "FAILED SYMBOLIZATION" << std::endl;
      NumberFailedSymbolization++;
      continue;
    }

    //
    // Write the symbolized address into the output trace.
    //

    OutStream << AddressSymbolized->c_str() << std::endl;
    NumberSymbolizedLines++;
  }

  //
  // Calculate the duration.
  //

  auto After = chrono::high_resolution_clock::now();
  auto Duration = chrono::duration_cast<chrono::seconds>(After - Before);

  const char *Unit = "";
  if (NumberSymbolizedLines >= 1'000'000) {
    NumberSymbolizedLines /= 1'000'000;
    Unit = "m";
  } else if (NumberSymbolizedLines >= 1'000) {
    NumberSymbolizedLines /= 1'000;
    Unit = "k";
  }

  printf("Successfully symbolized %" PRId64 "%s addresses (%" PRId64
         " failed) in %" PRId64 "s\n",
         NumberSymbolizedLines, Unit, NumberFailedSymbolization,
         Duration.count());

  return EXIT_SUCCESS;
}