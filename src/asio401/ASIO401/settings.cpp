#define _CRT_SECURE_NO_WARNINGS  // Avoid issues with toml.h

#include "settings.h"

#include "asio401.rc.h"
#include "log.h"

#include <toml/toml.h>

#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace asio401 {

	namespace {

		struct LevelChoice {
			double dbv;
			const wchar_t* label;
		};

		constexpr LevelChoice qa403InputLevels[] = {
			{ 0.0, L"0 dBV" }, { 6.0, L"+6 dBV" }, { 12.0, L"+12 dBV" }, { 18.0, L"+18 dBV" },
			{ 24.0, L"+24 dBV" }, { 30.0, L"+30 dBV" }, { 36.0, L"+36 dBV" }, { 42.0, L"+42 dBV" },
		};
		constexpr LevelChoice qa403OutputLevels[] = {
			{ -12.0, L"-12 dBV" }, { -2.0, L"-2 dBV" }, { 8.0, L"+8 dBV" }, { 18.0, L"+18 dBV" },
		};
		constexpr LevelChoice qa401InputLevels[] = {
			{ 6.0, L"+6 dBV (attenuator off)" }, { 26.0, L"+26 dBV (attenuator on)" },
		};
		constexpr LevelChoice qa401OutputLevels[] = {
			{ 5.5, L"+5.5 dBV" },
		};
		constexpr int64_t bufferSizeChoices[] = { 256, 512, 1024, 2048, 4096, 8192 };

		std::span<const LevelChoice> InputLevels(SettingsDevice device) {
			return device == SettingsDevice::QA401 ? std::span<const LevelChoice>(qa401InputLevels) : std::span<const LevelChoice>(qa403InputLevels);
		}
		std::span<const LevelChoice> OutputLevels(SettingsDevice device) {
			return device == SettingsDevice::QA401 ? std::span<const LevelChoice>(qa401OutputLevels) : std::span<const LevelChoice>(qa403OutputLevels);
		}
		double DefaultInputLevel(SettingsDevice device) { return device == SettingsDevice::QA401 ? 26.0 : 42.0; }
		double DefaultOutputLevel(SettingsDevice device) { return device == SettingsDevice::QA401 ? 5.5 : -12.0; }

		struct DialogState {
			SettingsDevice device;
			Config config;  // Current config on entry, edited config on exit
			std::vector<int64_t> bufferSizes;  // Combo box index -> buffer size; 0 means "automatic"
		};

		void AddChoice(HWND combo, const wchar_t* label, bool select) {
			const auto index = ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
			if (select) ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
		}

		void PopulateLevels(HWND combo, std::span<const LevelChoice> levels, double current) {
			bool found = false;
			for (const auto& level : levels) {
				const bool select = level.dbv == current;
				found = found || select;
				AddChoice(combo, level.label, select);
			}
			if (!found) ::SendMessageW(combo, CB_SETCURSEL, 0, 0);
		}

		void PopulateBufferSizes(HWND combo, DialogState& state) {
			const auto current = state.config.bufferSizeSamples;
			state.bufferSizes.push_back(0);
			AddChoice(combo, L"Automatic (host application chooses)", !current.has_value());
			bool found = false;
			for (const auto bufferSize : bufferSizeChoices) {
				wchar_t label[64];
				::swprintf_s(label, L"%lld samples (%.1f ms at 48 kHz)", static_cast<long long>(bufferSize), static_cast<double>(bufferSize) / 48.0);
				const bool select = current.has_value() && *current == bufferSize;
				found = found || select;
				state.bufferSizes.push_back(bufferSize);
				AddChoice(combo, label, select);
			}
			if (current.has_value() && !found) {
				wchar_t label[64];
				::swprintf_s(label, L"%lld samples (from configuration file)", static_cast<long long>(*current));
				state.bufferSizes.push_back(*current);
				AddChoice(combo, label, true);
			}
		}

		void Populate(HWND dialog, DialogState& state) {
			const auto& config = state.config;
			PopulateLevels(::GetDlgItem(dialog, IDC_INPUT_LEVEL), InputLevels(state.device), config.fullScaleInputLevelDBV.value_or(DefaultInputLevel(state.device)));
			PopulateLevels(::GetDlgItem(dialog, IDC_OUTPUT_LEVEL), OutputLevels(state.device), config.fullScaleOutputLevelDBV.value_or(DefaultOutputLevel(state.device)));
			PopulateBufferSizes(::GetDlgItem(dialog, IDC_BUFFER_SIZE), state);
			::CheckDlgButton(dialog, IDC_KEEP_LEVELS, config.resetLevelsOnClose ? BST_UNCHECKED : BST_CHECKED);
			::CheckDlgButton(dialog, IDC_FORCE_READ, config.forceRead ? BST_CHECKED : BST_UNCHECKED);
		}

		size_t Selection(HWND dialog, int control) {
			const auto selection = ::SendMessageW(::GetDlgItem(dialog, control), CB_GETCURSEL, 0, 0);
			return selection == CB_ERR ? 0 : static_cast<size_t>(selection);
		}

		void ReadBack(HWND dialog, DialogState& state) {
			auto& config = state.config;
			config.fullScaleInputLevelDBV = InputLevels(state.device)[Selection(dialog, IDC_INPUT_LEVEL)].dbv;
			config.fullScaleOutputLevelDBV = OutputLevels(state.device)[Selection(dialog, IDC_OUTPUT_LEVEL)].dbv;
			const auto bufferSize = state.bufferSizes[Selection(dialog, IDC_BUFFER_SIZE)];
			if (bufferSize == 0) config.bufferSizeSamples.reset(); else config.bufferSizeSamples = bufferSize;
			config.resetLevelsOnClose = ::IsDlgButtonChecked(dialog, IDC_KEEP_LEVELS) != BST_CHECKED;
			config.forceRead = ::IsDlgButtonChecked(dialog, IDC_FORCE_READ) == BST_CHECKED;
		}

		// If the owner window is hidden or parked off-screen, DS_CENTER puts the dialog off-screen as well. Bring it back.
		void EnsureOnScreen(HWND dialog) {
			RECT rect;
			if (::GetWindowRect(dialog, &rect) == 0) return;
			MONITORINFO monitorInfo = { sizeof(monitorInfo) };
			if (::GetMonitorInfoW(::MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitorInfo) == 0) return;
			const auto& work = monitorInfo.rcWork;
			if (rect.left >= work.left && rect.top >= work.top && rect.right <= work.right && rect.bottom <= work.bottom) return;
			const int width = rect.right - rect.left, height = rect.bottom - rect.top;
			Log() << "Settings dialog is off-screen, moving it to the nearest monitor";
			::SetWindowPos(dialog, nullptr, work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}

		INT_PTR CALLBACK SettingsDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
			switch (message) {
			case WM_INITDIALOG:
				::SetWindowLongPtrW(dialog, DWLP_USER, static_cast<LONG_PTR>(lParam));
				Populate(dialog, *reinterpret_cast<DialogState*>(lParam));
				EnsureOnScreen(dialog);
				::SetForegroundWindow(dialog);
				return TRUE;
			case WM_COMMAND:
				switch (LOWORD(wParam)) {
				case IDOK:
					ReadBack(dialog, *reinterpret_cast<DialogState*>(::GetWindowLongPtrW(dialog, DWLP_USER)));
					::EndDialog(dialog, IDOK);
					return TRUE;
				case IDCANCEL:
					::EndDialog(dialog, IDCANCEL);
					return TRUE;
				}
				break;
			}
			return FALSE;
		}

		bool SaveConfig(const Config& config, std::wstring& error) {
			const auto path = GetConfigFilePath();
			if (!path.has_value()) {
				error = L"Could not determine the location of the configuration file.";
				return false;
			}

			toml::Value root((toml::Table()));
			if (config.fullScaleInputLevelDBV.has_value()) root.set("fullScaleInputLevelDBV", *config.fullScaleInputLevelDBV);
			if (config.fullScaleOutputLevelDBV.has_value()) root.set("fullScaleOutputLevelDBV", *config.fullScaleOutputLevelDBV);
			if (config.bufferSizeSamples.has_value()) root.set("bufferSizeSamples", *config.bufferSizeSamples);
			root.set("forceRead", config.forceRead);
			root.set("resetLevelsOnClose", config.resetLevelsOnClose);

			Log() << "Writing configuration file: " << *path;
			std::ofstream stream(*path, std::ios::trunc);
			if (!stream) {
				error = L"Could not write the configuration file:\n" + path->wstring();
				return false;
			}
			stream.precision(1);  // tinytoml writes doubles with std::fixed, so this yields e.g. "18.0"
			stream << "# Written by the ASIO401 settings dialog. See CONFIGURATION.md for details.\n";
			root.write(&stream);
			stream.close();
			if (!stream) {
				error = L"Could not write the configuration file:\n" + path->wstring();
				return false;
			}
			return true;
		}

		// Any object inside this module will do to locate the module handle that holds the dialog resource.
		int moduleAnchor = 0;

	}

	void ShowSettingsDialog(HWND parent, SettingsDevice device, const Config& currentConfig) {
		HMODULE module = nullptr;
		if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&moduleAnchor), &module) == 0) {
			Log() << "Unable to determine module handle for settings dialog: " << ::GetLastError();
			return;
		}

		// The window handle the host gave us in ASIOInit() is not necessarily the window the user is interacting with
		// (e.g. ARTA passes a window that is not its device setup dialog). Prefer the window that is active on this
		// thread right now, so that the dialog shows on top of it and blocks it while open.
		HWND owner = ::GetActiveWindow();
		if (owner == nullptr) owner = parent;
		Log() << "Showing settings dialog from module " << module << " with owner window " << owner << " (host window " << parent << ", visible: " << ::IsWindowVisible(parent) << ")";

		DialogState state{ device, currentConfig, {} };
		const auto result = ::DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_SETTINGS), owner, SettingsDialogProc, reinterpret_cast<LPARAM>(&state));
		if (result == 0 || result == -1) {
			Log() << "Unable to show settings dialog: DialogBoxParam() returned " << result << ", Windows error " << ::GetLastError();
			return;
		}
		Log() << "Settings dialog returned " << result;
		if (result != IDOK) return;

		std::wstring error;
		if (!SaveConfig(state.config, error)) {
			::MessageBoxW(parent, error.c_str(), L"ASIO401 Settings", MB_OK | MB_ICONERROR);
		}
	}

}
