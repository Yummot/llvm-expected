//===- lib/Support/ErrorHandling.cpp - Callbacks for errors ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an API used to indicate fatal error conditions.  Non-fatal
// errors (most of them) should be handled through LLVMContext.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ErrorHandling.h"
#include "llvm-c/ErrorHandling.h"
#include "llvm/Config/config.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/WindowsError.h"
#include <cassert>
#include <cstdlib>
#include <mutex>
#include <new>
#include <iostream>
#include <sstream>

#if defined(EXPECTED_HAVE_UNISTD_H)
# include <unistd.h>
#endif

#if defined(_MSC_VER)
# include <io.h>
# include <fcntl.h>
#endif

/// EXPECTED_EXTENSION - Support compilers where we have a keyword to suppress
/// pedantic diagnostics.
#ifdef __GNUC__
#define EXPECTED_EXTENSION __extension__
#else
#define EXPECTED_EXTENSION
#endif

#if EXPECTED_ENABLE_THREADS == 1
#define IF_THREADS(X) X
#else
#define IF_THREADS(X)
#endif

using namespace llvm;

static fatal_error_handler_t ErrorHandler = nullptr;
static void *ErrorHandlerUserData = nullptr;

static fatal_error_handler_t BadAllocErrorHandler = nullptr;
static void *BadAllocErrorHandlerUserData = nullptr;

// Mutexes to synchronize installing error handlers and calling error handlers.
// Do not use ManagedStatic, or that may allocate memory while attempting to
// report an OOM.
//
// This usage of std::mutex has to be conditionalized behind ifdefs because
// of this script:
//   compiler-rt/lib/sanitizer_common/symbolizer/scripts/build_symbolizer.sh
// That script attempts to statically link the LLVM symbolizer library with the
// STL and hide all of its symbols with 'opt -internalize'. To reduce size, it
// cuts out the threading portions of the hermetic copy of libc++ that it
// builds. We can remove these ifdefs if that script goes away.
IF_THREADS(static std::mutex ErrorHandlerMutex);
IF_THREADS(static std::mutex BadAllocErrorHandlerMutex);

void llvm::install_fatal_error_handler(fatal_error_handler_t handler,
                                       void *user_data) {
  IF_THREADS(std::lock_guard<std::mutex> Lock(ErrorHandlerMutex));
  assert(!ErrorHandler && "Error handler already registered!\n");
  ErrorHandler = handler;
  ErrorHandlerUserData = user_data;
}

void llvm::remove_fatal_error_handler() {
  IF_THREADS(std::lock_guard<std::mutex> Lock(ErrorHandlerMutex));
  ErrorHandler = nullptr;
  ErrorHandlerUserData = nullptr;
}

void llvm::report_fatal_error(const char *Reason, bool GenCrashDiag) {
  report_fatal_error(std::string(Reason), GenCrashDiag);
}

void llvm::report_fatal_error(std::string Reason, bool GenCrashDiag) {
  llvm::fatal_error_handler_t handler = nullptr;
  void* handlerData = nullptr;
  {
    // Only acquire the mutex while reading the handler, so as not to invoke a
    // user-supplied callback under a lock.
    IF_THREADS(std::lock_guard<std::mutex> Lock(ErrorHandlerMutex));
    handler = ErrorHandler;
    handlerData = ErrorHandlerUserData;
  }

  if (handler) {
    handler(handlerData, Reason, GenCrashDiag);
  } else {
    // Blast the result out to stderr.  We don't try hard to make sure this
    // succeeds (e.g. handling EINTR) and we can't use errs() here because
    // raw ostreams can call report_fatal_error.
    char Buffer[64];
    std::ostringstream OS(Buffer);
    OS << "LLVM ERROR: " << Reason << "\n";
    std::string MessageStr = OS.str();
    ssize_t written = ::write(2, MessageStr.c_str(), MessageStr.size());
    (void)written; // If something went wrong, we deliberately just give up.
  }

  // If we reached here, we are failing ungracefully. Run the interrupt handlers
  // to make sure any special cleanups get done, in particular that we remove
  // files registered with RemoveFileOnSignal.
  sys::RunInterruptHandlers();

  exit(1);
}

void llvm::install_bad_alloc_error_handler(fatal_error_handler_t handler,
                                           void *user_data) {
  IF_THREADS(std::lock_guard<std::mutex> Lock(BadAllocErrorHandlerMutex));
  assert(!ErrorHandler && "Bad alloc error handler already registered!\n");
  BadAllocErrorHandler = handler;
  BadAllocErrorHandlerUserData = user_data;
}

void llvm::remove_bad_alloc_error_handler() {
  IF_THREADS(std::lock_guard<std::mutex> Lock(BadAllocErrorHandlerMutex));
  BadAllocErrorHandler = nullptr;
  BadAllocErrorHandlerUserData = nullptr;
}

void llvm::report_bad_alloc_error(const char *Reason, bool GenCrashDiag) {
  fatal_error_handler_t Handler = nullptr;
  void *HandlerData = nullptr;
  {
    // Only acquire the mutex while reading the handler, so as not to invoke a
    // user-supplied callback under a lock.
    IF_THREADS(std::lock_guard<std::mutex> Lock(BadAllocErrorHandlerMutex));
    Handler = BadAllocErrorHandler;
    HandlerData = BadAllocErrorHandlerUserData;
  }

  if (Handler) {
    Handler(HandlerData, Reason, GenCrashDiag);
    expected_unreachable("bad alloc handler should not return");
  }

#ifdef EXPECTED_ENABLE_EXCEPTIONS
  // If exceptions are enabled, make OOM in malloc look like OOM in new.
  throw std::bad_alloc();
#else
  // Don't call the normal error handler. It may allocate memory. Directly write
  // an OOM to stderr and abort.
  char OOMMessage[] = "LLVM ERROR: out of memory\n";
  ssize_t written = ::write(2, OOMMessage, strlen(OOMMessage));
  (void)written;
  abort();
#endif
}

void llvm::expected_unreachable_internal(const char *msg, const char *file,
                                         unsigned line) {
  // This code intentionally doesn't call the ErrorHandler callback, because
  // llvm_unreachable is intended to be used to indicate "impossible"
  // situations, and not legitimate runtime errors.
  if (msg)
    std::cerr << msg << "\n";
  std::cerr << "UNREACHABLE executed";
  if (file)
    std::cerr << " at " << file << ":" << line;
  std::cerr << "!\n";
  abort();
#ifdef EXPECTED_BUILTIN_UNREACHABLE
  // Windows systems and possibly others don't declare abort() to be noreturn,
  // so use the unreachable builtin to avoid a Clang self-host warning.
  EXPECTED_BUILTIN_UNREACHABLE;
#endif
}

static void bindingsErrorHandler(void *user_data, const std::string& reason,
                                 bool gen_crash_diag) {
  LLVMFatalErrorHandler handler =
      EXPECTED_EXTENSION reinterpret_cast<LLVMFatalErrorHandler>(user_data);
  handler(reason.c_str());
}

void LLVMInstallFatalErrorHandler(LLVMFatalErrorHandler Handler) {
  install_fatal_error_handler(bindingsErrorHandler,
                              EXPECTED_EXTENSION reinterpret_cast<void *>(Handler));
}

void LLVMResetFatalErrorHandler() {
  remove_fatal_error_handler();
}

#ifdef _WIN32

#include <winerror.h>

// I'd rather not double the line count of the following.
#define MAP_ERR_TO_COND(x, y)                                                  \
  case x:                                                                      \
    return make_error_code(errc::y)

std::error_code llvm::mapWindowsError(unsigned EV) {
  switch (EV) {
    MAP_ERR_TO_COND(ERROR_ACCESS_DENIED, permission_denied);
    MAP_ERR_TO_COND(ERROR_ALREADY_EXISTS, file_exists);
    MAP_ERR_TO_COND(ERROR_BAD_UNIT, no_such_device);
    MAP_ERR_TO_COND(ERROR_BUFFER_OVERFLOW, filename_too_long);
    MAP_ERR_TO_COND(ERROR_BUSY, device_or_resource_busy);
    MAP_ERR_TO_COND(ERROR_BUSY_DRIVE, device_or_resource_busy);
    MAP_ERR_TO_COND(ERROR_CANNOT_MAKE, permission_denied);
    MAP_ERR_TO_COND(ERROR_CANTOPEN, io_error);
    MAP_ERR_TO_COND(ERROR_CANTREAD, io_error);
    MAP_ERR_TO_COND(ERROR_CANTWRITE, io_error);
    MAP_ERR_TO_COND(ERROR_CURRENT_DIRECTORY, permission_denied);
    MAP_ERR_TO_COND(ERROR_DEV_NOT_EXIST, no_such_device);
    MAP_ERR_TO_COND(ERROR_DEVICE_IN_USE, device_or_resource_busy);
    MAP_ERR_TO_COND(ERROR_DIR_NOT_EMPTY, directory_not_empty);
    MAP_ERR_TO_COND(ERROR_DIRECTORY, invalid_argument);
    MAP_ERR_TO_COND(ERROR_DISK_FULL, no_space_on_device);
    MAP_ERR_TO_COND(ERROR_FILE_EXISTS, file_exists);
    MAP_ERR_TO_COND(ERROR_FILE_NOT_FOUND, no_such_file_or_directory);
    MAP_ERR_TO_COND(ERROR_HANDLE_DISK_FULL, no_space_on_device);
    MAP_ERR_TO_COND(ERROR_INVALID_ACCESS, permission_denied);
    MAP_ERR_TO_COND(ERROR_INVALID_DRIVE, no_such_device);
    MAP_ERR_TO_COND(ERROR_INVALID_FUNCTION, function_not_supported);
    MAP_ERR_TO_COND(ERROR_INVALID_HANDLE, invalid_argument);
    MAP_ERR_TO_COND(ERROR_INVALID_NAME, invalid_argument);
    MAP_ERR_TO_COND(ERROR_LOCK_VIOLATION, no_lock_available);
    MAP_ERR_TO_COND(ERROR_LOCKED, no_lock_available);
    MAP_ERR_TO_COND(ERROR_NEGATIVE_SEEK, invalid_argument);
    MAP_ERR_TO_COND(ERROR_NOACCESS, permission_denied);
    MAP_ERR_TO_COND(ERROR_NOT_ENOUGH_MEMORY, not_enough_memory);
    MAP_ERR_TO_COND(ERROR_NOT_READY, resource_unavailable_try_again);
    MAP_ERR_TO_COND(ERROR_OPEN_FAILED, io_error);
    MAP_ERR_TO_COND(ERROR_OPEN_FILES, device_or_resource_busy);
    MAP_ERR_TO_COND(ERROR_OUTOFMEMORY, not_enough_memory);
    MAP_ERR_TO_COND(ERROR_PATH_NOT_FOUND, no_such_file_or_directory);
    MAP_ERR_TO_COND(ERROR_BAD_NETPATH, no_such_file_or_directory);
    MAP_ERR_TO_COND(ERROR_READ_FAULT, io_error);
    MAP_ERR_TO_COND(ERROR_RETRY, resource_unavailable_try_again);
    MAP_ERR_TO_COND(ERROR_SEEK, io_error);
    MAP_ERR_TO_COND(ERROR_SHARING_VIOLATION, permission_denied);
    MAP_ERR_TO_COND(ERROR_TOO_MANY_OPEN_FILES, too_many_files_open);
    MAP_ERR_TO_COND(ERROR_WRITE_FAULT, io_error);
    MAP_ERR_TO_COND(ERROR_WRITE_PROTECT, permission_denied);
    MAP_ERR_TO_COND(WSAEACCES, permission_denied);
    MAP_ERR_TO_COND(WSAEBADF, bad_file_descriptor);
    MAP_ERR_TO_COND(WSAEFAULT, bad_address);
    MAP_ERR_TO_COND(WSAEINTR, interrupted);
    MAP_ERR_TO_COND(WSAEINVAL, invalid_argument);
    MAP_ERR_TO_COND(WSAEMFILE, too_many_files_open);
    MAP_ERR_TO_COND(WSAENAMETOOLONG, filename_too_long);
  default:
    return std::error_code(EV, std::system_category());
  }
}

#endif
