#include "QA403.h"

#include "log.h"

namespace asio401 {

	QA403::QA403(std::string_view devicePath) :
		qa40x(devicePath, /*registerPipeId*/0x01, /*writePipeId*/0x02, /*readPipeId*/0x82, /*requiresApp*/false, /*registerReadPipeId*/0x81) {}

	void QA403::Reset(FullScaleInputLevel fullScaleInputLevel, FullScaleOutputLevel fullScaleOutputLevel, SampleRate sampleRate) {
		Log() << "Resetting QA403";

		// Reset the hardware. This is especially important in case of a previous unclean stop,
		// where the hardware could be left in an inconsistent state.
		WriteRegister(8, 0);
		WriteRegister(5, uint32_t(fullScaleInputLevel));
		WriteRegister(6, uint32_t(fullScaleOutputLevel));
		// QuantAsylum did not publicly document sample rate setting, this is from private correspondence with them.
		WriteRegister(9, uint32_t(sampleRate));
		// Wait for a bit before setting the register again, otherwise it looks like the hardware
		// "skips past" the zero state (some kind of ABA problem?)
		::Sleep(50);

		Log() << "QA403 is reset";
	}

	void QA403::Start() {
		WriteRegister(8, 5);
	}

	uint32_t QA403::ReadRegister(uint8_t registerNumber) {
		// Protocol from QuantAsylum's PyQa40x (registers.py): send the register number with the MSB set on the register
		// pipe, then read the 32-bit big endian value from the register read pipe.
		WriteRegister(uint8_t(0x80 | registerNumber), 0);
		std::array<std::byte, 4> buffer;
		registerReadIOSlot.Execute(QA40x::RegisterReadChannel(qa40x), std::span<std::byte>(buffer));
		return (uint32_t(buffer[0]) << 24) | (uint32_t(buffer[1]) << 16) | (uint32_t(buffer[2]) << 8) | uint32_t(buffer[3]);
	}

	Calibration::Page QA403::ReadCalibrationPage() {
		// Protocol from QuantAsylum's PyQa40x (control.py, load_calibration()): select the calibration page through
		// register 0x0D, then read the page 32 bits at a time through register 0x19. Each word is stored little endian.
		Log() << "Reading QA403 calibration page";
		WriteRegister(0x0D, 0x10);
		Calibration::Page page;
		for (size_t wordIndex = 0; wordIndex < page.size() / 4; ++wordIndex) {
			const auto word = ReadRegister(0x19);
			page[wordIndex * 4 + 0] = std::byte(word >> 0);
			page[wordIndex * 4 + 1] = std::byte(word >> 8);
			page[wordIndex * 4 + 2] = std::byte(word >> 16);
			page[wordIndex * 4 + 3] = std::byte(word >> 24);
		}
		Log() << "QA403 calibration page read";
		return page;
	}

}