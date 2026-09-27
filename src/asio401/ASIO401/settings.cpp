#define _CRT_SECURE_NO_WARNINGS  // Avoid issues with toml.h

#include "settings.h"

#include "asio401.rc.h"
#include "log.h"

#include <toml/toml.h>

#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <optional>
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
			std::optional<Calibration> calibration;
			std::vector<int64_t> bufferSizes;  // Combo box index -> buffer size; 0 means "automatic"
			HFONT boldFont = nullptr;  // Owned; used for the section headings
			HFONT iconFont = nullptr;  // Owned; used for the copy buttons if the icon font is available
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

		size_t Selection(HWND dialog, int control) {
			const auto selection = ::SendMessageW(::GetDlgItem(dialog, control), CB_GETCURSEL, 0, 0);
			return selection == CB_ERR ? 0 : static_cast<size_t>(selection);
		}

		// --- Calibrated sensitivities section ---

		// Formats a number with a '.' decimal separator regardless of the host process locale, so that the value can be pasted as-is.
		std::wstring FormatNumber(double value, int decimals) {
			wchar_t buffer[64];
			::swprintf_s(buffer, L"%.*f", decimals, value);
			std::wstring result(buffer);
			for (auto& character : result) if (character == L',') character = L'.';
			return result;
		}

		// One entry per row of the section, in the order the rows appear in the dialog resource.
		std::array<std::wstring, IDC_CAL_ROW_COUNT> CalibrationRowValues(const Calibration& calibration, double inputLevelDBV, double outputLevelDBV) {
			std::array<std::wstring, IDC_CAL_ROW_COUNT> values;
			values.fill(L"-");
			constexpr double sqrt2 = 1.4142135623730951;
			if (const auto inputIndex = Calibration::InputIndex(inputLevelDBV); inputIndex.has_value()) {
				const auto& correction = calibration.input[*inputIndex];
				const auto leftPeakVolts = Calibration::InputFullScalePeakVolts(inputLevelDBV, correction.leftDB);
				const auto rightPeakVolts = Calibration::InputFullScalePeakVolts(inputLevelDBV, correction.rightDB);
				values[0] = FormatNumber(leftPeakVolts * 1000.0, 1);  // ARTA LineIn Sensitivity (mVpeak, left ch)
				values[1] = FormatNumber(correction.leftDB - correction.rightDB, 3);  // ARTA L/R channel diff. (dB), left relative to right
				values[3] = FormatNumber(leftPeakVolts / sqrt2, 3);  // REW input FS sine Vrms, left
				values[4] = FormatNumber(rightPeakVolts / sqrt2, 3);  // REW input FS sine Vrms, right
			}
			if (const auto outputIndex = Calibration::OutputIndex(outputLevelDBV); outputIndex.has_value()) {
				const auto& correction = calibration.output[*outputIndex];
				const auto leftPeakVolts = Calibration::OutputFullScalePeakVolts(outputLevelDBV, correction.leftDB);
				const auto rightPeakVolts = Calibration::OutputFullScalePeakVolts(outputLevelDBV, correction.rightDB);
				values[2] = FormatNumber(leftPeakVolts * 1000.0, 1);  // ARTA LineOut Sensitivity (mVpeak, left ch)
				values[5] = FormatNumber(leftPeakVolts / sqrt2, 3);  // REW output FS sine Vrms, left
				values[6] = FormatNumber(rightPeakVolts / sqrt2, 3);  // REW output FS sine Vrms, right
			}
			return values;
		}

		void UpdateCalibrationValues(HWND dialog, const DialogState& state) {
			if (!state.calibration.has_value()) return;
			const auto inputLevelDBV = InputLevels(state.device)[Selection(dialog, IDC_INPUT_LEVEL)].dbv;
			const auto outputLevelDBV = OutputLevels(state.device)[Selection(dialog, IDC_OUTPUT_LEVEL)].dbv;
			const auto values = CalibrationRowValues(*state.calibration, inputLevelDBV, outputLevelDBV);
			for (int row = 0; row < IDC_CAL_ROW_COUNT; ++row) ::SetDlgItemTextW(dialog, IDC_CAL_VALUE_FIRST + row, values[row].c_str());
		}

		// Returns true if the font that ends up selected for the given LOGFONT actually has the requested face name (i.e. no substitution happened).
		bool FontFaceAvailable(HWND window, HFONT font, const wchar_t* faceName) {
			const HDC dc = ::GetDC(window);
			if (dc == nullptr) return false;
			const auto previous = ::SelectObject(dc, font);
			wchar_t actualFaceName[LF_FACESIZE] = {};
			const bool available = ::GetTextFaceW(dc, LF_FACESIZE, actualFaceName) > 0 && ::_wcsicmp(actualFaceName, faceName) == 0;
			::SelectObject(dc, previous);
			::ReleaseDC(window, dc);
			return available;
		}

		void SetupCalibrationSection(HWND dialog, DialogState& state) {
			LOGFONTW logFont = {};
			const auto dialogFont = reinterpret_cast<HFONT>(::SendMessageW(dialog, WM_GETFONT, 0, 0));
			if (dialogFont != nullptr && ::GetObjectW(dialogFont, sizeof logFont, &logFont) == sizeof logFont) {
				auto boldLogFont = logFont;
				boldLogFont.lfWeight = FW_BOLD;
				state.boldFont = ::CreateFontIndirectW(&boldLogFont);
				if (state.boldFont != nullptr) {
					::SendDlgItemMessageW(dialog, IDC_CAL_ARTA_HEADING, WM_SETFONT, reinterpret_cast<WPARAM>(state.boldFont), TRUE);
					::SendDlgItemMessageW(dialog, IDC_CAL_REW_HEADING, WM_SETFONT, reinterpret_cast<WPARAM>(state.boldFont), TRUE);
				}

				// Windows 10+ ships an icon font with a "Copy" glyph. Fall back to a text label if it is not there.
				auto iconLogFont = logFont;
				::wcscpy_s(iconLogFont.lfFaceName, L"Segoe MDL2 Assets");
				iconLogFont.lfWeight = FW_NORMAL;
				state.iconFont = ::CreateFontIndirectW(&iconLogFont);
				if (state.iconFont != nullptr && !FontFaceAvailable(dialog, state.iconFont, iconLogFont.lfFaceName)) {
					::DeleteObject(state.iconFont);
					state.iconFont = nullptr;
				}
			}
			for (int row = 0; row < IDC_CAL_ROW_COUNT; ++row) {
				const auto button = ::GetDlgItem(dialog, IDC_CAL_COPY_FIRST + row);
				if (state.iconFont != nullptr) {
					::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state.iconFont), TRUE);
					::SetWindowTextW(button, L"\xE8C8");  // Segoe MDL2 Assets "Copy"
				}
				else {
					::SetWindowTextW(button, L"Copy");
				}
			}
			UpdateCalibrationValues(dialog, state);
		}

		// Hides the calibrated sensitivities section and moves everything below it up, shrinking the dialog accordingly.
		void CollapseCalibrationSection(HWND dialog) {
			RECT sectionRect, streamingRect;
			if (::GetWindowRect(::GetDlgItem(dialog, IDC_CAL_GROUP), &sectionRect) == 0 || ::GetWindowRect(::GetDlgItem(dialog, IDC_STREAMING_GROUP), &streamingRect) == 0) return;
			const int delta = streamingRect.top - sectionRect.top;

			::ShowWindow(::GetDlgItem(dialog, IDC_CAL_GROUP), SW_HIDE);
			::ShowWindow(::GetDlgItem(dialog, IDC_CAL_ARTA_HEADING), SW_HIDE);
			::ShowWindow(::GetDlgItem(dialog, IDC_CAL_REW_HEADING), SW_HIDE);
			::ShowWindow(::GetDlgItem(dialog, IDC_CAL_NOTE), SW_HIDE);
			for (int row = 0; row < IDC_CAL_ROW_COUNT; ++row) {
				::ShowWindow(::GetDlgItem(dialog, IDC_CAL_LABEL_FIRST + row), SW_HIDE);
				::ShowWindow(::GetDlgItem(dialog, IDC_CAL_VALUE_FIRST + row), SW_HIDE);
				::ShowWindow(::GetDlgItem(dialog, IDC_CAL_COPY_FIRST + row), SW_HIDE);
			}

			for (const int control : { IDC_STREAMING_GROUP, IDC_BUFFER_SIZE_LABEL, IDC_BUFFER_SIZE, IDC_FORCE_READ, IDC_FOOTER_TEXT, IDOK, IDCANCEL }) {
				const auto window = ::GetDlgItem(dialog, control);
				RECT rect;
				if (::GetWindowRect(window, &rect) == 0) continue;
				::MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT*>(&rect), 2);
				::SetWindowPos(window, nullptr, rect.left, rect.top - delta, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			}

			RECT dialogRect;
			if (::GetWindowRect(dialog, &dialogRect) == 0) return;
			// Keep the dialog centered where DS_CENTER put it: shrink it and move it down by half the removed height.
			::SetWindowPos(dialog, nullptr, dialogRect.left, dialogRect.top + delta / 2, dialogRect.right - dialogRect.left, dialogRect.bottom - dialogRect.top - delta, SWP_NOZORDER | SWP_NOACTIVATE);
		}

		void CopyToClipboard(HWND dialog, const std::wstring& text) {
			if (::OpenClipboard(dialog) == 0) return;
			::EmptyClipboard();
			const auto size = (text.size() + 1) * sizeof(wchar_t);
			if (const auto global = ::GlobalAlloc(GMEM_MOVEABLE, size); global != nullptr) {
				if (const auto memory = ::GlobalLock(global); memory != nullptr) {
					::memcpy(memory, text.c_str(), size);
					::GlobalUnlock(global);
					if (::SetClipboardData(CF_UNICODETEXT, global) == nullptr) ::GlobalFree(global);
				}
				else ::GlobalFree(global);
			}
			::CloseClipboard();
		}

		void CopyCalibrationValue(HWND dialog, int row) {
			const auto edit = ::GetDlgItem(dialog, IDC_CAL_VALUE_FIRST + row);
			wchar_t text[64] = {};
			::GetWindowTextW(edit, text, static_cast<int>(std::size(text)));
			CopyToClipboard(dialog, text);
			// Give some visual feedback: select the value that was just copied.
			::SetFocus(edit);
			::SendMessageW(edit, EM_SETSEL, 0, -1);
		}

		void Populate(HWND dialog, DialogState& state) {
			const auto& config = state.config;
			PopulateLevels(::GetDlgItem(dialog, IDC_INPUT_LEVEL), InputLevels(state.device), config.fullScaleInputLevelDBV.value_or(DefaultInputLevel(state.device)));
			PopulateLevels(::GetDlgItem(dialog, IDC_OUTPUT_LEVEL), OutputLevels(state.device), config.fullScaleOutputLevelDBV.value_or(DefaultOutputLevel(state.device)));
			PopulateBufferSizes(::GetDlgItem(dialog, IDC_BUFFER_SIZE), state);
			::CheckDlgButton(dialog, IDC_KEEP_LEVELS, config.resetLevelsOnClose ? BST_UNCHECKED : BST_CHECKED);
			::CheckDlgButton(dialog, IDC_FORCE_READ, config.forceRead ? BST_CHECKED : BST_UNCHECKED);
			if (state.device == SettingsDevice::QA403 && state.calibration.has_value()) SetupCalibrationSection(dialog, state);
			else CollapseCalibrationSection(dialog);
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
			case WM_COMMAND: {
				auto& state = *reinterpret_cast<DialogState*>(::GetWindowLongPtrW(dialog, DWLP_USER));
				const int control = LOWORD(wParam);
				switch (control) {
				case IDOK:
					ReadBack(dialog, state);
					::EndDialog(dialog, IDOK);
					return TRUE;
				case IDCANCEL:
					::EndDialog(dialog, IDCANCEL);
					return TRUE;
				case IDC_INPUT_LEVEL:
				case IDC_OUTPUT_LEVEL:
					if (HIWORD(wParam) == CBN_SELCHANGE) UpdateCalibrationValues(dialog, state);
					return TRUE;
				}
				if (control >= IDC_CAL_COPY_FIRST && control < IDC_CAL_COPY_FIRST + IDC_CAL_ROW_COUNT && HIWORD(wParam) == BN_CLICKED) {
					CopyCalibrationValue(dialog, control - IDC_CAL_COPY_FIRST);
					return TRUE;
				}
				break;
			}
			case WM_CTLCOLORSTATIC: {
				// Read-only edits are drawn as statics (dialog background) by default; give the value boxes a white background so they stand out from their labels.
				const int control = ::GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
				if (control >= IDC_CAL_VALUE_FIRST && control < IDC_CAL_VALUE_FIRST + IDC_CAL_ROW_COUNT) {
					::SetBkColor(reinterpret_cast<HDC>(wParam), ::GetSysColor(COLOR_WINDOW));
					::SetTextColor(reinterpret_cast<HDC>(wParam), ::GetSysColor(COLOR_WINDOWTEXT));
					return reinterpret_cast<INT_PTR>(::GetSysColorBrush(COLOR_WINDOW));
				}
				break;
			}
			case WM_DESTROY: {
				const auto statePointer = reinterpret_cast<DialogState*>(::GetWindowLongPtrW(dialog, DWLP_USER));
				if (statePointer == nullptr) break;
				auto& state = *statePointer;
				if (state.boldFont != nullptr) { ::DeleteObject(state.boldFont); state.boldFont = nullptr; }
				if (state.iconFont != nullptr) { ::DeleteObject(state.iconFont); state.iconFont = nullptr; }
				break;
			}
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

	void ShowSettingsDialog(HWND parent, SettingsDevice device, const Config& currentConfig, const std::optional<Calibration>& calibration) {
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

		DialogState state{ device, currentConfig, calibration, {} };
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
