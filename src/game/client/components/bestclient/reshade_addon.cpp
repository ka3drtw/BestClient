#if defined(_WIN32)

#include <reshade.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>

extern "C" __declspec(dllexport) const char *NAME = "BestClient ReShade Live Sync";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Applies BestClient ReShade state changes to the current runtime live.";
extern "C" __declspec(dllexport) const char *AUTHOR = "BestClient";

namespace
{
constexpr const char *EFFECT_NAME = "DeepFry.fx";
constexpr const char *TECHNIQUE_NAME = "DeepFry";
constexpr const char *UNIFORM_QUALITY = "Quality";
constexpr const char *UNIFORM_REDS = "Reds";
constexpr const char *TECHNIQUE_TOKEN = "DeepFry@DeepFry.fx";

struct SDeepFryState
{
	bool m_Enabled = false;
	float m_Quality = 1.0f;
	float m_Reds = 0.0f;
};

struct SRuntimeState
{
	std::filesystem::path m_PresetPath;
	std::filesystem::file_time_type m_LastWriteTime = {};
	bool m_HasWriteTime = false;
	SDeepFryState m_LastAppliedState;
	bool m_HasAppliedState = false;
};

std::unordered_map<reshade::api::effect_runtime *, SRuntimeState> gs_RuntimeStates;

void LogInfo(const char *pMessage)
{
	reshade::log::message(reshade::log::level::info, pMessage);
}

void LogWarning(const char *pMessage)
{
	reshade::log::message(reshade::log::level::warning, pMessage);
}

bool QueryWriteTime(const std::filesystem::path &Path, std::filesystem::file_time_type &WriteTime)
{
	std::error_code Error;
	WriteTime = std::filesystem::last_write_time(Path, Error);
	return !Error;
}

std::string Trim(std::string Text)
{
	const auto IsWhitespace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while(!Text.empty() && IsWhitespace((unsigned char)Text.front()))
		Text.erase(Text.begin());
	while(!Text.empty() && IsWhitespace((unsigned char)Text.back()))
		Text.pop_back();
	return Text;
}

std::optional<std::string> ReadTextFile(const std::filesystem::path &Path)
{
	std::ifstream File(Path, std::ios::in | std::ios::binary);
	if(!File.is_open())
		return std::nullopt;

	std::ostringstream Buffer;
	Buffer << File.rdbuf();
	return Buffer.str();
}

bool TryParseFloat(const std::string &Text, float &Value)
{
	char *pEnd = nullptr;
	Value = std::strtof(Text.c_str(), &pEnd);
	return pEnd != Text.c_str() && pEnd != nullptr && *pEnd == '\0';
}

bool ParseDeepFryState(const std::filesystem::path &PresetPath, SDeepFryState &State)
{
	const std::optional<std::string> Text = ReadTextFile(PresetPath);
	if(!Text.has_value())
		return false;

	State = SDeepFryState{};

	std::istringstream Stream(*Text);
	std::string Line;
	bool InDeepFrySection = false;

	while(std::getline(Stream, Line))
	{
		const std::string TrimmedLine = Trim(Line);
		if(TrimmedLine.empty() || TrimmedLine[0] == ';' || TrimmedLine[0] == '#')
			continue;

		if(TrimmedLine.front() == '[' && TrimmedLine.back() == ']')
		{
			const std::string SectionName = TrimmedLine.substr(1, TrimmedLine.size() - 2);
			InDeepFrySection = SectionName == EFFECT_NAME;
			continue;
		}

		const size_t EqualsPos = TrimmedLine.find('=');
		if(EqualsPos == std::string::npos)
			continue;

		const std::string Key = Trim(TrimmedLine.substr(0, EqualsPos));
		const std::string Value = Trim(TrimmedLine.substr(EqualsPos + 1));

		if(!InDeepFrySection)
		{
			if(Key == "Techniques")
				State.m_Enabled = Value.find(TECHNIQUE_TOKEN) != std::string::npos;
			continue;
		}

		if(Key == UNIFORM_QUALITY)
			TryParseFloat(Value, State.m_Quality);
		else if(Key == UNIFORM_REDS)
			TryParseFloat(Value, State.m_Reds);
	}

	return true;
}

bool FindDeepFryHandles(
	reshade::api::effect_runtime *pRuntime,
	reshade::api::effect_technique &Technique,
	reshade::api::effect_uniform_variable &QualityUniform,
	reshade::api::effect_uniform_variable &RedsUniform,
	std::string &EffectName)
{
	Technique = pRuntime->find_technique(nullptr, TECHNIQUE_NAME);
	if(Technique == 0)
		return false;

	char aEffectName[512] = {0};
	pRuntime->get_technique_effect_name(Technique, aEffectName);
	EffectName = aEffectName;
	if(EffectName.empty())
		EffectName = EFFECT_NAME;

	QualityUniform = pRuntime->find_uniform_variable(EffectName.c_str(), UNIFORM_QUALITY);
	RedsUniform = pRuntime->find_uniform_variable(EffectName.c_str(), UNIFORM_REDS);
	return QualityUniform != 0 && RedsUniform != 0;
}

bool ApplyDeepFryState(reshade::api::effect_runtime *pRuntime, const std::filesystem::path &PresetPath, SRuntimeState &RuntimeState)
{
	SDeepFryState NewState;
	if(!ParseDeepFryState(PresetPath, NewState))
	{
		char aMessage[4608];
		std::snprintf(aMessage, sizeof(aMessage), "[BestClient/ReShadeAddon] Failed to read preset state from: %s", PresetPath.string().c_str());
		LogWarning(aMessage);
		return false;
	}

	reshade::api::effect_technique Technique = {0};
	reshade::api::effect_uniform_variable QualityUniform = {0};
	reshade::api::effect_uniform_variable RedsUniform = {0};
	std::string EffectName;
	if(!FindDeepFryHandles(pRuntime, Technique, QualityUniform, RedsUniform, EffectName))
	{
		char aMessage[4608];
		std::snprintf(aMessage, sizeof(aMessage), "[BestClient/ReShadeAddon] DeepFry technique or uniforms are not available yet. Waiting for effect compilation.");
		LogWarning(aMessage);
		return false;
	}

	pRuntime->set_uniform_value_float(QualityUniform, &NewState.m_Quality, 1);
	pRuntime->set_uniform_value_float(RedsUniform, &NewState.m_Reds, 1);
	pRuntime->set_technique_state(Technique, NewState.m_Enabled);

	RuntimeState.m_LastAppliedState = NewState;
	RuntimeState.m_HasAppliedState = true;

	char aMessage[4608];
	std::snprintf(
		aMessage, sizeof(aMessage),
		"[BestClient/ReShadeAddon] Applied DeepFry live state from '%s': enabled=%d quality=%.6f reds=%.6f",
		EffectName.c_str(), NewState.m_Enabled ? 1 : 0, NewState.m_Quality, NewState.m_Reds);
	LogInfo(aMessage);
	return true;
}

void RefreshPresetPath(reshade::api::effect_runtime *pRuntime, SRuntimeState &State)
{
	char aPresetPath[4096];
	pRuntime->get_current_preset_path(aPresetPath);
	State.m_PresetPath = std::filesystem::u8path(aPresetPath);
}

void OnInitEffectRuntime(reshade::api::effect_runtime *pRuntime)
{
	SRuntimeState &State = gs_RuntimeStates[pRuntime];
	RefreshPresetPath(pRuntime, State);
	State.m_HasWriteTime = QueryWriteTime(State.m_PresetPath, State.m_LastWriteTime);
	const std::string PresetPathString = State.m_PresetPath.string();

	char aMessage[4608];
	std::snprintf(aMessage, sizeof(aMessage), "[BestClient/ReShadeAddon] Runtime initialized. Watching preset: %s", PresetPathString.c_str());
	LogInfo(aMessage);
	ApplyDeepFryState(pRuntime, State.m_PresetPath, State);
}

void OnDestroyEffectRuntime(reshade::api::effect_runtime *pRuntime)
{
	gs_RuntimeStates.erase(pRuntime);
	LogInfo("[BestClient/ReShadeAddon] Runtime destroyed.");
}

void OnPresent(reshade::api::effect_runtime *pRuntime)
{
	auto It = gs_RuntimeStates.find(pRuntime);
	if(It == gs_RuntimeStates.end())
		return;

	SRuntimeState &State = It->second;

	char aPresetPath[4096];
	pRuntime->get_current_preset_path(aPresetPath);
	const std::filesystem::path CurrentPresetPath = std::filesystem::u8path(aPresetPath);
	if(CurrentPresetPath != State.m_PresetPath)
	{
		State.m_PresetPath = CurrentPresetPath;
		State.m_HasWriteTime = QueryWriteTime(State.m_PresetPath, State.m_LastWriteTime);
	}

	if(State.m_PresetPath.empty())
		return;

	std::filesystem::file_time_type WriteTime;
	if(!QueryWriteTime(State.m_PresetPath, WriteTime))
		return;

	if(State.m_HasAppliedState && State.m_HasWriteTime && WriteTime == State.m_LastWriteTime)
		return;

	State.m_LastWriteTime = WriteTime;
	State.m_HasWriteTime = true;

	ApplyDeepFryState(pRuntime, State.m_PresetPath, State);
}
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE AddonModule, HMODULE ReShadeModule)
{
	if(!reshade::register_addon(AddonModule, ReShadeModule))
		return false;

	reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitEffectRuntime);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
	reshade::register_event<reshade::addon_event::reshade_present>(&OnPresent);
	LogInfo("[BestClient/ReShadeAddon] Add-on initialized.");
	return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE AddonModule, HMODULE ReShadeModule)
{
	(void)ReShadeModule;
	reshade::unregister_event<reshade::addon_event::reshade_present>(&OnPresent);
	reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
	reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&OnInitEffectRuntime);
	reshade::unregister_addon(AddonModule, ReShadeModule);
	gs_RuntimeStates.clear();
}

#endif
