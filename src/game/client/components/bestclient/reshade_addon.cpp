#if defined(_WIN32)

#include <reshade.hpp>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" __declspec(dllexport) const char *NAME = "BestClient ReShade Live Sync";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Applies BestClient ReShade state changes to the current runtime live.";
extern "C" __declspec(dllexport) const char *AUTHOR = "BestClient";

namespace
{
enum class EUniformValueType
{
	BOOL = 0,
	INT,
	UINT,
	FLOAT,
};

struct SUniformValue
{
	EUniformValueType m_Type = EUniformValueType::FLOAT;
	bool m_BoolValue = false;
	int32_t m_IntValue = 0;
	uint32_t m_UintValue = 0;
	float m_FloatValue = 0.0f;
};

struct SPresetState
{
	std::unordered_set<std::string> m_EnabledTokens;
	std::unordered_map<std::string, std::unordered_map<std::string, SUniformValue>> m_SectionValues;
};

struct SRuntimeState
{
	std::filesystem::path m_PresetPath;
	std::filesystem::file_time_type m_LastWriteTime = {};
	bool m_HasWriteTime = false;
	bool m_PresetReadWarningShown = false;
	bool m_HasAppliedPreset = false;
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

bool TryParseInt(const std::string &Text, int32_t &Value)
{
	char *pEnd = nullptr;
	const long ParsedValue = std::strtol(Text.c_str(), &pEnd, 10);
	if(pEnd == Text.c_str() || pEnd == nullptr || *pEnd != '\0')
		return false;
	Value = (int32_t)ParsedValue;
	return true;
}

bool TryParseUint(const std::string &Text, uint32_t &Value)
{
	char *pEnd = nullptr;
	const unsigned long ParsedValue = std::strtoul(Text.c_str(), &pEnd, 10);
	if(pEnd == Text.c_str() || pEnd == nullptr || *pEnd != '\0')
		return false;
	Value = (uint32_t)ParsedValue;
	return true;
}

bool TryParseBool(const std::string &Text, bool &Value)
{
	if(_stricmp(Text.c_str(), "true") == 0 || Text == "1")
	{
		Value = true;
		return true;
	}
	if(_stricmp(Text.c_str(), "false") == 0 || Text == "0")
	{
		Value = false;
		return true;
	}
	return false;
}

std::vector<std::string> SplitCommaSeparated(const std::string &Text)
{
	std::vector<std::string> vTokens;
	size_t TokenStart = 0;
	while(TokenStart <= Text.size())
	{
		const size_t TokenEnd = Text.find(',', TokenStart);
		const std::string Token = Trim(Text.substr(TokenStart, TokenEnd == std::string::npos ? std::string::npos : TokenEnd - TokenStart));
		if(!Token.empty())
			vTokens.push_back(Token);
		if(TokenEnd == std::string::npos)
			break;
		TokenStart = TokenEnd + 1;
	}
	return vTokens;
}

bool ParsePresetState(const std::filesystem::path &PresetPath, SPresetState &State)
{
	const std::optional<std::string> PresetText = ReadTextFile(PresetPath);
	if(!PresetText.has_value())
		return false;

	State = SPresetState{};

	std::istringstream Stream(*PresetText);
	std::string Line;
	std::string CurrentSection;

	while(std::getline(Stream, Line))
	{
		const std::string TrimmedLine = Trim(Line);
		if(TrimmedLine.empty() || TrimmedLine[0] == ';' || TrimmedLine[0] == '#')
			continue;

		if(TrimmedLine.front() == '[' && TrimmedLine.back() == ']')
		{
			CurrentSection = Trim(TrimmedLine.substr(1, TrimmedLine.size() - 2));
			continue;
		}

		const size_t EqualsPos = TrimmedLine.find('=');
		if(EqualsPos == std::string::npos)
			continue;

		const std::string Key = Trim(TrimmedLine.substr(0, EqualsPos));
		const std::string Value = Trim(TrimmedLine.substr(EqualsPos + 1));

		if(CurrentSection.empty())
		{
			if(Key == "Techniques")
			{
				for(const std::string &Token : SplitCommaSeparated(Value))
					State.m_EnabledTokens.insert(Token);
			}
			continue;
		}

		SUniformValue ParsedValue;
		bool BoolValue = false;
		int32_t IntValue = 0;
		uint32_t UintValue = 0;
		float FloatValue = 0.0f;
		if(TryParseBool(Value, BoolValue))
		{
			ParsedValue.m_Type = EUniformValueType::BOOL;
			ParsedValue.m_BoolValue = BoolValue;
		}
		else if(TryParseInt(Value, IntValue))
		{
			ParsedValue.m_Type = EUniformValueType::INT;
			ParsedValue.m_IntValue = IntValue;
		}
		else if(TryParseUint(Value, UintValue))
		{
			ParsedValue.m_Type = EUniformValueType::UINT;
			ParsedValue.m_UintValue = UintValue;
		}
		else if(TryParseFloat(Value, FloatValue))
		{
			ParsedValue.m_Type = EUniformValueType::FLOAT;
			ParsedValue.m_FloatValue = FloatValue;
		}
		else
		{
			continue;
		}

		State.m_SectionValues[CurrentSection][Key] = ParsedValue;
	}

	return true;
}

std::string BuildTechniqueToken(const char *pTechniqueName, const char *pEffectName)
{
	if(pTechniqueName == nullptr || pTechniqueName[0] == '\0')
		return "";
	if(pEffectName == nullptr || pEffectName[0] == '\0')
		return pTechniqueName;
	return std::string(pTechniqueName) + "@" + pEffectName;
}

void ApplyTechniqueStates(reshade::api::effect_runtime *pRuntime, const SPresetState &PresetState, int &NumAppliedTechniques)
{
	NumAppliedTechniques = 0;
	pRuntime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_technique Technique) {
		char aTechniqueName[512] = {0};
		char aEffectName[512] = {0};
		pCurrentRuntime->get_technique_name(Technique, aTechniqueName);
		pCurrentRuntime->get_technique_effect_name(Technique, aEffectName);
		const std::string Token = BuildTechniqueToken(aTechniqueName, aEffectName);
		const bool Enabled = PresetState.m_EnabledTokens.find(Token) != PresetState.m_EnabledTokens.end();
		pCurrentRuntime->set_technique_state(Technique, Enabled);
		++NumAppliedTechniques;
	});
}

int ApplyUniformStates(reshade::api::effect_runtime *pRuntime, const SPresetState &PresetState)
{
	int NumAppliedUniforms = 0;

	for(const auto &[EffectName, SectionValues] : PresetState.m_SectionValues)
	{
		if(SectionValues.empty())
			continue;

		pRuntime->enumerate_uniform_variables(EffectName.c_str(), [&](reshade::api::effect_runtime *pCurrentRuntime, reshade::api::effect_uniform_variable Variable) {
			char aUniformName[512] = {0};
			pCurrentRuntime->get_uniform_variable_name(Variable, aUniformName);
			if(aUniformName[0] == '\0')
				return;

			const auto ValueIt = SectionValues.find(aUniformName);
			if(ValueIt == SectionValues.end())
				return;

			reshade::api::format BaseType = reshade::api::format::unknown;
			uint32_t Rows = 0;
			uint32_t Columns = 0;
			uint32_t ArrayLength = 0;
			pCurrentRuntime->get_uniform_variable_type(Variable, &BaseType, &Rows, &Columns, &ArrayLength);
			if(Rows != 1 || Columns != 1 || ArrayLength != 1)
				return;

			const SUniformValue &UniformValue = ValueIt->second;
			switch(BaseType)
			{
			case reshade::api::format::r32_typeless:
				if(UniformValue.m_Type == EUniformValueType::BOOL)
				{
					pCurrentRuntime->set_uniform_value_bool(Variable, &UniformValue.m_BoolValue, 1);
					++NumAppliedUniforms;
				}
				break;
			case reshade::api::format::r32_sint:
				if(UniformValue.m_Type == EUniformValueType::INT)
				{
					pCurrentRuntime->set_uniform_value_int(Variable, &UniformValue.m_IntValue, 1);
					++NumAppliedUniforms;
				}
				else if(UniformValue.m_Type == EUniformValueType::UINT)
				{
					const int32_t SignedValue = (int32_t)UniformValue.m_UintValue;
					pCurrentRuntime->set_uniform_value_int(Variable, &SignedValue, 1);
					++NumAppliedUniforms;
				}
				break;
			case reshade::api::format::r32_uint:
				if(UniformValue.m_Type == EUniformValueType::UINT)
				{
					pCurrentRuntime->set_uniform_value_uint(Variable, &UniformValue.m_UintValue, 1);
					++NumAppliedUniforms;
				}
				else if(UniformValue.m_Type == EUniformValueType::INT && UniformValue.m_IntValue >= 0)
				{
					const uint32_t UnsignedValue = (uint32_t)UniformValue.m_IntValue;
					pCurrentRuntime->set_uniform_value_uint(Variable, &UnsignedValue, 1);
					++NumAppliedUniforms;
				}
				break;
			case reshade::api::format::r32_float:
				if(UniformValue.m_Type == EUniformValueType::FLOAT)
				{
					pCurrentRuntime->set_uniform_value_float(Variable, &UniformValue.m_FloatValue, 1);
					++NumAppliedUniforms;
				}
				else if(UniformValue.m_Type == EUniformValueType::INT)
				{
					const float FloatValue = (float)UniformValue.m_IntValue;
					pCurrentRuntime->set_uniform_value_float(Variable, &FloatValue, 1);
					++NumAppliedUniforms;
				}
				else if(UniformValue.m_Type == EUniformValueType::UINT)
				{
					const float FloatValue = (float)UniformValue.m_UintValue;
					pCurrentRuntime->set_uniform_value_float(Variable, &FloatValue, 1);
					++NumAppliedUniforms;
				}
				break;
			default:
				break;
			}
		});
	}

	return NumAppliedUniforms;
}

bool ApplyPresetState(reshade::api::effect_runtime *pRuntime, const std::filesystem::path &PresetPath, SRuntimeState &RuntimeState)
{
	SPresetState PresetState;
	if(!ParsePresetState(PresetPath, PresetState))
	{
		if(!RuntimeState.m_PresetReadWarningShown)
		{
			char aMessage[4608];
			std::snprintf(aMessage, sizeof(aMessage), "[BestClient/ReShadeAddon] Failed to read preset state from: %s", PresetPath.string().c_str());
			LogWarning(aMessage);
			RuntimeState.m_PresetReadWarningShown = true;
		}
		return false;
	}
	RuntimeState.m_PresetReadWarningShown = false;

	int NumAppliedTechniques = 0;
	ApplyTechniqueStates(pRuntime, PresetState, NumAppliedTechniques);
	const int NumAppliedUniforms = ApplyUniformStates(pRuntime, PresetState);

	RuntimeState.m_HasAppliedPreset = true;

	char aMessage[4608];
	std::snprintf(
		aMessage, sizeof(aMessage),
		"[BestClient/ReShadeAddon] Applied preset live: techniques=%d sections=%zu uniforms=%d",
		NumAppliedTechniques,
		PresetState.m_SectionValues.size(),
		NumAppliedUniforms);
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
	ApplyPresetState(pRuntime, State.m_PresetPath, State);
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

	if(State.m_HasAppliedPreset && State.m_HasWriteTime && WriteTime == State.m_LastWriteTime)
		return;

	State.m_LastWriteTime = WriteTime;
	State.m_HasWriteTime = true;

	ApplyPresetState(pRuntime, State.m_PresetPath, State);
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
