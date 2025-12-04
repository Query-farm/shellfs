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

		// When writing to a PIPE ignore the SIGPIPE error and consider that the write succeeded.
		config.AddExtensionOption("ignore_sigpipe", "Ignore SIGPIPE", LogicalType::BOOLEAN, Value(false));

		QueryFarmSendTelemetry(loader, "shellfs", "2025120401");
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
