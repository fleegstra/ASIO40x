#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace asio401 {

	// Factory calibration data stored in the QA402/QA403 flash.
	//
	// The layout and the formulas below come from QuantAsylum's own PyQa40x library (src/PyQa40x/control.py and
	// analyzer.py, https://github.com/QuantAsylum/PyQa40x). The device returns a 512-byte page holding, for every input
	// and output range, a 6-byte record { int16 level_dBV; float correction_dB } per channel. The correction is not a
	// small trim: together with the fixed terms below it is what maps digital full scale to actual volts.
	struct Calibration {
		static constexpr size_t pageSizeInBytes = 512;
		using Page = std::array<std::byte, pageSizeInBytes>;

		static constexpr std::array<int, 8> inputLevelsDBV = { 0, 6, 12, 18, 24, 30, 36, 42 };
		static constexpr std::array<int, 4> outputLevelsDBV = { -12, -2, 8, 18 };

		struct Correction {
			double leftDB;
			double rightDB;
		};
		std::array<Correction, inputLevelsDBV.size()> input;
		std::array<Correction, outputLevelsDBV.size()> output;

		// Peak voltage (between In+ and In-) of a sine that reaches digital full scale, for the given input range.
		// PyQa40x: volts = sample * 10^(correction/20) * 10^((level - 6)/20); the -6 dB is because the ADC input is differential.
		static double InputFullScalePeakVolts(double levelDBV, double correctionDB) {
			return std::pow(10.0, correctionDB / 20.0) * std::pow(10.0, (levelDBV - 6.0) / 20.0);
		}
		// Peak voltage on one output BNC of a sine that reaches digital full scale, for the given output range.
		// PyQa40x: sample = volts * 10^(-(level + 3)/20) * 10^(correction/20).
		static double OutputFullScalePeakVolts(double levelDBV, double correctionDB) {
			return std::pow(10.0, (levelDBV + 3.0 - correctionDB) / 20.0);
		}

		static std::optional<size_t> InputIndex(double levelDBV) { return FindLevel(inputLevelsDBV, levelDBV); }
		static std::optional<size_t> OutputIndex(double levelDBV) { return FindLevel(outputLevelsDBV, levelDBV); }

		// Returns nullopt if the page does not look like calibration data (e.g. the stored levels don't match).
		static std::optional<Calibration> Parse(const Page& page) {
			Calibration calibration;
			if (!ParseRecords(page, 24, inputLevelsDBV, calibration.input)) return std::nullopt;
			if (!ParseRecords(page, 120, outputLevelsDBV, calibration.output)) return std::nullopt;
			return calibration;
		}

	private:
		template <size_t N>
		static std::optional<size_t> FindLevel(const std::array<int, N>& levels, double levelDBV) {
			for (size_t index = 0; index < N; ++index) if (double(levels[index]) == levelDBV) return index;
			return std::nullopt;
		}

		struct Record {
			int16_t levelDBV;
			float correctionDB;
		};
		static Record ReadRecord(const Page& page, size_t offset) {
			// Little endian, as written by PyQa40x's struct.unpack_from('<hf').
			const auto bytes = std::span<const std::byte>(page).subspan(offset, 6);
			Record record;
			record.levelDBV = int16_t(uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8));
			const uint32_t correctionBits = uint32_t(bytes[2]) | (uint32_t(bytes[3]) << 8) | (uint32_t(bytes[4]) << 16) | (uint32_t(bytes[5]) << 24);
			std::memcpy(&record.correctionDB, &correctionBits, sizeof record.correctionDB);
			return record;
		}
		template <size_t N>
		static bool ParseRecords(const Page& page, size_t firstOffset, const std::array<int, N>& levels, std::array<Correction, N>& corrections) {
			for (size_t index = 0; index < N; ++index) {
				const auto offset = firstOffset + index * 12;  // Left record, then right record 6 bytes later
				const auto left = ReadRecord(page, offset), right = ReadRecord(page, offset + 6);
				if (left.levelDBV != levels[index] || right.levelDBV != levels[index]) return false;
				if (!std::isfinite(left.correctionDB) || !std::isfinite(right.correctionDB)) return false;
				if (std::abs(left.correctionDB) > 40 || std::abs(right.correctionDB) > 40) return false;
				corrections[index] = { left.correctionDB, right.correctionDB };
			}
			return true;
		}
	};

}
