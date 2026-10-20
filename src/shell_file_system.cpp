#ifndef _WIN32
// Included before the DuckDB headers, which redefine __restrict (breaks <spawn.h> on macOS).
#include <spawn.h>
#endif

#include "shell_file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <cctype>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>

#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

#include <cstring>
#include <cstdlib>

#else
#include "duckdb/common/windows_util.hpp"

#include <io.h>
#include <string>
#include <stdio.h>

#endif

namespace duckdb
{

	struct ParsedInputCommand
	{
		std::string command;
		std::unordered_set<int> allowed_exit_codes;
	};

	// Trim utility
	static inline std::string trim(const std::string &s)
	{
		auto start = s.find_first_not_of(" \t\n\r");
		auto end = s.find_last_not_of(" \t\n\r");
		if (start == std::string::npos)
			return "";
		return s.substr(start, end - start + 1);
	}

	static inline bool isNonNegativeInteger(const std::string &s)
	{
		if (s.empty())
			return false;
		for (char c : s)
		{
			if (!std::isdigit(static_cast<unsigned char>(c)))
				return false;
		}
		return true;
	}

	ParsedInputCommand parseInputCommand(const std::string &input)
	{
		ParsedInputCommand result;

		if (input.empty() || input.back() != '|')
		{
			throw InvalidInputException("Command must end with '|'.");
		}

		std::string marker = "{allowed_exit_codes=";
		auto lastPipe = input.size() - 1; // index of final '|'

		// Look for the last '{allowed_exit_codes=...}' that ends right before the final '|'
		auto closeBrace = input.rfind('}', lastPipe);
		if (closeBrace != std::string::npos && closeBrace + 1 == lastPipe)
		{
			auto openBrace = input.rfind(marker, closeBrace);
			if (openBrace != std::string::npos)
			{
				// Extract the command (everything before the spec)
				result.command = trim(input.substr(0, openBrace));

				// Extract codes inside {...}
				std::string codes_str = input.substr(openBrace + marker.size(),
																						 closeBrace - (openBrace + marker.size()));
				std::stringstream ss(codes_str);
				std::string token;

				while (std::getline(ss, token, ','))
				{
					token = trim(token);
					if (token.empty())
						continue;

					if (!isNonNegativeInteger(token))
					{
						throw InvalidInputException("Invalid exit code: '%s'. Must be a non-negative integer.", token);
					}

					int value;
					try
					{
						value = std::stoi(token);
					}
					catch (const std::out_of_range &)
					{
						throw InvalidInputException("Exit code out of range: '%s'. Value too large.", token);
					}
					result.allowed_exit_codes.insert(value); // deduplicated automatically
				}

				if (result.allowed_exit_codes.empty())
				{
					throw InvalidInputException("No valid exit codes parsed.");
				}

				return result;
			}
		}

		// No final allowed_exit_codes spec → default to 0
		result.command = trim(input.substr(0, lastPipe));
		result.allowed_exit_codes.insert(0);
		return result;
	}

#ifndef _WIN32
	// Blocks SIGPIPE for the calling thread while a write to a pipe is in progress, so that writing to a
	// command that has exited raises EPIPE instead of terminating the whole process. A SIGPIPE raised by
	// the write is consumed before the previous signal mask is restored.
	class SigpipeGuard
	{
	public:
		SigpipeGuard()
		{
			sigset_t pending;
			sigemptyset(&sigpipe_set);
			sigaddset(&sigpipe_set, SIGPIPE);
			sigemptyset(&pending);
			sigpending(&pending);
			was_pending = sigismember(&pending, SIGPIPE) == 1;
			blocked = pthread_sigmask(SIG_BLOCK, &sigpipe_set, &old_mask) == 0;
		}

		~SigpipeGuard()
		{
			if (!blocked)
			{
				return;
			}
			if (!was_pending)
			{
				sigset_t pending;
				sigemptyset(&pending);
				if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
				{
					int signal_number;
					sigwait(&sigpipe_set, &signal_number);
				}
			}
			pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
		}

	private:
		sigset_t sigpipe_set;
		sigset_t old_mask;
		bool was_pending = false;
		bool blocked = false;
	};

	// Starts "/bin/sh -c command" with one end of a pipe connected to its stdin (writing) or stdout
	// (reading). Unlike popen(), the child always starts with the default SIGPIPE disposition and an
	// empty signal mask, even when the host process (e.g. the DuckDB CLI or Python) ignores SIGPIPE,
	// so a command whose output is no longer read terminates by SIGPIPE as it would in a shell.
	static int SpawnShellCommand(const string &command, bool for_writing, pid_t &child_pid)
	{
		int fds[2];
		if (pipe(fds) != 0)
		{
			return -1;
		}
		// The parent's end of the pipe must not leak into this or any other child process.
		const int parent_fd = for_writing ? fds[1] : fds[0];
		const int child_fd = for_writing ? fds[0] : fds[1];
		const int child_target = for_writing ? STDIN_FILENO : STDOUT_FILENO;
		fcntl(fds[0], F_SETFD, FD_CLOEXEC);
		fcntl(fds[1], F_SETFD, FD_CLOEXEC);

		posix_spawn_file_actions_t actions;
		posix_spawnattr_t attr;
		posix_spawn_file_actions_init(&actions);
		posix_spawnattr_init(&attr);

		if (child_fd == child_target)
		{
			// dup2() onto itself would not clear FD_CLOEXEC.
			fcntl(child_fd, F_SETFD, 0);
		}
		else
		{
			posix_spawn_file_actions_adddup2(&actions, child_fd, child_target);
		}

		sigset_t default_signals;
		sigemptyset(&default_signals);
		sigaddset(&default_signals, SIGPIPE);
		posix_spawnattr_setsigdefault(&attr, &default_signals);
		sigset_t empty_mask;
		sigemptyset(&empty_mask);
		posix_spawnattr_setsigmask(&attr, &empty_mask);
		posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

		const char *argv[] = {"sh", "-c", command.c_str(), nullptr};
		int spawn_result =
				posix_spawn(&child_pid, "/bin/sh", &actions, &attr, const_cast<char *const *>(argv), environ);

		posix_spawn_file_actions_destroy(&actions);
		posix_spawnattr_destroy(&attr);
		close(child_fd);

		if (spawn_result != 0)
		{
			close(parent_fd);
			errno = spawn_result;
			return -1;
		}
		return parent_fd;
	}
#endif

	struct ShellFileHandle : public FileHandle
	{
		friend class ShellFileSystem;

	public:
#ifndef _WIN32
		ShellFileHandle(FileSystem &file_system, string path, int fd, pid_t child_pid, FileOpenFlags flags,
										std::unordered_set<int> allowed_exit_codes, bool ignore_sigpipe)
				: FileHandle(file_system, std::move(path), std::move(flags)), fd(fd), child_pid(child_pid),
					allowed_exit_codes(std::move(allowed_exit_codes)), ignore_sigpipe(ignore_sigpipe)
		{
		}
#else
		ShellFileHandle(FileSystem &file_system, string path, FILE *pipe, FileOpenFlags flags,
										std::unordered_set<int> allowed_exit_codes, bool ignore_sigpipe)
				: FileHandle(file_system, std::move(path), std::move(flags)), pipe(pipe),
					allowed_exit_codes(std::move(allowed_exit_codes)), ignore_sigpipe(ignore_sigpipe)
		{
		}
#endif

		~ShellFileHandle() override
		{
			try
			{
				ShellFileHandle::Close();
			}
			catch (const std::exception &e)
			{
				printf("Exception in ShellFileHandle destructor: %s\n", e.what());
			}
		}

	private:
#ifndef _WIN32
		int fd;
		pid_t child_pid;
		//! Set once a write found that the command no longer reads its input (only with ignore_sigpipe).
		bool output_closed = false;
#else
		FILE *pipe;
#endif
		std::unordered_set<int> allowed_exit_codes;
		//! Value of the ignore_sigpipe setting when the pipe was opened.
		bool ignore_sigpipe;

		bool IsOpen() const
		{
#ifndef _WIN32
			return fd != -1;
#else
			return pipe != NULL;
#endif
		}

	public:
		void Close() override
		{
			if (!IsOpen())
			{
				return;
			}

#ifndef _WIN32
			close(fd);
			// Indicate that the pipe has been closed.
			fd = -1;

			int result;
			pid_t wait_result;
			do
			{
				wait_result = waitpid(child_pid, &result, 0);
			} while (wait_result == -1 && errno == EINTR);

			if (wait_result == -1)
			{
				throw IOException({{"errno", std::to_string(errno)}}, "Could not close pipe \"%s\": %s", path,
													strerror(errno));
			}
			if (WIFEXITED(result))
			{
				int exit_status = WEXITSTATUS(result);
				if (allowed_exit_codes.find(exit_status) == allowed_exit_codes.end())
				{
					throw IOException("Pipe process exited abnormally code=%d: %s", exit_status, path);
				}
			}
			else if (WIFSIGNALED(result))
			{
				int signal_number = WTERMSIG(result);
				// With ignore_sigpipe a command that stopped because DuckDB closed the pipe before reading
				// all of its output (e.g. a LIMIT) is not an error.
				if (!(ignore_sigpipe && signal_number == SIGPIPE))
				{
					throw IOException("Pipe process exited with signal signal=%d: %s", signal_number, path);
				}
			}
#else
			int result = _pclose(pipe);
			// Indicate that the pipe has been closed.
			pipe = NULL;

			if (result == -1)
			{
				throw IOException({{"errno", std::to_string(errno)}}, "Could not close pipe \"%s\": %s", path,
													strerror(errno));
			}
			// On Windows, _pclose() returns the exit status directly
			if (allowed_exit_codes.find(result) == allowed_exit_codes.end())
			{
				throw IOException("Pipe process exited abnormally code=%d: %s", result, path);
			}
#endif
		};
	};

	void ShellFileSystem::Reset(FileHandle &handle)
	{
		throw InternalException("Cannot reset shell file system");
	}

	int64_t ShellFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes)
	{
		auto &shell_handle = handle.Cast<ShellFileHandle>();
		if (!shell_handle.IsOpen())
		{
			return 0;
		}

#ifndef _WIN32
		int64_t bytes_read = 0;
		// Fill the buffer like fread() did: DuckDB treats a short read as the end of the stream.
		while (bytes_read < nr_bytes)
		{
			auto result = read(shell_handle.fd, data_ptr_cast(buffer) + bytes_read, nr_bytes - bytes_read);
			if (result == -1)
			{
				if (errno == EINTR)
				{
					continue;
				}
				throw IOException({{"errno", std::to_string(errno)}}, "Could not read from pipe \"%s\": %s",
													handle.path, strerror(errno));
			}
			if (result == 0)
			{
				break;
			}
			bytes_read += result;
		}
#else
		int64_t bytes_read = fread(buffer, 1, nr_bytes, shell_handle.pipe);
		if (bytes_read < nr_bytes && ferror(shell_handle.pipe))
		{
			throw IOException({{"errno", std::to_string(errno)}}, "Could not read from pipe \"%s\": %s", handle.path,
												strerror(errno));
		}
#endif
		if (bytes_read == 0)
		{
			// Since the last read() returned 0 bytes, presume that EOF has been encountered, and rather than
			// having the close, by doing this if there are errors with the pipe they are caught in the query
			// rather than in the destructor.
			handle.Close();
		}
		return bytes_read;
	}

	int64_t ShellFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes)
	{
		auto &shell_handle = handle.Cast<ShellFileHandle>();
		const int64_t requested_bytes = nr_bytes;

#ifndef _WIN32
		if (shell_handle.output_closed)
		{
			// The command stopped reading (ignore_sigpipe): discard the rest of the output.
			return requested_bytes;
		}

		SigpipeGuard sigpipe_guard;
		while (nr_bytes > 0)
		{
			auto bytes_to_write = MinValue<idx_t>(idx_t(NumericLimits<int32_t>::Maximum()), idx_t(nr_bytes));
			auto current_bytes_written = write(shell_handle.fd, buffer, bytes_to_write);
			if (current_bytes_written == -1 && errno == EINTR)
			{
				continue;
			}
			if (current_bytes_written == -1 && errno == EPIPE && shell_handle.ignore_sigpipe)
			{
				// The command exited (or closed its input) before reading everything, e.g. "| head -1".
				// Treat the write as successful; the command's exit status is still checked on close.
				shell_handle.output_closed = true;
				return requested_bytes;
			}
			if (current_bytes_written <= 0)
			{
				throw IOException({{"errno", std::to_string(errno)}}, "Could not write to pipe \"%s\": %s", handle.path,
													strerror(errno));
			}
			buffer = (void *)(data_ptr_cast(buffer) + current_bytes_written);
			nr_bytes -= current_bytes_written;
		}
#else
		while (nr_bytes > 0)
		{
			auto bytes_to_write = MinValue<idx_t>(idx_t(NumericLimits<int32_t>::Maximum()), idx_t(nr_bytes));
			int64_t current_bytes_written = fwrite(buffer, 1, bytes_to_write, shell_handle.pipe);
			if (current_bytes_written <= 0)
			{
				throw IOException({{"errno", std::to_string(errno)}}, "Could not write to pipe \"%s\": %s", handle.path,
													strerror(errno));
			}
			buffer = (void *)(data_ptr_cast(buffer) + current_bytes_written);
			nr_bytes -= current_bytes_written;
		}
#endif

		return requested_bytes;
	}

	int64_t ShellFileSystem::GetFileSize(FileHandle &handle)
	{
		// You can't know the size of the data that will come over a pipe
		// some code uses the size to allocate buffers, so don't return
		// a very large number.
		return 0;
	}

	timestamp_t ShellFileSystem::GetLastModifiedTime(FileHandle &handle)
	{
		// You can't know the last modified time of a pipe
		return timestamp_t(0);
	}

	unique_ptr<FileHandle> ShellFileSystem::OpenFile(const string &path, FileOpenFlags flags,
																									 optional_ptr<FileOpener> opener)
	{
		Value value;
		bool ignore_sigpipe = false;
		if (FileOpener::TryGetCurrentSetting(opener, "ignore_sigpipe", value))
		{
			ignore_sigpipe = value.GetValue<bool>();
		}

		const bool for_writing = path.front() == '|';
		string command;
		std::unordered_set<int> allowed_exit_codes;
		if (for_writing)
		{
			// We want to write to the pipe.
			command = path.substr(1, path.size());
			allowed_exit_codes.insert(0);
		}
		else
		{
			// We want to read from the pipe
			auto parsed = parseInputCommand(path);
			command = std::move(parsed.command);
			allowed_exit_codes = std::move(parsed.allowed_exit_codes);
		}

#ifndef _WIN32
		pid_t child_pid;
		int fd = SpawnShellCommand(command, for_writing, child_pid);
		if (fd == -1)
		{
			throw IOException({{"errno", std::to_string(errno)}}, "Could not open pipe for %s \"%s\": %s",
												for_writing ? "writing" : "reading", path, strerror(errno));
		}
		return make_uniq<ShellFileHandle>(*this, path, fd, child_pid, flags, std::move(allowed_exit_codes),
																			ignore_sigpipe);
#else
		FILE *pipe = _popen(command.c_str(), for_writing ? "w" : "r");
		if (!pipe)
		{
			throw IOException({{"errno", std::to_string(errno)}}, "Could not open pipe for %s \"%s\": %s",
												for_writing ? "writing" : "reading", path, strerror(errno));
		}
		return make_uniq<ShellFileHandle>(*this, path, pipe, flags, std::move(allowed_exit_codes), ignore_sigpipe);
#endif
	}

	bool ShellFileSystem::CanHandleFile(const string &fpath)
	{
		if (fpath.empty())
		{
			return false;
		}
		// If the filename ends with | or starts with |
		// it can be handled by this file system.
		return fpath.back() == '|' || fpath.front() == '|';
	}

} // namespace duckdb
