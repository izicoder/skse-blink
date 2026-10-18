#include "PCH.h"
#include "Config.h"
#include "Log.h"

namespace BlinkDash
{
	namespace
	{
		// Distance bounds are the menu slider bounds, which are also Blink.cpp's
		// kMaxDistanceCap: anything outside is silently capped at blink time, so it is
		// reported and clamped here instead.
		constexpr float kMinDistance = 100.0f;
		constexpr float kMaxDistance = 2020.0f;
		constexpr float kMaxStaminaCost = 1000.0f;
		constexpr float kMinRetrigger = 0.02f;   // below this a human cannot tap it anyway
		constexpr float kMaxRetrigger = 2.0f;
		constexpr float kMaxCooldown = 5.0f;

		// Same path discovery as before: the JSON sits next to the loaded DLL,
		// so under MO2 it lives inside the mod folder and not the game install.
		std::filesystem::path ResolvePath()
		{
			HMODULE module = nullptr;
			const auto got = ::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&Config::Path),
				&module);
			BD("config: GetModuleHandleExW -> {} handle=0x{:X}", got ? "ok" : "FAILED", reinterpret_cast<std::uintptr_t>(module));

			wchar_t buffer[MAX_PATH * 8]{};
			const auto chars = ::GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
			BD("config: GetModuleFileNameW -> {} chars", chars);

			std::filesystem::path path{ std::wstring_view{ buffer, static_cast<std::size_t>(chars) } };
			path = path.parent_path() / L"BlinkDash.json";
			BD("config: resolved path = {}", path.string());
			return path;
		}

		nlohmann::json ToJson(const Settings& a_settings)
		{
			return nlohmann::json{
				{ "hotkey", a_settings.hotkey },
				{ "distance", a_settings.distance },
				{ "staminaCost", a_settings.staminaCost },
				{ "retriggerDelay", a_settings.retriggerDelay },
				{ "cooldown", a_settings.cooldown },
				{ "effectShader", a_settings.effectShader },
				{ "debugOverlay", a_settings.debugOverlay },
			};
		}
	}

	Settings& Config::Get()
	{
		static Settings settings;
		return settings;
	}

	std::string Config::Describe()
	{
		const auto& s = Get();
		return std::format(
			"hotkey=0x{:X} distance={:.0f} staminaCost={:.0f} retriggerDelay={:.3f}s ({:.0f}ms) cooldown={:.3f}s ({:.0f}ms) effectShader='{}' debugOverlay={}",
			s.hotkey, s.distance, s.staminaCost, s.retriggerDelay, s.retriggerDelay * 1000.0f,
			s.cooldown, s.cooldown * 1000.0f,
			s.effectShader.empty() ? "none" : s.effectShader, s.debugOverlay);
	}

	std::filesystem::path Config::Path()
	{
		static const auto path = ResolvePath();
		return path;
	}

	bool Config::Validate()
	{
		auto& s = Get();
		bool changed = false;

		if (s.hotkey == 0) {
			BDW("config: hotkey 0 is not bindable, restoring default 0x103 (mouse button 4)");
			s.hotkey = 0x103;
			changed = true;
		}
		if (std::isnan(s.distance) || std::isnan(s.staminaCost) || std::isnan(s.retriggerDelay)) {
			BDW("config: NaN found in the settings, resetting everything to defaults");
			s = Settings{};
			changed = true;
		}
		if (s.distance < kMinDistance || s.distance > kMaxDistance) {
			BDW("config: distance {:.1f} out of range, clamping to [{:.1f}, {:.1f}]", s.distance, kMinDistance, kMaxDistance);
			s.distance = std::clamp(s.distance, kMinDistance, kMaxDistance);
			changed = true;
		}
		if (s.staminaCost < 0.0f || s.staminaCost > kMaxStaminaCost) {
			BDW("config: staminaCost {:.1f} out of range, clamping to [0, {:.1f}]", s.staminaCost, kMaxStaminaCost);
			s.staminaCost = std::clamp(s.staminaCost, 0.0f, kMaxStaminaCost);
			changed = true;
		}
		if (s.retriggerDelay < kMinRetrigger || s.retriggerDelay > kMaxRetrigger) {
			BDW("config: retriggerDelay {:.3f}s out of range, clamping to [{:.2f}, {:.2f}]",
				s.retriggerDelay, kMinRetrigger, kMaxRetrigger);
			s.retriggerDelay = std::clamp(s.retriggerDelay, kMinRetrigger, kMaxRetrigger);
			changed = true;
		}
		if (s.cooldown < 0.0f || s.cooldown > kMaxCooldown) {
			BDW("config: cooldown {:.3f}s out of range, clamping to [0, {:.2f}]", s.cooldown, kMaxCooldown);
			s.cooldown = std::clamp(s.cooldown, 0.0f, kMaxCooldown);
			changed = true;
		}
		if (s.effectShader.size() > 64) {
			BDW("config: effectShader name is {} chars, truncating", s.effectShader.size());
			s.effectShader.resize(64);
			changed = true;
		}

		BD("config: validation finished, changed={}", changed);
		return changed;
	}

	void Config::Load()
	{
		BDI("config: Load() begin, path = {}", Path().string());

		const auto path = Path();
		std::error_code ec;
		const bool exists = std::filesystem::exists(path, ec);
		BD("config: exists = {} (error_code='{}')", exists, ec.message());

		if (!exists) {
			BDW("config: no config file, writing defaults");
			Save();
			BDI("config: Load() end (defaults written)");
			return;
		}

		std::string raw;
		try {
			std::ifstream file{ path, std::ios::binary };
			if (!file) {
				BDE("config: could not open the file for reading");
				return;
			}
			raw.assign(std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{});
			BD("config: read {} raw bytes: {}", raw.size(), raw);
		} catch (const std::exception& e) {
			BDE("config: read threw: {}", e.what());
			return;
		}

		try {
			auto json = nlohmann::json::parse(raw);
			if (!json.is_object()) {
				BDE("config: root json is {}, not an object, ignoring the file", json.type_name());
				return;
			}

			auto& s = Get();
			for (const auto& [key, value] : json.items()) {
				if (key == "hotkey" || key == "distance" || key == "staminaCost" || key == "retriggerDelay" || key == "effectShader") {
					BD("config:   {} = {}", key, value.dump());
				} else {
					BDW("config:   ignoring obsolete option '{}' = {}", key, value.dump());
				}
			}

			if (json.contains("hotkey")) {
				s.hotkey = json.at("hotkey").get<std::uint32_t>();
			}
			if (json.contains("distance")) {
				s.distance = json.at("distance").get<float>();
			}
			if (json.contains("staminaCost")) {
				s.staminaCost = json.at("staminaCost").get<float>();
			}
			if (json.contains("retriggerDelay")) {
				s.retriggerDelay = json.at("retriggerDelay").get<float>();
			}
			if (json.contains("cooldown")) {
				s.cooldown = json.at("cooldown").get<float>();
			}
			if (json.contains("effectShader")) {
				s.effectShader = json.at("effectShader").get<std::string>();
			}
			if (json.contains("debugOverlay")) {
				s.debugOverlay = json.at("debugOverlay").get<bool>();
			}

			Validate();
			BDI("config: loaded from {}", path.string());
			BDI("config: active values: {}", Describe());
		} catch (const std::exception& e) {
			BDE("config: parse/apply blew up: {}", e.what());
			BDW("config: keeping the in-memory values: {}", Describe());
		}

		BDI("config: Load() end");
	}

	void Config::Save()
	{
		BDI("config: Save() begin");

		const auto path = Path();
		Validate();

		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		BD("config: create_directories('{}') error_code='{}'", path.parent_path().string(), ec.message());

		const auto payload = ToJson(Get()).dump(4);
		BD("config: saving values: {}", Describe());
		BD("config: json payload: {}", payload);

		try {
			std::ofstream file{ path, std::ios::binary | std::ios::trunc };
			if (!file) {
				BDE("config: could not open the file for writing");
				return;
			}
			file << payload;
			file.flush();
			BD("config: wrote {} bytes, stream good={}", payload.size(), file.good());
			BDI("config: saved to {}", path.string());
		} catch (const std::exception& e) {
			BDE("config: write threw: {}", e.what());
		}

		BDI("config: Save() end");
	}
}
