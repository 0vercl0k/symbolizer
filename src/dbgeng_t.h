// Axel '0vercl0k' Souchet - September 12 2020
#pragma once
#include <cstdint>
#include <dbgeng.h>
#include <filesystem>
#include <fmt/printf.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <windows.h>

namespace fs = std::filesystem;

#pragma comment(lib, "dbgeng")

#if defined(__i386__) || defined(_M_IX86)
#define SYMBOLIZER_ARCH "x86"
#elif defined(__amd64__) || defined(_M_X64)
#define SYMBOLIZER_ARCH "x64"
#else
#error Platform not supported.
#endif

//
// The trace style supported.
//

enum class TraceStyle_t { Modoff, FullSymbol };

//
// The below class is the abstraction we use to interact with the DbgEng APIs.
//

class DbgEng_t {
  //
  // Highly inspired from:
  // C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\sdk\samples\dumpstk
  // The below is only used for debugging purposes; it allows to see the
  // messages outputed by the DbgEng APIs like you would see them in a WinDbg
  // output window.
  //

  class StdioOutputCallbacks_t final : public IDebugOutputCallbacks {
  public:
    // IUnknown
    STDMETHODIMP
    QueryInterface(REFIID InterfaceId, PVOID *Interface) noexcept override {
      *Interface = NULL;

      if (IsEqualIID(InterfaceId, __uuidof(IUnknown)) ||
          IsEqualIID(InterfaceId, __uuidof(IDebugOutputCallbacks))) {
        *Interface = (IDebugOutputCallbacks *)this;
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() noexcept override {
      // This class is designed to be static so
      // there's no true refcount.
      return 1;
    }

    STDMETHODIMP_(ULONG) Release() noexcept override {
      // This class is designed to be static so
      // there's no true refcount.
      return 0;
    }

    STDMETHODIMP Output(ULONG, PCSTR Text) noexcept override {
      fmt::print("{}", Text);
      return S_OK;
    }
  };

  //
  // This is the internal cache. Granted that resolving symbols is a pretty slow
  // process and the fact that traces usually contain a smaller number of
  // *unique* addresses executed, this gets us a really nice boost.
  //

  std::unordered_map<uint64_t, std::string> Cache_;

  //
  // The below are the various interfaces we need to do symbol resolution as
  // well as loading the crash-dump.
  //

  IDebugClient *Client_ = nullptr;
  IDebugControl *Control_ = nullptr;
  IDebugSymbols3 *Symbols_ = nullptr;

#ifdef SYMBOLIZER_DEBUG
  StdioOutputCallbacks_t StdioOutputCallbacks_;
#endif

public:
  DbgEng_t() = default;
  ~DbgEng_t() {
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

  //
  // Rule of three.
  //

  DbgEng_t(const DbgEng_t &) = delete;
  DbgEng_t &operator=(DbgEng_t &) = delete;

  //
  // Initialize the COM interfaces and load the crash-dump.
  //

  bool Init(const fs::path &DumpPath) {
    //
    // Ensure that we have dbghelp.dll / dbgcore.dll / dbgeng.dll /
    // symsrv.dll in the current directory otherwise things don't work. cf
    // https://docs.microsoft.com/en-us/windows/win32/debug/using-symsrv
    // "Installation"
    //

    char ExePathBuffer[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, &ExePathBuffer[0],
                            sizeof(ExePathBuffer))) {
      fmt::print("GetModuleFileNameA failed\n");
      return false;
    }

    //
    // Let's check if the dlls exist in the same path as the application.
    //

    const fs::path ExePath(ExePathBuffer);
    const fs::path ParentDir(ExePath.parent_path());
    const std::vector<std::string_view> Dlls = {"dbghelp.dll", "symsrv.dll",
                                                "dbgeng.dll", "dbgcore.dll"};
    const fs::path DefaultDbgDllLocation(
        R"(c:\program Files (x86)\windows kits\10\debuggers\)" SYMBOLIZER_ARCH);

    for (const auto &Dll : Dlls) {
      if (fs::exists(ParentDir / Dll)) {
        continue;
      }

      //
      // Apparently it doesn't. Be nice and try to find them by ourselves.
      //

      const fs::path DbgDllLocation(DefaultDbgDllLocation / Dll);
      if (!fs::exists(DbgDllLocation)) {

        //
        // If it doesn't exist we have to exit.
        //

        fmt::print("The debugger class expects debug dlls in the directory "
                   "where the application is running from.\n");
        return false;
      }

      //
      // Sounds like we are able to fix the problem ourselves. Copy the files
      // in the directory where the application is running from and move on!
      //

      fs::copy(DbgDllLocation, ParentDir);
      fmt::print("Copied {} into the "
                 "executable directory..\n",
                 DbgDllLocation.generic_string().c_str());
    }

    //
    // Initialize the various COM interfaces that we need.
    //

    fmt::print("Initializing the debugger instance..\n");
    HRESULT Status = DebugCreate(__uuidof(IDebugClient), (void **)&Client_);
    if (FAILED(Status)) {
      fmt::print("DebugCreate failed with hr={}\n", Status);
      return false;
    }

    Status =
        Client_->QueryInterface(__uuidof(IDebugControl), (void **)&Control_);
    if (FAILED(Status)) {
      fmt::print("QueryInterface/IDebugControl failed with hr={}\n", Status);
      return false;
    }

    Status =
        Client_->QueryInterface(__uuidof(IDebugSymbols3), (void **)&Symbols_);
    if (FAILED(Status)) {
      fmt::print("QueryInterface/IDebugSymbols failed with hr={}\n", Status);
      return false;
    }

    //
    // Turn the below on to debug issues related to dbghelp.
    //

#ifdef SYMBOLIZER_DEBUG
    const uint32_t SYMOPT_DEBUG = 0x80000000;
    Status = Symbols_->SetSymbolOptions(SYMOPT_DEBUG);
    if (FAILED(Status)) {
      fmt::print("IDebugSymbols::SetSymbolOptions failed with hr={}\n", Status);
      return false;
    }

    Client_->SetOutputCallbacks(&StdioOutputCallbacks_);
#endif

    //
    // We can now open the crash-dump using the dbghelp APIs.
    //

    fmt::print("Opening the dump file..\n");
    const std::string &DumpFileString = DumpPath.string();
    const char *DumpFileA = DumpFileString.c_str();
    Status = Client_->OpenDumpFile(DumpFileA);
    if (FAILED(Status)) {
      fmt::print("OpenDumpFile({}) failed with hr={}\n", DumpFileA, Status);
      return false;
    }

    //
    // Note that the engine doesn't completely attach to the dump file until the
    // WaitForEvent method has been called. When a dump file is created from a
    // process or kernel, information about the last event is stored in the
    // dump file. After the dump file is opened, the next time execution is
    // attempted, the engine will generate this event for the event callbacks.
    // Only then does the dump file become available in the debugging session.
    // https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/dbgeng/nf-dbgeng-idebugclient-opendumpfile
    //

    Status = WaitForEvent();
    if (FAILED(Status)) {
      fmt::print("WaitForEvent for OpenDumpFile failed with hr={}\n", Status);
      return false;
    }

    return true;
  }

  //
  // This returns the symbolized version of |SymbolAddress| according to a
  // |Style|.
  //

  std::optional<std::reference_wrapper<std::string>>
  Symbolize(const uint64_t SymbolAddress, const TraceStyle_t Style) {
    //
    // Fast path for the addresses we have symbolized already.
    //

    if (Cache_.contains(SymbolAddress)) {
      return Cache_.at(SymbolAddress);
    }

    //
    // Slow path, we need to ask dbgeng..
    //

    const auto &Res = Style == TraceStyle_t::Modoff
                          ? SymbolizeModoff(SymbolAddress)
                          : SymbolizeFull(SymbolAddress);

    //
    // If there has been an issue during symbolization, bail as it is not
    // expected.
    //

    if (!Res) {
      return {};
    }

    //
    // Feed the result into the cache.
    //

    Cache_.emplace(SymbolAddress, *Res);

    //
    // Return the entry directly from the cache.
    //

    return Cache_.at(SymbolAddress);
  }

private:
  //
  // This returns a module+offset symbolization of |SymbolAddress|.
  //

  std::optional<std::string> SymbolizeModoff(const uint64_t SymbolAddress) {
    constexpr size_t NameSizeMax = MAX_PATH;
    char Buffer[NameSizeMax] = {};

    //
    // module+offset style.
    //

    ULONG Index;
    ULONG64 Base;
    HRESULT Status =
        Symbols_->GetModuleByOffset(SymbolAddress, 0, &Index, &Base);
    if (FAILED(Status)) {
      fmt::print("GetModuleByOffset failed with hr={}\n", Status);
      return {};
    }

    ULONG NameSize;
    Status = Symbols_->GetModuleNameString(DEBUG_MODNAME_MODULE, Index, Base,
                                           &Buffer[0], NameSizeMax, &NameSize);
    if (FAILED(Status)) {
      fmt::print("GetModuleNameString failed with hr={}\n", Status);
      return {};
    }

    const uint64_t Offset = SymbolAddress - Base;
    return fmt::format("{}+0x{:x}", Buffer, Offset);
  }

  //
  // Symbolizes |SymbolAddress| with module+offset style.
  //

  std::optional<std::string> SymbolizeFull(const uint64_t SymbolAddress) {
    //
    // Full symbol style!
    //

    constexpr size_t NameSizeMax = MAX_PATH;
    char Buffer[NameSizeMax] = {};

    uint64_t Displacement = 0;
    const HRESULT Status = Symbols_->GetNameByOffset(
        SymbolAddress, &Buffer[0], NameSizeMax, nullptr, &Displacement);
    if (FAILED(Status)) {
      fmt::print("GetNameByOffset failed with hr={}\n", Status);
      return {};
    }

    return fmt::format("{}+0x{:x}", Buffer, Displacement);
  }

  //
  // Waits for the dbghelp machinery to signal that they are done.
  //

  HRESULT WaitForEvent() const noexcept {
    const HRESULT Status = Control_->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
    if (FAILED(Status)) {
      fmt::print("Execute::WaitForEvent failed with {}\n", Status);
    }
    return Status;
  }
};
