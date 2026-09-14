#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace asio401 {

	// Location of ASIO401.toml, i.e. directly inside the user profile folder.
	std::optional<std::filesystem::path> GetConfigFilePath();

	struct Config {
		std::optional<double> fullScaleInputLevelDBV;
		std::optional<double> fullScaleOutputLevelDBV;
		std::optional<int64_t> bufferSizeSamples;
		bool forceRead = false;
		bool resetLevelsOnClose = true;
	};

	std::optional<Config> LoadConfig();

}