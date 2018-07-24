#include "common/filesystem/filesystem_impl.h"

#if !defined(WIN32)
#include <dirent.h>
#else
#include <Windows.h>
#include <fcntl.h>
#include <filesystem>
#endif
#include <sys/stat.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include "envoy/common/exception.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/stats/stats.h"

#include "common/api/os_sys_calls_impl.h"
#include "common/common/assert.h"
#include "common/common/fmt.h"
#include "common/common/thread.h"

#include "absl/strings/match.h"

namespace Envoy {
namespace Filesystem {

bool fileExists(const std::string& path) {
  std::ifstream input_file(path);
  return input_file.is_open();
}

bool directoryExists(const std::string& path) {
 #if !defined(WIN32)
  DIR* const dir = opendir(path.c_str());
  const bool dir_exists = nullptr != dir;
  if (dir_exists) {
    closedir(dir);
  }

  return dir_exists;
#else
  WIN32_FIND_DATA findFileData;
  HANDLE handle = FindFirstFile(path.c_str(), &findFileData);
  auto dir_exists = (handle != INVALID_HANDLE_VALUE);
  if (dir_exists) {
    FindClose(handle);
  }
  return dir_exists;
#endif
}

ssize_t fileSize(const std::string& path) {
  struct stat info;
  if (stat(path.c_str(), &info) != 0) {
    return -1;
  }
  return info.st_size;
}

std::string fileReadToEnd(const std::string& path) {
  std::ios::sync_with_stdio(false);

  std::ifstream file(path);
  if (!file) {
    throw EnvoyException(fmt::format("unable to read file: {}", path));
  }

  std::stringstream file_string;
  file_string << file.rdbuf();

  return file_string.str();
}

std::string canonicalPath(const std::string& path) {
  // TODO(htuch): When we are using C++17, switch to std::filesystem::canonical.
#if defined(_WIN32)
	std::error_code code;
	auto resolved_path = std::experimental::filesystem::canonical(std::experimental::filesystem::path(path), code);
	if (code.value() != ERROR_SUCCESS)
	{
		throw EnvoyException(fmt::format("Unable to determine canonical path for {}", path));
	}
	std::string resolved_path_string{ resolved_path.string() };
#else
  char* resolved_path = ::realpath(path.c_str(), nullptr);
  if (resolved_path == nullptr) {
	  throw EnvoyException(fmt::format("Unable to determine canonical path for {}", path));
  }
  std::string resolved_path_string{ resolved_path };
  free(resolved_path);
#endif

  return resolved_path_string;
}

bool illegalPath(const std::string& path) {
  try {
    const std::string canonical_path = canonicalPath(path);
    // Platform specific path sanity; we provide a convenience to avoid Envoy
    // instances poking in bad places. We may have to consider conditioning on
    // platform in the future, growing these or relaxing some constraints (e.g.
    // there are valid reasons to go via /proc for file paths).
    // TODO(htuch): Optimize this as a hash lookup if we grow any further.
    if (absl::StartsWith(canonical_path, "/dev") || absl::StartsWith(canonical_path, "/sys") ||
        absl::StartsWith(canonical_path, "/proc")) {
      return true;
    }
    return false;
  } catch (const EnvoyException& ex) {
    ENVOY_LOG_MISC(debug, "Unable to determine canonical path for {}: {}", path, ex.what());
    return true;
  }
}

FileImpl::FileImpl(const std::string& path, Event::Dispatcher& dispatcher,
                   Thread::BasicLockable& lock, Stats::Store& stats_store,
                   std::chrono::milliseconds flush_interval_msec)
    : path_(path), file_lock_(lock), flush_timer_(dispatcher.createTimer([this]() -> void {
        stats_.flushed_by_timer_.inc();
        flush_event_.notify_one();
        flush_timer_->enableTimer(flush_interval_msec_);
      })),
      os_sys_calls_(Api::OsSysCallsSingleton::get()), flush_interval_msec_(flush_interval_msec),
      stats_{FILESYSTEM_STATS(POOL_COUNTER_PREFIX(stats_store, "filesystem."),
                              POOL_GAUGE_PREFIX(stats_store, "filesystem."))} {
  open();
}

void FileImpl::open() {
#if !defined(WIN32)
  fd_ = os_sys_calls_.open(path_.c_str(), O_RDWR | O_APPEND | O_CREAT,
                           S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
#else
  fd_ = os_sys_calls_.open(path_.c_str(), O_RDWR | O_APPEND | O_CREAT, _S_IREAD | _S_IWRITE);
#endif

  if (-1 == fd_) {
    throw EnvoyException(fmt::format("unable to open file '{}': {}", path_, strerror(errno)));
  }
}

void FileImpl::reopen() { reopen_file_ = true; }

FileImpl::~FileImpl() {
  {
    std::unique_lock<std::mutex> lock(write_lock_);
    flush_thread_exit_ = true;
    flush_event_.notify_one();
  }

  if (flush_thread_ != nullptr) {
    flush_thread_->join();
  }

  // Flush any remaining data. If file was not opened for some reason, skip flushing part.
  if (fd_ != -1) {
    if (flush_buffer_.length() > 0) {
      doWrite(flush_buffer_);
    }

    os_sys_calls_.close(fd_);
  }
}

void FileImpl::doWrite(Buffer::Instance& buffer) {
  uint64_t num_slices = buffer.getRawSlices(nullptr, 0);
#if !defined(_MSC_VER)
  Buffer::RawSlice slices[num_slices];
#else
  Buffer::RawSlice* slices =
    reinterpret_cast<Buffer::RawSlice*>(_alloca(sizeof(Buffer::RawSlice) * num_slices));
#endif
  buffer.getRawSlices(slices, num_slices);

  // We must do the actual writes to disk under lock, so that we don't intermix chunks from
  // different FileImpl pointing to the same underlying file. This can happen either via hot
  // restart or if calling code opens the same underlying file into a different FileImpl in the
  // same process.
  // TODO PERF: Currently, we use a single cross process lock to serialize all disk writes. This
  //            will never block network workers, but does mean that only a single flush thread can
  //            actually flush to disk. In the future it would be nice if we did away with the cross
  //            process lock or had multiple locks.
  {
    std::lock_guard<Thread::BasicLockable> lock(file_lock_);
    for (int i = 0; i < num_slices; i++) {
      Buffer::RawSlice& slice = slices[i];
      ssize_t rc = os_sys_calls_.write(fd_, slice.mem_, slice.len_);
      ASSERT(rc == static_cast<ssize_t>(slice.len_));
      stats_.write_completed_.inc();
    }
  }

  stats_.write_total_buffered_.sub(buffer.length());
  buffer.drain(buffer.length());
}

void FileImpl::flushThreadFunc() {

  while (true) {
    std::unique_lock<std::mutex> flush_lock;

    {
      std::unique_lock<std::mutex> write_lock(write_lock_);

      // flush_event_ can be woken up either by large enough flush_buffer or by timer.
      // In case it was timer, flush_buffer_ can be empty.
      while (flush_buffer_.length() == 0 && !flush_thread_exit_) {
        flush_event_.wait(write_lock);
      }

      if (flush_thread_exit_) {
        return;
      }

      flush_lock = std::unique_lock<std::mutex>(flush_lock_);
      ASSERT(flush_buffer_.length() > 0);
      about_to_write_buffer_.move(flush_buffer_);
      ASSERT(flush_buffer_.length() == 0);
    }

    // if we failed to open file before (-1 == fd_), then simply ignore
    if (fd_ != -1) {
      try {
        if (reopen_file_) {
          reopen_file_ = false;
          os_sys_calls_.close(fd_);
          open();
        }

        doWrite(about_to_write_buffer_);
      } catch (const EnvoyException&) {
        stats_.reopen_failed_.inc();
      }
    }
  }
}

void FileImpl::flush() {
  std::unique_lock<std::mutex> flush_buffer_lock;

  {
    std::lock_guard<std::mutex> write_lock(write_lock_);

    // flush_lock_ must be held while checking this or else it is
    // possible that flushThreadFunc() has already moved data from
    // flush_buffer_ to about_to_write_buffer_, has unlocked write_lock_,
    // but has not yet completed doWrite(). This would allow flush() to
    // return before the pending data has actually been written to disk.
    flush_buffer_lock = std::unique_lock<std::mutex>(flush_lock_);

    if (flush_buffer_.length() == 0) {
      return;
    }

    about_to_write_buffer_.move(flush_buffer_);
    ASSERT(flush_buffer_.length() == 0);
  }

  doWrite(about_to_write_buffer_);
}

void FileImpl::write(absl::string_view data) {
  std::lock_guard<std::mutex> lock(write_lock_);

  if (flush_thread_ == nullptr) {
    createFlushStructures();
  }

  stats_.write_buffered_.inc();
  stats_.write_total_buffered_.add(data.length());
  flush_buffer_.add(data.data(), data.size());
  if (flush_buffer_.length() > MIN_FLUSH_SIZE) {
    flush_event_.notify_one();
  }
}

void FileImpl::createFlushStructures() {
  flush_thread_.reset(new Thread::Thread([this]() -> void { flushThreadFunc(); }));
  flush_timer_->enableTimer(flush_interval_msec_);
}

} // namespace Filesystem
} // namespace Envoy
