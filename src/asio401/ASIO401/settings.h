#pragma once

#include "calibration.h"
#include "config.h"

#include <windows.h>

#include <optional>

namespace asio401 {

	enum class SettingsDevice { QA401, QA403 };

	// Shows the modal settings dialog. If the user clicks OK, the configuration file is overwritten with the new settings.
	// calibration is displayed as read-only sensitivities; pass nullopt if the device has none (the section is then hidden).
	void ShowSettingsDialog(HWND parent, SettingsDevice device, const Config& currentConfig, const std::optional<Calibration>& calibration);

}
