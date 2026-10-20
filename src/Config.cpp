#include "PCH.h"
#include "Config.h"
#include "Limits.h"
#include "Log.h"

namespace BlinkDash
{
	namespace
	{
		// Guards the settings against the two threads that touch them. Recursive because Load
		// and Save call Validate and Describe, which take it as well; the lock is only held for
		// a copy or a few assignments, so recursion is cheaper than splitting the API.
		std::recursive_mutex s_lock;

		// Distance bounds are the menu slider bounds, which are also the blink's hard cap:
		// anything outside is refused, so it is reported and clamped here instead. Every tunable
		// is an integer: the menu sliders are integer steps, so nothing is lost by storing them
		// exactly. The values themselves live in Limits.h, shared with the menu and the blink.
		using namespace Limits;

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

		// Schema version of BlinkDash.json. Bump this whenever a key changes name, type or
		// meaning, and add exactly one migration step for the jump (see Migrate).
		constexpr int kSettingsFormatVersion = 2;

		// Every key this build understands. Anything else is carried over untouched, so a file
		// written by a newer build survives a downgrade instead of losing its extra options.
		bool IsKnownKey(std::string_view a_key)
		{
			static constexpr std::string_view kKeys[] = {
				"version", "hotkey", "distance", "costAmount", "costResource", "costOutsideCombat",
				"retriggerMs", "cooldownMs", "maxSlope", "effectShader", "requiredPerk",
				"debugOverlay", "logging"
			};
			return std::ranges::find(kKeys, a_key) != std::end(kKeys);
		}

		// Options read from the file that this build does not know. They are merged back on save.
		nlohmann::json s_unknown{ nlohmann::json::object() };

		// Format version of the file that was read. Saving stamps the higher of this and
		// kSettingsFormatVersion: a file from a newer build must never be rewritten as an older
		// format, or the next build would run its migrations over data that already has them.
		int s_fileVersion = kSettingsFormatVersion;

		// The resource is stored as a string: a hand-edited JSON stays readable, and an
		// unknown value can be reported instead of silently turning into stamina.
		std::string CostKey(CostResource a_resource)
		{
			return a_resource == CostResource::kMagicka ? "magicka" : "stamina";
		}

		std::optional<CostResource> CostFromKey(std::string_view a_key)
		{
			if (a_key == "stamina") {
				return CostResource::kStamina;
			}
			if (a_key == "magicka") {
				return CostResource::kMagicka;
			}
			return std::nullopt;
		}

		// Settings are stored as integers, but older configs hold floats ("distance": 200.0,
		// and seconds for the timing values), so every numeric read goes through one rounding
		// path instead of trusting the JSON type. A NaN or infinity is refused rather than
		// cast (which would be undefined behaviour).
		std::optional<int> ReadInt(const nlohmann::json& a_json, const char* a_key)
		{
			if (!a_json.contains(a_key) || !a_json.at(a_key).is_number()) {
				return std::nullopt;
			}
			const double value = a_json.at(a_key).get<double>();
			if (!std::isfinite(value)) {
				BDW("config: '{}' is {}, ignoring it", a_key, a_json.at(a_key).dump());
				return std::nullopt;
			}
			return static_cast<int>(std::lround((std::clamp)(value, -1.0e9, 1.0e9)));
		}

		// A legacy timing value in the old seconds format, converted to the milliseconds the
		// settings use now ("retriggerDelay": 0.2 -> retriggerMs 200).
		std::optional<int> ReadSecondsAsMs(const nlohmann::json& a_json, const char* a_key)
		{
			if (!a_json.contains(a_key) || !a_json.at(a_key).is_number()) {
				return std::nullopt;
			}
			const double value = a_json.at(a_key).get<double>();
			if (!std::isfinite(value)) {
				BDW("config: '{}' is {}, ignoring it", a_key, a_json.at(a_key).dump());
				return std::nullopt;
			}
			return static_cast<int>(std::lround((std::clamp)(value, -1.0e6, 1.0e6) * 1000.0));
		}

		// Typed readers for the non-numeric keys. They exist because json::get<T>() throws on a
		// type mismatch, and the load applies every key inside one try: a single hand-edited
		// value ("hotkey": "mouse4", "logging": "yes") used to abandon every key after it and
		// leave a half-applied config behind. A wrong type now costs that one key, nothing more.
		std::optional<bool> ReadBool(const nlohmann::json& a_json, const char* a_key)
		{
			if (!a_json.contains(a_key)) {
				return std::nullopt;
			}
			const auto& value = a_json.at(a_key);
			if (!value.is_boolean()) {
				BDW("config: '{}' is {}, expected true or false, keeping the default", a_key, value.type_name());
				return std::nullopt;
			}
			return value.get<bool>();
		}

		std::optional<std::string> ReadString(const nlohmann::json& a_json, const char* a_key)
		{
			if (!a_json.contains(a_key)) {
				return std::nullopt;
			}
			const auto& value = a_json.at(a_key);
			if (!value.is_string()) {
				BDW("config: '{}' is {}, expected a string, keeping the default", a_key, value.type_name());
				return std::nullopt;
			}
			return value.get<std::string>();
		}

		// Migration 1 -> 2 (any 1.0.x / 1.1.x file): the cost was a single float amount called
		// staminaCost, and the timing values were floating point seconds. Version 2 stores the
		// cost together with the resource it comes from, all tunables as integers, and the
		// timing values as milliseconds.
		void MigrateV1ToV2(nlohmann::json& a_json)
		{
			if (!a_json.contains("costAmount")) {
				if (const auto value = ReadInt(a_json, "staminaCost")) {
					a_json["costAmount"] = *value;
				}
			}
			if (!a_json.contains("retriggerMs")) {
				if (const auto value = ReadSecondsAsMs(a_json, "retriggerDelay")) {
					a_json["retriggerMs"] = *value;
				}
			}
			if (!a_json.contains("cooldownMs")) {
				if (const auto value = ReadSecondsAsMs(a_json, "cooldown")) {
					a_json["cooldownMs"] = *value;
				}
			}
			a_json.erase("staminaCost");
			a_json.erase("retriggerDelay");
			a_json.erase("cooldown");
		}

		// Walks a document from the version it was written with up to kSettingsFormatVersion.
		// Every later format is one more case here, so an old file converts on launch and the
		// save that follows rewrites it in the current shape.
		void Migrate(nlohmann::json& a_json, int a_fromVersion)
		{
			for (int version = a_fromVersion; version < kSettingsFormatVersion; ++version) {
				switch (version) {
				case 1:
					MigrateV1ToV2(a_json);
					BDW("config: migrated the file from format 1 to 2 (integer settings, millisecond timing, costAmount/costResource)");
					break;
				default:
					BDW("config: no migration from format {}, leaving the file as it is", version);
					return;
				}
			}
		}

		nlohmann::json ToJson(const Settings& a_settings)
		{
			nlohmann::json json = s_unknown;  // keep what this build does not understand...
			json.update(nlohmann::json{
				{ "hotkey", a_settings.hotkey },
				{ "distance", a_settings.distance },
				{ "costAmount", a_settings.costAmount },
				{ "costResource", CostKey(a_settings.costResource) },
				{ "costOutsideCombat", a_settings.costOutsideCombat },
				{ "retriggerMs", a_settings.retriggerMs },
				{ "cooldownMs", a_settings.cooldownMs },
				{ "maxSlope", a_settings.maxSlope },
				{ "effectShader", a_settings.effectShader },
				{ "requiredPerk", a_settings.requiredPerk },
				{ "debugOverlay", a_settings.debugOverlay },
				{ "logging", a_settings.logging },
			});
			json["version"] = s_fileVersion;  // ...and stamp the format we wrote (never lower than what we read)
			return json;
		}
	}

	Settings& Config::Get()
	{
		static Settings settings;
		return settings;
	}

	Settings Config::Snapshot()
	{
		std::scoped_lock lock{ s_lock };
		return Get();
	}

	void Config::SetEffectShader(std::string a_value)
	{
		std::scoped_lock lock{ s_lock };
		Get().effectShader = std::move(a_value);
	}

	void Config::SetRequiredPerk(std::string a_value)
	{
		std::scoped_lock lock{ s_lock };
		Get().requiredPerk = std::move(a_value);
	}

	void Config::ResetToDefaults()
	{
		std::scoped_lock lock{ s_lock };
		Get() = Settings{};
	}

	std::string Config::Describe()
	{
		std::scoped_lock lock{ s_lock };
		const auto& s = Get();
		return std::format(
			"hotkey=0x{:X} distance={} cost={} {} outsideCombat={} retrigger={}ms cooldown={}ms maxSlope={}deg effectShader='{}' requiredPerk='{}' debugOverlay={} logging={}",
			s.hotkey, s.distance, s.costAmount, CostKey(s.costResource), s.costOutsideCombat,
			s.retriggerMs, s.cooldownMs, s.maxSlope,
			s.effectShader.empty() ? "none" : s.effectShader,
			s.requiredPerk.empty() ? "none" : s.requiredPerk, s.debugOverlay, s.logging);
	}

	std::filesystem::path Config::Path()
	{
		static const auto path = ResolvePath();
		return path;
	}

	bool Config::Validate()
	{
		std::scoped_lock lock{ s_lock };
		auto& s = Get();
		bool changed = false;

		// Encoded hotkeys are a scancode, 0x100|mouse button or 0x1000|gamepad index; nothing
		// legal reaches 0x10000, so anything above it is a corrupt or hand-typed value.
		if (s.hotkey == 0 || s.hotkey > 0xFFFF) {
			BDW("config: hotkey 0x{:X} is not bindable, restoring default 0x103 (mouse button 4)", s.hotkey);
			s.hotkey = 0x103;
			changed = true;
		}
		// NaN cannot reach an int: the JSON reads round through ReadInt, which refuses
		// non-finite numbers, so a corrupt file keeps its defaults instead of resetting everything.
		if (s.distance < kMinDistance || s.distance > kMaxDistance) {
			BDW("config: distance {} out of range, clamping to [{}, {}]", s.distance, kMinDistance, kMaxDistance);
			s.distance = std::clamp(s.distance, kMinDistance, kMaxDistance);
			changed = true;
		}
		if (s.costAmount < 0 || s.costAmount > kMaxCost) {
			BDW("config: costAmount {} out of range, clamping to [0, {}]", s.costAmount, kMaxCost);
			s.costAmount = std::clamp(s.costAmount, 0, kMaxCost);
			changed = true;
		}
		if (s.retriggerMs < kMinTapWindowMs || s.retriggerMs > kMaxTapWindowMs) {
			BDW("config: retriggerMs {} out of range, clamping to [{}, {}]", s.retriggerMs, kMinTapWindowMs, kMaxTapWindowMs);
			s.retriggerMs = std::clamp(s.retriggerMs, kMinTapWindowMs, kMaxTapWindowMs);
			changed = true;
		}
		if (s.cooldownMs < 0 || s.cooldownMs > kMaxCooldownMs) {
			BDW("config: cooldownMs {} out of range, clamping to [0, {}]", s.cooldownMs, kMaxCooldownMs);
			s.cooldownMs = std::clamp(s.cooldownMs, 0, kMaxCooldownMs);
			changed = true;
		}
		if (s.maxSlope < kMinSlopeDegrees || s.maxSlope > kMaxSlopeDegrees) {
			BDW("config: maxSlope {} out of range, clamping to [{}, {}]", s.maxSlope, kMinSlopeDegrees, kMaxSlopeDegrees);
			s.maxSlope = std::clamp(s.maxSlope, kMinSlopeDegrees, kMaxSlopeDegrees);
			changed = true;
		}
		if (s.effectShader.size() > 64) {
			BDW("config: effectShader name is {} chars, truncating", s.effectShader.size());
			s.effectShader.resize(64);
			changed = true;
		}
		if (s.requiredPerk.size() > 64) {
			BDW("config: requiredPerk name is {} chars, truncating", s.requiredPerk.size());
			s.requiredPerk.resize(64);
			changed = true;
		}

		BD("config: validation finished, changed={}", changed);
		return changed;
	}

	void Config::Load()
	{
		std::scoped_lock lock{ s_lock };
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

			// Format version first: a file without one predates versioning and is treated as 1.
			int fileVersion = 1;
			if (const auto value = ReadInt(json, "version")) {
				fileVersion = *value;
			}
			BD("config: file format version = {} (this build writes {})", fileVersion, kSettingsFormatVersion);
			if (fileVersion > kSettingsFormatVersion) {
				BDW("config: the file was written by a newer build; its unknown options are kept as they are");
			} else {
				Migrate(json, fileVersion);
			}
			// Never stamp a newer file down: the UI saves on every menu close, so a downgrade
			// plus one menu visit used to rewrite a v3 file as v2, and the next upgrade then
			// migrated data that was already migrated.
			s_fileVersion = (std::max)(fileVersion, kSettingsFormatVersion);

			// Everything this build knows is logged and applied below; anything else is kept
			// aside so saving does not silently drop an option from another build.
			s_unknown = nlohmann::json::object();
			for (const auto& [key, value] : json.items()) {
				if (IsKnownKey(key)) {
					BD("config:   {} = {}", key, value.dump());
				} else {
					s_unknown[key] = value;
					BDW("config:   keeping unknown option '{}' = {}", key, value.dump());
				}
			}

			if (const auto value = ReadInt(json, "hotkey")) {
				// Validate() rejects anything outside the encodable range, so a negative or
				// absurd number cannot survive as a hotkey that nothing can ever press.
				s.hotkey = static_cast<std::uint32_t>(*value);
			}
			if (const auto value = ReadInt(json, "distance")) {
				s.distance = *value;
			}
			if (const auto value = ReadInt(json, "costAmount")) {
				s.costAmount = *value;
			} else if (const auto legacy = ReadInt(json, "staminaCost")) {
				// Pre-1.1 key: the amount used to be stamina-only, so it still means the same
				// number. Reading it keeps a hand-tuned cost alive when the new build is
				// dropped over an old config; the next save rewrites the file with the new keys.
				s.costAmount = *legacy;
				BDW("config: legacy key 'staminaCost' = {} read as costAmount", s.costAmount);
			}
			if (const auto key = ReadString(json, "costResource")) {
				if (const auto parsed = CostFromKey(*key)) {
					s.costResource = *parsed;
				} else {
					BDW("config: unknown costResource '{}', keeping '{}'", *key, CostKey(s.costResource));
				}
			}
			if (const auto value = ReadBool(json, "costOutsideCombat")) {
				s.costOutsideCombat = *value;
			}
			if (const auto value = ReadInt(json, "retriggerMs")) {
				s.retriggerMs = *value;
			} else if (const auto legacy = ReadSecondsAsMs(json, "retriggerDelay")) {
				// Pre-1.2 key in seconds: 0.2 -> 200 ms.
				s.retriggerMs = *legacy;
				BDW("config: legacy key 'retriggerDelay' read as retriggerMs={}", s.retriggerMs);
			}
			if (const auto value = ReadInt(json, "cooldownMs")) {
				s.cooldownMs = *value;
			} else if (const auto legacy = ReadSecondsAsMs(json, "cooldown")) {
				// Pre-1.2 key in seconds: 1.0 -> 1000 ms.
				s.cooldownMs = *legacy;
				BDW("config: legacy key 'cooldown' read as cooldownMs={}", s.cooldownMs);
			}
			if (const auto value = ReadInt(json, "maxSlope")) {
				s.maxSlope = *value;
			}
			if (const auto value = ReadString(json, "effectShader")) {
				s.effectShader = *value;
			}
			if (const auto value = ReadString(json, "requiredPerk")) {
				s.requiredPerk = *value;
			}
			if (const auto value = ReadBool(json, "debugOverlay")) {
				s.debugOverlay = *value;
			}
			if (const auto value = ReadBool(json, "logging")) {
				s.logging = *value;
			}

			const bool corrected = Validate();
			Log::SetEnabled(s.logging);
			BDI("config: loaded from {}", path.string());
			BDI("config: active values: {}", Describe());

			// Rewrite in the current format straight away when the file was migrated or
			// repaired, so every launch after this one reads a file that needs neither. A file
			// from a *newer* build is left alone: it is read as far as this build understands it
			// and must not be stamped back to an older version.
			if (fileVersion < kSettingsFormatVersion || corrected) {
				BDI("config: rewriting the file in format {}", kSettingsFormatVersion);
				Save();
			}
		} catch (const std::exception& e) {
			BDE("config: parse/apply blew up: {}", e.what());
			BDW("config: keeping the in-memory values: {}", Describe());
		}

		BDI("config: Load() end");
	}

	void Config::Save()
	{
		std::scoped_lock lock{ s_lock };
		BDI("config: Save() begin");

		const auto path = Path();
		Validate();

		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		BD("config: create_directories('{}') error_code='{}'", path.parent_path().string(), ec.message());

		const auto payload = ToJson(Get()).dump(4);
		BD("config: saving values: {}", Describe());
		BD("config: json payload: {}", payload);

		// Written to a sibling file and moved over the real one. Truncating the live file
		// first means a crash or a power loss mid-write leaves invalid JSON, which Load cannot
		// parse - the user then silently runs on defaults, and the file is never rewritten.
		auto temp = path;
		temp += L".tmp";

		try {
			{
				std::ofstream file{ temp, std::ios::binary | std::ios::trunc };
				if (!file) {
					BDE("config: could not open '{}' for writing", temp.string());
					return;
				}
				file << payload;
				file.flush();
				if (!file.good()) {
					BDE("config: writing '{}' failed, the old config is untouched", temp.string());
					return;
				}
			}  // closed before the move: Windows will not replace an open file

			// MoveFileExW replaces the destination atomically on NTFS; std::filesystem::rename
			// is allowed to refuse an existing target on Windows, so it is not used here.
			if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				BDE("config: could not replace '{}' (error {}), the old config is untouched",
					path.string(), ::GetLastError());
				return;
			}
			BD("config: wrote {} bytes", payload.size());
			BDI("config: saved to {}", path.string());
		} catch (const std::exception& e) {
			BDE("config: write threw: {}", e.what());
		}

		BDI("config: Save() end");
	}
}
