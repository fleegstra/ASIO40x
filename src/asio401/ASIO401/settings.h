#pragma once

#include "config.h"

#include <windows.h>

namespace asio401 {

	enum class SettingsDevice { QA401, QA403 };

	// Shows the modal settings dialog. If the user clicks OK, the configuration file is overwritten with the new settings.
	void ShowSettingsDialog(HWND parent, SettingsDevice device, const Config& currentConfig);

}
