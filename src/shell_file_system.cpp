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
#include "duckdb/common/string_util.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <cctype>
#include <chrono>
#include <thread>
#include <memory>

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
#include <poll.h>

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
	//
	// This relies on SIGPIPE being directed at the writing thread, as on Linux. On macOS (XNU) the SIGPIPE
	// raised by write() is sent to the whole process and can be delivered to any thread that does not block
	// it, so there the write end of the pipe is additionally marked with F_SETNOSIGPIPE (see
	// SpawnShellCommand) and no signal is raised at all.
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
#ifdef F_SETNOSIGPIPE
		if (for_writing)
		{
			// Writing to the pipe after the command exited fails with EPIPE without raising SIGPIPE, which
			// would otherwise be delivered to (and kill) the process on platforms where it is not
			// thread-directed (macOS).
			fcntl(parent_fd, F_SETNOSIGPIPE, 1);
		}
#endif

		// DuckDB's end of the pipe is non-blocking: Read and Write wait for it with poll() and a timeout, so that
		// they notice an interrupted query, and a large write() must not block once the pipe is full.
		fcntl(parent_fd, F_SETFL, fcntl(parent_fd, F_GETFL) | O_NONBLOCK);

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

	//! How often a blocked read from / write to a pipe wakes up to check whether the query was interrupted.
	static constexpr int PIPE_POLL_INTERVAL_MS = 100;
	//! How long a command is given to exit after DuckDB stopped reading its output early (e.g. LIMIT) before
	//! it is terminated. A command that is still writing normally exits at once with SIGPIPE.
	static constexpr int64_t EARLY_CLOSE_GRACE_MS = 1000;
	//! How long a command is given to exit after SIGTERM before it is killed with SIGKILL.
	static constexpr int64_t TERMINATE_GRACE_MS = 1000;

	struct ShellFileHandle : public FileHandle
	{
		friend class ShellFileSystem;

	public:
#ifndef _WIN32
		ShellFileHandle(FileSystem &file_system, string path, int fd, pid_t child_pid, FileOpenFlags flags,
										bool for_writing, std::unordered_set<int> allowed_exit_codes, bool ignore_sigpipe,
										weak_ptr<ClientContext> context)
				: FileHandle(file_system, std::move(path), std::move(flags)), fd(fd), child_pid(child_pid),
					for_writing(for_writing), allowed_exit_codes(std::move(allowed_exit_codes)),
					ignore_sigpipe(ignore_sigpipe), context(std::move(context))
		{
		}
#else
		ShellFileHandle(FileSystem &file_system, string path, FILE *pipe, FileOpenFlags flags, bool for_writing,
										std::unordered_set<int> allowed_exit_codes, bool ignore_sigpipe,
										weak_ptr<ClientContext> context)
				: FileHandle(file_system, std::move(path), std::move(flags)), pipe(pipe), for_writing(for_writing),
					allowed_exit_codes(std::move(allowed_exit_codes)), ignore_sigpipe(ignore_sigpipe),
					context(std::move(context))
		{
		}
#endif

		~ShellFileHandle() override
		{
			// A destructor must not throw: report a failure of the command through DuckDB's logger instead.
			string error;
			try
			{
				error = Finish(true);
			}
			catch (const InterruptException &)
			{
				// The query was interrupted; the command has been terminated.
			}
			catch (const std::exception &e)
			{
				error = e.what();
			}
			if (!error.empty())
			{
				LogWarning(error);
			}
		}

	private:
#ifndef _WIN32
		int fd;
		pid_t child_pid;
#else
		FILE *pipe;
#endif
		//! True for '| command' (DuckDB writes to the command's stdin), false for 'command |'.
		bool for_writing;
		//! Set once a read found the end of the command's output.
		bool reached_eof = false;
		//! Set once a write found that the command no longer reads its input (only with ignore_sigpipe).
		bool output_closed = false;
		std::unordered_set<int> allowed_exit_codes;
		//! Value of the ignore_sigpipe setting when the pipe was opened.
		bool ignore_sigpipe;
		//! The client that opened the pipe, used to notice an interrupted query and for logging.
		weak_ptr<ClientContext> context;

		bool IsOpen() const
		{
#ifndef _WIN32
			return fd != -1;
#else
			return pipe != NULL;
#endif
		}

		bool InterruptRequested() const
		{
			auto client = context.lock();
			return client && client->IsInterrupted();
		}

		void LogWarning(const string &message)
		{
			try
			{
				auto client = context.lock();
				if (client)
				{
					DUCKDB_LOG_WARNING(*client, "shellfs: " + message);
				}
			}
			catch (...)
			{
			}
		}

#ifndef _WIN32
		enum class WaitResult
		{
			EXITED,
			TIMED_OUT,
			INTERRUPTED
		};

		//! Waits for the command to exit without blocking indefinitely in waitpid(): gives up after timeout_ms
		//! (when not negative) or, with check_interrupt, as soon as the query is interrupted.
		WaitResult WaitForChild(int &status, int64_t timeout_ms, bool check_interrupt)
		{
			const auto start = std::chrono::steady_clock::now();
			int64_t sleep_us = 50;
			while (true)
			{
				pid_t wait_result = waitpid(child_pid, &status, WNOHANG);
				if (wait_result == child_pid)
				{
					return WaitResult::EXITED;
				}
				if (wait_result == -1)
				{
					if (errno == EINTR)
					{
						continue;
					}
					throw IOException({{"errno", std::to_string(errno)}}, "Could not close pipe \"%s\": %s", path,
														strerror(errno));
				}
				if (check_interrupt && InterruptRequested())
				{
					return WaitResult::INTERRUPTED;
				}
				if (timeout_ms >= 0)
				{
					auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
														 std::chrono::steady_clock::now() - start)
														 .count();
					if (elapsed >= timeout_ms)
					{
						return WaitResult::TIMED_OUT;
					}
				}
				std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
				sleep_us = MinValue<int64_t>(sleep_us * 2, 20000);
			}
		}

		//! Terminates the command (SIGTERM, then SIGKILL if it does not exit in time) and reaps it.
		void TerminateChild()
		{
			int status;
			kill(child_pid, SIGTERM);
			try
			{
				if (WaitForChild(status, TERMINATE_GRACE_MS, false) == WaitResult::EXITED)
				{
					return;
				}
			}
			catch (...)
			{
				return;
			}
			kill(child_pid, SIGKILL);
			pid_t wait_result;
			do
			{
				wait_result = waitpid(child_pid, &status, 0);
			} while (wait_result == -1 && errno == EINTR);
		}
#endif

	public:
		//! Closes the pipe and terminates the command because the query was interrupted.
		void Abort()
		{
#ifndef _WIN32
			if (IsOpen())
			{
				close(fd);
				fd = -1;
				TerminateChild();
			}
#endif
		}

		//! Closes the pipe and waits for the command to exit. Returns a description of a failure of the
		//! command (non-zero exit status, signal), or an empty string. Throws InterruptException if the query
		//! was interrupted while waiting (the command is then terminated).
		//!
		//! DuckDB closes a pipe it read to the end, or wrote everything to, explicitly. A pipe that is still open
		//! when the handle is destroyed (from_destructor) was abandoned: DuckDB stopped reading early (e.g. a
		//! LIMIT) or the query writing to it failed or was interrupted.
		string Finish(bool from_destructor = false)
		{
			if (!IsOpen())
			{
				return string();
			}

#ifndef _WIN32
			close(fd);
			// Indicate that the pipe has been closed.
			fd = -1;

			int result = 0;
			if (from_destructor || (!for_writing && !reached_eof))
			{
				// The pipe was abandoned. A command that is still writing to it now exits with SIGPIPE, one that
				// reads from it sees the end of its input; one that does not exit soon is terminated. Either way
				// the command stopped because DuckDB no longer uses it, which is not an error.
				if (WaitForChild(result, EARLY_CLOSE_GRACE_MS, false) != WaitResult::EXITED)
				{
					TerminateChild();
				}
				return string();
			}

			if (WaitForChild(result, -1, true) == WaitResult::INTERRUPTED)
			{
				TerminateChild();
				throw InterruptException();
			}
			if (WIFEXITED(result))
			{
				int exit_status = WEXITSTATUS(result);
				if (allowed_exit_codes.find(exit_status) == allowed_exit_codes.end())
				{
					return StringUtil::Format("Pipe process exited abnormally code=%d: %s", exit_status, path);
				}
			}
			else if (WIFSIGNALED(result))
			{
				int signal_number = WTERMSIG(result);
				if (!(ignore_sigpipe && signal_number == SIGPIPE))
				{
					return StringUtil::Format("Pipe process exited with signal signal=%d: %s", signal_number, path);
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
			if (from_destructor || (!for_writing && !reached_eof))
			{
				// The pipe was abandoned: see the POSIX implementation above.
				return string();
			}
			// On Windows, _pclose() returns the exit status directly
			if (allowed_exit_codes.find(result) == allowed_exit_codes.end())
			{
				return StringUtil::Format("Pipe process exited abnormally code=%d: %s", result, path);
			}
#endif
			return string();
		}

		void Close() override
		{
			auto error = Finish();
			if (!error.empty())
			{
				throw IOException(error);
			}
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
			// Wait for data with a timeout so that a command that produces no output does not block an
			// interrupted query forever.
			struct pollfd poll_fd;
			poll_fd.fd = shell_handle.fd;
			poll_fd.events = POLLIN;
			poll_fd.revents = 0;
			int poll_result = poll(&poll_fd, 1, PIPE_POLL_INTERVAL_MS);
			if (poll_result == -1 && errno != EINTR)
			{
				throw IOException({{"errno", std::to_string(errno)}}, "Could not read from pipe \"%s\": %s",
													handle.path, strerror(errno));
			}
			if (poll_result <= 0)
			{
				if (shell_handle.InterruptRequested())
				{
					shell_handle.Abort();
					throw InterruptException();
				}
				continue;
			}
			auto result = read(shell_handle.fd, data_ptr_cast(buffer) + bytes_read, nr_bytes - bytes_read);
			if (result == -1)
			{
				if (errno == EINTR || errno == EAGAIN)
				{
					continue;
				}
				throw IOException({{"errno", std::to_string(errno)}}, "Could not read from pipe \"%s\": %s",
													handle.path, strerror(errno));
			}
			if (result == 0)
			{
				shell_handle.reached_eof = true;
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
		if (bytes_read < nr_bytes)
		{
			shell_handle.reached_eof = true;
		}
#endif
		if (shell_handle.reached_eof)
		{
			// The end of the output was reached: wait for the command now, so that a failure (non-zero exit
			// status) is reported by the query rather than lost in the destructor.
			handle.Close();
		}
		return bytes_read;
	}

	int64_t ShellFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes)
	{
		auto &shell_handle = handle.Cast<ShellFileHandle>();
		const int64_t requested_bytes = nr_bytes;
		if (!shell_handle.IsOpen())
		{
			throw IOException("Could not write to pipe \"%s\": the pipe is closed", handle.path);
		}

#ifndef _WIN32
		if (shell_handle.output_closed)
		{
			// The command stopped reading (ignore_sigpipe): discard the rest of the output.
			return requested_bytes;
		}

		SigpipeGuard sigpipe_guard;
		while (nr_bytes > 0)
		{
			// Wait until the command can take more input with a timeout, so that a command that stopped
			// reading its input does not block an interrupted query forever.
			struct pollfd poll_fd;
			poll_fd.fd = shell_handle.fd;
			poll_fd.events = POLLOUT;
			poll_fd.revents = 0;
			int poll_result = poll(&poll_fd, 1, PIPE_POLL_INTERVAL_MS);
			if (poll_result == -1 && errno != EINTR)
			{
				throw IOException({{"errno", std::to_string(errno)}}, "Could not write to pipe \"%s\": %s", handle.path,
													strerror(errno));
			}
			if (poll_result <= 0)
			{
				if (shell_handle.InterruptRequested())
				{
					shell_handle.Abort();
					throw InterruptException();
				}
				continue;
			}
			auto bytes_to_write = MinValue<idx_t>(idx_t(NumericLimits<int32_t>::Maximum()), idx_t(nr_bytes));
			auto current_bytes_written = write(shell_handle.fd, buffer, bytes_to_write);
			if (current_bytes_written == -1 && (errno == EINTR || errno == EAGAIN))
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
		if (shell_handle.output_closed)
		{
			// The command stopped reading (ignore_sigpipe): discard the rest of the output.
			return requested_bytes;
		}

		while (nr_bytes > 0)
		{
			auto bytes_to_write = MinValue<idx_t>(idx_t(NumericLimits<int32_t>::Maximum()), idx_t(nr_bytes));
			int64_t current_bytes_written = fwrite(buffer, 1, bytes_to_write, shell_handle.pipe);
			if (current_bytes_written < int64_t(bytes_to_write) && errno == EPIPE && shell_handle.ignore_sigpipe)
			{
				// The command exited before reading everything; see the POSIX implementation above.
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
		// Running a shell command is external access by definition. DuckDB's own check of the path against
		// allowed_directories / allowed_paths is a string prefix match, which a command such as
		// '/allowed/dir/x; other-command |' would pass, so refuse every command here.
		auto &config = DBConfig::GetConfig(db);
		if (!Settings::Get<EnableExternalAccessSetting>(config))
		{
			throw PermissionException("Cannot run shell command \"%s\": shellfs commands are disabled because "
																"enable_external_access is false",
																path);
		}
		Value value;
		bool enabled = true;
		if (FileOpener::TryGetCurrentSetting(opener, "shellfs_enabled", value) ||
				config.TryGetCurrentSetting("shellfs_enabled", value))
		{
			enabled = value.IsNull() || value.GetValue<bool>();
		}
		if (!enabled)
		{
			throw PermissionException("Cannot run shell command \"%s\": shellfs commands are disabled because "
																"shellfs_enabled is false",
																path);
		}

		bool ignore_sigpipe = false;
		if (FileOpener::TryGetCurrentSetting(opener, "ignore_sigpipe", value))
		{
			ignore_sigpipe = !value.IsNull() && value.GetValue<bool>();
		}

		const bool for_writing = path.front() == '|';
		if (for_writing && flags.OpenForReading())
		{
			throw IOException("Cannot read from \"%s\": a path that starts with '|' is a command that DuckDB writes "
												"to. To read the output of a command, end the path with '|', e.g. 'seq 10 |'",
												path);
		}
		if (!for_writing && flags.OpenForWriting())
		{
			throw IOException("Cannot write to \"%s\": a path that ends with '|' is a command that DuckDB reads "
												"from. To write to a command, start the path with '|', e.g. '| cat > out.csv'",
												path);
		}

		weak_ptr<ClientContext> client;
		auto client_context = FileOpener::TryGetClientContext(opener);
		if (client_context)
		{
			try
			{
				client = client_context->shared_from_this();
			}
			catch (const std::bad_weak_ptr &)
			{
				// Not owned by a shared_ptr: interrupts cannot be noticed while waiting for the command.
			}
		}

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
		return make_uniq<ShellFileHandle>(*this, path, fd, child_pid, flags, for_writing, std::move(allowed_exit_codes),
																			ignore_sigpipe, std::move(client));
#else
		// Binary mode: in the default text mode the CRT translates "\n" to "\r\n" when writing (and back,
		// and stops at Ctrl-Z, when reading), which corrupts binary data such as Parquet files.
		FILE *pipe = _popen(command.c_str(), for_writing ? "wb" : "rb");
		if (!pipe)
		{
			throw IOException({{"errno", std::to_string(errno)}}, "Could not open pipe for %s \"%s\": %s",
												for_writing ? "writing" : "reading", path, strerror(errno));
		}
		return make_uniq<ShellFileHandle>(*this, path, pipe, flags, for_writing, std::move(allowed_exit_codes),
																			ignore_sigpipe, std::move(client));
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
