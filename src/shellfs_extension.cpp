#define DUCKDB_EXTENSION_MAIN

#include "shellfs_extension.hpp"
#include "shell_file_system.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "query_farm_telemetry.hpp"
namespace duckdb
{

	static void LoadInternal(ExtensionLoader &loader)
	{
		// Register a scalar function
		auto &instance = loader.GetDatabaseInstance();
		auto &fs = instance.GetFileSystem();

		fs.RegisterSubSystem(make_uniq<ShellFileSystem>());

		auto &config = DBConfig::GetConfig(instance);

		// When a command stops reading its input before DuckDB has written everything (e.g. '| head -1'),
		// discard the rest of the output instead of failing; and when DuckDB stops reading a command's output
		// early (e.g. LIMIT), do not treat the command being terminated by SIGPIPE as an error.
		config.AddExtensionOption("ignore_sigpipe",
															"Treat a pipe closed early by either side (EPIPE / SIGPIPE) as success instead of an error",
															LogicalType::BOOLEAN, Value(false));

		QueryFarmSendTelemetry(loader, "shellfs", "2026102001");
	}

	void ShellfsExtension::Load(ExtensionLoader &loader)
	{
		LoadInternal(loader);
	}

	std::string ShellfsExtension::Name()
	{
		return "shellfs";
	}

} // namespace duckdb

extern "C"
{

	DUCKDB_CPP_EXTENSION_ENTRY(shellfs, loader)
	{
		duckdb::LoadInternal(loader);
	}
}
