#pragma once

#include "duckdb/common/file_system.hpp"

namespace duckdb
{

	class ShellFileSystem : public FileSystem
	{
	public:
		duckdb::unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
																						optional_ptr<FileOpener> opener = nullptr) final;

		int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
		int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

		int64_t GetFileSize(FileHandle &handle) override;
		timestamp_t GetLastModifiedTime(FileHandle &handle) override;

		vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override
		{
			return {path};
		}

		bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override
		{
			return false;
		}

		void Reset(FileHandle &handle) override;
		bool OnDiskFile(FileHandle &handle) override
		{
			return false;
		};
		bool CanSeek() override
		{
			return false;
		}

		bool CanHandleFile(const string &fpath) override;

		bool IsPipe(const string &filename, optional_ptr<FileOpener> opener) override
		{
			return true;
		}
		void FileSync(FileHandle &handle) override
		{
		}

		std::string GetName() const override
		{
			return "ShellFileSystem";
		}
	};

} // namespace duckdb
