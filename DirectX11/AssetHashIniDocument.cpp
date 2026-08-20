#include "AssetHashIniDocument.h"

#include <algorithm>
#include <cwctype>
#include <set>
#include <sstream>

namespace
{
const wchar_t *kBlockBegin = L"; <asset-hash-stream>";
const wchar_t *kBlockEnd = L"; </asset-hash-stream>";
const wchar_t *kMipMultiplicityKey = L"asset_hash_mip_multiplicity";
constexpr uint32_t kMaxMipMultiplicity = 32;

struct MipDimensions
{
	uint32_t width;
	uint32_t height;

	bool operator<(const MipDimensions& other) const
	{
		return width < other.width ||
			(width == other.width && height < other.height);
	}
};

typedef std::map<MipDimensions, uint32_t> MipMultiplicityMap;

struct SectionData
{
	std::wstring identity_key;
	std::wstring identity_name;
	std::wstring identity_value;
	std::vector<std::pair<std::wstring, std::wstring>> additional_identities;
	bool identity_alias = false;
	bool active_path_identity = false;
	bool generated_block = false;
	std::wstring section_name;
	std::vector<std::wstring> body;
	std::vector<AssetHashObservation> hashes;
	MipMultiplicityMap mip_multiplicities;
};

std::wstring Trim(const std::wstring& value)
{
	size_t begin = value.find_first_not_of(L" \t\r");
	if (begin == std::wstring::npos)
		return L"";
	size_t end = value.find_last_not_of(L" \t\r");
	return value.substr(begin, end - begin + 1);
}

std::wstring Lower(std::wstring value)
{
	std::transform(value.begin(), value.end(), value.begin(), towlower);
	return value;
}

bool StartsWithInsensitive(
	const std::wstring& value,
	const std::wstring& prefix)
{
	return value.size() >= prefix.size() &&
		Lower(value.substr(0, prefix.size())) == Lower(prefix);
}

bool ParseAssignment(
	const std::wstring& line,
	std::wstring *key,
	std::wstring *value,
	bool *commented)
{
	std::wstring text = Trim(line);
	*commented = false;
	if (!text.empty() && text[0] == L';') {
		*commented = true;
		text = Trim(text.substr(1));
	}
	size_t equals = text.find(L'=');
	if (equals == std::wstring::npos)
		return false;
	*key = Lower(Trim(text.substr(0, equals)));
	*value = Trim(text.substr(equals + 1));
	return !key->empty();
}

bool ParseSectionHeader(const std::wstring& line, std::wstring *name)
{
	std::wstring text = Trim(line);
	if (text.size() < 3 || text.front() != L'[')
		return false;
	size_t end = text.find(L']', 1);
	if (end == std::wstring::npos)
		return false;
	*name = Trim(text.substr(1, end - 1));
	return true;
}

std::vector<std::wstring> SplitLines(const std::wstring& text)
{
	std::vector<std::wstring> lines;
	std::wstring line;
	std::wistringstream stream(text);
	while (std::getline(stream, line)) {
		if (!line.empty() && line.back() == L'\r')
			line.pop_back();
		lines.push_back(line);
	}
	return lines;
}

std::wstring JoinLines(const std::vector<std::wstring>& lines)
{
	std::wstring output;
	for (size_t i = 0; i < lines.size(); ++i) {
		output += lines[i];
		output += L"\r\n";
	}
	return output;
}

bool ParseHash(const std::wstring& value, uint32_t *hash)
{
	wchar_t *end = nullptr;
	unsigned long parsed = wcstoul(value.c_str(), &end, 16);
	if (end == value.c_str() || *Trim(end).c_str())
		return false;
	*hash = static_cast<uint32_t>(parsed);
	return true;
}

bool ParseDimensions(const std::wstring& line, uint32_t *width, uint32_t *height)
{
	std::wstring text = Trim(line);
	if (text.empty() || text[0] != L';')
		return false;
	text = Trim(text.substr(1));
	size_t separator = text.find_first_of(L"xX");
	if (separator == std::wstring::npos)
		return false;
	wchar_t *width_end = nullptr;
	wchar_t *height_end = nullptr;
	unsigned long parsed_width = wcstoul(text.c_str(), &width_end, 10);
	unsigned long parsed_height =
		wcstoul(text.c_str() + separator + 1, &height_end, 10);
	if (width_end != text.c_str() + separator ||
			*Trim(height_end).c_str())
		return false;
	*width = static_cast<uint32_t>(parsed_width);
	*height = static_cast<uint32_t>(parsed_height);
	return true;
}

bool ParseUnknownDimensions(const std::wstring& line)
{
	return Lower(Trim(line)) == L"; unknown-resolution";
}

bool ParseMipMultiplicity(const std::wstring& value, uint32_t *multiplicity)
{
	wchar_t *end = nullptr;
	unsigned long parsed = wcstoul(value.c_str(), &end, 10);
	if (end == value.c_str() || *Trim(end).c_str() ||
			parsed < 2 || parsed > kMaxMipMultiplicity)
		return false;
	*multiplicity = static_cast<uint32_t>(parsed);
	return true;
}

std::wstring IdentityKey(
	const std::wstring& name,
	const std::wstring& value)
{
	return Lower(name) + L"=" + Lower(value);
}

std::wstring CanonicalIdentityName(const std::wstring& key)
{
	if (key == L"match_asset_path" || key == L"path")
		return L"match_asset_path";
	if (key == L"match_asset_name" || key == L"name")
		return L"match_asset_name";
	return std::wstring();
}

void AddIdentity(
	SectionData *data,
	const std::wstring& name,
	const std::wstring& value)
{
	std::wstring key = IdentityKey(name, value);
	if (data->identity_key == key)
		return;
	for (const auto& identity : data->additional_identities) {
		if (IdentityKey(identity.first, identity.second) == key)
			return;
	}

	if (data->identity_key.empty()) {
		data->identity_name = name;
		data->identity_value = value;
		data->identity_key = key;
		return;
	}

	if (name == L"match_asset_path" &&
			data->identity_name == L"match_asset_name") {
		data->additional_identities.push_back({
			data->identity_name,
			data->identity_value});
		data->identity_name = name;
		data->identity_value = value;
		data->identity_key = key;
		return;
	}

	data->additional_identities.push_back({name, value});
}

SectionData ParseSection(
	const std::vector<std::wstring>& lines,
	size_t begin,
	size_t end)
{
	SectionData data;
	ParseSectionHeader(lines[begin], &data.section_name);
	uint32_t pending_width = 0;
	uint32_t pending_height = 0;
	bool has_pending_dimensions = false;
	bool first_generated_section = true;

	for (size_t i = begin + 1; i < end; ++i) {
		std::wstring section_name;
		if (ParseSectionHeader(lines[i], &section_name)) {
			first_generated_section = false;
			continue;
		}

		uint32_t width = 0;
		uint32_t height = 0;
		if (ParseDimensions(lines[i], &width, &height)) {
			pending_width = width;
			pending_height = height;
			has_pending_dimensions = true;
			continue;
		}
		if (ParseUnknownDimensions(lines[i])) {
			pending_width = 0;
			pending_height = 0;
			has_pending_dimensions = true;
			continue;
		}

		std::wstring key;
		std::wstring value;
		bool commented = false;
		if (ParseAssignment(lines[i], &key, &value, &commented)) {
			if (key == kMipMultiplicityKey && commented &&
					has_pending_dimensions) {
				uint32_t multiplicity = 0;
				if (ParseMipMultiplicity(value, &multiplicity)) {
					data.mip_multiplicities[
						{pending_width, pending_height}] = multiplicity;
				}
				continue;
			}
			std::wstring identity_name = CanonicalIdentityName(key);
			if (!identity_name.empty()) {
				AddIdentity(&data, identity_name, value);
				data.identity_alias = data.identity_alias || key != identity_name;
				data.active_path_identity = data.active_path_identity ||
					(!commented && identity_name == L"match_asset_path");
				continue;
			}
			if (key == L"hash" && !commented) {
				uint32_t hash = 0;
				if (ParseHash(value, &hash)) {
					data.hashes.push_back({
						hash,
						has_pending_dimensions ? pending_width : 0,
						has_pending_dimensions ? pending_height : 0});
				}
				has_pending_dimensions = false;
				continue;
			}
			if (key == L"asset_hash_compiler_version" ||
					key == L"game_exe_version" ||
					key == L"game_version")
				continue;
		}

		if (first_generated_section)
			data.body.push_back(lines[i]);
	}
	return data;
}

SectionData ParseBlock(
	const std::vector<std::wstring>& lines,
	size_t begin,
	size_t end)
{
	SectionData data;
	data.generated_block = true;
	uint32_t pending_width = 0;
	uint32_t pending_height = 0;
	bool has_pending_dimensions = false;
	bool in_first_section = false;
	bool first_section_complete = false;

	for (size_t i = begin + 1; i < end; ++i) {
		uint32_t width = 0;
		uint32_t height = 0;
		if (ParseDimensions(lines[i], &width, &height)) {
			pending_width = width;
			pending_height = height;
			has_pending_dimensions = true;
			continue;
		}
		if (ParseUnknownDimensions(lines[i])) {
			pending_width = 0;
			pending_height = 0;
			has_pending_dimensions = true;
			continue;
		}

		std::wstring section_name;
		if (ParseSectionHeader(lines[i], &section_name)) {
			if (data.section_name.empty()) {
				data.section_name = section_name;
				size_t suffix = Lower(data.section_name).find(
					L"_assethash_");
				if (suffix != std::wstring::npos)
					data.section_name.resize(suffix);
				in_first_section = true;
			} else {
				in_first_section = false;
				first_section_complete = true;
			}
			continue;
		}

		std::wstring key;
		std::wstring value;
		bool commented = false;
		if (ParseAssignment(lines[i], &key, &value, &commented)) {
			if (key == kMipMultiplicityKey && commented &&
					has_pending_dimensions) {
				uint32_t multiplicity = 0;
				if (ParseMipMultiplicity(value, &multiplicity)) {
					data.mip_multiplicities[
						{pending_width, pending_height}] = multiplicity;
				}
				continue;
			}
			std::wstring identity_name = CanonicalIdentityName(key);
			if (!identity_name.empty()) {
				AddIdentity(&data, identity_name, value);
				data.identity_alias = data.identity_alias || key != identity_name;
				data.active_path_identity = data.active_path_identity ||
					(!commented && identity_name == L"match_asset_path");
				continue;
			}
			if (key == L"hash" && !commented) {
				uint32_t hash = 0;
				if (ParseHash(value, &hash)) {
					data.hashes.push_back({
						hash,
						has_pending_dimensions ? pending_width : 0,
						has_pending_dimensions ? pending_height : 0});
				}
				has_pending_dimensions = false;
				continue;
			}
			if (key == L"asset_hash_compiler_version" ||
					key == L"game_exe_version" ||
					key == L"game_version")
				continue;
		}

		if (in_first_section && !first_section_complete)
			data.body.push_back(lines[i]);
	}
	return data;
}

std::map<std::wstring, SectionData> ParseGeneratedBlocks(
	const std::wstring& document)
{
	std::map<std::wstring, SectionData> blocks;
	std::vector<std::wstring> lines = SplitLines(document);
	for (size_t i = 0; i < lines.size(); ++i) {
		if (Trim(lines[i]) != kBlockBegin)
			continue;
		size_t end = i + 1;
		while (end < lines.size() && Trim(lines[end]) != kBlockEnd)
			++end;
		if (end == lines.size())
			break;
		SectionData block = ParseBlock(lines, i, end);
		if (!block.identity_key.empty())
			blocks[block.identity_key] = block;
		i = end;
	}
	return blocks;
}

void NormalizeHashes(std::vector<AssetHashObservation> *hashes)
{
	std::sort(
		hashes->begin(),
		hashes->end(),
		[](const AssetHashObservation& left,
		   const AssetHashObservation& right) {
			uint32_t left_max = std::max(left.width, left.height);
			uint32_t right_max = std::max(right.width, right.height);
			if (left_max != right_max)
				return left_max > right_max;
			uint64_t left_area =
				static_cast<uint64_t>(left.width) * left.height;
			uint64_t right_area =
				static_cast<uint64_t>(right.width) * right.height;
			if (left_area != right_area)
				return left_area > right_area;
			if (left.width != right.width)
				return left.width > right.width;
			if (left.height != right.height)
				return left.height > right.height;
			return left.hash < right.hash;
		});
	hashes->erase(
		std::unique(
			hashes->begin(),
			hashes->end(),
			[](const AssetHashObservation& left,
			   const AssetHashObservation& right) {
				return left.hash == right.hash;
			}),
		hashes->end());
}

void AppendHashObservations(
	std::vector<AssetHashObservation> *hashes,
	const std::vector<AssetHashObservation>& additional)
{
	hashes->insert(hashes->end(), additional.begin(), additional.end());
	NormalizeHashes(hashes);
}

void MergeMipMultiplicities(
	MipMultiplicityMap *multiplicities,
	const MipMultiplicityMap& additional)
{
	for (const auto& entry : additional) {
		auto existing = multiplicities->find(entry.first);
		if (existing == multiplicities->end() ||
				entry.second > existing->second)
			(*multiplicities)[entry.first] = entry.second;
	}
}

std::map<MipDimensions, std::vector<AssetHashObservation>> GroupHashesByMip(
	const std::vector<AssetHashObservation>& hashes)
{
	std::map<MipDimensions, std::vector<AssetHashObservation>> groups;
	for (const AssetHashObservation& observation : hashes) {
		groups[{observation.width, observation.height}].push_back(observation);
	}
	for (auto& group : groups)
		NormalizeHashes(&group.second);
	return groups;
}

MipMultiplicityMap DetectMipMultiplicities(
	const std::vector<AssetHashObservation>& hashes)
{
	MipMultiplicityMap multiplicities;
	for (const auto& group : GroupHashesByMip(hashes)) {
		if (group.second.size() > 1) {
			multiplicities[group.first] =
				static_cast<uint32_t>(group.second.size());
		}
	}
	return multiplicities;
}

void MergeHashObservations(
	std::vector<AssetHashObservation> *hashes,
	MipMultiplicityMap *multiplicities,
	const std::vector<AssetHashObservation>& observed)
{
	std::map<MipDimensions, std::vector<AssetHashObservation>> stored_groups =
		GroupHashesByMip(*hashes);
	std::map<MipDimensions, std::vector<AssetHashObservation>> observed_groups =
		GroupHashesByMip(observed);

	for (const auto& observed_group : observed_groups) {
		auto marked = multiplicities->find(observed_group.first);
		uint32_t required = marked == multiplicities->end()
			? 1
			: marked->second;
		if (observed_group.second.size() >= required) {
			stored_groups[observed_group.first] = observed_group.second;
			if (observed_group.second.size() > 1) {
				(*multiplicities)[observed_group.first] =
					static_cast<uint32_t>(observed_group.second.size());
			} else {
				multiplicities->erase(observed_group.first);
			}
		} else {
			AppendHashObservations(
				&stored_groups[observed_group.first],
				observed_group.second);
		}
	}

	hashes->clear();
	for (const auto& stored_group : stored_groups) {
		hashes->insert(
			hashes->end(),
			stored_group.second.begin(),
			stored_group.second.end());
	}
	NormalizeHashes(hashes);
}

void RemoveAmbiguousHashes(
	std::vector<AssetHashObservation> *hashes,
	const std::set<uint32_t>& ambiguous_hashes)
{
	hashes->erase(
		std::remove_if(
			hashes->begin(),
			hashes->end(),
		[&ambiguous_hashes](const AssetHashObservation& observation) {
			return ambiguous_hashes.find(observation.hash) !=
				ambiguous_hashes.end();
		}),
		hashes->end());
}

void TrimBodyBlankLines(std::vector<std::wstring> *body);

std::wstring PathSectionName(
	const std::wstring& asset_path);

void AppendIdentitySection(
	std::vector<std::wstring> *output,
	const SectionData& section)
{
	std::wstring section_name = section.section_name;
	if (section.generated_block &&
			section.identity_name == L"match_asset_path") {
		section_name = PathSectionName(section.identity_value);
	}
	output->push_back(L"[" + section_name + L"]");
	output->push_back(
		section.identity_name + L" = " + section.identity_value);
	for (const auto& identity : section.additional_identities) {
		output->push_back(identity.first + L" = " + identity.second);
	}
	std::vector<std::wstring> body = section.body;
	TrimBodyBlankLines(&body);
	output->insert(output->end(), body.begin(), body.end());
	output->push_back(L"");
}

void TrimBodyBlankLines(std::vector<std::wstring> *body)
{
	while (!body->empty() && Trim(body->front()).empty())
		body->erase(body->begin());
	while (!body->empty() && Trim(body->back()).empty())
		body->pop_back();
}

void RemoveZeroMatchPriority(std::vector<std::wstring> *body)
{
	body->erase(
		std::remove_if(
			body->begin(),
			body->end(),
			[](const std::wstring& line) {
				std::wstring key;
				std::wstring value;
				bool commented = false;
				return ParseAssignment(line, &key, &value, &commented) &&
					!commented && key == L"match_priority" &&
					Trim(value) == L"0";
			}),
		body->end());
}

void EnsureHashMatchPriority(std::vector<std::wstring> *body)
{
	for (const std::wstring& line : *body) {
		std::wstring key;
		std::wstring value;
		bool commented = false;
		if (ParseAssignment(line, &key, &value, &commented) &&
				!commented && key == L"match_priority")
			return;
	}
	body->insert(body->begin(), L"match_priority = 0");
}

std::wstring AssetPathSectionName(const std::wstring& asset_path)
{
	size_t separator = asset_path.rfind(L'.');
	if (separator == std::wstring::npos || separator + 1 == asset_path.size())
		separator = asset_path.find_last_of(L"/\\");
	std::wstring name = separator == std::wstring::npos ?
		asset_path : asset_path.substr(separator + 1);
	for (wchar_t& character : name) {
		if (!iswalnum(character) && character != L'_' && character != L'-')
			character = L'_';
	}
	return name.empty() ? L"asset" : name;
}

std::wstring PathSectionName(
	const std::wstring& asset_path)
{
	return L"TextureOverride_" + AssetPathSectionName(asset_path);
}

bool NeedsPathSectionCanonicalization(const SectionData& section)
{
	return section.generated_block && section.active_path_identity &&
		section.identity_name == L"match_asset_path" &&
		section.section_name != PathSectionName(section.identity_value);
}

std::wstring GeneratedSectionName(const std::wstring& hash_text)
{
	return L"TextureOverride_Texture_" + hash_text;
}

void AppendPathBlockStart(
	std::vector<std::wstring> *output,
	const SectionData& source,
	const std::wstring& asset_path,
	const std::wstring& game_version)
{
	std::vector<std::wstring> body = source.body;
	TrimBodyBlankLines(&body);
	RemoveZeroMatchPriority(&body);
	output->push_back(kBlockBegin);
	output->push_back(L"; match_asset_path = " + asset_path);
	for (const auto& identity : source.additional_identities) {
		output->push_back(L"; " + identity.first + L" = " + identity.second);
	}
	output->push_back(L"; asset_hash_compiler_version = Ver1.1");
	output->push_back(L"; game_version = " + game_version);
	output->push_back(
		L"[" + PathSectionName(asset_path) + L"]");
	output->push_back(L"match_asset_path = " + asset_path);
	for (const auto& identity : source.additional_identities) {
		output->push_back(identity.first + L" = " + identity.second);
	}
	output->insert(output->end(), body.begin(), body.end());
}

void AppendPathBlockEnd(std::vector<std::wstring> *output)
{
	output->push_back(L"");
	output->push_back(kBlockEnd);
}

void AppendPathBlock(
	std::vector<std::wstring> *output,
	const SectionData& source,
	const std::wstring& asset_path,
	const std::wstring& game_version)
{
	AppendPathBlockStart(output, source, asset_path, game_version);
	AppendPathBlockEnd(output);
}

void AppendUnverifiedHashSections(
	std::vector<std::wstring> *output,
	const SectionData& source,
	std::vector<AssetHashObservation> hashes)
{
	NormalizeHashes(&hashes);
	std::vector<std::wstring> body = source.body;
	TrimBodyBlankLines(&body);
	EnsureHashMatchPriority(&body);
	for (size_t i = 0; i < hashes.size(); ++i) {
		const AssetHashObservation& observation = hashes[i];
		output->push_back(L"");
		if (observation.width && observation.height) {
			output->push_back(
				L"; " + std::to_wstring(observation.width) + L"x" +
				std::to_wstring(observation.height));
		} else {
			output->push_back(L"; unknown-resolution");
		}
		const MipDimensions dimensions = {
			observation.width,
			observation.height};
		bool first_at_mip = !i ||
			hashes[i - 1].width != observation.width ||
			hashes[i - 1].height != observation.height;
		auto multiplicity = source.mip_multiplicities.find(dimensions);
		if (first_at_mip && multiplicity != source.mip_multiplicities.end()) {
			output->push_back(
				L"; " + std::wstring(kMipMultiplicityKey) + L" = " +
				std::to_wstring(multiplicity->second));
		}
		wchar_t hash_text[9];
		swprintf(hash_text, 9, L"%08x", observation.hash);
		output->push_back(L"[" + GeneratedSectionName(hash_text) + L"]");
		output->push_back(L"hash = " + std::wstring(hash_text));
		output->insert(output->end(), body.begin(), body.end());
	}
}

void AppendPathBlockWithUnverifiedHashes(
	std::vector<std::wstring> *output,
	const SectionData& source,
	const std::wstring& asset_path,
	const std::wstring& game_version,
	const std::vector<AssetHashObservation>& unverified_hashes)
{
	AppendPathBlockStart(output, source, asset_path, game_version);
	AppendUnverifiedHashSections(
		output,
		source,
		unverified_hashes);
	AppendPathBlockEnd(output);
}

bool ResolveObservedPath(
	const SectionData& section,
	const AssetHashObservationMap& observations,
	const AssetHashPathIdentityMap& legacy_hash_identities,
	const std::set<uint32_t>& ambiguous_hashes,
	std::wstring *asset_path,
	std::vector<AssetHashObservation> *current_hashes)
{
	auto observed = observations.find(section.identity_key);
	if (observed == observations.end())
		return false;
	if (section.identity_name == L"match_asset_path") {
		*asset_path = section.identity_value;
		*current_hashes = observed->second;
		NormalizeHashes(current_hashes);
		return !current_hashes->empty();
	}

	std::wstring resolved_path;
	for (const AssetHashObservation& observation : observed->second) {
		if (ambiguous_hashes.find(observation.hash) != ambiguous_hashes.end())
			continue;
		auto identity = legacy_hash_identities.find(observation.hash);
		if (identity == legacy_hash_identities.end())
			continue;
		if (!resolved_path.empty() &&
				_wcsicmp(
					resolved_path.c_str(),
					identity->second.asset_path.c_str()))
			return false;
		resolved_path = identity->second.asset_path;
		AppendHashObservations(current_hashes, identity->second.hashes);
	}
	if (resolved_path.empty() || current_hashes->empty())
		return false;
	*asset_path = resolved_path;
	RemoveAmbiguousHashes(current_hashes, ambiguous_hashes);
	return !current_hashes->empty();
}

void AppendGeneratedBlock(
	std::vector<std::wstring> *output,
	const SectionData& source,
	std::vector<AssetHashObservation> hashes,
	const MipMultiplicityMap& mip_multiplicities,
	const std::wstring& game_version)
{
	NormalizeHashes(&hashes);
	std::vector<std::wstring> body = source.body;
	TrimBodyBlankLines(&body);
	EnsureHashMatchPriority(&body);
	output->push_back(kBlockBegin);
	output->push_back(
		L"; " + source.identity_name + L" = " + source.identity_value);
	for (const auto& identity : source.additional_identities) {
		output->push_back(L"; " + identity.first + L" = " + identity.second);
	}
	output->push_back(L"; asset_hash_compiler_version = Ver1.1");
	output->push_back(L"; game_version = " + game_version);

	for (size_t i = 0; i < hashes.size(); ++i) {
		const AssetHashObservation& observation = hashes[i];
		const MipDimensions dimensions = {
			observation.width,
			observation.height};
		if (i)
			output->push_back(L"");
		if (observation.width && observation.height) {
			output->push_back(
				L"; " + std::to_wstring(observation.width) + L"x" +
				std::to_wstring(observation.height));
		} else {
			output->push_back(L"; unknown-resolution");
		}
		bool first_at_mip = !i ||
			hashes[i - 1].width != observation.width ||
			hashes[i - 1].height != observation.height;
		auto multiplicity = mip_multiplicities.find(dimensions);
		if (first_at_mip && multiplicity != mip_multiplicities.end() &&
				multiplicity->second > 1) {
			output->push_back(
				L"; " + std::wstring(kMipMultiplicityKey) + L" = " +
				std::to_wstring(multiplicity->second));
		}
		wchar_t hash_text[9];
		swprintf(hash_text, 9, L"%08x", observation.hash);
		std::wstring section_name = GeneratedSectionName(hash_text);
		output->push_back(L"[" + section_name + L"]");
		output->push_back(L"hash = " + std::wstring(hash_text));
		output->insert(
			output->end(),
			body.begin(),
			body.end());
	}
	output->push_back(L"");
	output->push_back(kBlockEnd);
}
}

std::set<std::wstring> CollectAssetHashIniIdentities(
	const std::wstring& document)
{
	std::set<std::wstring> identities;
	std::vector<std::wstring> lines = SplitLines(document);
	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			size_t end = i + 1;
			while (end < lines.size() && Trim(lines[end]) != kBlockEnd)
				++end;
			if (end < lines.size()) {
				SectionData block = ParseBlock(lines, i, end);
				if (!block.identity_key.empty())
					identities.insert(block.identity_key);
				for (const auto& identity : block.additional_identities) {
					identities.insert(IdentityKey(identity.first, identity.second));
				}
				++end;
			}
			i = end;
			continue;
		}
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		SectionData section = ParseSection(lines, i, end);
		if (!section.identity_key.empty())
			identities.insert(section.identity_key);
		for (const auto& identity : section.additional_identities) {
			identities.insert(IdentityKey(identity.first, identity.second));
		}
		i = end;
	}
	return identities;
}

bool AssetHashIniUsesIdentityAliases(const std::wstring& document)
{
	std::vector<std::wstring> lines = SplitLines(document);
	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			size_t end = i + 1;
			while (end < lines.size() && Trim(lines[end]) != kBlockEnd)
				++end;
			if (end < lines.size()) {
				if (ParseBlock(lines, i, end).identity_alias)
					return true;
				++end;
			}
			i = end;
			continue;
		}
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		if (ParseSection(lines, i, end).identity_alias)
			return true;
		i = end;
	}
	return false;
}

bool AssetHashIniNeedsCanonicalization(const std::wstring& document)
{
	std::vector<std::wstring> lines = SplitLines(document);
	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) != kBlockBegin) {
			++i;
			continue;
		}
		size_t end = i + 1;
		while (end < lines.size() && Trim(lines[end]) != kBlockEnd)
			++end;
		if (end == lines.size())
			return false;
		if (NeedsPathSectionCanonicalization(ParseBlock(lines, i, end)))
			return true;
		i = end + 1;
	}
	return false;
}

std::set<uint32_t> CollectAssetHashIniLegacyHashes(
	const std::wstring& document)
{
	std::set<uint32_t> hashes;
	std::vector<std::wstring> lines = SplitLines(document);
	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			while (i < lines.size() && Trim(lines[i]) != kBlockEnd)
				++i;
			if (i < lines.size())
				++i;
			continue;
		}
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		SectionData section = ParseSection(lines, i, end);
		if (section.identity_key.empty() && section.hashes.size() == 1)
			hashes.insert(section.hashes.front().hash);
		i = end;
	}
	return hashes;
}

std::wstring TransformAssetHashIniDocument(
	const std::wstring& source,
	const std::wstring& previous_output,
	const AssetHashObservationMap& observations,
	const AssetHashPathIdentityMap& legacy_hash_identities,
	const std::set<uint32_t>& ambiguous_hashes,
	const std::wstring& game_version)
{
	std::map<std::wstring, SectionData> previous =
		ParseGeneratedBlocks(previous_output);
	std::vector<std::wstring> lines = SplitLines(source);
	std::vector<std::wstring> output;
	struct LegacyPathGroup
	{
		std::wstring asset_path;
		std::vector<AssetHashObservation> hashes;
		std::vector<std::wstring> body;
		bool safe;
	};
	std::map<std::wstring, LegacyPathGroup> legacy_path_groups;
	std::set<uint32_t> source_legacy_hashes;

	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			while (i < lines.size() && Trim(lines[i]) != kBlockEnd)
				++i;
			if (i < lines.size())
				++i;
			continue;
		}
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		SectionData section = ParseSection(lines, i, end);
		if (section.identity_key.empty() && section.hashes.size() == 1) {
			uint32_t hash = section.hashes.front().hash;
			source_legacy_hashes.insert(hash);
			auto identity =
				legacy_hash_identities.find(hash);
			if (identity != legacy_hash_identities.end()) {
				std::wstring path_key = Lower(identity->second.asset_path);
				std::vector<std::wstring> body = section.body;
				TrimBodyBlankLines(&body);
				auto group = legacy_path_groups.find(path_key);
				if (group == legacy_path_groups.end()) {
					legacy_path_groups.emplace(
						path_key,
						LegacyPathGroup{
							identity->second.asset_path,
							identity->second.hashes,
							body,
							true});
					} else {
						group->second.hashes.insert(
							group->second.hashes.end(),
							identity->second.hashes.begin(),
							identity->second.hashes.end());
						if (group->second.body != body)
							group->second.safe = false;
					}
			}
		}
		i = end;
	}

	for (auto& group : legacy_path_groups) {
		for (const AssetHashObservation& observation :
				group.second.hashes) {
			if (source_legacy_hashes.find(observation.hash) ==
					source_legacy_hashes.end())
				continue;
			auto identity =
				legacy_hash_identities.find(observation.hash);
			if (identity == legacy_hash_identities.end() ||
					Lower(identity->second.asset_path) != group.first) {
				group.second.safe = false;
				break;
			}
		}
	}

	std::set<std::wstring> emitted_legacy_paths;
	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			size_t end = i + 1;
			while (end < lines.size() && Trim(lines[end]) != kBlockEnd)
				++end;
			if (end == lines.size()) {
				output.insert(output.end(), lines.begin() + i, lines.end());
				break;
			}
			SectionData block = ParseBlock(lines, i, end);
			std::vector<AssetHashObservation> hashes = block.hashes;
			MipMultiplicityMap mip_multiplicities =
				block.mip_multiplicities;
			auto old = previous.find(block.identity_key);
			if (old != previous.end()) {
				AppendHashObservations(&hashes, old->second.hashes);
				MergeMipMultiplicities(
					&mip_multiplicities,
					old->second.mip_multiplicities);
			}
			RemoveAmbiguousHashes(&hashes, ambiguous_hashes);
			auto observed = observations.find(block.identity_key);
			if (observed != observations.end()) {
				std::vector<AssetHashObservation> safe_observed =
					observed->second;
				RemoveAmbiguousHashes(
					&safe_observed,
					ambiguous_hashes);
				MergeHashObservations(
					&hashes,
					&mip_multiplicities,
					safe_observed);
			}
			if (hashes.empty() && block.active_path_identity)
				AppendPathBlock(
					&output,
					block,
					block.identity_value,
					game_version);
			else if (hashes.empty())
				AppendIdentitySection(&output, block);
			else
				AppendGeneratedBlock(
					&output,
					block,
					hashes,
					mip_multiplicities,
					game_version);
			i = end + 1;
			continue;
		}

		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			output.push_back(lines[i++]);
			continue;
		}

		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		SectionData section = ParseSection(lines, i, end);
		if (section.identity_key.empty()) {
			if (section.hashes.size() == 1) {
				auto identity = legacy_hash_identities.find(
					section.hashes.front().hash);
				if (identity != legacy_hash_identities.end()) {
					std::wstring path_key =
						Lower(identity->second.asset_path);
					auto group = legacy_path_groups.find(path_key);
					if (group != legacy_path_groups.end() &&
							group->second.safe) {
						if (emitted_legacy_paths.insert(path_key).second) {
							section.identity_name = L"match_asset_path";
							section.identity_value =
								group->second.asset_path;
							section.identity_key = IdentityKey(
								section.identity_name,
								section.identity_value);
							AppendGeneratedBlock(
								&output,
								section,
								group->second.hashes,
								DetectMipMultiplicities(
									group->second.hashes),
								game_version);
						}
						i = end;
						continue;
					}
				}
			}
			output.insert(
				output.end(),
				lines.begin() + i,
				lines.begin() + end);
			i = end;
			continue;
		}

		std::vector<AssetHashObservation> hashes = section.hashes;
		MipMultiplicityMap mip_multiplicities =
			section.mip_multiplicities;
		auto old = previous.find(section.identity_key);
		if (old != previous.end()) {
			AppendHashObservations(&hashes, old->second.hashes);
			MergeMipMultiplicities(
				&mip_multiplicities,
				old->second.mip_multiplicities);
		}
		RemoveAmbiguousHashes(&hashes, ambiguous_hashes);
		auto observed = observations.find(section.identity_key);
		if (observed != observations.end()) {
			std::vector<AssetHashObservation> safe_observed =
				observed->second;
			RemoveAmbiguousHashes(
				&safe_observed,
				ambiguous_hashes);
			MergeHashObservations(
				&hashes,
				&mip_multiplicities,
				safe_observed);
		}

		if (hashes.empty()) {
			if (section.identity_alias) {
				AppendIdentitySection(&output, section);
			} else {
				output.insert(
					output.end(),
					lines.begin() + i,
					lines.begin() + end);
			}
		} else {
			AppendGeneratedBlock(
				&output,
				section,
				hashes,
				mip_multiplicities,
				game_version);
		}
		i = end;
	}
	return JoinLines(output);
}

static std::wstring TransformAssetHashIniDocumentToPathsInternal(
	const std::wstring& source,
	const AssetHashObservationMap& observations,
	const AssetHashPathIdentityMap& legacy_hash_identities,
	const std::set<uint32_t>& ambiguous_hashes,
	const std::wstring& game_version,
	bool remove_unverified_stream_hashes)
{
	struct LegacyPathGroup
	{
		std::wstring asset_path;
		SectionData source;
		std::vector<std::wstring> comparable_body;
		bool safe;
	};

	std::vector<std::wstring> lines = SplitLines(source);
	std::map<std::wstring, LegacyPathGroup> legacy_path_groups;
	std::set<std::wstring> validated_identity_paths;
	std::map<uint32_t, std::wstring> validated_hash_paths;
	std::set<uint32_t> ambiguous_validated_hashes;
	auto record_validated_path = [&validated_hash_paths,
			&ambiguous_validated_hashes](
			const std::wstring& asset_path,
			const std::vector<AssetHashObservation>& current_hashes) {
		for (const AssetHashObservation& observation : current_hashes) {
			if (ambiguous_validated_hashes.find(observation.hash) !=
					ambiguous_validated_hashes.end())
				continue;
			auto inserted = validated_hash_paths.emplace(
				observation.hash,
				asset_path);
			if (!inserted.second &&
					_wcsicmp(inserted.first->second.c_str(), asset_path.c_str())) {
				validated_hash_paths.erase(inserted.first);
				ambiguous_validated_hashes.insert(observation.hash);
			}
		}
	};

	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			size_t end = i + 1;
			while (end < lines.size() && Trim(lines[end]) != kBlockEnd)
				++end;
			if (end < lines.size()) {
				SectionData block = ParseBlock(lines, i, end);
				std::wstring asset_path;
				std::vector<AssetHashObservation> current_hashes;
				if (ResolveObservedPath(
						block,
						observations,
						legacy_hash_identities,
						ambiguous_hashes,
						&asset_path,
						&current_hashes)) {
					validated_identity_paths.insert(Lower(asset_path));
					record_validated_path(asset_path, current_hashes);
				}
				++end;
			}
			i = end;
			continue;
		}
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		SectionData section = ParseSection(lines, i, end);
		if (!section.identity_key.empty()) {
			std::wstring asset_path;
			std::vector<AssetHashObservation> current_hashes;
			if (ResolveObservedPath(
					section,
					observations,
					legacy_hash_identities,
					ambiguous_hashes,
					&asset_path,
					&current_hashes)) {
				validated_identity_paths.insert(Lower(asset_path));
				record_validated_path(asset_path, current_hashes);
			}
		}
		i = end;
	}

	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			size_t end = i + 1;
			while (end < lines.size() && Trim(lines[end]) != kBlockEnd)
				++end;
			if (end < lines.size())
				++end;
			i = end;
			continue;
		}
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		SectionData section = ParseSection(lines, i, end);
		if (!section.identity_key.empty()) {
			i = end;
			continue;
		}
		if (section.hashes.size() == 1 &&
				ambiguous_hashes.find(section.hashes.front().hash) ==
					ambiguous_hashes.end()) {
			const uint32_t hash = section.hashes.front().hash;
			std::wstring asset_path;
			auto validated = validated_hash_paths.find(hash);
			if (validated != validated_hash_paths.end()) {
				asset_path = validated->second;
			} else {
				auto identity = legacy_hash_identities.find(hash);
				if (identity != legacy_hash_identities.end())
					asset_path = identity->second.asset_path;
			}
			if (!asset_path.empty()) {
				std::wstring path_key = Lower(asset_path);
				std::vector<std::wstring> comparable_body = section.body;
				TrimBodyBlankLines(&comparable_body);
				RemoveZeroMatchPriority(&comparable_body);
				auto group = legacy_path_groups.find(path_key);
				if (group == legacy_path_groups.end()) {
					legacy_path_groups.emplace(
						path_key,
						LegacyPathGroup{
							asset_path,
							section,
							comparable_body,
							true});
				} else if (group->second.comparable_body != comparable_body) {
					group->second.safe = false;
				}
			}
		}
		i = end;
	}

	std::vector<std::wstring> output;
	std::set<std::wstring> emitted_legacy_paths = validated_identity_paths;
	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			size_t end = i + 1;
			while (end < lines.size() && Trim(lines[end]) != kBlockEnd)
				++end;
			if (end == lines.size()) {
				output.insert(output.end(), lines.begin() + i, lines.end());
				break;
			}

			SectionData block = ParseBlock(lines, i, end);
			std::wstring asset_path;
			std::vector<AssetHashObservation> current_hashes;
			if (!ResolveObservedPath(
					block,
					observations,
					legacy_hash_identities,
					ambiguous_hashes,
					&asset_path,
					&current_hashes)) {
				if (NeedsPathSectionCanonicalization(block)) {
					if (remove_unverified_stream_hashes || block.hashes.empty()) {
						AppendPathBlock(
							&output,
							block,
							block.identity_value,
							game_version);
					} else {
						AppendPathBlockWithUnverifiedHashes(
							&output,
							block,
							block.identity_value,
							game_version,
							block.hashes);
					}
				} else if (block.identity_alias) {
					if (block.hashes.empty()) {
						AppendIdentitySection(&output, block);
					} else {
						AppendGeneratedBlock(
							&output,
							block,
							block.hashes,
							block.mip_multiplicities,
							game_version);
					}
				} else {
					output.insert(
						output.end(),
						lines.begin() + i,
						lines.begin() + end + 1);
				}
				i = end + 1;
				continue;
			}

			std::vector<AssetHashObservation> unverified_hashes;
			for (const AssetHashObservation& stored : block.hashes) {
				bool current = std::any_of(
					current_hashes.begin(),
					current_hashes.end(),
					[&stored](const AssetHashObservation& observed) {
						return observed.hash == stored.hash;
					});
				if (!current)
					unverified_hashes.push_back(stored);
			}
			if (remove_unverified_stream_hashes) {
				AppendPathBlock(&output, block, asset_path, game_version);
			} else {
				AppendPathBlockWithUnverifiedHashes(
					&output,
					block,
					asset_path,
					game_version,
					unverified_hashes);
			}
			i = end + 1;
			continue;
		}

		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			output.push_back(lines[i++]);
			continue;
		}

		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		SectionData section = ParseSection(lines, i, end);

		if (!section.identity_key.empty()) {
			std::wstring asset_path;
			std::vector<AssetHashObservation> current_hashes;
			if (ResolveObservedPath(
					section,
					observations,
					legacy_hash_identities,
					ambiguous_hashes,
					&asset_path,
					&current_hashes)) {
				AppendPathBlock(&output, section, asset_path, game_version);
			} else if (section.identity_alias) {
				AppendIdentitySection(&output, section);
			} else {
				output.insert(
					output.end(),
					lines.begin() + i,
					lines.begin() + end);
			}
			i = end;
			continue;
		}

		bool converted = false;
		if (section.hashes.size() == 1 &&
				ambiguous_hashes.find(section.hashes.front().hash) ==
					ambiguous_hashes.end()) {
			const uint32_t hash = section.hashes.front().hash;
			std::wstring asset_path;
			auto validated = validated_hash_paths.find(hash);
			if (validated != validated_hash_paths.end()) {
				asset_path = validated->second;
			} else {
				auto identity = legacy_hash_identities.find(hash);
				if (identity != legacy_hash_identities.end())
					asset_path = identity->second.asset_path;
			}
			if (!asset_path.empty()) {
				std::wstring path_key = Lower(asset_path);
				auto group = legacy_path_groups.find(path_key);
				if (group != legacy_path_groups.end() && group->second.safe) {
					if (emitted_legacy_paths.insert(path_key).second) {
						AppendPathBlock(
							&output,
							group->second.source,
							group->second.asset_path,
							game_version);
					}
					converted = true;
				}
			}
		}
		if (!converted) {
			output.insert(
				output.end(),
				lines.begin() + i,
				lines.begin() + end);
		}
		i = end;
	}
	return JoinLines(output);
}

std::wstring TransformAssetHashIniDocumentToPaths(
	const std::wstring& source,
	const AssetHashObservationMap& observations,
	const AssetHashPathIdentityMap& legacy_hash_identities,
	const std::set<uint32_t>& ambiguous_hashes,
	const std::wstring& game_version)
{
	return TransformAssetHashIniDocumentToPathsInternal(
		source,
		observations,
		legacy_hash_identities,
		ambiguous_hashes,
		game_version,
		false);
}

std::wstring TransformAssetHashIniDocumentToCleanPaths(
	const std::wstring& source,
	const AssetHashObservationMap& observations,
	const AssetHashPathIdentityMap& legacy_hash_identities,
	const std::set<uint32_t>& ambiguous_hashes,
	const std::wstring& game_version)
{
	return TransformAssetHashIniDocumentToPathsInternal(
		source,
		observations,
		legacy_hash_identities,
		ambiguous_hashes,
		game_version,
		true);
}

namespace
{
struct VbDrawSignature
{
	uint32_t first_index;
	uint32_t index_count;

	bool operator<(const VbDrawSignature& other) const
	{
		return first_index < other.first_index ||
			(first_index == other.first_index && index_count < other.index_count);
	}

	bool operator==(const VbDrawSignature& other) const
	{
		return first_index == other.first_index &&
			index_count == other.index_count;
	}
};

struct VbFamilyKey
{
	std::wstring variable;
	uint32_t old_hash;

	bool operator<(const VbFamilyKey& other) const
	{
		return variable < other.variable ||
			(variable == other.variable && old_hash < other.old_hash);
	}
};

struct VbCandidateSection
{
	size_t begin;
	size_t end;
	VbFamilyKey family;
	VbDrawSignature signature;
};

bool ParseDecimal(const std::wstring& value, uint32_t *number)
{
	wchar_t *end = nullptr;
	unsigned long parsed = wcstoul(value.c_str(), &end, 10);
	if (end == value.c_str() || *Trim(end).c_str())
		return false;
	*number = static_cast<uint32_t>(parsed);
	return true;
}

std::set<std::wstring> ExtractVariables(
	const std::vector<std::wstring>& lines,
	size_t begin,
	size_t end)
{
	std::set<std::wstring> variables;
	for (size_t i = begin; i < end; ++i) {
		for (size_t offset = 0;
				(offset = lines[i].find(L'$', offset)) != std::wstring::npos;) {
			size_t finish = offset + 1;
			while (finish < lines[i].size() &&
					(iswalnum(lines[i][finish]) ||
					 lines[i][finish] == L'_' ||
					 lines[i][finish] == L'\\'))
				++finish;
			if (finish > offset + 1)
				variables.insert(Lower(lines[i].substr(offset, finish - offset)));
			offset = finish;
		}
	}
	return variables;
}

void AddPathVariables(
	std::map<std::wstring, std::set<std::wstring>> *path_variables,
	const SectionData& section,
	const std::vector<std::wstring>& lines,
	size_t begin,
	size_t end)
{
	if (section.identity_name != L"match_asset_path")
		return;
	std::set<std::wstring> variables = ExtractVariables(lines, begin, end);
	(*path_variables)[Lower(section.identity_value)].insert(
		variables.begin(),
		variables.end());
}
}

std::wstring TransformVbHashIniDocument(
	const std::wstring& source,
	const VbHashObservationList& observations,
	const ShapeKeyHashObservationList& shape_key_observations,
	bool allow_pathless_observations)
{
	std::vector<std::wstring> lines = SplitLines(source);
	std::map<std::wstring, std::set<std::wstring>> path_variables;
	std::set<std::wstring> referenced_variables;

	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			size_t end = i + 1;
			while (end < lines.size() && Trim(lines[end]) != kBlockEnd)
				++end;
			if (end == lines.size())
				break;
			SectionData block = ParseBlock(lines, i, end);
			AddPathVariables(&path_variables, block, lines, i + 1, end);
			i = end + 1;
			continue;
		}

		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name)) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		if (StartsWithInsensitive(section_name, L"TextureOverride")) {
			SectionData section = ParseSection(lines, i, end);
			AddPathVariables(&path_variables, section, lines, i + 1, end);
		}
		i = end;
	}
	for (const auto& path : path_variables) {
		referenced_variables.insert(
			path.second.begin(),
			path.second.end());
	}

	std::vector<VbCandidateSection> candidates;
	std::map<VbFamilyKey, std::set<VbDrawSignature>> family_signatures;
	std::map<std::wstring, std::set<uint32_t>> primary_family_hashes;
	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			while (i < lines.size() && Trim(lines[i]) != kBlockEnd)
				++i;
			if (i < lines.size())
				++i;
			continue;
		}
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;

		uint32_t hash = 0;
		uint32_t first_index = 0;
		uint32_t index_count = 0;
		bool has_hash = false;
		bool has_first_index = false;
		bool has_index_count = false;
		std::set<std::wstring> assigned_variables;
		for (size_t line = i + 1; line < end; ++line) {
			std::wstring key;
			std::wstring value;
			bool commented = false;
			if (!ParseAssignment(lines[line], &key, &value, &commented) || commented)
				continue;
			if (key == L"hash")
				has_hash = ParseHash(value, &hash);
			else if (key == L"match_first_index")
				has_first_index = ParseDecimal(value, &first_index);
			else if (key == L"match_index_count")
				has_index_count = ParseDecimal(value, &index_count);
			else if (!key.empty() && key.front() == L'$' && Trim(value) == L"1")
				assigned_variables.insert(key);
		}

		std::vector<std::wstring> family_variables;
		for (const std::wstring& variable : assigned_variables) {
			if (referenced_variables.find(variable) != referenced_variables.end())
				family_variables.push_back(variable);
		}
		if (has_hash && has_first_index && has_index_count &&
				family_variables.size() == 1) {
			VbFamilyKey family = {family_variables.front(), hash};
			VbDrawSignature signature = {first_index, index_count};
			candidates.push_back({i, end, family, signature});
			family_signatures[family].insert(signature);
			std::wstring lowered_name = Lower(section_name);
			const std::wstring component_prefix =
				L"textureoverridecomponent";
			if (lowered_name.find(component_prefix) == 0) {
				size_t position = component_prefix.size();
				while (position < lowered_name.size() &&
						iswdigit(lowered_name[position]))
					++position;
				std::wstring variable_suffix;
				size_t suffix = family.variable.rfind(L"_ib");
				if (suffix != std::wstring::npos)
					variable_suffix = family.variable.substr(suffix);
				if (position > component_prefix.size() &&
						lowered_name.substr(position) == variable_suffix)
					primary_family_hashes[family.variable].insert(hash);
			}
		}
		i = end;
	}

	struct ObservedDrawFamily
	{
		std::set<VbDrawSignature> signatures;
		std::set<uint32_t> vertex_counts;
	};
	typedef std::map<uint32_t, ObservedDrawFamily> ObservedHashSignatures;
	std::map<std::wstring, ObservedHashSignatures> observed_signatures;
	for (const VbHashObservation& observation : observations) {
		if (allow_pathless_observations && observation.asset_path.empty()) {
			VbDrawSignature signature = {
				observation.first_index,
				observation.index_count};
			for (const auto& family : family_signatures) {
				ObservedDrawFamily& observed =
					observed_signatures[family.first.variable][observation.hash];
				observed.signatures.insert(signature);
				if (observation.vertex_count)
					observed.vertex_counts.insert(observation.vertex_count);
			}
			continue;
		}
		auto path = path_variables.find(Lower(observation.asset_path));
		if (path == path_variables.end() || !observation.hash)
			continue;
		VbDrawSignature signature = {
			observation.first_index,
			observation.index_count};
		for (const std::wstring& variable : path->second) {
			ObservedDrawFamily& family =
				observed_signatures[variable][observation.hash];
			family.signatures.insert(signature);
			if (observation.vertex_count)
				family.vertex_counts.insert(observation.vertex_count);
		}
	}

	struct VbFamilyUpdate
	{
		uint32_t hash;
		uint32_t vertex_count;
		std::map<VbDrawSignature, VbDrawSignature> signatures;
	};
	std::map<VbFamilyKey, VbFamilyUpdate> replacements;
	for (const auto& family : family_signatures) {
		size_t equivalent_families = 0;
		for (const auto& other : family_signatures) {
			if (other.first.variable == family.first.variable &&
					other.second == family.second)
				++equivalent_families;
		}
		if (equivalent_families != 1)
			continue;

		auto variable_observations = observed_signatures.find(family.first.variable);
		if (variable_observations == observed_signatures.end())
			continue;
		std::vector<std::pair<uint32_t, std::vector<VbDrawSignature>>>
			matches;
		for (const auto& observed : variable_observations->second) {
			if (observed.second.signatures == family.second) {
				matches.push_back({
					observed.first,
					std::vector<VbDrawSignature>(
						observed.second.signatures.begin(),
						observed.second.signatures.end())});
				continue;
			}

			std::vector<VbDrawSignature> old_signatures(
				family.second.begin(), family.second.end());
			size_t same_shape_families = 0;
			for (const auto& other : family_signatures) {
				if (other.first.variable != family.first.variable ||
						other.second.size() != old_signatures.size())
					continue;
				bool contiguous = true;
				VbDrawSignature previous = {};
				bool has_previous = false;
				for (const VbDrawSignature& signature : other.second) {
					if (has_previous &&
							previous.first_index + previous.index_count !=
								signature.first_index) {
						contiguous = false;
						break;
					}
					previous = signature;
					has_previous = true;
				}
				if (contiguous)
					++same_shape_families;
			}
			if (same_shape_families != 1)
				continue;
			std::vector<VbDrawSignature> new_signatures(
				observed.second.signatures.begin(),
				observed.second.signatures.end());
			if (old_signatures.size() < 2 ||
					new_signatures.size() < old_signatures.size())
				continue;
			bool old_contiguous = true;
			for (size_t i = 1; i < old_signatures.size(); ++i) {
				if (old_signatures[i - 1].first_index +
						old_signatures[i - 1].index_count !=
						old_signatures[i].first_index) {
					old_contiguous = false;
					break;
				}
			}
			if (!old_contiguous)
				continue;
			for (size_t begin = 0;
					begin + old_signatures.size() <= new_signatures.size();
					++begin) {
				bool contiguous = true;
				for (size_t i = 1; i < old_signatures.size(); ++i) {
					const VbDrawSignature& previous =
						new_signatures[begin + i - 1];
					if (previous.first_index + previous.index_count !=
							new_signatures[begin + i].first_index) {
						contiguous = false;
						break;
					}
				}
				if (contiguous) {
					matches.push_back({
						observed.first,
						std::vector<VbDrawSignature>(
							new_signatures.begin() + begin,
							new_signatures.begin() + begin +
								old_signatures.size())});
				}
			}
		}
		if (matches.size() != 1)
			continue;
		const ObservedDrawFamily& observed =
			variable_observations->second[matches.front().first];
		VbFamilyUpdate update = {
			matches.front().first,
			observed.vertex_counts.size() == 1
				? *observed.vertex_counts.begin()
				: 0,
			{}};
		std::vector<VbDrawSignature> old_signatures(
			family.second.begin(), family.second.end());
		for (size_t i = 0; i < old_signatures.size(); ++i)
			update.signatures[old_signatures[i]] = matches.front().second[i];
		replacements[family.first] = std::move(update);
	}

	for (const VbCandidateSection& candidate : candidates) {
		auto replacement = replacements.find(candidate.family);
		if (replacement == replacements.end())
			continue;
		for (size_t line = candidate.begin + 1; line < candidate.end; ++line) {
			std::wstring key;
			std::wstring value;
			bool commented = false;
			if (!ParseAssignment(lines[line], &key, &value, &commented) || commented)
				continue;
			std::wstring replacement_text;
			if (key == L"hash" &&
					replacement->second.hash != candidate.family.old_hash) {
				wchar_t hash_text[9];
				swprintf(hash_text, 9, L"%08x", replacement->second.hash);
				replacement_text = hash_text;
			} else {
				auto signature = replacement->second.signatures.find(
					candidate.signature);
				if (signature == replacement->second.signatures.end())
					continue;
				if (key == L"match_first_index" &&
						signature->second.first_index !=
							candidate.signature.first_index)
					replacement_text = std::to_wstring(
						signature->second.first_index);
				else if (key == L"match_index_count" &&
						signature->second.index_count !=
							candidate.signature.index_count)
					replacement_text = std::to_wstring(
						signature->second.index_count);
			}
			if (!replacement_text.empty()) {
				size_t equals = lines[line].find(L'=');
				lines[line] = lines[line].substr(0, equals + 1) +
					L" " + replacement_text;
			}
		}
	}

	struct ShapeKeyResourceState
	{
		uint32_t byte_width = 0;
		uint32_t stages = 0;
		bool uav0 = false;
		bool uav1 = false;
		bool ambiguous_width = false;
	};
	std::map<uint32_t, ShapeKeyResourceState> shape_resources;
	for (const ShapeKeyHashObservation& observation : shape_key_observations) {
		if (!observation.hash || !observation.byte_width)
			continue;
		ShapeKeyResourceState& resource = shape_resources[observation.hash];
		if (resource.byte_width &&
				resource.byte_width != observation.byte_width)
			resource.ambiguous_width = true;
		else
			resource.byte_width = observation.byte_width;
		if (observation.filter_index == 3333)
			resource.stages |= 1;
		else if (observation.filter_index == 4444)
			resource.stages |= 2;
		if (observation.unordered_access && observation.slot == 0)
			resource.uav0 = true;
		if (observation.unordered_access && observation.slot == 1)
			resource.uav1 = true;
	}

	struct ShapeKeyRoot
	{
		uint32_t old_hash;
		bool offsets;
	};
	std::map<std::pair<std::wstring, uint32_t>, std::vector<ShapeKeyRoot>>
		shape_roots;
	auto component_suffix = [](const std::wstring& section_name) {
		std::wstring lowered = Lower(section_name);
		size_t suffix = lowered.rfind(L"_ib");
		if (suffix == std::wstring::npos || suffix + 3 == lowered.size())
			return std::wstring();
		for (size_t i = suffix + 3; i < lowered.size(); ++i) {
			if (!iswdigit(lowered[i]))
				return std::wstring();
		}
		return lowered.substr(suffix);
	};
	for (size_t i = 0; i < lines.size();) {
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		std::wstring lowered = Lower(section_name);
		bool offsets = lowered.find(L"shapekeyoffsets") !=
			std::wstring::npos;
		bool scale = lowered.find(L"shapekeyscale") !=
			std::wstring::npos;
		if (offsets || scale) {
			const std::wstring token = offsets
				? L"shapekeyoffsets"
				: L"shapekeyscale";
			size_t token_end = lowered.find(token) + token.size();
			size_t suffix_begin = lowered.rfind(L"_ib");
			if (suffix_begin == std::wstring::npos)
				suffix_begin = lowered.size();
			std::wstring host_suffix = lowered.substr(
				token_end, suffix_begin - token_end);
			uint32_t host_hash = 0;
			if (!host_suffix.empty() &&
					(host_suffix.size() != 9 || host_suffix.front() != L'_' ||
					 !ParseHash(host_suffix.substr(1), &host_hash))) {
				i = end;
				continue;
			}
			uint32_t hash = 0;
			for (size_t line = i + 1; line < end; ++line) {
				std::wstring key;
				std::wstring value;
				bool commented = false;
				if (ParseAssignment(lines[line], &key, &value, &commented) &&
						!commented && key == L"hash" &&
						ParseHash(value, &hash))
					break;
			}
			if (hash) {
				std::wstring variable =
					L"$object_detected" + component_suffix(section_name);
				shape_roots[{variable, host_hash}].push_back({hash, offsets});
			}
		}
		i = end;
	}

	std::map<uint32_t, uint32_t> shape_replacements;
	std::set<uint32_t> observed_target_hashes;
	std::set<uint32_t> observed_target_vertex_counts;
	for (const VbHashObservation& observation : observations) {
		if (observation.hash)
			observed_target_hashes.insert(observation.hash);
		if (observation.vertex_count)
			observed_target_vertex_counts.insert(observation.vertex_count);
	}
	bool has_target_shape_roots = false;
	bool target_shape_roots_resolved = true;
	for (const auto& roots : shape_roots) {
		uint32_t host_hash = roots.first.second;
		if (!host_hash) {
			auto primary = primary_family_hashes.find(roots.first.first);
			if (primary == primary_family_hashes.end() ||
					primary->second.size() != 1)
				continue;
			host_hash = *primary->second.begin();
		}
		auto family = replacements.find({roots.first.first, host_hash});
		uint64_t vertex_count = 0;
		if (family != replacements.end()) {
			vertex_count = family->second.vertex_count;
		} else if (observed_target_hashes.size() == 1 &&
				observed_target_vertex_counts.size() == 1 &&
				*observed_target_hashes.begin() == host_hash) {
			vertex_count = *observed_target_vertex_counts.begin();
		} else {
			continue;
		}
		has_target_shape_roots = true;
		size_t offsets_roots = 0;
		size_t scale_roots = 0;
		uint32_t old_offsets = 0;
		uint32_t old_scale = 0;
		for (const ShapeKeyRoot& root : roots.second) {
			if (root.offsets) {
				++offsets_roots;
				old_offsets = root.old_hash;
			} else {
				++scale_roots;
				old_scale = root.old_hash;
			}
		}
		if (offsets_roots != 1 || scale_roots != 1) {
			target_shape_roots_resolved = false;
			continue;
		}

		uint32_t current_offsets = 0;
		uint32_t current_scale = 0;
		size_t offsets_matches = 0;
		size_t scale_matches = 0;
		for (const auto& resource : shape_resources) {
			if (resource.second.ambiguous_width ||
					!resource.second.stages)
				continue;
			if (resource.second.uav0 &&
					resource.second.stages == 3 &&
					resource.second.byte_width == vertex_count * 24) {
				current_offsets = resource.first;
				++offsets_matches;
			}
			if (resource.second.uav1 &&
					resource.second.stages == 3 &&
					resource.second.byte_width == vertex_count * 4) {
				current_scale = resource.first;
				++scale_matches;
			}
		}
		if (offsets_matches != 1 || scale_matches != 1) {
			target_shape_roots_resolved = false;
			continue;
		}
		if (current_offsets != old_offsets)
			shape_replacements[old_offsets] = current_offsets;
		if (current_scale != old_scale)
			shape_replacements[old_scale] = current_scale;
	}
	if (has_target_shape_roots && !target_shape_roots_resolved)
		return source;

	if (!shape_replacements.empty()) {
		for (size_t i = 0; i < lines.size();) {
			std::wstring section_name;
			if (!ParseSectionHeader(lines[i], &section_name) ||
					!StartsWithInsensitive(section_name, L"TextureOverride")) {
				++i;
				continue;
			}
			size_t end = i + 1;
			std::wstring next_section;
			while (end < lines.size() &&
					!ParseSectionHeader(lines[end], &next_section) &&
					Trim(lines[end]) != kBlockBegin)
				++end;
			if (Lower(section_name).find(L"shapekey") == std::wstring::npos) {
				i = end;
				continue;
			}
			for (size_t line = i + 1; line < end; ++line) {
				std::wstring key;
				std::wstring value;
				bool commented = false;
				uint32_t hash = 0;
				if (!ParseAssignment(lines[line], &key, &value, &commented) ||
						commented || key != L"hash" ||
						!ParseHash(value, &hash))
					continue;
				auto replacement = shape_replacements.find(hash);
				if (replacement == shape_replacements.end())
					continue;
				wchar_t hash_text[9];
				swprintf(hash_text, 9, L"%08x", replacement->second);
				size_t equals = lines[line].find(L'=');
				lines[line] = lines[line].substr(0, equals + 1) +
					L" " + hash_text;
			}
			i = end;
		}
	}
	return JoinLines(lines);
}

bool CollectVbHashIniMeshVertexCount(
	const std::wstring& source,
	uint32_t *vertex_count)
{
	if (!vertex_count)
		return false;
	std::vector<std::wstring> lines = SplitLines(source);
	bool in_constants = false;
	bool found = false;
	uint32_t parsed = 0;
	for (const std::wstring& line : lines) {
		std::wstring section;
		if (ParseSectionHeader(line, &section)) {
			in_constants = !_wcsicmp(section.c_str(), L"Constants");
			continue;
		}
		if (!in_constants)
			continue;
		std::wstring trimmed = Lower(Trim(line));
		const std::wstring key = L"global $mesh_vertex_count";
		if (trimmed.compare(0, key.size(), key))
			continue;
		size_t equals = trimmed.find(L'=', key.size());
		if (equals == std::wstring::npos ||
				!Trim(trimmed.substr(key.size(), equals - key.size())).empty())
			continue;
		uint32_t value = 0;
		if (!ParseDecimal(Trim(trimmed.substr(equals + 1)), &value) || !value)
			continue;
		if (found && parsed != value)
			return false;
		parsed = value;
		found = true;
	}
	if (!found)
		return false;
	*vertex_count = parsed;
	return true;
}

std::set<uint32_t> CollectVbHashIniCandidates(
	const std::wstring& source)
{
	std::vector<std::wstring> lines = SplitLines(source);
	std::set<std::wstring> path_variables;
	std::set<uint32_t> hashes;
	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			size_t end = i + 1;
			while (end < lines.size() && Trim(lines[end]) != kBlockEnd)
				++end;
			if (end == lines.size())
				break;
			SectionData block = ParseBlock(lines, i, end);
			if (block.identity_name == L"match_asset_path") {
				std::set<std::wstring> variables =
					ExtractVariables(lines, i + 1, end);
				path_variables.insert(variables.begin(), variables.end());
			}
			i = end + 1;
			continue;
		}
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name)) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		if (StartsWithInsensitive(section_name, L"TextureOverride")) {
			SectionData section = ParseSection(lines, i, end);
			if (section.identity_name == L"match_asset_path") {
				std::set<std::wstring> variables =
					ExtractVariables(lines, i + 1, end);
				path_variables.insert(variables.begin(), variables.end());
			}
		}
		i = end;
	}

	for (size_t i = 0; i < lines.size();) {
		if (Trim(lines[i]) == kBlockBegin) {
			while (i < lines.size() && Trim(lines[i]) != kBlockEnd)
				++i;
			if (i < lines.size())
				++i;
			continue;
		}
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		uint32_t hash = 0;
		bool has_hash = false;
		bool has_first_index = false;
		bool has_index_count = false;
		bool has_path_variable = false;
		for (size_t line = i + 1; line < end; ++line) {
			std::wstring key;
			std::wstring value;
			bool commented = false;
			if (!ParseAssignment(lines[line], &key, &value, &commented) || commented)
				continue;
			if (key == L"hash")
				has_hash = ParseHash(value, &hash);
			else if (key == L"match_first_index")
				has_first_index = true;
			else if (key == L"match_index_count")
				has_index_count = true;
			else if (!key.empty() && key.front() == L'$' &&
					Trim(value) == L"1" &&
					path_variables.find(key) != path_variables.end())
				has_path_variable = true;
		}
		if (has_hash && has_first_index && has_index_count && has_path_variable)
			hashes.insert(hash);
		i = end;
	}
	return hashes;
}

std::set<std::tuple<uint32_t, uint32_t, uint32_t>>
CollectVbHashIniSignatures(const std::wstring& source)
{
	std::vector<std::wstring> lines = SplitLines(source);
	std::set<std::tuple<uint32_t, uint32_t, uint32_t>> signatures;
	for (size_t i = 0; i < lines.size();) {
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride")) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		uint32_t hash = 0;
		uint32_t first_index = 0;
		uint32_t index_count = 0;
		bool has_hash = false;
		bool has_first_index = false;
		bool has_index_count = false;
		for (size_t line = i + 1; line < end; ++line) {
			std::wstring key;
			std::wstring value;
			bool commented = false;
			if (!ParseAssignment(lines[line], &key, &value, &commented) || commented)
				continue;
			if (key == L"hash")
				has_hash = ParseHash(value, &hash);
			else if (key == L"match_first_index")
				has_first_index = ParseDecimal(value, &first_index);
			else if (key == L"match_index_count")
				has_index_count = ParseDecimal(value, &index_count);
		}
		if (has_hash && has_first_index && has_index_count)
			signatures.insert({hash, first_index, index_count});
		i = end;
	}
	return signatures;
}

std::set<std::pair<uint32_t, uint32_t>>
CollectVbHashIniDrawSignatures(const std::wstring& source)
{
	std::set<std::pair<uint32_t, uint32_t>> signatures;
	for (const auto& signature : CollectVbHashIniSignatures(source)) {
		signatures.insert({std::get<1>(signature), std::get<2>(signature)});
	}
	return signatures;
}

std::set<uint32_t> CollectShapeKeyHashIniCandidates(
	const std::wstring& source)
{
	std::vector<std::wstring> lines = SplitLines(source);
	std::set<uint32_t> hashes;
	for (size_t i = 0; i < lines.size();) {
		std::wstring section_name;
		if (!ParseSectionHeader(lines[i], &section_name) ||
				!StartsWithInsensitive(section_name, L"TextureOverride") ||
				Lower(section_name).find(L"shapekey") == std::wstring::npos) {
			++i;
			continue;
		}
		size_t end = i + 1;
		std::wstring next_section;
		while (end < lines.size() &&
				!ParseSectionHeader(lines[end], &next_section) &&
				Trim(lines[end]) != kBlockBegin)
			++end;
		for (size_t line = i + 1; line < end; ++line) {
			std::wstring key;
			std::wstring value;
			bool commented = false;
			uint32_t hash = 0;
			if (ParseAssignment(lines[line], &key, &value, &commented) &&
					!commented && key == L"hash" && ParseHash(value, &hash)) {
				hashes.insert(hash);
				break;
			}
		}
		i = end;
	}
	return hashes;
}
