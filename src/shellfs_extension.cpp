#define DUCKDB_EXTENSION_MAIN

#include "shellfs_extension.hpp"
#include "shell_file_system.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "query_farm_telemetry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
namespace duckdb
{

	static void SetShellfsEnabled(ClientContext &context, SetScope scope, Value &parameter)
	{
		if (parameter.IsNull())
		{
			throw InvalidInputException("shellfs_enabled cannot be NULL");
		}
		if (!parameter.GetValue<bool>())
		{
			return;
		}
		Value current;
		if (context.TryGetCurrentSetting("shellfs_enabled", current) && !current.IsNull() && !current.GetValue<bool>())
		{
			throw InvalidInputException("shellfs_enabled cannot be set back to true once it has been set to false");
		}
	}

	static void LoadInternal(ExtensionLoader &loader)
	{
		// Register a scalar function
		auto &instance = loader.GetDatabaseInstance();
		auto &fs = instance.GetFileSystem();

		fs.RegisterSubSystem(make_uniq<ShellFileSystem>(instance));

		auto &config = DBConfig::GetConfig(instance);

		// When a command stops reading its input before DuckDB has written everything (e.g. '| head -1'),
		// discard the rest of the output instead of failing; and when DuckDB stops reading a command's output
		// early (e.g. LIMIT), do not treat the command being terminated by SIGPIPE as an error.
		config.AddExtensionOption("ignore_sigpipe",
															"Treat a pipe closed early by either side (EPIPE / SIGPIPE) as success instead of an error",
															LogicalType::BOOLEAN, Value(false));

		// Turns off running shell commands through shellfs for the session (or, with SET GLOBAL, for every
		// connection). It is one-way: once false it cannot be set back to true (or RESET), so it can be used to
		// lock shellfs down without also disabling all other external access with enable_external_access.
		config.AddExtensionOption("shellfs_enabled",
															"Allow shellfs to run shell commands ('command |' and '| command'). Once set to false "
															"it cannot be enabled again",
															LogicalType::BOOLEAN, Value(true), SetShellfsEnabled);

		QueryFarmSendTelemetry(loader, "shellfs", SHELLFS_EXTENSION_VERSION);
	}

	void ShellfsExtension::Load(ExtensionLoader &loader)
	{
		LoadInternal(loader);
	}

	std::string ShellfsExtension::Name()
	{
		return "shellfs";
	}

	std::string ShellfsExtension::Version() const
	{
		return SHELLFS_EXTENSION_VERSION;
	}

} // namespace duckdb

extern "C"
{

	DUCKDB_CPP_EXTENSION_ENTRY(shellfs, loader)
	{
		duckdb::LoadInternal(loader);
	}
}
