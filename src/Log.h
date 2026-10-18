#pragma once

#include "PCH.h"
#include "Version.h"

// trace: the workhorse - one of these on every single step
#define BD(...)  ::BlinkDash::Log::Write(spdlog::level::trace, __FILE__, __LINE__, __VA_ARGS__)
// info: milestones that must stay visible even if a level is misconfigured
#define BDI(...) ::BlinkDash::Log::Write(spdlog::level::info, __FILE__, __LINE__, __VA_ARGS__)
#define BDW(...) ::BlinkDash::Log::Write(spdlog::level::warn, __FILE__, __LINE__, __VA_ARGS__)
#define BDE(...) ::BlinkDash::Log::Write(spdlog::level::err, __FILE__, __LINE__, __VA_ARGS__)

// Every log line in this plugin goes through these helpers so that:
//   * the level is forced to trace at load (so nothing is filtered out)
//   * the file sink is flushed after every single line, so the log always ends
//     with the last line that ran before a crash
//   * a bad format string can never take the game down
namespace BlinkDash::Log
{
	inline std::string ModulePath()
	{
		HMODULE module = nullptr;
		::GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&ModulePath),
			&module);

		wchar_t buffer[MAX_PATH * 8]{};
		const auto chars = ::GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
		if (chars == 0) {
			return "<unknown module path>";
		}
		return std::filesystem::path{ std::wstring_view{ buffer, chars } }.string();
	}

	inline std::string ExePath()
	{
		wchar_t buffer[MAX_PATH * 8]{};
		const auto chars = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
		if (chars == 0) {
			return "<unknown>";
		}
		return std::filesystem::path{ std::wstring_view{ buffer, chars } }.string();
	}

	inline std::string ThreadId()
	{
		return std::to_string(::GetCurrentThreadId());
	}

	inline void WriteLine(spdlog::level::level_enum a_level, const char* a_file, int a_line, std::string_view a_text)
	{
		try {
			spdlog::log(spdlog::source_loc{ a_file, a_line, "" }, a_level, "{}", a_text);
		} catch (...) {
		}
		try {
			if (auto logger = spdlog::default_logger()) {
				logger->flush();
			}
		} catch (...) {
		}
	}

	template <class... Args>
	void Write(spdlog::level::level_enum a_level, const char* a_file, int a_line, spdlog::format_string_t<Args...> a_fmt, Args&&... a_args)
	{
		try {
			spdlog::log(
				spdlog::source_loc{ a_file, a_line, "" },
				a_level,
				a_fmt,
				std::forward<Args>(a_args)...);
		} catch (...) {
			// a logging failure must never escalate into a game crash
		}
		try {
			if (auto logger = spdlog::default_logger()) {
				logger->flush();
			}
		} catch (...) {
		}
	}

	// Forced right after SKSE::Init so that absolutely everything is captured.
	inline void Init()
	{
		try {
			auto logger = spdlog::default_logger();
			if (logger) {
				logger->set_level(spdlog::level::trace);
				logger->flush_on(spdlog::level::trace);
			}
			spdlog::set_level(spdlog::level::trace);
			spdlog::flush_on(spdlog::level::trace);
		} catch (...) {
		}

		BDI("======================================================================");
		BDI("BlinkDash log level forced to TRACE, flushing after every line");
		BDI("plugin version   : {}", BLINKDASH_VERSION);
		BDI("dll path         : {}", ModulePath());
		BDI("process id       : {}", ::GetCurrentProcessId());
		BDI("thread id        : {}", ThreadId());
		BDI("game exe         : {}", ExePath());
		BDI("======================================================================");
	}

	// The logging setting: off silences the file completely, on restores the trace level
	// and the per-line flush. The plugin still runs either way.
	inline void SetEnabled(bool a_enabled)
	{
		const auto level = a_enabled ? spdlog::level::trace : spdlog::level::off;
		try {
			if (auto logger = spdlog::default_logger()) {
				logger->set_level(level);
				logger->flush_on(level);
			}
			spdlog::set_level(level);
			spdlog::flush_on(level);
		} catch (...) {
		}
	}

	inline void Banner()
	{
		BDI("BlinkDash boot sequence starting");
	}
}
