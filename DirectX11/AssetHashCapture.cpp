#include "AssetHashCapture.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "AssetHashIniDocument.h"
#include "AssetPathTextureIdentity.h"
#include "globals.h"
#include "IniHandler.h"
#include "log.h"
#include "Overlay.h"
#include "ResourceHash.h"

namespace
{
enum class CaptureMode
{
	Off,
	Backup,
	Aggressive,
	PathConversion,
	CleanPathConversion,
};

constexpr size_t kMaxHistoricalIdentities = 8192;
constexpr size_t kMaxHashesPerIdentity = 32;
constexpr size_t kMaxHistoricalHashObservations = 32768;
constexpr size_t kMaxIdentityCharacters = 2048;
constexpr size_t kMaxRecentIdentities = 32768;
constexpr size_t kMaxRecentHashObservations = 131072;
constexpr size_t kMaxVbHashObservations = 65536;
constexpr ULONGLONG kRecentReleasedTtlMs = 300000;
SRWLOCK capture_lock = SRWLOCK_INIT;
HANDLE capture_event = nullptr;
LONG worker_started = 0;
CaptureMode capture_mode = CaptureMode::Off;
std::atomic<bool> capture_enabled(false);
bool capture_dirty = false;
DWORD status_until = 0;
std::vector<std::wstring> source_files;
std::set<std::wstring> watched_identities;
std::set<uint32_t> watched_legacy_hashes;
std::set<uint32_t> watched_shape_key_hashes;
std::set<std::tuple<uint32_t, uint32_t, uint32_t>> vb_probe_keys;
std::set<std::pair<uint32_t, uint32_t>> shape_key_probe_keys;
std::set<uint32_t> model_vertex_probe_hashes;
std::map<uint32_t, uint32_t> model_vertex_counts;
std::map<std::pair<uint32_t, uint32_t>, std::set<std::wstring>>
	model_draw_signature_sources;
std::map<std::wstring, size_t> model_draw_signature_counts;
std::map<uint32_t, std::set<std::pair<uint32_t, uint32_t>>>
	model_draw_signatures;
std::map<uint32_t, std::map<std::wstring, size_t>> model_source_scores;
std::wstring target_source_file;
uint32_t target_vb_hash = 0;
uint32_t target_vertex_count = 0;
bool target_profile_loaded = false;
AssetHashObservationMap captured_hashes;
size_t captured_observation_count = 0;
VbHashObservationList captured_vb_hashes;
std::set<std::tuple<std::wstring, uint32_t, uint32_t, uint32_t, uint32_t>>
	captured_vb_hash_keys;
ShapeKeyHashObservationList captured_shape_key_hashes;
std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool>>
	captured_shape_key_hash_keys;
std::map<std::wstring, std::wstring> observed_name_paths;
std::set<std::wstring> ambiguous_names;

struct RecentAssetHistory
{
	std::wstring asset_path;
	std::vector<AssetHashObservation> hashes;
	size_t live_resources;
	ULONGLONG expires_at;
	ULONGLONG last_seen;
};

std::map<std::wstring, RecentAssetHistory> recent_assets;
std::map<uintptr_t, std::wstring> active_recent_resources;
size_t recent_observation_count = 0;

void ResetCaptureSessionLocked()
{
	captured_hashes.clear();
	captured_observation_count = 0;
	captured_vb_hashes.clear();
	captured_vb_hash_keys.clear();
	captured_shape_key_hashes.clear();
	captured_shape_key_hash_keys.clear();
	vb_probe_keys.clear();
	shape_key_probe_keys.clear();
	model_vertex_probe_hashes.clear();
	model_vertex_counts.clear();
	model_draw_signature_sources.clear();
	model_draw_signature_counts.clear();
	model_draw_signatures.clear();
	model_source_scores.clear();
	target_source_file.clear();
	target_vb_hash = 0;
	target_vertex_count = 0;
	target_profile_loaded = false;
	observed_name_paths.clear();
	ambiguous_names.clear();
	recent_assets.clear();
	active_recent_resources.clear();
	recent_observation_count = 0;
	capture_dirty = false;
}

std::wstring Lower(std::wstring value)
{
	std::transform(value.begin(), value.end(), value.begin(), towlower);
	return value;
}

std::wstring IdentityKey(
	const std::wstring& name,
	const std::wstring& value)
{
	return Lower(name) + L"=" + Lower(value);
}

bool AddHistoricalObservation(
	const std::wstring& key,
	const AssetHashObservation& observation)
{
	auto identity = captured_hashes.find(key);
	if (identity == captured_hashes.end()) {
		if (captured_hashes.size() >= kMaxHistoricalIdentities ||
				captured_observation_count >=
					kMaxHistoricalHashObservations)
			return false;
		identity = captured_hashes.emplace(
			key,
			std::vector<AssetHashObservation>()).first;
	}
	std::vector<AssetHashObservation>& hashes = identity->second;
	auto existing = std::find_if(
		hashes.begin(),
		hashes.end(),
		[&observation](const AssetHashObservation& item) {
			return item.hash == observation.hash;
		});
	if (existing != hashes.end())
		return false;
	if (hashes.size() >= kMaxHashesPerIdentity ||
			captured_observation_count >=
				kMaxHistoricalHashObservations)
		return false;
	hashes.push_back(observation);
	++captured_observation_count;
	return true;
}

void EraseRecentAsset(
	std::map<std::wstring, RecentAssetHistory>::iterator asset)
{
	recent_observation_count -= asset->second.hashes.size();
	recent_assets.erase(asset);
}

void PruneReleasedRecentAssets(ULONGLONG now)
{
	for (auto asset = recent_assets.begin(); asset != recent_assets.end();) {
		if (!asset->second.live_resources &&
				asset->second.expires_at <= now) {
			recent_observation_count -= asset->second.hashes.size();
			asset = recent_assets.erase(asset);
		} else {
			++asset;
		}
	}
}

bool EvictOldestReleasedRecentAsset()
{
	auto oldest = recent_assets.end();
	for (auto asset = recent_assets.begin(); asset != recent_assets.end();
			++asset) {
		if (asset->second.live_resources)
			continue;
		if (oldest == recent_assets.end() ||
				asset->second.last_seen < oldest->second.last_seen)
			oldest = asset;
	}
	if (oldest == recent_assets.end())
		return false;
	EraseRecentAsset(oldest);
	return true;
}

bool EnsureRecentCapacity(bool needs_identity, bool needs_observation)
{
	PruneReleasedRecentAssets(GetTickCount64());
	while ((needs_identity &&
				recent_assets.size() >= kMaxRecentIdentities) ||
			(needs_observation &&
				recent_observation_count >=
					kMaxRecentHashObservations)) {
		if (!EvictOldestReleasedRecentAsset())
			return false;
	}
	return true;
}

bool AddRecentObservation(
	uintptr_t resource_address,
	const std::wstring& asset_path,
	const AssetHashObservation& observation)
{
	const std::wstring key = Lower(asset_path);
	auto asset = recent_assets.find(key);
	if (asset == recent_assets.end()) {
		if (!EnsureRecentCapacity(true, true))
			return false;
		RecentAssetHistory history = {
			asset_path,
			std::vector<AssetHashObservation>(),
			0,
			0,
			GetTickCount64()};
		asset = recent_assets.emplace(key, std::move(history)).first;
	}

	if (resource_address) {
		auto active = active_recent_resources.find(resource_address);
		if (active == active_recent_resources.end()) {
			active_recent_resources.emplace(resource_address, key);
			++asset->second.live_resources;
		} else if (active->second != key) {
			auto previous = recent_assets.find(active->second);
			if (previous != recent_assets.end() &&
					previous->second.live_resources) {
				if (!--previous->second.live_resources)
					previous->second.expires_at =
						GetTickCount64() + kRecentReleasedTtlMs;
			}
			active->second = key;
			++asset->second.live_resources;
		}
		asset->second.expires_at = 0;
	}
	asset->second.last_seen = GetTickCount64();

	auto existing = std::find_if(
		asset->second.hashes.begin(),
		asset->second.hashes.end(),
		[&observation](const AssetHashObservation& item) {
			return item.hash == observation.hash;
		});
	if (existing != asset->second.hashes.end())
		return false;
	if (asset->second.hashes.size() >= kMaxHashesPerIdentity ||
			!EnsureRecentCapacity(false, true))
		return false;
	asset->second.hashes.push_back(observation);
	++recent_observation_count;
	return true;
}

bool PromoteRecentObservations()
{
	PruneReleasedRecentAssets(GetTickCount64());
	std::map<std::wstring, std::wstring> unique_name_paths;
	std::set<std::wstring> duplicate_names;
	for (const auto& entry : recent_assets) {
		std::wstring name = Lower(
			ExtractUnrealAssetName(entry.second.asset_path));
		if (name.empty())
			continue;
		auto inserted = unique_name_paths.emplace(name, entry.first);
		if (!inserted.second && inserted.first->second != entry.first)
			duplicate_names.insert(name);
	}

	bool changed = false;
	for (const auto& entry : recent_assets) {
		const RecentAssetHistory& asset = entry.second;
		const std::wstring path_key =
			IdentityKey(L"match_asset_path", asset.asset_path);
		const std::wstring name =
			Lower(ExtractUnrealAssetName(asset.asset_path));
		const std::wstring name_key =
			IdentityKey(L"match_asset_name", name);
		const bool match_path =
			watched_identities.find(path_key) != watched_identities.end();
		const bool match_name =
			!name.empty() &&
			duplicate_names.find(name) == duplicate_names.end() &&
			watched_identities.find(name_key) != watched_identities.end();
		for (const AssetHashObservation& observation : asset.hashes) {
			if (match_path &&
					AddHistoricalObservation(path_key, observation))
				changed = true;
			if (match_name &&
					AddHistoricalObservation(name_key, observation))
				changed = true;
		}
	}
	return changed;
}

AssetHashPathIdentityMap BuildLegacyHashIdentitySnapshot()
{
	AssetHashPathIdentityMap identities;
	std::set<uint32_t> ambiguous_hashes;
	for (const auto& entry : recent_assets) {
		const RecentAssetHistory& asset = entry.second;
		for (const AssetHashObservation& observation : asset.hashes) {
			if (ambiguous_hashes.find(observation.hash) !=
					ambiguous_hashes.end())
				continue;
			auto inserted = identities.emplace(
				observation.hash,
				AssetHashPathIdentity{
					asset.asset_path,
					asset.hashes});
			if (!inserted.second &&
					_wcsicmp(
						inserted.first->second.asset_path.c_str(),
						asset.asset_path.c_str())) {
				identities.erase(inserted.first);
				ambiguous_hashes.insert(observation.hash);
			}
		}
	}
	return identities;
}

std::set<uint32_t> BuildAmbiguousHashSnapshot()
{
	std::map<uint32_t, std::wstring> hash_paths;
	std::set<uint32_t> ambiguous_hashes;
	for (const auto& entry : recent_assets) {
		for (const AssetHashObservation& observation :
				entry.second.hashes) {
			if (ambiguous_hashes.find(observation.hash) !=
					ambiguous_hashes.end())
				continue;
			auto inserted = hash_paths.emplace(
				observation.hash,
				entry.first);
			if (!inserted.second &&
					inserted.first->second != entry.first) {
				hash_paths.erase(inserted.first);
				ambiguous_hashes.insert(observation.hash);
			}
		}
	}
	return ambiguous_hashes;
}

bool ReadUtf8File(const std::wstring& path, std::wstring *text)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return false;
	std::string bytes(
		(std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());
	if (bytes.size() >= 3 &&
			static_cast<unsigned char>(bytes[0]) == 0xef &&
			static_cast<unsigned char>(bytes[1]) == 0xbb &&
			static_cast<unsigned char>(bytes[2]) == 0xbf)
		bytes.erase(0, 3);
	if (bytes.empty()) {
		text->clear();
		return true;
	}
	int length = MultiByteToWideChar(
		CP_UTF8,
		MB_ERR_INVALID_CHARS,
		bytes.data(),
		static_cast<int>(bytes.size()),
		nullptr,
		0);
	if (!length)
		return false;
	text->resize(length);
	return MultiByteToWideChar(
		CP_UTF8,
		MB_ERR_INVALID_CHARS,
		bytes.data(),
		static_cast<int>(bytes.size()),
		&(*text)[0],
		length) == length;
}

bool RemoveLastPathComponent(std::wstring *path)
{
	size_t separator = path->find_last_of(L"\\/");
	if (separator == std::wstring::npos)
		return false;
	path->resize(separator);
	return true;
}

bool ParseMountedGameVersion(
	const std::wstring& text,
	std::wstring *version,
	uint32_t components[3])
{
	size_t mount_begin = text.find(L"::Mount::");
	if (mount_begin == std::wstring::npos)
		return false;
	size_t mount_end = text.find(L"::Del::", mount_begin);
	if (mount_end == std::wstring::npos)
		mount_end = text.size();

	bool found = false;
	size_t search = mount_begin;
	while (search < mount_end) {
		size_t marker = text.find(L"Resource/", search);
		if (marker == std::wstring::npos || marker >= mount_end)
			break;
		size_t cursor = marker + 9;
		uint32_t parsed[3] = {};
		size_t version_begin = cursor;
		bool valid = true;
		for (size_t i = 0; i < 3; ++i) {
			if (cursor >= mount_end ||
					text[cursor] < L'0' ||
					text[cursor] > L'9') {
				valid = false;
				break;
			}
			uint64_t value = 0;
			while (cursor < mount_end &&
					text[cursor] >= L'0' &&
					text[cursor] <= L'9') {
				value = value * 10 + (text[cursor] - L'0');
				if (value > UINT32_MAX) {
					valid = false;
					break;
				}
				++cursor;
			}
			if (!valid)
				break;
			parsed[i] = static_cast<uint32_t>(value);
			wchar_t separator = i == 2 ? L'/' : L'.';
			if (cursor >= mount_end || text[cursor] != separator) {
				valid = false;
				break;
			}
			++cursor;
		}
		if (valid &&
				(!found ||
					parsed[0] > components[0] ||
					(parsed[0] == components[0] &&
						parsed[1] > components[1]) ||
					(parsed[0] == components[0] &&
						parsed[1] == components[1] &&
						parsed[2] > components[2]))) {
			found = true;
			for (size_t i = 0; i < 3; ++i)
				components[i] = parsed[i];
			version->assign(
				text,
				version_begin,
				cursor - version_begin - 1);
		}
		search = marker + 1;
	}
	return found;
}

std::wstring DetectMountedGameVersion()
{
	std::vector<wchar_t> module_path(32768);
	DWORD length = GetModuleFileNameW(
		nullptr,
		module_path.data(),
		static_cast<DWORD>(module_path.size()));
	if (!length || length >= module_path.size())
		return L"";

	std::wstring client_root(module_path.data(), length);
	if (!RemoveLastPathComponent(&client_root) ||
			!RemoveLastPathComponent(&client_root) ||
			!RemoveLastPathComponent(&client_root))
		return L"";
	std::wstring resources_root =
		client_root + L"\\Saved\\Resources";
	std::wstring search_path = resources_root + L"\\*";
	WIN32_FIND_DATAW data = {};
	HANDLE search = FindFirstFileW(search_path.c_str(), &data);
	if (search == INVALID_HANDLE_VALUE)
		return L"";

	std::wstring detected;
	uint32_t detected_components[3] = {};
	do {
		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
				!wcscmp(data.cFileName, L".") ||
				!wcscmp(data.cFileName, L".."))
			continue;
		std::wstring mount_path =
			resources_root + L"\\" + data.cFileName +
			L"\\Mount\\MountResource.txt";
		std::wstring mount;
		if (!ReadUtf8File(mount_path, &mount))
			continue;
		std::wstring candidate;
		uint32_t candidate_components[3] = {};
		if (!ParseMountedGameVersion(
				mount,
				&candidate,
				candidate_components))
			continue;
		if (detected.empty() ||
				candidate_components[0] > detected_components[0] ||
				(candidate_components[0] == detected_components[0] &&
					candidate_components[1] > detected_components[1]) ||
				(candidate_components[0] == detected_components[0] &&
					candidate_components[1] == detected_components[1] &&
					candidate_components[2] > detected_components[2])) {
			detected = candidate;
			for (size_t i = 0; i < 3; ++i)
				detected_components[i] = candidate_components[i];
		}
	} while (FindNextFileW(search, &data));
	FindClose(search);
	return detected;
}

bool EncodeUtf8(const std::wstring& text, std::string *bytes)
{
	if (text.empty()) {
		bytes->clear();
		return true;
	}
	int length = WideCharToMultiByte(
		CP_UTF8,
		0,
		text.data(),
		static_cast<int>(text.size()),
		nullptr,
		0,
		nullptr,
		nullptr);
	if (!length)
		return false;
	bytes->resize(length);
	return WideCharToMultiByte(
		CP_UTF8,
		0,
		text.data(),
		static_cast<int>(text.size()),
		&(*bytes)[0],
		length,
		nullptr,
		nullptr) == length;
}

bool AtomicWriteUtf8(
	const std::wstring& destination,
	const std::wstring& text)
{
	std::string bytes;
	if (!EncodeUtf8(text, &bytes))
		return false;

	wchar_t suffix[64];
	swprintf_s(
		suffix,
		L".%lu.%lu.tmp",
		GetCurrentProcessId(),
		GetCurrentThreadId());
	std::wstring temporary = destination + suffix;
	HANDLE file = CreateFileW(
		temporary.c_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	DWORD written = 0;
	bool success =
		(bytes.empty() ||
			(WriteFile(
				file,
				bytes.data(),
				static_cast<DWORD>(bytes.size()),
				&written,
				nullptr) &&
			 written == bytes.size())) &&
		FlushFileBuffers(file);
	CloseHandle(file);
	if (!success) {
		DeleteFileW(temporary.c_str());
		return false;
	}

	DWORD attributes = GetFileAttributesW(destination.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		success = !!ReplaceFileW(
			destination.c_str(),
			temporary.c_str(),
			nullptr,
			REPLACEFILE_WRITE_THROUGH,
			nullptr,
			nullptr);
	} else {
		success = !!MoveFileExW(
			temporary.c_str(),
			destination.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	}
	if (!success)
		DeleteFileW(temporary.c_str());
	return success;
}

void WriteSnapshot(
	const std::vector<std::wstring>& sources,
	const AssetHashObservationMap& observations,
	const VbHashObservationList& vb_observations,
	const ShapeKeyHashObservationList& shape_key_observations,
	const AssetHashPathIdentityMap& legacy_hash_identities,
	const std::set<uint32_t>& ambiguous_hashes,
	const std::wstring& target_source,
	CaptureMode mode)
{
	static const std::wstring game_version = []() {
		std::wstring detected = DetectMountedGameVersion();
		if (detected.empty()) {
			LogInfo(
				"> Asset Hash Capture could not detect mounted game version\n");
			return std::wstring(L"unknown");
		}
		LogInfo(
			"> Asset Hash Capture detected game version %ls\n",
			detected.c_str());
		return detected;
	}();
	for (const std::wstring& source_path : sources) {
		std::wstring source;
		if (!ReadUtf8File(source_path, &source))
			continue;
		const bool direct_write = mode != CaptureMode::Backup;
		std::wstring output_path =
			direct_write ? source_path : source_path + L".hashcache";
		std::wstring previous = direct_write ? source : std::wstring();
		if (!direct_write)
			ReadUtf8File(output_path, &previous);
		std::wstring transformed = mode == CaptureMode::PathConversion
			? TransformAssetHashIniDocumentToPaths(
				source,
				observations,
				legacy_hash_identities,
				ambiguous_hashes,
				game_version)
			: mode == CaptureMode::CleanPathConversion
			? TransformAssetHashIniDocumentToCleanPaths(
				source,
				observations,
				legacy_hash_identities,
				ambiguous_hashes,
				game_version)
			: TransformAssetHashIniDocument(
				source,
				previous,
				observations,
				legacy_hash_identities,
				ambiguous_hashes,
				game_version);
		if (!target_source.empty() &&
				!_wcsicmp(source_path.c_str(), target_source.c_str())) {
			transformed = TransformVbHashIniDocument(
				transformed,
				vb_observations,
				shape_key_observations,
				true);
		}
		if (transformed == previous)
			continue;
		if (!AtomicWriteUtf8(output_path, transformed)) {
			LogOverlayW(
				LOG_WARNING,
				L"Asset Hash Capture failed to update:\n%ls\n",
				output_path.c_str());
		}
	}
}

DWORD WINAPI CaptureWriterThread(void *)
{
	for (;;) {
		WaitForSingleObject(capture_event, INFINITE);
		Sleep(200);

		std::wstring profile_source;
		AcquireSRWLockShared(&capture_lock);
		if (!target_source_file.empty() && !target_profile_loaded)
			profile_source = target_source_file;
		ReleaseSRWLockShared(&capture_lock);
		if (!profile_source.empty()) {
			std::wstring profile_document;
			if (ReadUtf8File(profile_source, &profile_document)) {
				std::set<uint32_t> shape_hashes =
					CollectShapeKeyHashIniCandidates(profile_document);
				AcquireSRWLockExclusive(&capture_lock);
				if (!_wcsicmp(target_source_file.c_str(), profile_source.c_str())) {
					watched_shape_key_hashes = std::move(shape_hashes);
					target_profile_loaded = true;
				}
				ReleaseSRWLockExclusive(&capture_lock);
			}
		}

		std::vector<std::wstring> sources;
		AssetHashObservationMap observations;
		VbHashObservationList vb_observations;
		ShapeKeyHashObservationList shape_key_observations;
		AssetHashPathIdentityMap legacy_hash_identities;
		std::set<uint32_t> ambiguous_hashes;
		std::wstring target_source;
		CaptureMode mode = CaptureMode::Off;
		AcquireSRWLockExclusive(&capture_lock);
		if (capture_mode == CaptureMode::Off || !capture_dirty) {
			ReleaseSRWLockExclusive(&capture_lock);
			continue;
		}
		capture_dirty = false;
		mode = capture_mode;
		sources = source_files;
		observations = captured_hashes;
		vb_observations = captured_vb_hashes;
		shape_key_observations = captured_shape_key_hashes;
		target_source = target_source_file;
		PruneReleasedRecentAssets(GetTickCount64());
		legacy_hash_identities = BuildLegacyHashIdentitySnapshot();
		ambiguous_hashes = BuildAmbiguousHashSnapshot();
		ReleaseSRWLockExclusive(&capture_lock);
		LogInfo(
			"> Asset Hash Capture ambiguous hashes=%llu\n",
			static_cast<unsigned long long>(ambiguous_hashes.size()));
		WriteSnapshot(
			sources,
			observations,
			vb_observations,
			shape_key_observations,
			legacy_hash_identities,
			ambiguous_hashes,
			target_source,
			mode);
	}
}

void EnsureWriterThread()
{
	if (InterlockedCompareExchange(&worker_started, 1, 0))
		return;
	capture_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	HANDLE thread = capture_event
		? CreateThread(
			nullptr,
			0,
			CaptureWriterThread,
			nullptr,
			0,
			nullptr)
		: nullptr;
	if (!thread) {
		if (capture_event)
			CloseHandle(capture_event);
		capture_event = nullptr;
		InterlockedExchange(&worker_started, 0);
	} else {
		CloseHandle(thread);
	}
}

void SignalWriter()
{
	if (capture_event)
		SetEvent(capture_event);
}

void GatherExistingResources()
{
	struct ExistingResource
	{
		uintptr_t resource_address;
		std::wstring asset_path;
		uint32_t hash;
		uint32_t width;
		uint32_t height;
	};
	std::vector<ExistingResource> resources;
	std::set<std::wstring> identities;
	std::set<uint32_t> legacy_hashes;
	size_t texture_count = 0;
	size_t texture_with_path_count = 0;
	AcquireSRWLockShared(&capture_lock);
	identities = watched_identities;
	legacy_hashes = watched_legacy_hashes;
	ReleaseSRWLockShared(&capture_lock);
	EnterCriticalSectionPretty(&G->mResourcesLock);
	for (const auto& entry : G->mResources) {
		const ResourceHandleInfo& info = entry.second;
		if (info.type != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
			continue;
		++texture_count;
		if (info.asset_path.empty() ||
				info.asset_path.size() > kMaxIdentityCharacters)
			continue;
		++texture_with_path_count;
		const std::wstring asset_name =
			ExtractUnrealAssetName(info.asset_path);
		if (identities.find(IdentityKey(
					L"match_asset_path",
					info.asset_path)) == identities.end() &&
				identities.find(IdentityKey(
					L"match_asset_name",
					asset_name)) == identities.end() &&
				legacy_hashes.find(info.hash) == legacy_hashes.end())
			continue;
		resources.push_back({
			reinterpret_cast<uintptr_t>(entry.first),
			info.asset_path,
			info.hash,
			info.desc2D.Width,
			info.desc2D.Height});
	}
	LeaveCriticalSection(&G->mResourcesLock);
	LogInfo(
		"Asset Hash live scan textures=%llu with_path=%llu matched=%llu\n",
		static_cast<unsigned long long>(texture_count),
		static_cast<unsigned long long>(texture_with_path_count),
		static_cast<unsigned long long>(resources.size()));
	for (const ExistingResource& resource : resources) {
		ObserveAssetHashForAuthoring(
			resource.resource_address,
			resource.asset_path,
			resource.hash,
			resource.width,
			resource.height);
	}
}
}

void ToggleAssetHashCapture(HackerDevice *, void *)
{
	EnsureWriterThread();
	CaptureMode mode = CaptureMode::Off;
	AcquireSRWLockExclusive(&capture_lock);
	if (capture_mode == CaptureMode::Off) {
		ResetCaptureSessionLocked();
		capture_mode = CaptureMode::Backup;
	} else {
		capture_mode = CaptureMode::Off;
	}
	mode = capture_mode;
	capture_enabled.store(mode != CaptureMode::Off, std::memory_order_release);
	if (mode == CaptureMode::Off)
		capture_dirty = false;
	status_until = GetTickCount() + 2500;
	ReleaseSRWLockExclusive(&capture_lock);

	if (mode == CaptureMode::Backup) {
		RefreshAssetHashCaptureSources();
		LogOverlay(
			LOG_NOTICE,
			"Asset Hash Capture: ON (BACKUP)\n");
	} else {
		LogOverlay(
			LOG_NOTICE,
			"Asset Hash Capture: OFF\n");
	}
	LogInfo(
		"> Asset Hash Capture mode %s\n",
		mode == CaptureMode::Backup ? "BACKUP" : "OFF");
}

void ToggleAggressiveAssetHashCapture(HackerDevice *, void *)
{
	EnsureWriterThread();
	CaptureMode mode = CaptureMode::Off;
	AcquireSRWLockExclusive(&capture_lock);
	if (capture_mode == CaptureMode::Aggressive) {
		capture_mode = CaptureMode::Off;
	} else {
		if (capture_mode == CaptureMode::Off)
			ResetCaptureSessionLocked();
		capture_mode = CaptureMode::Aggressive;
	}
	mode = capture_mode;
	capture_enabled.store(mode != CaptureMode::Off, std::memory_order_release);
	if (mode == CaptureMode::Off)
		capture_dirty = false;
	status_until = GetTickCount() + 2500;
	ReleaseSRWLockExclusive(&capture_lock);

	if (mode == CaptureMode::Aggressive) {
		RefreshAssetHashCaptureSources();
		LogOverlay(
			LOG_NOTICE,
			"Asset Hash Capture: ON (AGGRESSIVE)\n");
	} else {
		LogOverlay(
			LOG_NOTICE,
			"Asset Hash Capture: OFF\n");
	}
	LogInfo(
		"> Asset Hash Capture mode %s\n",
		mode == CaptureMode::Aggressive ? "AGGRESSIVE" : "OFF");
}

void ToggleAssetHashPathConversion(HackerDevice *, void *)
{
	EnsureWriterThread();
	CaptureMode mode = CaptureMode::Off;
	AcquireSRWLockExclusive(&capture_lock);
	if (capture_mode == CaptureMode::PathConversion) {
		capture_mode = CaptureMode::Off;
	} else {
		if (capture_mode == CaptureMode::Off)
			ResetCaptureSessionLocked();
		capture_mode = CaptureMode::PathConversion;
	}
	mode = capture_mode;
	capture_enabled.store(mode != CaptureMode::Off, std::memory_order_release);
	if (mode == CaptureMode::Off)
		capture_dirty = false;
	status_until = GetTickCount() + 2500;
	ReleaseSRWLockExclusive(&capture_lock);

	if (mode == CaptureMode::PathConversion) {
		RefreshAssetHashCaptureSources();
		LogOverlay(
			LOG_NOTICE,
			"Asset Hash Capture: ON (PATH CONVERSION)\n");
	} else {
		LogOverlay(
			LOG_NOTICE,
			"Asset Hash Capture: OFF\n");
	}
	LogInfo(
		"> Asset Hash Capture mode %s\n",
		mode == CaptureMode::PathConversion ? "PATH CONVERSION" : "OFF");
}

void ToggleAssetHashCleanPathConversion(HackerDevice *, void *)
{
	EnsureWriterThread();
	CaptureMode mode = CaptureMode::Off;
	AcquireSRWLockExclusive(&capture_lock);
	if (capture_mode == CaptureMode::CleanPathConversion) {
		capture_mode = CaptureMode::Off;
	} else {
		if (capture_mode == CaptureMode::Off)
			ResetCaptureSessionLocked();
		capture_mode = CaptureMode::CleanPathConversion;
	}
	mode = capture_mode;
	capture_enabled.store(mode != CaptureMode::Off, std::memory_order_release);
	if (mode == CaptureMode::Off)
		capture_dirty = false;
	status_until = GetTickCount() + 2500;
	ReleaseSRWLockExclusive(&capture_lock);

	if (mode == CaptureMode::CleanPathConversion) {
		RefreshAssetHashCaptureSources();
		LogOverlay(
			LOG_NOTICE,
			"Asset Hash Capture: ON (PATH CLEANUP)\n");
	} else {
		LogOverlay(
			LOG_NOTICE,
			"Asset Hash Capture: OFF\n");
	}
	LogInfo(
		"> Asset Hash Capture mode %s\n",
		mode == CaptureMode::CleanPathConversion ? "PATH CLEANUP" : "OFF");
}

void RefreshAssetHashCaptureSources()
{
	std::vector<std::wstring> files;
	GetLoadedTextureOverrideIniFiles(&files);

	std::set<std::wstring> identities;
	std::set<uint32_t> legacy_hashes;
	std::set<uint32_t> shape_key_hashes;
	std::map<std::pair<uint32_t, uint32_t>, std::set<std::wstring>>
		draw_signature_sources;
	std::map<std::wstring, size_t> draw_signature_counts;
	bool has_identity_aliases = false;
	bool needs_canonicalization = false;
	for (const std::wstring& file : files) {
		std::wstring document;
		if (!ReadUtf8File(file, &document))
			continue;
		has_identity_aliases = has_identity_aliases ||
			AssetHashIniUsesIdentityAliases(document);
		needs_canonicalization = needs_canonicalization ||
			AssetHashIniNeedsCanonicalization(document);
		std::set<std::wstring> found =
			CollectAssetHashIniIdentities(document);
		for (const std::wstring& identity : found) {
			if (identity.size() <= kMaxIdentityCharacters &&
					identities.size() < kMaxHistoricalIdentities)
				identities.insert(identity);
		}
		std::set<uint32_t> found_legacy_hashes =
			CollectAssetHashIniLegacyHashes(document);
		legacy_hashes.insert(
			found_legacy_hashes.begin(),
			found_legacy_hashes.end());
		std::set<std::pair<uint32_t, uint32_t>> draw_signatures =
			CollectVbHashIniDrawSignatures(document);
		if (!draw_signatures.empty()) {
			draw_signature_counts[file] = draw_signatures.size();
			for (const auto& signature : draw_signatures)
				draw_signature_sources[signature].insert(file);
		}
	}

	AcquireSRWLockExclusive(&capture_lock);
	source_files = std::move(files);
	watched_identities = std::move(identities);
	watched_legacy_hashes = std::move(legacy_hashes);
	watched_shape_key_hashes = std::move(shape_key_hashes);
	model_draw_signature_sources = std::move(draw_signature_sources);
	model_draw_signature_counts = std::move(draw_signature_counts);
	model_vertex_probe_hashes.clear();
	model_vertex_counts.clear();
	model_draw_signatures.clear();
	model_source_scores.clear();
	target_source_file.clear();
	target_vb_hash = 0;
	target_vertex_count = 0;
	target_profile_loaded = false;
	for (auto i = captured_hashes.begin();
			i != captured_hashes.end();) {
		if (watched_identities.find(i->first) ==
				watched_identities.end()) {
			captured_observation_count -= i->second.size();
			i = captured_hashes.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = observed_name_paths.begin();
			i != observed_name_paths.end();) {
		if (watched_identities.find(
					IdentityKey(L"match_asset_name", i->first)) ==
				watched_identities.end()) {
			i = observed_name_paths.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = ambiguous_names.begin(); i != ambiguous_names.end();) {
		if (watched_identities.find(
					IdentityKey(L"match_asset_name", *i)) ==
				watched_identities.end()) {
			i = ambiguous_names.erase(i);
		} else {
			++i;
		}
	}
	PromoteRecentObservations();
	if (capture_mode != CaptureMode::Off &&
			(has_identity_aliases ||
			 needs_canonicalization ||
			 !captured_hashes.empty() ||
			 !recent_assets.empty()))
		capture_dirty = true;
	bool signal = capture_dirty;
	ReleaseSRWLockExclusive(&capture_lock);
	if (signal)
		SignalWriter();
	GatherExistingResources();
}

void ObserveAssetHashForAuthoring(
	uintptr_t resource_address,
	const std::wstring& asset_path,
	uint32_t hash,
	uint32_t width,
	uint32_t height)
{
	if (asset_path.empty() || asset_path.size() > kMaxIdentityCharacters)
		return;
	const std::wstring asset_name = ExtractUnrealAssetName(asset_path);
	const AssetHashObservation observation = {hash, width, height};
	const std::wstring path_key =
		IdentityKey(L"match_asset_path", asset_path);
	const std::wstring name_key =
		IdentityKey(L"match_asset_name", asset_name);

	bool changed = false;
	AcquireSRWLockExclusive(&capture_lock);
	bool write_enabled = capture_mode != CaptureMode::Off;
	bool recent_changed =
		AddRecentObservation(resource_address, asset_path, observation);

	if (write_enabled &&
			watched_identities.find(path_key) != watched_identities.end()) {
		if (AddHistoricalObservation(path_key, observation))
			changed = true;
	}

	if (write_enabled && !asset_name.empty() &&
			watched_identities.find(name_key) !=
				watched_identities.end()) {
		std::wstring lowered_name = Lower(asset_name);
		auto prior = observed_name_paths.find(lowered_name);
		if (prior == observed_name_paths.end()) {
			observed_name_paths.emplace(lowered_name, asset_path);
		} else if (_wcsicmp(
				prior->second.c_str(),
				asset_path.c_str())) {
			ambiguous_names.insert(lowered_name);
			auto identity = captured_hashes.find(name_key);
			if (identity != captured_hashes.end()) {
				captured_observation_count -= identity->second.size();
				captured_hashes.erase(identity);
			}
		}
		if (ambiguous_names.find(lowered_name) ==
				ambiguous_names.end()) {
			if (AddHistoricalObservation(name_key, observation))
				changed = true;
		}
	}

	if ((changed || recent_changed) && write_enabled)
		capture_dirty = true;
	ReleaseSRWLockExclusive(&capture_lock);
	if ((changed || recent_changed) && write_enabled)
		SignalWriter();
}

void ObserveVbHashForAuthoring(
	const std::wstring& asset_path,
	uint32_t hash,
	uint32_t first_index,
	uint32_t index_count,
	uint32_t vertex_count)
{
	if (asset_path.size() > kMaxIdentityCharacters || !hash || !index_count)
		return;
	bool changed = false;
	AcquireSRWLockExclusive(&capture_lock);
	if (capture_mode != CaptureMode::Off &&
			(!asset_path.empty() || !target_source_file.empty()) &&
			captured_vb_hashes.size() < kMaxVbHashObservations) {
		auto key = std::make_tuple(
			Lower(asset_path),
			hash,
			first_index,
			index_count,
			vertex_count);
		if (captured_vb_hash_keys.insert(key).second) {
			captured_vb_hashes.push_back({
				asset_path,
				hash,
				first_index,
				index_count,
				vertex_count});
			capture_dirty = true;
			changed = true;
		}
	}
	ReleaseSRWLockExclusive(&capture_lock);
	if (changed)
		SignalWriter();
}

void ObserveShapeKeyHashForAuthoring(
	uint32_t hash,
	uint32_t byte_width,
	uint32_t structure_byte_stride,
	uint32_t filter_index,
	uint32_t slot,
	bool unordered_access)
{
	if (!hash || !byte_width)
		return;
	bool changed = false;
	AcquireSRWLockExclusive(&capture_lock);
	if (capture_mode != CaptureMode::Off &&
			target_vertex_count &&
			(byte_width == static_cast<uint64_t>(target_vertex_count) * 24 ||
			 byte_width == static_cast<uint64_t>(target_vertex_count) * 4) &&
			captured_shape_key_hashes.size() < kMaxVbHashObservations) {
		auto key = std::make_tuple(
			hash,
			byte_width,
			structure_byte_stride,
			filter_index,
			slot,
			unordered_access);
		if (captured_shape_key_hash_keys.insert(key).second) {
			captured_shape_key_hashes.push_back({
				hash,
				byte_width,
				structure_byte_stride,
				filter_index,
				slot,
				unordered_access});
			capture_dirty = true;
			changed = true;
		}
	}
	ReleaseSRWLockExclusive(&capture_lock);
	if (changed)
		SignalWriter();
}

void RetireAssetHashForAuthoring(uintptr_t resource_address)
{
	if (!resource_address)
		return;
	AcquireSRWLockExclusive(&capture_lock);
	auto active = active_recent_resources.find(resource_address);
	if (active != active_recent_resources.end()) {
		auto asset = recent_assets.find(active->second);
		if (asset != recent_assets.end() && asset->second.live_resources) {
			if (!--asset->second.live_resources) {
				asset->second.last_seen = GetTickCount64();
				asset->second.expires_at =
					asset->second.last_seen + kRecentReleasedTtlMs;
			}
		}
		active_recent_resources.erase(active);
	}
	ReleaseSRWLockExclusive(&capture_lock);
}

bool AssetHashCaptureStatusVisible()
{
	AcquireSRWLockShared(&capture_lock);
	bool visible =
		capture_mode != CaptureMode::Off ||
		static_cast<LONG>(status_until - GetTickCount()) > 0;
	ReleaseSRWLockShared(&capture_lock);
	return visible;
}

bool AssetHashCaptureEnabled()
{
	return capture_enabled.load(std::memory_order_acquire);
}

bool AssetHashCaptureNeedsVbObservation(
	uint32_t hash,
	uint32_t first_index,
	uint32_t index_count,
	uint32_t vertex_count)
{
	std::wstring activated_source;
	uint32_t activated_vertex_count = 0;
	AcquireSRWLockExclusive(&capture_lock);
	if (capture_mode != CaptureMode::Off && target_source_file.empty()) {
		if (vertex_count)
			model_vertex_counts[hash] = vertex_count;
		const auto draw_signature = std::make_pair(first_index, index_count);
		bool signature_added = false;
		if (hash && index_count &&
				model_draw_signatures[hash].insert(draw_signature).second) {
			signature_added = true;
			auto sources = model_draw_signature_sources.find(draw_signature);
			if (sources != model_draw_signature_sources.end()) {
				for (const std::wstring& source : sources->second)
					++model_source_scores[hash][source];
			}
		}
		std::wstring candidate;
		size_t candidate_score = 0;
		bool ambiguous = false;
		if (signature_added) {
			for (const auto& score : model_source_scores[hash]) {
				auto count = model_draw_signature_counts.find(score.first);
				if (count == model_draw_signature_counts.end())
					continue;
				if (count->second < 2)
					continue;
				const size_t threshold = std::min<size_t>(3, count->second);
				if (score.second < threshold)
					continue;
				if (score.second > candidate_score) {
					candidate = score.first;
					candidate_score = score.second;
					ambiguous = false;
				} else if (score.second == candidate_score) {
					ambiguous = true;
				}
			}
		}
		if (!candidate.empty() && !ambiguous) {
			target_source_file = candidate;
			target_vb_hash = hash;
			target_vertex_count = model_vertex_counts[hash];
			target_profile_loaded = false;
			vb_probe_keys.clear();
			activated_source = target_source_file;
			activated_vertex_count = target_vertex_count;
		}
	}
	const auto key = std::make_tuple(hash, first_index, index_count);
	bool needed = capture_mode != CaptureMode::Off &&
		!target_source_file.empty() && hash == target_vb_hash &&
		vb_probe_keys.insert(key).second;
	ReleaseSRWLockExclusive(&capture_lock);
	if (!activated_source.empty())
		LogInfo(
			"> Asset Hash Capture selected current model INI: %ls "
			"(VB0=%08x, vertices=%u)\n",
			activated_source.c_str(),
			hash,
			activated_vertex_count);
	return needed;
}

bool AssetHashCaptureNeedsCurrentModelVertexCount(uint32_t hash)
{
	AcquireSRWLockExclusive(&capture_lock);
	bool needed = capture_mode != CaptureMode::Off &&
		target_source_file.empty() && hash &&
		model_vertex_probe_hashes.insert(hash).second;
	ReleaseSRWLockExclusive(&capture_lock);
	return needed;
}

bool AssetHashCaptureNeedsShapeKeyObservation(
	uint32_t hash,
	uint32_t filter_index)
{
	AcquireSRWLockExclusive(&capture_lock);
	bool needed = capture_mode != CaptureMode::Off &&
		!watched_shape_key_hashes.empty() &&
		shape_key_probe_keys.insert({hash, filter_index}).second;
	ReleaseSRWLockExclusive(&capture_lock);
	return needed;
}

const wchar_t *AssetHashCaptureStatusText()
{
	AcquireSRWLockShared(&capture_lock);
	CaptureMode mode = capture_mode;
	ReleaseSRWLockShared(&capture_lock);
	switch (mode) {
		case CaptureMode::Backup:
			return L"Asset Hash Capture: ON (BACKUP)";
		case CaptureMode::Aggressive:
			return L"Asset Hash Capture: ON (AGGRESSIVE)";
		case CaptureMode::PathConversion:
			return L"Asset Hash Capture: ON (PATH CONVERSION)";
		case CaptureMode::CleanPathConversion:
			return L"Asset Hash Capture: ON (PATH CLEANUP)";
		default:
			return L"Asset Hash Capture: OFF";
	}
}
