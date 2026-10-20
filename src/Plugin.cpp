#include "PCH.h"
#include "Version.h"
#include "Blink.h"
#include "Config.h"
#include "Debug.h"
#include "Effects.h"
#include "Input.h"
#include "Log.h"
#include "Perks.h"
#include "UI.h"

namespace
{
	std::atomic<bool> s_configLoaded{ false };
	std::atomic<bool> s_effectsInit{ false };
	std::atomic<bool> s_perksInit{ false };
	std::atomic<bool> s_inputRegistered{ false };
	std::atomic<bool> s_uiRegistered{ false };

	const char* MessageName(std::uint32_t a_type)
	{
		using MI = SKSE::MessagingInterface;
		switch (a_type) {
		case MI::kPostLoad:     return "kPostLoad";
		case MI::kPostPostLoad: return "kPostPostLoad";
		case MI::kPreLoadGame:  return "kPreLoadGame";
		case MI::kPostLoadGame: return "kPostLoadGame";
		case MI::kSaveGame:     return "kSaveGame";
		case MI::kDeleteGame:   return "kDeleteGame";
		case MI::kInputLoaded:  return "kInputLoaded";
		case MI::kNewGame:      return "kNewGame";
		case MI::kDataLoaded:   return "kDataLoaded";
		default:                return "unknown";
		}
	}

	void MessageHandler(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			BDE("msg: received a null message, ignoring");
			return;
		}

		BDI("msg: type={} ({}) sender={} dataLen={} data=0x{:X}",
			a_message->type,
			MessageName(a_message->type),
			a_message->sender ? a_message->sender : "<null>",
			a_message->dataLen,
			reinterpret_cast<std::uintptr_t>(a_message->data));

		try {
			switch (a_message->type) {
			case SKSE::MessagingInterface::kDataLoaded:
				{
					BDI("msg: kDataLoaded - bringing the mod online");
					if (!s_configLoaded.exchange(true)) {
						BlinkDash::Config::Load();
					} else {
						BD("msg: config was already loaded, not loading twice");
					}
					if (!s_effectsInit.exchange(true)) {
						BlinkDash::Effects::Init();
					} else {
						BD("msg: effects were already initialised, not doing it twice");
					}
					if (!s_perksInit.exchange(true)) {
						BlinkDash::Perks::Init();
					} else {
						BD("msg: perks were already initialised, not doing it twice");
					}
					if (!s_inputRegistered.exchange(true)) {
						BlinkDash::Input::Register();
					} else {
						BD("msg: input sink was already registered, not registering twice");
					}
					if (!s_uiRegistered.exchange(true)) {
						BlinkDash::UI::Register();
					} else {
						BD("msg: menu was already registered, not registering twice");
					}
					BDI("msg: kDataLoaded complete, BlinkDash v{} is live ({} effect shaders, {} perks in this load order)",
						BLINKDASH_VERSION, BlinkDash::Effects::Entries().size(), BlinkDash::Perks::Entries().size());
				}
				break;
			case SKSE::MessagingInterface::kPostLoad:
				BD("msg: kPostLoad - all SKSE plugins have loaded");
				break;
			case SKSE::MessagingInterface::kPostPostLoad:
				BD("msg: kPostPostLoad - post load pass finished");
				break;
			case SKSE::MessagingInterface::kInputLoaded:
				BD("msg: kInputLoaded - input devices are up");
				break;
			case SKSE::MessagingInterface::kNewGame:
				BD("msg: kNewGame - a new game is starting");
				break;
			case SKSE::MessagingInterface::kPreLoadGame:
				BD("msg: kPreLoadGame - a save is about to load");
				// Nothing held now will get a release event across the load, and the last
				// blink's geometry belongs to the save that is being left behind.
				BlinkDash::Input::State::Clear();
				BlinkDash::Debug::Clear();
				break;
			case SKSE::MessagingInterface::kPostLoadGame:
				BD("msg: kPostLoadGame - a save finished loading");
				BlinkDash::Input::State::Clear();
				break;
			case SKSE::MessagingInterface::kSaveGame:
				BD("msg: kSaveGame - the game is saving");
				break;
			case SKSE::MessagingInterface::kDeleteGame:
				BD("msg: kDeleteGame - a save was deleted");
				break;
			default:
				BD("msg: no specific handler for message type {}", a_message->type);
				break;
			}
		} catch (const std::exception& e) {
			BDE("msg: handler for type {} threw: {}", a_message->type, e.what());
		} catch (...) {
			BDE("msg: handler for type {} threw a non-std exception", a_message->type);
		}

		BD("msg: handler for type {} finished", a_message->type);
	}
}

extern "C" DLLEXPORT bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	// Checked before SKSE::Init: the library dereferences the interface, so a null one would
	// crash before the plugin could say a word about it.
	if (!a_skse) {
		return false;
	}

	SKSE::Init(a_skse);

	// From here on every single line lands in SKSE/BlinkDash.log and is flushed.
	BlinkDash::Log::Init();

	BDI("=== SKSEPlugin_Load begin ===");
	BD("load interface pointer = 0x{:X}", reinterpret_cast<std::uintptr_t>(a_skse));

	BlinkDash::Log::Banner();

	BD("querying messaging interface");
	const auto messaging = SKSE::GetMessagingInterface();
	BD("messaging interface = 0x{:X}", reinterpret_cast<std::uintptr_t>(messaging));
	if (!messaging) {
		BDE("no messaging interface available, refusing to continue");
		return false;
	}

	messaging->RegisterListener(MessageHandler);
	BDI("messaging listener registered, SKSEPlugin_Load returning true");
	return true;
}
