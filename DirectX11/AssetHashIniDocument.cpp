#include "AssetHashIniDocument.h"

#include <algorithm>
#include <cwctype>
#include <set>
#include <sstream>

namespace
{
const wchar_t *kBlockBegin = L"; <asset-hash-stream>";
const wchar_t *kBlockEnd = L"; </asset-hash-stream>";

struct SectionData
{
	std::wstring identity_key;
	std::wstring identity_name;
	std::wstring identity_value;
	std::wstring section_name;
	std::vector<std::wstring> body;
	std::vector<AssetHashObservation> hashes;
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

std::wstring IdentityKey(
	const std::wstring& name,
	const std::wstring& value)
{
	return Lower(name) + L"=" + Lower(value);
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

		std::wstring key;
		std::wstring value;
		bool commented = false;
		if (ParseAssignment(lines[i], &key, &value, &commented)) {
			if ((key == L"match_asset_path" ||
					key == L"match_asset_name") &&
					data.identity_key.empty()) {
				data.identity_name = key;
				data.identity_value = value;
				data.identity_key = IdentityKey(key, value);
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
			if ((key == L"match_asset_path" ||
					key == L"match_asset_name") &&
					data.identity_key.empty()) {
				data.identity_name = key;
				data.identity_value = value;
				data.identity_key = IdentityKey(key, value);
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

void MergeHashObservations(
	std::vector<AssetHashObservation> *hashes,
	const std::vector<AssetHashObservation>& observed)
{
	AppendHashObservations(hashes, observed);
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

void AppendIdentitySection(
	std::vector<std::wstring> *output,
	const SectionData& section)
{
	output->push_back(L"[" + section.section_name + L"]");
	output->push_back(
		section.identity_name + L" = " + section.identity_value);
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

std::wstring GeneratedSectionName(
	const std::wstring& source_name,
	const std::wstring& hash_text)
{
	std::wstring base_name = source_name;
	std::wstring lowered = Lower(base_name);
	size_t legacy_suffix = lowered.find(L"_assethash_");
	if (legacy_suffix != std::wstring::npos)
		base_name.erase(legacy_suffix);

	size_t separator = base_name.rfind(L'_');
	if (separator != std::wstring::npos &&
			base_name.size() - separator - 1 == 8) {
		bool hash_suffix = true;
		for (size_t i = separator + 1; i < base_name.size(); ++i) {
			if (!iswxdigit(base_name[i])) {
				hash_suffix = false;
				break;
			}
		}
		if (hash_suffix)
			return base_name.substr(0, separator + 1) + hash_text;
	}
	return base_name + L"_" + hash_text;
}

void AppendGeneratedBlock(
	std::vector<std::wstring> *output,
	const SectionData& source,
	std::vector<AssetHashObservation> hashes,
	const std::wstring& game_version)
{
	NormalizeHashes(&hashes);
	std::vector<std::wstring> body = source.body;
	TrimBodyBlankLines(&body);
	output->push_back(kBlockBegin);
	output->push_back(
		L"; " + source.identity_name + L" = " + source.identity_value);
	output->push_back(L"; asset_hash_compiler_version = Ver1.0");
	output->push_back(L"; game_version = " + game_version);

	for (size_t i = 0; i < hashes.size(); ++i) {
		const AssetHashObservation& observation = hashes[i];
		if (i)
			output->push_back(L"");
		if (observation.width && observation.height) {
			output->push_back(
				L"; " + std::to_wstring(observation.width) + L"x" +
				std::to_wstring(observation.height));
		} else {
			output->push_back(L"; unknown-resolution");
		}
		wchar_t hash_text[9];
		swprintf(hash_text, 9, L"%08x", observation.hash);
		std::wstring section_name = GeneratedSectionName(
			source.section_name,
			hash_text);
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
	for (const std::wstring& line : lines) {
		std::wstring key;
		std::wstring value;
		bool commented = false;
		if (!ParseAssignment(line, &key, &value, &commented))
			continue;
		if (key == L"match_asset_path" ||
				key == L"match_asset_name")
			identities.insert(IdentityKey(key, value));
	}
	return identities;
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
			auto old = previous.find(block.identity_key);
			if (old != previous.end())
				AppendHashObservations(&hashes, old->second.hashes);
			auto observed = observations.find(block.identity_key);
				if (observed != observations.end()) {
					MergeHashObservations(
						&hashes,
						observed->second);
				}
				RemoveAmbiguousHashes(&hashes, ambiguous_hashes);
				if (hashes.empty())
					AppendIdentitySection(&output, block);
				else
					AppendGeneratedBlock(
						&output,
						block,
						hashes,
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
		auto old = previous.find(section.identity_key);
		if (old != previous.end())
			AppendHashObservations(&hashes, old->second.hashes);
		auto observed = observations.find(section.identity_key);
			if (observed != observations.end()) {
				MergeHashObservations(
					&hashes,
					observed->second);
			}
			RemoveAmbiguousHashes(&hashes, ambiguous_hashes);

			if (hashes.empty()) {
				output.insert(
				output.end(),
				lines.begin() + i,
				lines.begin() + end);
		} else {
				AppendGeneratedBlock(
					&output,
					section,
					hashes,
					game_version);
		}
		i = end;
	}
	return JoinLines(output);
}
