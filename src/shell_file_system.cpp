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

	struct ShellFileHandle : public FileHandle
	{
		friend class ShellFileSystem;

	public:
		ShellFileHandle(FileSystem &file_system, string path, FILE *pipe, FileOpenFlags flags)
				: FileHandle(file_system, std::move(path), std::move(flags)), pipe(pipe)
		{
			allowed_exit_codes.insert(0);
		}

		ShellFileHandle(FileSystem &file_system, string path, FILE *pipe, FileOpenFlags flags, std::unordered_set<int> allowed_exit_codes)
				: FileHandle(file_system, std::move(path), std::move(flags)), pipe(pipe), allowed_exit_codes(std::move(allowed_exit_codes))
		{
		}
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
		FILE *pipe;
		std::unordered_set<int> allowed_exit_codes;

	public:
		void Close() override
		{
			if (!pipe)
			{
				return;
			}

			int result;

#ifndef _WIN32
			result = pclose(pipe);
#else
			result = _pclose(pipe);
#endif
			// Indicate that the pipe has been closed.
			pipe = NULL;

			if (result == -1)
			{
				throw IOException("Could not close pipe \"%s\": %s", {{"errno", std::to_string(errno)}}, path,
													strerror(errno));
			}
			else
			{
#ifndef _WIN32
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
					throw IOException("Pipe process exited with signal signal=%d: %s", signal_number, path);
				}
#else
				// On Windows, _pclose() returns the exit status directly
				if (allowed_exit_codes.find(result) == allowed_exit_codes.end())
				{
					throw IOException("Pipe process exited abnormally code=%d: %s", result, path);
				}
#endif
			}
		};
	};

	void ShellFileSystem::Reset(FileHandle &handle)
	{
		throw InternalException("Cannot reset shell file system");
	}

	int64_t ShellFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes)
	{
		FILE *pipe = handle.Cast<ShellFileHandle>().pipe;

		if (!pipe)
		{
			return 0;
		}

		int64_t bytes_read = fread(buffer, 1, nr_bytes, pipe);
		if (bytes_read == -1)
		{
			throw IOException("Could not read from pipe \"%s\": %s", {{"errno", std::to_string(errno)}}, handle.path,
												strerror(errno));
		}
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
		FILE *pipe = handle.Cast<ShellFileHandle>().pipe;
		int64_t bytes_written = 0;

		while (nr_bytes > 0)
		{
			auto bytes_to_write = MinValue<idx_t>(idx_t(NumericLimits<int32_t>::Maximum()), idx_t(nr_bytes));
			int64_t current_bytes_written = fwrite(buffer, 1, bytes_to_write, pipe);
			if (current_bytes_written <= 0)
			{
				throw IOException("Could not write to pipe \"%s\": %s", {{"errno", std::to_string(errno)}}, handle.path,
													strerror(errno));
			}
			bytes_written += current_bytes_written;
			buffer = (void *)(data_ptr_cast(buffer) + current_bytes_written);
			nr_bytes -= current_bytes_written;
		}

		return bytes_written;
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
		FILE *pipe;
		unique_ptr<ShellFileHandle> result;
		if (path.front() == '|')
		{
			// We want to write to the pipe.
#ifndef _WIN32
			pipe = popen(path.substr(1, path.size()).c_str(), "w");
#else
			pipe = _popen(path.substr(1, path.size()).c_str(), "w");
#endif
			if (!pipe)
			{
				throw IOException("Could not open pipe for writing \"%s\": %s", {{"errno", std::to_string(errno)}}, path,
													strerror(errno));
			}
			result = make_uniq<ShellFileHandle>(*this, path, pipe, flags);
		}
		else
		{
			// We want to read from the pipe
			auto parsed = parseInputCommand(path);

#ifndef _WIN32
			pipe = popen(parsed.command.c_str(), "r");
#else
			pipe = _popen(parsed.command.c_str(), "r");
#endif
			if (!pipe)
			{
				throw IOException("Could not open pipe for reading \"%s\": %s", {{"errno", std::to_string(errno)}}, path,
													strerror(errno));
			}
			result = make_uniq<ShellFileHandle>(*this, path, pipe, flags, parsed.allowed_exit_codes);
		}

#ifndef _WIN32
		Value value;
		bool ignore_sigpipe = false;
		if (FileOpener::TryGetCurrentSetting(opener, "ignore_sigpipe", value))
		{
			ignore_sigpipe = value.GetValue<bool>();
		}

		if (ignore_sigpipe)
		{
			signal(SIGPIPE, SIG_IGN);
		}
#endif

		return result;
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
