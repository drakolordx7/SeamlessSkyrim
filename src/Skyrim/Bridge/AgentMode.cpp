#include "Skyrim/Bridge/AgentMode.h"

#include <Windows.h>
#include <deque>

namespace
{
	decltype(&::SetForegroundWindow) originalSetForegroundWindow = nullptr;
	decltype(&::SetFocus)            originalSetFocus = nullptr;
	decltype(&::ShowWindow)          originalShowWindow = nullptr;
	decltype(&::SetWindowPos)        originalSetWindowPos = nullptr;
	decltype(&::CreateWindowExW)     originalCreateWindowExW = nullptr;
	decltype(&::CreateWindowExA)     originalCreateWindowExA = nullptr;

	BOOL WINAPI GuardedSetForegroundWindow(HWND)
	{
		return TRUE;  // pretend it worked; the game doesn't need to be in front to run
	}

	HWND WINAPI GuardedSetFocus(HWND a_window)
	{
		// SetFocus on a top-level window activates it. Report the window as focused instead.
		return a_window;
	}

	int NonActivating(int a_command)
	{
		switch (a_command) {
		case SW_SHOWNORMAL:
		case SW_RESTORE:
		case SW_SHOWDEFAULT:
			return SW_SHOWNOACTIVATE;
		case SW_SHOW:
			return SW_SHOWNA;
		default:
			return a_command;
		}
	}

	BOOL WINAPI GuardedShowWindow(HWND a_window, int a_command)
	{
		return originalShowWindow(a_window, NonActivating(a_command));
	}

	BOOL WINAPI GuardedSetWindowPos(HWND a_window, HWND a_after, int a_x, int a_y, int a_cx, int a_cy, UINT a_flags)
	{
		return originalSetWindowPos(a_window, a_after, a_x, a_y, a_cx, a_cy, a_flags | SWP_NOACTIVATE);
	}

	// A top-level window created with WS_VISIBLE is shown *and activated*; create it hidden and
	// show it without activation instead.
	template <class Char, class Original>
	HWND CreateHidden(Original a_original, DWORD a_exStyle, const Char* a_class, const Char* a_name, DWORD a_style, int a_x, int a_y, int a_w, int a_h, HWND a_parent, HMENU a_menu, HINSTANCE a_instance, LPVOID a_param)
	{
		const bool topLevelVisible = !a_parent && (a_style & WS_VISIBLE);
		const HWND window = a_original(a_exStyle, a_class, a_name, topLevelVisible ? a_style & ~WS_VISIBLE : a_style, a_x, a_y, a_w, a_h, a_parent, a_menu, a_instance, a_param);
		if (window && topLevelVisible) {
			originalShowWindow(window, SW_SHOWNA);
		}
		return window;
	}

	HWND WINAPI GuardedCreateWindowExW(DWORD a_exStyle, LPCWSTR a_class, LPCWSTR a_name, DWORD a_style, int a_x, int a_y, int a_w, int a_h, HWND a_parent, HMENU a_menu, HINSTANCE a_instance, LPVOID a_param)
	{
		return CreateHidden(originalCreateWindowExW, a_exStyle, a_class, a_name, a_style, a_x, a_y, a_w, a_h, a_parent, a_menu, a_instance, a_param);
	}

	HWND WINAPI GuardedCreateWindowExA(DWORD a_exStyle, LPCSTR a_class, LPCSTR a_name, DWORD a_style, int a_x, int a_y, int a_w, int a_h, HWND a_parent, HMENU a_menu, HINSTANCE a_instance, LPVOID a_param)
	{
		return CreateHidden(originalCreateWindowExA, a_exStyle, a_class, a_name, a_style, a_x, a_y, a_w, a_h, a_parent, a_menu, a_instance, a_param);
	}

	template <class F>
	void Patch(F& a_original, F a_replacement, const char* a_function)
	{
		// Chains with any earlier IAT hook (other mods patch window creation too).
		const auto previous = SKSE::PatchIAT(a_replacement, "user32.dll", a_function);
		a_original = previous ? reinterpret_cast<F>(previous) : reinterpret_cast<F>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), a_function));
		logger::info("agent mode: guarded {} ({})", a_function, previous ? "IAT" : "not imported, resolved directly");
	}
}

namespace AgentMode
{
	namespace
	{
		bool EnvFlag(const char* a_name)
		{
			char       value[8]{};
			const auto length = ::GetEnvironmentVariableA(a_name, value, sizeof(value));
			return length > 0 && value[0] == '1';
		}
	}

	bool Enabled()
	{
		static const bool enabled = EnvFlag("SEAMLESS_AGENT");
		return enabled;
	}

	bool Background()
	{
		static const bool background = EnvFlag("SEAMLESS_AGENT_BACKGROUND");
		return background;
	}

	void InstallFocusGuards()
	{
		if (!Background()) {
			return;
		}
		Patch(originalShowWindow, &GuardedShowWindow, "ShowWindow");
		Patch(originalSetForegroundWindow, &GuardedSetForegroundWindow, "SetForegroundWindow");
		Patch(originalSetFocus, &GuardedSetFocus, "SetFocus");
		Patch(originalSetWindowPos, &GuardedSetWindowPos, "SetWindowPos");
		Patch(originalCreateWindowExW, &GuardedCreateWindowExW, "CreateWindowExW");
		Patch(originalCreateWindowExA, &GuardedCreateWindowExA, "CreateWindowExA");
	}

	namespace
	{
		bool GameHasFocus()
		{
			DWORD process = 0;
			::GetWindowThreadProcessId(::GetForegroundWindow(), &process);
			return process == ::GetCurrentProcessId();
		}

		// Keys from the bridge: each entry a key going down or up, one per keyboard read, so the
		// game sees the press and the release in different frames.
		std::mutex                                    keysLock;
		std::deque<std::pair<std::uint32_t, bool>>    keys;  // under keysLock

		void InjectKey()
		{
			std::pair<std::uint32_t, bool> key;
			{
				std::scoped_lock guard(keysLock);
				if (keys.empty()) {
					return;
				}
				key = keys.front();
				keys.pop_front();
			}
			// The user event the key stands for where input goes now (a message box's "Accept" for
			// E): the topmost input context that maps it. The topmost alone (the cursor's, over a
			// message box) maps no keys at all, and without a user event menus ignore the key.
			auto*            controls = RE::ControlMap::GetSingleton();
			const auto&      stack = controls->GetRuntimeData().contextPriorityStack;
			std::string_view name;
			// Not ControlMap::GetUserEventName: it gives nothing for a key mapped twice in a context.
			for (auto i = stack.size(); i > 0 && name.empty(); --i) {
				const auto* context = controls->controlMap[stack[i - 1]];
				if (!context) {
					continue;
				}
				for (const auto& mapping : context->deviceMappings[RE::INPUT_DEVICE::kKeyboard]) {
					if (mapping.inputKey == key.first) {
						name = mapping.eventID;
						break;
					}
				}
			}
			const RE::BSFixedString userEvent(name);
			RE::BSInputEventQueue::GetSingleton()->AddButtonEvent(RE::INPUT_DEVICE::kKeyboard, static_cast<std::int32_t>(key.first), key.second ? 1.0f : 0.0f, key.second ? 0.0f : 0.1f, userEvent);
		}

		template <class Device>
		struct PollHook
		{
			static void Thunk(Device* a_device, float a_delta)
			{
				if (!Background() || GameHasFocus()) {
					func(a_device, a_delta);
				}
				if constexpr (std::is_same_v<Device, RE::BSWin32KeyboardDevice>) {
					InjectKey();
				}
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};
	}

	void QueueKey(std::uint32_t a_scanCode, int a_repeat)
	{
		std::scoped_lock guard(keysLock);
		for (int i = 0; i < a_repeat; ++i) {
			keys.emplace_back(a_scanCode, true);
			keys.emplace_back(a_scanCode, false);
		}
	}

	void InstallInputGuards()
	{
		// With bAlwaysActive the game keeps reading keyboard and mouse while it sits unfocused on
		// its own desktop: whatever the user typed elsewhere opened the inventory and the map,
		// walked the player into doors and paused test runs. Only read input while the game window
		// really is in front (the MCP server focuses it for its own key presses).
		REL::Relocation<std::uintptr_t> keyboard{ RE::VTABLE_BSWin32KeyboardDevice[0] };
		PollHook<RE::BSWin32KeyboardDevice>::func = keyboard.write_vfunc(0x2, PollHook<RE::BSWin32KeyboardDevice>::Thunk);
		REL::Relocation<std::uintptr_t> mouse{ RE::VTABLE_BSWin32MouseDevice[0] };
		PollHook<RE::BSWin32MouseDevice>::func = mouse.write_vfunc(0x2, PollHook<RE::BSWin32MouseDevice>::Thunk);
		logger::info("agent mode: bridge keys injected{}", Background() ? "; other input only while focused" : "");
	}

	namespace
	{
		class AutoLoadSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			std::string save;

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME && !save.empty()) {
					logger::info("agent mode: loading '{}' at the main menu", save);
					RE::BGSSaveLoadManager::GetSingleton()->Load(std::exchange(save, std::string()).c_str());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} autoLoad;
	}

	void InstallAutoLoad()
	{
		char value[260]{};
		if (!Enabled() || ::GetEnvironmentVariableA("SEAMLESS_AGENT_LOAD", value, sizeof(value)) == 0 || !value[0]) {
			return;
		}
		autoLoad.save = value;
		auto* ui = RE::UI::GetSingleton();
		// The main menu is usually up already when the game data has finished loading (kDataLoaded):
		// its opening was missed, and the game sat in it. Loaded on the next frame then.
		if (ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
			SKSE::GetTaskInterface()->AddTask([] {
				if (!autoLoad.save.empty()) {
					logger::info("agent mode: loading '{}' (main menu already up)", autoLoad.save);
					RE::BGSSaveLoadManager::GetSingleton()->Load(std::exchange(autoLoad.save, std::string()).c_str());
				}
			});
		} else if (ui) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&autoLoad);
		}
	}
}

namespace AgentMode
{
	namespace
	{
		std::atomic<bool> muted = false;
		// The volume the game set before the mute, given back on unmute.
		float audibleVolume = 1.0f;

		RE::IXAudio2Voice* MasteringVoice(RE::BSXAudio2Audio* a_audio)
		{
			// IXAudio2MasteringVoice adds nothing that is needed here to IXAudio2Voice.
			return reinterpret_cast<RE::IXAudio2Voice*>(a_audio->masteringVoice);
		}

		void ApplyMute(RE::BSXAudio2Audio* a_audio)
		{
			MasteringVoice(a_audio)->SetVolume(muted ? 0.0f : audibleVolume);
		}

		struct AudioInitHook
		{
			static bool Thunk(RE::BSXAudio2Audio* a_audio, REX::W32::HWND* a_window)
			{
				const bool initialized = func(a_audio, a_window);
				// No audio device: the game runs without sound and has no mastering voice.
				if (initialized) {
					MasteringVoice(a_audio)->GetVolume(&audibleVolume);
					ApplyMute(a_audio);
					logger::info("agent mode: audio {}", muted ? "muted" : "on");
				}
				return initialized;
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};
	}

	void InstallMute()
	{
		if (!Enabled()) {
			return;
		}
		muted = EnvFlag("SEAMLESS_AGENT_MUTE");
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSXAudio2Audio[0] };
		AudioInitHook::func = vtable.write_vfunc(0x1, AudioInitHook::Thunk);
	}

	void SetMuted(bool a_muted)
	{
		muted = a_muted;
		ApplyMute(RE::BSXAudio2GameSound::GetAudioImplementation());
	}

	bool Muted()
	{
		return muted;
	}
}
