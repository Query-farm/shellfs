#pragma once

#include "duckdb.hpp"

//! The extension version (YYYYMMDDNN), reported by Version() and the telemetry.
#define SHELLFS_EXTENSION_VERSION "2026110701"

namespace duckdb
{

	class ShellfsExtension : public Extension
	{
	public:
		void Load(ExtensionLoader &loader) override;
		std::string Name() override;
		std::string Version() const override;
	};

} // namespace duckdb
