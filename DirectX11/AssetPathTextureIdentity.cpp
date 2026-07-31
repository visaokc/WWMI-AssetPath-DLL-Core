#include "AssetPathTextureIdentity.h"

#include <Windows.h>
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "globals.h"
#include "log.h"
#include "ResourceHash.h"
#include "DLLMainHook.h"
#include "AssetHashCapture.h"

namespace
{

struct UnrealFString
{
	wchar_t *data;
	int32_t count;
	int32_t capacity;
};

constexpr unsigned kTextureObjectSearchFrames = 16;
constexpr unsigned kMaxResolverCandidates = 4;
constexpr size_t kMaxProducerIdentityEntries = 16384;
constexpr LONG kAssetPathUsageOverrides = 0x1;
constexpr LONG kAssetPathUsageFrameAnalysis = 0x2;
constexpr LONG kAssetPathUsageAlwaysCapture = 0x4;
constexpr LONG kInstallStateUninitialized = 0;
constexpr LONG kInstallStateInitializing = 1;
constexpr LONG kInstallStateReady = 2;
constexpr LONG kInstallStateFailed = 3;
constexpr unsigned kRhiTextureCreationVtableLimit = 160;
constexpr unsigned kRhiTextureLabelSlotDelta = 28;
constexpr unsigned kRhiTextureCreationSlotMinimum = 64;
constexpr unsigned kRhiTextureCreationSlotMaximum = 84;
constexpr unsigned kRhiStackCandidateCount = 64;
constexpr LONG kRhiProbeAttemptLimit = 4096;
constexpr size_t kRhiTextureObjectScanSize = 0x300;
constexpr LONG kRhiTextureLabelProbeLimit = 96;
constexpr size_t kRhiResourceOffsetUnknown = static_cast<size_t>(-1);
constexpr size_t kRhiResourceOffsetConsensusSamples = 12;
constexpr unsigned kRhiResourceOffsetConsensusHits = 10;
constexpr unsigned kRhiResourceOffsetConsensusLead = 4;

SRWLOCK producer_identity_lock = SRWLOCK_INIT;
SRWLOCK resolver_selection_lock = SRWLOCK_INIT;
SRWLOCK install_wait_lock = SRWLOCK_INIT;
SRWLOCK rhi_label_report_lock = SRWLOCK_INIT;
SRWLOCK rhi_label_identity_lock = SRWLOCK_INIT;
SRWLOCK asset_path_match_report_lock = SRWLOCK_INIT;
CONDITION_VARIABLE install_condition = CONDITION_VARIABLE_INIT;
std::unordered_map<uintptr_t, std::wstring> producer_owner_paths;
struct UnrealObjectSnapshot
{
	uintptr_t object;
	uintptr_t vtable;
	int32_t internal_index;
	uintptr_t class_private;
	uintptr_t outer_private;
};
std::unordered_map<uintptr_t, UnrealObjectSnapshot> pending_producer_owners;
struct ProducerWrapperIdentity
{
	uintptr_t source;
	uintptr_t resource;
	std::wstring asset_path;
};
struct PendingProducerIdentity
{
	uintptr_t source;
	uintptr_t resource;
	UnrealObjectSnapshot object;
};
std::unordered_map<uintptr_t, ProducerWrapperIdentity> producer_wrapper_paths;
std::unordered_map<uintptr_t, uintptr_t> producer_resource_wrappers;
std::unordered_map<uintptr_t, PendingProducerIdentity>
	pending_producer_wrappers;
std::unordered_map<uintptr_t, uintptr_t> pending_resource_wrappers;
thread_local std::wstring active_producer_asset_path;
// Runtime identity capture stays active so config reloads can match resources
// that were created before an asset path override was loaded.
std::atomic<LONG> asset_path_usage{kAssetPathUsageAlwaysCapture};
std::atomic<LONG> install_state{kInstallStateUninitialized};
std::atomic<bool> install_wait_timed_out{false};
std::atomic<bool> resolver_worker_started{false};
std::atomic<LONG> rhi_probe_attempts{0};
std::atomic<LONG> rhi_probe_state{0};
std::atomic<LONG> rhi_label_probe_count{0};
struct RhiTextureResourceMatch
{
	size_t offset;
	uintptr_t resource;
	uint32_t hash;
};
struct PendingRhiTextureLabel
{
	std::wstring label;
	std::vector<RhiTextureResourceMatch> matches;
};
size_t rhi_texture_resource_offset = kRhiResourceOffsetUnknown;
size_t rhi_texture_resource_offset_samples = 0;
std::unordered_map<size_t, unsigned> rhi_texture_resource_offset_votes;
std::vector<PendingRhiTextureLabel> pending_rhi_texture_labels;
std::unordered_set<std::wstring> reported_asset_name_matches;
using RhiTextureLabelFunction = void (__fastcall*)(
	uintptr_t dynamic_rhi,
	uintptr_t texture,
	const wchar_t *name);
RhiTextureLabelFunction original_rhi_texture_label = nullptr;

bool AssetPathBridgeReady()
{
	return install_state.load(
		std::memory_order_acquire) == kInstallStateReady;
}

void ErasePendingProducerWrapper(
	std::unordered_map<uintptr_t, PendingProducerIdentity>::iterator identity)
{
	auto resource = pending_resource_wrappers.find(identity->second.resource);
	if (resource != pending_resource_wrappers.end() &&
			resource->second == identity->first)
		pending_resource_wrappers.erase(resource);
	pending_producer_wrappers.erase(identity);
}

void EraseProducerWrapperIdentity(
	std::unordered_map<uintptr_t, ProducerWrapperIdentity>::iterator identity)
{
	auto resource = producer_resource_wrappers.find(
		identity->second.resource);
	if (resource != producer_resource_wrappers.end() &&
			resource->second == identity->first)
		producer_resource_wrappers.erase(resource);
	producer_wrapper_paths.erase(identity);
}

void StoreProducerWrapperIdentity(
	uintptr_t wrapper,
	uintptr_t source,
	uintptr_t resource,
	const std::wstring& asset_path)
{
	auto pending = pending_producer_wrappers.find(wrapper);
	if (pending != pending_producer_wrappers.end())
		ErasePendingProducerWrapper(pending);
	auto existing = producer_wrapper_paths.find(wrapper);
	if (existing != producer_wrapper_paths.end())
		EraseProducerWrapperIdentity(existing);
	if (!resource)
		return;

	auto pending_resource_owner = pending_resource_wrappers.find(resource);
	if (pending_resource_owner != pending_resource_wrappers.end()) {
		auto pending_identity = pending_producer_wrappers.find(
			pending_resource_owner->second);
		if (pending_identity != pending_producer_wrappers.end())
			ErasePendingProducerWrapper(pending_identity);
		else
			pending_resource_wrappers.erase(pending_resource_owner);
	}
	auto resource_owner = producer_resource_wrappers.find(resource);
	if (resource_owner != producer_resource_wrappers.end()) {
		auto old_identity = producer_wrapper_paths.find(
			resource_owner->second);
		if (old_identity != producer_wrapper_paths.end())
			EraseProducerWrapperIdentity(old_identity);
		else
			producer_resource_wrappers.erase(resource_owner);
	}

	if (producer_wrapper_paths.size() >= kMaxProducerIdentityEntries)
		EraseProducerWrapperIdentity(producer_wrapper_paths.begin());

	producer_wrapper_paths[wrapper] = {
		source,
		resource,
		asset_path,
	};
	producer_resource_wrappers[resource] = wrapper;
}

void StorePendingProducerIdentity(
	uintptr_t wrapper,
	uintptr_t source,
	uintptr_t resource,
	const UnrealObjectSnapshot& object)
{
	auto existing = pending_producer_wrappers.find(wrapper);
	if (existing != pending_producer_wrappers.end())
		ErasePendingProducerWrapper(existing);
	auto resolved = producer_wrapper_paths.find(wrapper);
	if (resolved != producer_wrapper_paths.end())
		EraseProducerWrapperIdentity(resolved);
	if (!resource)
		return;

	auto resolved_resource_owner = producer_resource_wrappers.find(resource);
	if (resolved_resource_owner != producer_resource_wrappers.end()) {
		auto resolved_identity = producer_wrapper_paths.find(
			resolved_resource_owner->second);
		if (resolved_identity != producer_wrapper_paths.end())
			EraseProducerWrapperIdentity(resolved_identity);
		else
			producer_resource_wrappers.erase(resolved_resource_owner);
	}
	auto resource_owner = pending_resource_wrappers.find(resource);
	if (resource_owner != pending_resource_wrappers.end()) {
		auto old_identity = pending_producer_wrappers.find(
			resource_owner->second);
		if (old_identity != pending_producer_wrappers.end())
			ErasePendingProducerWrapper(old_identity);
		else
			pending_resource_wrappers.erase(resource_owner);
	}

	if (pending_producer_wrappers.size() >= kMaxProducerIdentityEntries)
		ErasePendingProducerWrapper(pending_producer_wrappers.begin());

	pending_producer_wrappers[wrapper] = {
		source,
		resource,
		object,
	};
	pending_resource_wrappers[resource] = wrapper;
}

void BackfillPendingProducerIdentities();

using ProducerOwnerConstructor =
	uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t);
using TextureProducerFunction =
	void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uint32_t, uint8_t);

ProducerOwnerConstructor original_producer_owner_constructor = nullptr;
TextureProducerFunction original_texture_producer = nullptr;

struct AssetPathRuntime
{
	uintptr_t module_base;
	uintptr_t module_end;
	uintptr_t get_path_name;
	uintptr_t free_memory;
	uintptr_t texture_creation_begin;
	uintptr_t texture_creation_end;
	uintptr_t producer_owner_constructor;
	uintptr_t texture_producer;
	uintptr_t get_path_name_candidates[kMaxResolverCandidates];
	unsigned get_path_name_candidate_count;
};

AssetPathRuntime asset_path_runtime = {};
LONG resolver_dynamic_reported = 0;

struct BytePattern
{
	const uint8_t *bytes;
	const char *mask;
	size_t size;
};

// These signatures identify function shapes only. Runtime addresses are
// discovered from the current executable and are never loaded from a profile.
const uint8_t kGetPathNamePatternBytes[] = {
	0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xda,
	0x49, 0x8b, 0xc0, 0x33, 0xd2, 0x4c, 0x8b, 0xc3, 0x48,
	0x89, 0x13, 0x48, 0x89, 0x53, 0x08, 0x48, 0x8b, 0xd0,
};
const uint8_t kFreeMemoryPatternBytes[] = {
	0x48, 0x85, 0xc9, 0x74, 0x2e, 0x53, 0x48, 0x83, 0xec,
	0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x0d, 0x00, 0x00,
	0x00, 0x00, 0x48, 0x85, 0xc9, 0x75, 0x0c,
};
const uint8_t kTextureCreationPatternBytes[] = {
	0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
	0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24,
};
const uint8_t kProducerOwnerPatternBytes[] = {
	0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x60,
	0x4d, 0x8b, 0xc8, 0xc6, 0x44, 0x24, 0x20, 0x01, 0x4c,
	0x8b, 0x82, 0x68, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0xf2,
	0x48, 0x8b, 0xd9,
};
const uint8_t kTextureProducerPatternBytes[] = {
	0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24,
	0x10, 0x57, 0x48, 0x83, 0xec, 0x40, 0x80, 0x3d, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x41, 0x8b, 0xd9, 0x49, 0x8b,
	0xf8, 0x48, 0x8b, 0xf1, 0x74, 0x11,
};

const BytePattern kGetPathNamePattern = {
	kGetPathNamePatternBytes,
	"xxxxxxxxxxxxxxxxxxxxxxxxxxx",
	ARRAYSIZE(kGetPathNamePatternBytes),
};
const BytePattern kFreeMemoryPattern = {
	kFreeMemoryPatternBytes,
	"xxxxxxxxxxxxxxxx????xxxxx",
	ARRAYSIZE(kFreeMemoryPatternBytes),
};
const BytePattern kTextureCreationPattern = {
	kTextureCreationPatternBytes,
	"xxxxxxxxxxxxxxxxx",
	ARRAYSIZE(kTextureCreationPatternBytes),
};
const BytePattern kProducerOwnerPattern = {
	kProducerOwnerPatternBytes,
	"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
	ARRAYSIZE(kProducerOwnerPatternBytes),
};
const BytePattern kTextureProducerPattern = {
	kTextureProducerPatternBytes,
	"xxxxxxxxxxxxxxxxx????xxxxxxxxxxxx",
	ARRAYSIZE(kTextureProducerPatternBytes),
};

bool ReadableMemory(uintptr_t address, size_t required)
{
	MEMORY_BASIC_INFORMATION info = {};
	if (!address || !VirtualQuery(
			reinterpret_cast<const void*>(address),
			&info,
			sizeof(info)))
		return false;

	DWORD protection = info.Protect;
	DWORD base = protection & 0xff;
	if (info.State != MEM_COMMIT ||
			(protection & (PAGE_GUARD | PAGE_NOACCESS)) ||
			!(base == PAGE_READONLY ||
			  base == PAGE_READWRITE ||
			  base == PAGE_WRITECOPY ||
			  base == PAGE_EXECUTE_READ ||
			  base == PAGE_EXECUTE_READWRITE ||
			  base == PAGE_EXECUTE_WRITECOPY))
		return false;

	uintptr_t end =
		reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
	return end > address && end - address >= required;
}

bool GetModuleRange(uintptr_t *module_base, uintptr_t *module_end)
{
	*module_base =
		reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	if (!*module_base || !ReadableMemory(*module_base, sizeof(IMAGE_DOS_HEADER)))
		return false;

	IMAGE_DOS_HEADER *dos =
		reinterpret_cast<IMAGE_DOS_HEADER*>(*module_base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
			dos->e_lfanew <= 0 ||
			!ReadableMemory(
				*module_base + dos->e_lfanew,
				sizeof(IMAGE_NT_HEADERS)))
		return false;

	IMAGE_NT_HEADERS *nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
		*module_base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;

	*module_end = *module_base + nt->OptionalHeader.SizeOfImage;
	return *module_end > *module_base;
}

bool ExecutableMemory(uintptr_t address, size_t required)
{
	MEMORY_BASIC_INFORMATION info = {};
	if (!address || !VirtualQuery(
			reinterpret_cast<const void*>(address),
			&info,
			sizeof(info)))
		return false;
	DWORD protection = info.Protect & 0xff;
	uintptr_t region_end =
		reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
	return info.State == MEM_COMMIT &&
		!(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
		(protection == PAGE_EXECUTE ||
		 protection == PAGE_EXECUTE_READ ||
		 protection == PAGE_EXECUTE_READWRITE ||
		 protection == PAGE_EXECUTE_WRITECOPY) &&
		region_end > address &&
		region_end - address >= required;
}

bool PatternBytesMatch(uintptr_t address, const BytePattern& pattern)
{
	const uint8_t *candidate = reinterpret_cast<const uint8_t*>(address);
	for (size_t i = 0; i < pattern.size; ++i) {
		if (pattern.mask[i] == 'x' && candidate[i] != pattern.bytes[i])
			return false;
	}
	return true;
}

bool RuntimeFunctionRange(
	uintptr_t address,
	uintptr_t *function_begin,
	uintptr_t *function_end)
{
	DWORD64 image_base = 0;
	PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
		address,
		&image_base,
		nullptr);
	if (!function)
		return false;
	*function_begin =
		static_cast<uintptr_t>(image_base + function->BeginAddress);
	*function_end =
		static_cast<uintptr_t>(image_base + function->EndAddress);
	return *function_begin == address && *function_end > *function_begin;
}

unsigned FindFunctionPatternCandidates(
	uintptr_t module_base,
	uintptr_t module_end,
	const BytePattern& pattern,
	uintptr_t *results,
	unsigned capacity)
{
	unsigned matches = 0;
	uintptr_t cursor = module_base;
	while (cursor < module_end) {
		MEMORY_BASIC_INFORMATION info = {};
		if (!VirtualQuery(
				reinterpret_cast<const void*>(cursor),
				&info,
				sizeof(info)))
			break;
		uintptr_t region_begin =
			(std::max)(
				cursor,
				reinterpret_cast<uintptr_t>(info.BaseAddress));
		uintptr_t region_end =
			(std::min)(
				module_end,
				reinterpret_cast<uintptr_t>(info.BaseAddress) +
					info.RegionSize);
		if (region_end <= cursor)
			break;
		if (ExecutableMemory(region_begin, 1) &&
				region_end - region_begin >= pattern.size) {
			uintptr_t last = region_end - pattern.size;
			uintptr_t candidate = region_begin;
			while (candidate <= last) {
				const void *found = memchr(
					reinterpret_cast<const void*>(candidate),
					pattern.bytes[0],
					static_cast<size_t>(last - candidate + 1));
				if (!found)
					break;
				candidate = reinterpret_cast<uintptr_t>(found);
				uintptr_t function_begin = 0;
				uintptr_t function_end = 0;
				if (PatternBytesMatch(candidate, pattern) &&
						RuntimeFunctionRange(
							candidate,
							&function_begin,
							&function_end)) {
					if (matches < capacity)
						results[matches] = candidate;
					++matches;
				}
				++candidate;
			}
		}
		cursor = region_end;
	}
	return matches;
}

FILE *OpenAssetPathResolverReport(const wchar_t *mode)
{
	wchar_t path[MAX_PATH];
	wcscpy_s(path, G->SHADER_PATH);
	wchar_t *separator = wcsrchr(path, L'\\');
	if (!separator)
		return nullptr;
	separator[1] = L'\0';
	wcscat_s(path, L"AssetPathResolver.log");
	return _wfsopen(path, mode, _SH_DENYNO);
}

FILE *OpenRhiTextureLabelProbeReport(const wchar_t *mode)
{
	wchar_t path[MAX_PATH];
	wcscpy_s(path, G->SHADER_PATH);
	wchar_t *separator = wcsrchr(path, L'\\');
	if (!separator)
		return nullptr;
	separator[1] = L'\0';
	wcscat_s(path, L"RhiTextureLabelProbe.log");
	return _wfsopen(path, mode, _SH_DENYNO);
}

bool ResolveAssetPathRuntime()
{
	uintptr_t module_base = 0;
	uintptr_t module_end = 0;
	if (!GetModuleRange(&module_base, &module_end))
		return false;
	FILE *report = OpenAssetPathResolverReport(L"wb");
	if (report)
		fprintf(
			report,
			"timestamp=0x%08x image_size=0x%08llx\n",
			reinterpret_cast<IMAGE_NT_HEADERS*>(
				module_base +
				reinterpret_cast<IMAGE_DOS_HEADER*>(
					module_base)->e_lfanew)->FileHeader.TimeDateStamp,
			static_cast<unsigned long long>(module_end - module_base));

	AssetPathRuntime resolved = {};
	resolved.module_base = module_base;
	resolved.module_end = module_end;
	struct ScanTarget
	{
		const char *name;
		const BytePattern *pattern;
		uintptr_t *candidates;
		unsigned *count;
	};
	uintptr_t free_memory_candidates[kMaxResolverCandidates] = {};
	uintptr_t producer_owner_candidates[kMaxResolverCandidates] = {};
	uintptr_t texture_producer_candidates[kMaxResolverCandidates] = {};
	unsigned free_memory_count = 0;
	unsigned producer_owner_count = 0;
	unsigned texture_producer_count = 0;
	ScanTarget targets[] = {
		{"get_path_name", &kGetPathNamePattern,
			resolved.get_path_name_candidates,
			&resolved.get_path_name_candidate_count},
		{"free_memory", &kFreeMemoryPattern,
			free_memory_candidates,
			&free_memory_count},
		{"producer_owner", &kProducerOwnerPattern,
			producer_owner_candidates,
			&producer_owner_count},
		{"texture_producer", &kTextureProducerPattern,
			texture_producer_candidates,
			&texture_producer_count},
	};

	for (auto& target : targets) {
		*target.count = FindFunctionPatternCandidates(
			module_base,
			module_end,
			*target.pattern,
			target.candidates,
			kMaxResolverCandidates);
		LogInfo(
			"Asset Path resolver target=%s matches=%u\n",
			target.name,
			*target.count);
		if (report) {
			fprintf(
				report,
				"target=%s matches=%u",
				target.name,
				*target.count);
			for (unsigned i = 0;
					i < *target.count && i < kMaxResolverCandidates;
					++i)
				fprintf(
					report,
					" candidate%u=0x%llx",
					i,
					static_cast<unsigned long long>(
						target.candidates[i] - module_base));
			fputc('\n', report);
		}
	}
	if (report)
		fprintf(
			report,
			"target=texture_creation resolution=call-stack-semantic\n");

	bool dynamic_complete =
		resolved.get_path_name_candidate_count >= 1 &&
		resolved.get_path_name_candidate_count <= kMaxResolverCandidates &&
		free_memory_count == 1 &&
		producer_owner_count == 1 &&
		texture_producer_count == 1;
	if (dynamic_complete) {
		resolved.free_memory = free_memory_candidates[0];
		resolved.producer_owner_constructor = producer_owner_candidates[0];
		resolved.texture_producer = texture_producer_candidates[0];
		if (resolved.get_path_name_candidate_count == 1)
			resolved.get_path_name =
				resolved.get_path_name_candidates[0];
	}
	if (dynamic_complete) {
		asset_path_runtime = resolved;
		const bool semantic_pending =
			!resolved.get_path_name ||
			!resolved.texture_creation_begin;
		LogInfo(
			"Asset Path resolver mode=%s\n",
			semantic_pending
				? "dynamic-semantic-pending"
				: "dynamic");
		if (report) {
			fprintf(
				report,
				"mode=%s\n",
				semantic_pending
					? "dynamic-semantic-pending"
					: "dynamic");
			fclose(report);
		}
		if (!semantic_pending)
			InterlockedExchange(&resolver_dynamic_reported, 1);
		return true;
	}

	LogInfo("Asset Path resolver failed closed: dynamic targets are not unique\n");
	if (report) {
		fprintf(report, "mode=failed-closed\n");
		fclose(report);
	}
	return false;
}

bool CallUnrealGetPathName(
	uintptr_t get_path_name_address,
	uintptr_t object,
	std::wstring *output)
{
	using GetPathNameFn =
		void(__fastcall*)(void*, UnrealFString*, void*);
	using FreeFn = void(__fastcall*)(void*);

	GetPathNameFn get_path_name = reinterpret_cast<GetPathNameFn>(
		get_path_name_address);
	FreeFn free_memory = reinterpret_cast<FreeFn>(
		asset_path_runtime.free_memory);
	if (!get_path_name || !free_memory)
		return false;
	UnrealFString path = {};
	bool success = false;

	__try {
		get_path_name(reinterpret_cast<void*>(object), &path, nullptr);
		if (path.data &&
				path.count > 1 &&
				path.count < 1024 &&
				path.capacity >= path.count &&
				path.capacity < 0x10000 &&
				ReadableMemory(
					reinterpret_cast<uintptr_t>(path.data),
					static_cast<size_t>(path.count) * sizeof(wchar_t))) {
			output->assign(
				path.data,
				static_cast<size_t>(path.count - 1));
			success = true;
		}
		if (path.data)
			free_memory(path.data);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		success = false;
	}
	return success;
}

bool UnrealGetPathName(
	uintptr_t object,
	std::wstring *output)
{
	AcquireSRWLockShared(&resolver_selection_lock);
	uintptr_t get_path_name = asset_path_runtime.get_path_name;
	ReleaseSRWLockShared(&resolver_selection_lock);
	return CallUnrealGetPathName(
		get_path_name,
		object,
		output);
}

void AppendGetPathNameSelection(
	uintptr_t selected,
	unsigned valid_candidates)
{
	FILE *report = OpenAssetPathResolverReport(L"ab");
	if (!report)
		return;
	fprintf(
		report,
		"semantic=get_path_name valid=%u selected_rva=0x%llx\n",
		valid_candidates,
		selected
			? static_cast<unsigned long long>(
				selected - asset_path_runtime.module_base)
			: 0);
	fclose(report);
}

void ReportResolverDynamicIfComplete()
{
	AcquireSRWLockShared(&resolver_selection_lock);
	bool complete =
		asset_path_runtime.get_path_name &&
		asset_path_runtime.texture_creation_begin;
	ReleaseSRWLockShared(&resolver_selection_lock);
	if (!complete ||
			InterlockedCompareExchange(
				&resolver_dynamic_reported,
				1,
				0))
		return;
	FILE *report = OpenAssetPathResolverReport(L"ab");
	if (report) {
		fprintf(report, "mode=dynamic\n");
		fclose(report);
	}
	LogInfo("Asset Path resolver mode=dynamic\n");
}

bool ResolveGetPathNameSemantically(
	uintptr_t object,
	uintptr_t class_private)
{
	AcquireSRWLockShared(&resolver_selection_lock);
	bool resolved = asset_path_runtime.get_path_name != 0;
	ReleaseSRWLockShared(&resolver_selection_lock);
	if (resolved)
		return true;

	AcquireSRWLockExclusive(&resolver_selection_lock);
	if (asset_path_runtime.get_path_name) {
		ReleaseSRWLockExclusive(&resolver_selection_lock);
		return true;
	}

	uintptr_t selected = 0;
	unsigned valid = 0;
	std::wstring selected_object_path;
	for (unsigned i = 0;
			i < asset_path_runtime.get_path_name_candidate_count;
			++i) {
		uintptr_t candidate =
			asset_path_runtime.get_path_name_candidates[i];
		std::wstring class_path;
		std::wstring object_path;
		if (!CallUnrealGetPathName(
					candidate,
					class_private,
					&class_path) ||
				class_path != L"/Script/Engine.Texture2D" ||
				!CallUnrealGetPathName(
					candidate,
					object,
					&object_path) ||
				object_path.compare(0, 6, L"/Game/") != 0)
			continue;
		if (!selected) {
			selected = candidate;
			selected_object_path = object_path;
		} else if (object_path != selected_object_path) {
			selected = 0;
			valid = 0;
			break;
		}
		++valid;
	}
	if (selected)
		asset_path_runtime.get_path_name = selected;
	ReleaseSRWLockExclusive(&resolver_selection_lock);
	if (selected)
		AppendGetPathNameSelection(selected, valid);
	ReportResolverDynamicIfComplete();
	if (selected)
		BackfillPendingProducerIdentities();
	return selected != 0;
}

void AppendTextureCreationSelection(uintptr_t selected)
{
	FILE *report = OpenAssetPathResolverReport(L"ab");
	if (!report)
		return;
	fprintf(
		report,
		"semantic=texture_creation selected_rva=0x%llx\n",
		static_cast<unsigned long long>(
			selected - asset_path_runtime.module_base));
	fclose(report);
}

bool TextureCreationFrameCandidate(
	uintptr_t instruction,
	uintptr_t *function_begin,
	uintptr_t *function_end)
{
	AcquireSRWLockShared(&resolver_selection_lock);
	uintptr_t selected_begin =
		asset_path_runtime.texture_creation_begin;
	uintptr_t selected_end =
		asset_path_runtime.texture_creation_end;
	ReleaseSRWLockShared(&resolver_selection_lock);
	if (selected_begin) {
		*function_begin = selected_begin;
		*function_end = selected_end;
		return instruction >= *function_begin &&
			instruction < *function_end;
	}

	DWORD64 image_base = 0;
	PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
		instruction,
		&image_base,
		nullptr);
	if (!function)
		return false;
	*function_begin =
		static_cast<uintptr_t>(image_base + function->BeginAddress);
	*function_end =
		static_cast<uintptr_t>(image_base + function->EndAddress);
	return instruction >= *function_begin &&
		instruction < *function_end &&
		ExecutableMemory(
			*function_begin,
			kTextureCreationPattern.size) &&
		PatternBytesMatch(
			*function_begin,
			kTextureCreationPattern);
}

void SelectTextureCreationFrame(
	uintptr_t function_begin,
	uintptr_t function_end)
{
	bool selected = false;
	AcquireSRWLockExclusive(&resolver_selection_lock);
	if (!asset_path_runtime.texture_creation_begin) {
		asset_path_runtime.texture_creation_begin = function_begin;
		asset_path_runtime.texture_creation_end = function_end;
		selected = true;
	}
	ReleaseSRWLockExclusive(&resolver_selection_lock);
	if (selected) {
		AppendTextureCreationSelection(function_begin);
		ReportResolverDynamicIfComplete();
	}
}

bool CaptureUnrealObjectSnapshot(
	uintptr_t object,
	uintptr_t module_base,
	uintptr_t module_end,
	UnrealObjectSnapshot *snapshot)
{
	if (!ReadableMemory(object, 0x30))
		return false;

	uint8_t header[0x30] = {};
	memcpy(header, reinterpret_cast<const void*>(object), sizeof(header));
	uintptr_t vtable =
		*reinterpret_cast<const uintptr_t*>(header + 0x00);
	int32_t internal_index =
		*reinterpret_cast<const int32_t*>(header + 0x0c);
	uintptr_t class_private =
		*reinterpret_cast<const uintptr_t*>(header + 0x10);
	uintptr_t outer_private =
		*reinterpret_cast<const uintptr_t*>(header + 0x28);
	uintptr_t first_virtual_function = 0;
	if (vtable < module_base ||
			vtable >= module_end ||
			!ReadableMemory(vtable, sizeof(first_virtual_function)))
		return false;
	memcpy(
		&first_virtual_function,
		reinterpret_cast<const void*>(vtable),
		sizeof(first_virtual_function));
	if (first_virtual_function < module_base ||
			first_virtual_function >= module_end ||
			internal_index < 0 ||
			internal_index > 0x4000000 ||
			!ReadableMemory(class_private, 0x30) ||
			(outer_private &&
			 !ReadableMemory(outer_private, sizeof(void*))))
		return false;
	*snapshot = {
		object,
		vtable,
		internal_index,
		class_private,
		outer_private,
	};
	return true;
}

bool SnapshotStillMatches(
	const UnrealObjectSnapshot& expected,
	uintptr_t module_base,
	uintptr_t module_end)
{
	UnrealObjectSnapshot current = {};
	return CaptureUnrealObjectSnapshot(
				expected.object,
				module_base,
				module_end,
				&current) &&
		current.vtable == expected.vtable &&
		current.internal_index == expected.internal_index &&
		current.class_private == expected.class_private &&
		current.outer_private == expected.outer_private;
}

bool TryUnrealTextureObject(
	uintptr_t object,
	uintptr_t module_base,
	uintptr_t module_end,
	std::wstring *asset_path,
	UnrealObjectSnapshot *snapshot = nullptr)
{
	UnrealObjectSnapshot captured = {};
	if (!CaptureUnrealObjectSnapshot(
				object,
				module_base,
				module_end,
				&captured))
		return false;
	if (snapshot)
		*snapshot = captured;
	if (!ResolveGetPathNameSemantically(object, captured.class_private))
		return false;

	std::wstring class_path;
	if (!UnrealGetPathName(captured.class_private, &class_path) ||
			class_path != L"/Script/Engine.Texture2D")
		return false;
	if (!UnrealGetPathName(object, asset_path))
		return false;
	return asset_path->compare(0, 6, L"/Game/") == 0;
}

bool UnwindContext(CONTEXT *context)
{
	DWORD64 image_base = 0;
	PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
		context->Rip,
		&image_base,
		nullptr);
	if (function) {
		PVOID handler_data = nullptr;
		DWORD64 establisher_frame = 0;
		RtlVirtualUnwind(
			UNW_FLAG_NHANDLER,
			image_base,
			context->Rip,
			function,
			context,
			&handler_data,
			&establisher_frame,
			nullptr);
		return context->Rip != 0;
	}

	if (!ReadableMemory(
			static_cast<uintptr_t>(context->Rsp),
			sizeof(uintptr_t)))
		return false;
	context->Rip = *reinterpret_cast<const DWORD64*>(context->Rsp);
	context->Rsp += sizeof(DWORD64);
	return context->Rip != 0;
}

uintptr_t ReadPointer(uintptr_t address)
{
	if (!ReadableMemory(address, sizeof(uintptr_t)))
		return 0;
	__try {
		return *reinterpret_cast<const uintptr_t*>(address);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

uintptr_t ReadRhiTextureResource(uintptr_t texture)
{
	size_t offset = kRhiResourceOffsetUnknown;
	AcquireSRWLockShared(&rhi_label_identity_lock);
	offset = rhi_texture_resource_offset;
	ReleaseSRWLockShared(&rhi_label_identity_lock);
	if (!texture || offset == kRhiResourceOffsetUnknown)
		return 0;
	return ReadPointer(texture + offset);
}

bool SamePendingIdentity(
	const PendingProducerIdentity& left,
	const PendingProducerIdentity& right)
{
	return left.source == right.source &&
		left.resource == right.resource &&
		left.object.object == right.object.object &&
		left.object.vtable == right.object.vtable &&
		left.object.internal_index == right.object.internal_index &&
		left.object.class_private == right.object.class_private &&
		left.object.outer_private == right.object.outer_private;
}

void StoreResourceAssetPath(
	uintptr_t resource_address,
	const std::wstring& asset_path)
{
	ID3D11Resource *resource = reinterpret_cast<ID3D11Resource*>(
		resource_address);
	if (!resource)
		return;

	uint32_t hash = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool observed = false;
	EnterCriticalSectionPretty(&G->mResourcesLock);
	auto info = lookup_resource_handle_info(resource);
	if (info != G->mResources.end() &&
			info->second.type == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		info->second.asset_path = asset_path;
		hash = info->second.hash;
		width = info->second.desc2D.Width;
		height = info->second.desc2D.Height;
		observed = true;
	}
	LeaveCriticalSection(&G->mResourcesLock);
	if (observed)
		ObserveAssetHashForAuthoring(
			resource_address,
			asset_path,
			hash,
			width,
			height);
}

void BackfillPendingProducerIdentities()
{
	uintptr_t module_base = 0;
	uintptr_t module_end = 0;
	if (!GetModuleRange(&module_base, &module_end))
		return;

	std::vector<std::pair<uintptr_t, PendingProducerIdentity>> pending;
	AcquireSRWLockShared(&producer_identity_lock);
	pending.reserve(pending_producer_wrappers.size());
	for (const auto& identity : pending_producer_wrappers)
		pending.push_back(identity);
	ReleaseSRWLockShared(&producer_identity_lock);

	unsigned resolved = 0;
	unsigned dropped = 0;
	for (const auto& candidate : pending) {
		const uintptr_t wrapper = candidate.first;
		const PendingProducerIdentity& expected = candidate.second;
		std::wstring asset_path;
		bool valid =
			ReadRhiTextureResource(wrapper) == expected.resource &&
			SnapshotStillMatches(
				expected.object,
				module_base,
				module_end) &&
			TryUnrealTextureObject(
				expected.object.object,
				module_base,
				module_end,
				&asset_path);

		AcquireSRWLockExclusive(&producer_identity_lock);
		auto current = pending_producer_wrappers.find(wrapper);
		if (current == pending_producer_wrappers.end() ||
				!SamePendingIdentity(current->second, expected)) {
			ReleaseSRWLockExclusive(&producer_identity_lock);
			continue;
		}
		if (!valid ||
				ReadRhiTextureResource(wrapper) !=
					expected.resource ||
				!SnapshotStillMatches(
					expected.object,
					module_base,
					module_end)) {
			ErasePendingProducerWrapper(current);
			++dropped;
			ReleaseSRWLockExclusive(&producer_identity_lock);
			continue;
		}
		ErasePendingProducerWrapper(current);
		StoreProducerWrapperIdentity(
			wrapper,
			expected.source,
			expected.resource,
			asset_path);
		ReleaseSRWLockExclusive(&producer_identity_lock);

		StoreResourceAssetPath(expected.resource, asset_path);
		++resolved;
	}

	AcquireSRWLockShared(&producer_identity_lock);
	size_t remaining = pending_producer_wrappers.size();
	ReleaseSRWLockShared(&producer_identity_lock);
	FILE *report = OpenAssetPathResolverReport(L"ab");
	if (report) {
		fprintf(
			report,
			"semantic=pending-backfill resolved=%u dropped=%u remaining=%llu\n",
			resolved,
			dropped,
			static_cast<unsigned long long>(remaining));
		fclose(report);
	}
}

bool FindKnownProducerOwner(const CONTEXT& context, uintptr_t *owner)
{
	const uintptr_t registers[] = {
		static_cast<uintptr_t>(context.Rbx),
		static_cast<uintptr_t>(context.Rsi),
		static_cast<uintptr_t>(context.Rdi),
		static_cast<uintptr_t>(context.R12),
		static_cast<uintptr_t>(context.R13),
		static_cast<uintptr_t>(context.R14),
		static_cast<uintptr_t>(context.R15),
		static_cast<uintptr_t>(context.Rbp),
	};
	AcquireSRWLockShared(&producer_identity_lock);
	for (uintptr_t candidate : registers) {
		if (producer_owner_paths.find(candidate) !=
					producer_owner_paths.end() ||
				pending_producer_owners.find(candidate) !=
					pending_producer_owners.end()) {
			*owner = candidate;
			ReleaseSRWLockShared(&producer_identity_lock);
			return true;
		}
	}
	ReleaseSRWLockShared(&producer_identity_lock);
	return false;
}

bool CaptureProducerOwner(uintptr_t *owner)
{
	CONTEXT context = {};
	RtlCaptureContext(&context);
	for (unsigned depth = 0; depth < 32 && context.Rip; ++depth) {
		if (FindKnownProducerOwner(context, owner))
			return true;
		if (!UnwindContext(&context))
			break;
	}
	return false;
}

uintptr_t __fastcall ProducerOwnerHook(
	uintptr_t owner,
	uintptr_t source,
	uintptr_t argument)
{
	if (!AssetPathBridgeReady() ||
			!AssetPathTextureIdentityRequired())
		return original_producer_owner_constructor(
			owner,
			source,
			argument);

	uintptr_t module_base = 0;
	uintptr_t module_end = 0;
	std::wstring asset_path;
	UnrealObjectSnapshot snapshot = {};
	bool identified =
		GetModuleRange(&module_base, &module_end) &&
		TryUnrealTextureObject(
			source,
			module_base,
			module_end,
			&asset_path,
			&snapshot);
	AcquireSRWLockExclusive(&producer_identity_lock);
	producer_owner_paths.erase(owner);
	pending_producer_owners.erase(owner);
	if (identified && owner) {
		if (producer_owner_paths.size() >=
				kMaxProducerIdentityEntries)
			producer_owner_paths.erase(
				producer_owner_paths.begin());
		producer_owner_paths[owner] = asset_path;
	} else if (snapshot.object && owner) {
		if (pending_producer_owners.size() >=
				kMaxProducerIdentityEntries)
			pending_producer_owners.erase(
				pending_producer_owners.begin());
		pending_producer_owners[owner] = snapshot;
	}
	ReleaseSRWLockExclusive(&producer_identity_lock);
	return original_producer_owner_constructor(owner, source, argument);
}

void __fastcall TextureProducerHook(
	uintptr_t source,
	uintptr_t unused,
	uintptr_t wrapper,
	uint32_t subresource,
	uint8_t flag)
{
	if (!AssetPathBridgeReady() ||
			!AssetPathTextureIdentityRequired()) {
		original_texture_producer(
			source,
			unused,
			wrapper,
			subresource,
			flag);
		return;
	}

	uintptr_t owner = 0;
	uintptr_t cached_resource = 0;
	std::wstring asset_path;
	UnrealObjectSnapshot pending_object = {};
	bool owner_identified = false;
	bool pending_owner_identified = false;
	CaptureProducerOwner(&owner);
	AcquireSRWLockExclusive(&producer_identity_lock);
	auto owner_path = producer_owner_paths.find(owner);
	if (owner_path != producer_owner_paths.end()) {
		asset_path = owner_path->second;
		owner_identified = true;
		producer_owner_paths.erase(owner_path);
	} else {
		auto pending_owner = pending_producer_owners.find(owner);
		if (pending_owner != pending_producer_owners.end()) {
			pending_object = pending_owner->second;
			pending_owner_identified = true;
			pending_producer_owners.erase(pending_owner);
		} else {
			// A wrapper address is reusable only while its producer source matches.
			auto wrapper_path = producer_wrapper_paths.find(wrapper);
			if (wrapper_path != producer_wrapper_paths.end()) {
					cached_resource = ReadRhiTextureResource(wrapper);
				if (wrapper_path->second.source == source &&
						wrapper_path->second.resource &&
						wrapper_path->second.resource ==
							cached_resource)
					asset_path = wrapper_path->second.asset_path;
				else
					EraseProducerWrapperIdentity(wrapper_path);
			}
		}
	}
	ReleaseSRWLockExclusive(&producer_identity_lock);

	std::wstring previous_path =
		std::move(active_producer_asset_path);
	if (owner_identified)
		active_producer_asset_path = asset_path;
	else
		active_producer_asset_path.clear();
	original_texture_producer(
		source,
		unused,
		wrapper,
		subresource,
		flag);
	active_producer_asset_path = std::move(previous_path);

	uintptr_t resource_address = ReadRhiTextureResource(wrapper);
	if (asset_path.empty() && pending_owner_identified) {
		uintptr_t module_base = 0;
		uintptr_t module_end = 0;
		if (GetModuleRange(&module_base, &module_end) &&
				SnapshotStillMatches(
					pending_object,
					module_base,
					module_end) &&
				TryUnrealTextureObject(
					pending_object.object,
					module_base,
					module_end,
					&asset_path)) {
			owner_identified = true;
		} else {
			AcquireSRWLockExclusive(&producer_identity_lock);
			StorePendingProducerIdentity(
				wrapper,
				source,
				resource_address,
				pending_object);
			ReleaseSRWLockExclusive(&producer_identity_lock);
			return;
		}
	}
	if (asset_path.empty())
		return;
	if (!owner_identified &&
			resource_address != cached_resource) {
		AcquireSRWLockExclusive(&producer_identity_lock);
		auto wrapper_path = producer_wrapper_paths.find(wrapper);
		if (wrapper_path != producer_wrapper_paths.end())
			EraseProducerWrapperIdentity(wrapper_path);
		ReleaseSRWLockExclusive(&producer_identity_lock);
		return;
	}
	AcquireSRWLockExclusive(&producer_identity_lock);
	StoreProducerWrapperIdentity(
		wrapper,
		source,
		resource_address,
		asset_path);
	ReleaseSRWLockExclusive(&producer_identity_lock);

	StoreResourceAssetPath(resource_address, asset_path);
}

bool CopyRhiTextureLabel(
	const wchar_t *name,
	std::wstring *label)
{
	if (!name || !label)
		return false;

	label->clear();
	label->reserve(128);
	__try {
		for (size_t i = 0; i < 512; ++i) {
			wchar_t character = name[i];
			if (!character)
				return !label->empty();
			if (character < 0x20)
				return false;
			label->push_back(character);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		label->clear();
		return false;
	}
	label->clear();
	return false;
}

std::string RhiTextureLabelUtf8(const std::wstring& label)
{
	if (label.empty())
		return {};
	int size = WideCharToMultiByte(
		CP_UTF8,
		0,
		label.data(),
		static_cast<int>(label.size()),
		nullptr,
		0,
		nullptr,
		nullptr);
	if (size <= 0)
		return {};
	std::string utf8(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(
		CP_UTF8,
		0,
		label.data(),
		static_cast<int>(label.size()),
		&utf8[0],
		size,
		nullptr,
		nullptr);
	return utf8;
}

int StoreRhiTextureLabel(
	uintptr_t resource_address,
	const std::wstring& label)
{
	int result = 0;
	EnterCriticalSectionPretty(&G->mResourcesLock);
	auto resource = G->mResources.find(
		reinterpret_cast<ID3D11Resource*>(resource_address));
	if (resource != G->mResources.end() &&
			resource->second.type == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		resource->second.asset_name = label;
		result = resource->second.asset_path.empty() ? 1 : 2;
	}
	LeaveCriticalSection(&G->mResourcesLock);
	return result;
}

void LogRhiTextureLabelBinding(
	LONG sequence,
	uintptr_t texture,
	const std::wstring& label,
	size_t offset,
	uintptr_t resource,
	int result)
{
	if (sequence > kRhiTextureLabelProbeLimit)
		return;
	std::string utf8 = RhiTextureLabelUtf8(label);
	const char *result_name =
		result == 1 ? "stored-name-only" :
		result == 2 ? "stored-name-with-path" :
		"resource-missing";
	AcquireSRWLockExclusive(&rhi_label_report_lock);
	FILE *report = OpenRhiTextureLabelProbeReport(L"ab");
	if (report) {
		fprintf(
			report,
			"label sequence=%ld mode=bound texture=0x%llx name=%s "
			"offset=0x%zx resource=0x%llx result=%s\n",
			sequence,
			static_cast<unsigned long long>(texture),
			utf8.c_str(),
			offset,
			static_cast<unsigned long long>(resource),
			result_name);
		fclose(report);
	}
	ReleaseSRWLockExclusive(&rhi_label_report_lock);
}

void ProbeRhiTextureLabel(
	uintptr_t texture,
	const wchar_t *name)
{
	LONG sequence = rhi_label_probe_count.fetch_add(
		1,
		std::memory_order_acq_rel) + 1;

	std::wstring label;
	if (!CopyRhiTextureLabel(name, &label))
		return;

	size_t selected_offset = kRhiResourceOffsetUnknown;
	AcquireSRWLockShared(&rhi_label_identity_lock);
	selected_offset = rhi_texture_resource_offset;
	ReleaseSRWLockShared(&rhi_label_identity_lock);
	if (selected_offset != kRhiResourceOffsetUnknown) {
		uintptr_t resource = ReadPointer(texture + selected_offset);
		int result = StoreRhiTextureLabel(resource, label);
		LogRhiTextureLabelBinding(
			sequence,
			texture,
			label,
			selected_offset,
			resource,
			result);
		return;
	}

	std::vector<RhiTextureResourceMatch> matches;
	if (ReadableMemory(texture, kRhiTextureObjectScanSize)) {
		EnterCriticalSectionPretty(&G->mResourcesLock);
		for (size_t offset = 0;
				offset + sizeof(uintptr_t) <= kRhiTextureObjectScanSize;
				offset += sizeof(uintptr_t)) {
			uintptr_t candidate = *reinterpret_cast<const uintptr_t*>(
				texture + offset);
			auto resource = G->mResources.find(
				reinterpret_cast<ID3D11Resource*>(candidate));
			if (resource == G->mResources.end() ||
					resource->second.type !=
						D3D11_RESOURCE_DIMENSION_TEXTURE2D)
				continue;
			matches.push_back({
				offset,
				candidate,
				resource->second.hash,
			});
		}
		LeaveCriticalSection(&G->mResourcesLock);
	}

	if (sequence <= kRhiTextureLabelProbeLimit) {
		std::string utf8 = RhiTextureLabelUtf8(label);
		AcquireSRWLockExclusive(&rhi_label_report_lock);
		FILE *report = OpenRhiTextureLabelProbeReport(L"ab");
		if (report) {
			fprintf(
				report,
				"label sequence=%ld mode=learning texture=0x%llx "
				"name=%s matches=%zu",
				sequence,
				static_cast<unsigned long long>(texture),
				utf8.c_str(),
				matches.size());
			for (const RhiTextureResourceMatch& match : matches) {
				fprintf(
					report,
					" offset=0x%zx resource=0x%llx hash=%08x",
					match.offset,
					static_cast<unsigned long long>(match.resource),
					match.hash);
			}
			fprintf(report, "\n");
			fclose(report);
		}
		ReleaseSRWLockExclusive(&rhi_label_report_lock);
	}

	std::vector<PendingRhiTextureLabel> backfill;
	unsigned best_votes = 0;
	unsigned runner_up_votes = 0;
	size_t best_offset = kRhiResourceOffsetUnknown;
	size_t sample_count = 0;
	AcquireSRWLockExclusive(&rhi_label_identity_lock);
	if (rhi_texture_resource_offset == kRhiResourceOffsetUnknown) {
		++rhi_texture_resource_offset_samples;
		for (const RhiTextureResourceMatch& match : matches)
			++rhi_texture_resource_offset_votes[match.offset];
		pending_rhi_texture_labels.push_back({label, matches});
		for (const auto& vote : rhi_texture_resource_offset_votes) {
			if (vote.second > best_votes) {
				runner_up_votes = best_votes;
				best_votes = vote.second;
				best_offset = vote.first;
			} else if (vote.second > runner_up_votes) {
				runner_up_votes = vote.second;
			}
		}
		sample_count = rhi_texture_resource_offset_samples;
		if (sample_count >= kRhiResourceOffsetConsensusSamples &&
				best_votes >= kRhiResourceOffsetConsensusHits &&
				best_votes >= runner_up_votes +
					kRhiResourceOffsetConsensusLead &&
				best_votes * 5 >= sample_count * 4) {
			rhi_texture_resource_offset = best_offset;
			backfill.swap(pending_rhi_texture_labels);
		}
	} else {
		best_offset = rhi_texture_resource_offset;
	}
	ReleaseSRWLockExclusive(&rhi_label_identity_lock);

	if (backfill.empty())
		return;

	unsigned stored = 0;
	unsigned preserved = 0;
	unsigned missing = 0;
	for (const PendingRhiTextureLabel& pending : backfill) {
		uintptr_t resource = 0;
		for (const RhiTextureResourceMatch& match : pending.matches) {
			if (match.offset == best_offset) {
				resource = match.resource;
				break;
			}
		}
		int result = StoreRhiTextureLabel(resource, pending.label);
		if (result == 1)
			++stored;
		else if (result == 2)
			++preserved;
		else
			++missing;
	}

	AcquireSRWLockExclusive(&rhi_label_report_lock);
	FILE *report = OpenRhiTextureLabelProbeReport(L"ab");
	if (report) {
		fprintf(
			report,
			"offset_consensus selected=0x%zx samples=%zu hits=%u "
			"runner_up=%u backfill_stored=%u backfill_preserved=%u "
			"backfill_missing=%u\n",
			best_offset,
			sample_count,
			best_votes,
			runner_up_votes,
			stored,
			preserved,
			missing);
		fclose(report);
	}
	ReleaseSRWLockExclusive(&rhi_label_report_lock);
	BackfillPendingProducerIdentities();
}

void __fastcall RhiTextureLabelHook(
	uintptr_t dynamic_rhi,
	uintptr_t texture,
	const wchar_t *name)
{
	RhiTextureLabelFunction original = original_rhi_texture_label;
	if (!original)
		return;
	original(dynamic_rhi, texture, name);
	ProbeRhiTextureLabel(texture, name);
}

}

bool AssetPathTextureIdentityRequired()
{
	return asset_path_usage.load(
		std::memory_order_relaxed) != 0;
}

static DWORD WINAPI AssetPathResolverWorker(void*)
{
	bool installed = InstallAssetPathTextureIdentityBridge();
	LogInfo(
		"Asset Path resolver worker ready=%u\n",
		installed ? 1 : 0);
	return 0;
}

void StartAssetPathTextureIdentityBridgeWorker()
{
	if (resolver_worker_started.exchange(
			true,
			std::memory_order_acq_rel))
		return;

	HANDLE thread = CreateThread(
		nullptr,
		0,
		AssetPathResolverWorker,
		nullptr,
		0,
		nullptr);
	if (thread)
		CloseHandle(thread);
	else
		resolver_worker_started.store(
			false,
			std::memory_order_release);
}

static void SetAssetPathUsage(LONG usage, bool enabled)
{
	if (enabled)
		asset_path_usage.fetch_or(
			usage,
			std::memory_order_relaxed);
	else
		asset_path_usage.fetch_and(
			~usage,
			std::memory_order_relaxed);
}

void SetAssetPathTextureOverridesEnabled(bool enabled)
{
	SetAssetPathUsage(kAssetPathUsageOverrides, enabled);
}

void SetAssetPathFrameAnalysisEnabled(bool enabled)
{
	SetAssetPathUsage(kAssetPathUsageFrameAnalysis, enabled);
}

std::wstring ExtractUnrealAssetName(const std::wstring& asset_path)
{
	size_t separator = asset_path.rfind(L'.');
	if (separator != std::wstring::npos &&
			separator + 1 < asset_path.size())
		return asset_path.substr(separator + 1);

	separator = asset_path.rfind(L'/');
	if (separator != std::wstring::npos &&
			separator + 1 < asset_path.size())
		return asset_path.substr(separator + 1);
	return asset_path;
}

void ObserveAssetPathNameMatch(
	const std::wstring& asset_name,
	const std::wstring& asset_path)
{
	AcquireSRWLockExclusive(&asset_path_match_report_lock);
	bool first = reported_asset_name_matches.insert(asset_name).second;
	ReleaseSRWLockExclusive(&asset_path_match_report_lock);
	if (!first)
		return;

	std::string name_utf8 = RhiTextureLabelUtf8(asset_name);
	std::string path_utf8 = RhiTextureLabelUtf8(asset_path);
	AcquireSRWLockExclusive(&rhi_label_report_lock);
	FILE *report = OpenRhiTextureLabelProbeReport(L"ab");
	if (report) {
		fprintf(
			report,
			"name_match asset_name=%s asset_path=%s\n",
			name_utf8.c_str(),
			path_utf8.empty() ? "<unavailable>" : path_utf8.c_str());
		fclose(report);
	}
	ReleaseSRWLockExclusive(&rhi_label_report_lock);
}

bool InstallAssetPathTextureIdentityBridge()
{
	LONG expected = kInstallStateUninitialized;
	if (!install_state.compare_exchange_strong(
			expected,
			kInstallStateInitializing,
			std::memory_order_acq_rel,
			std::memory_order_acquire)) {
		if (expected == kInstallStateInitializing) {
			if (install_wait_timed_out.load(
					std::memory_order_acquire))
				return false;
			AcquireSRWLockExclusive(&install_wait_lock);
			while (install_state.load(
						std::memory_order_acquire) ==
					kInstallStateInitializing) {
				if (!SleepConditionVariableSRW(
						&install_condition,
						&install_wait_lock,
						5000,
						0)) {
					if (install_state.load(
								std::memory_order_acquire) ==
							kInstallStateInitializing)
						install_wait_timed_out.store(
							true,
							std::memory_order_release);
					break;
				}
			}
			ReleaseSRWLockExclusive(&install_wait_lock);
		}
		return AssetPathBridgeReady();
	}

	bool installed = false;
	if (!ResolveAssetPathRuntime()) {
		LogInfo("Asset Path producer bridge initialization failed\n");
	} else {
		SIZE_T owner_hook_id = 0;
		SIZE_T producer_hook_id = 0;
		DWORD owner_error = cHookMgr.Hook(
			&owner_hook_id,
			reinterpret_cast<void**>(
				&original_producer_owner_constructor),
			reinterpret_cast<void*>(
				asset_path_runtime.producer_owner_constructor),
			reinterpret_cast<void*>(ProducerOwnerHook));
		DWORD producer_error = cHookMgr.Hook(
			&producer_hook_id,
			reinterpret_cast<void**>(&original_texture_producer),
			reinterpret_cast<void*>(asset_path_runtime.texture_producer),
			reinterpret_cast<void*>(TextureProducerHook));
		LogInfo(
			"Asset Path producer bridge hooks: owner=0x%x producer=0x%x\n",
			owner_error,
			producer_error);
		installed =
			owner_error == ERROR_SUCCESS &&
			producer_error == ERROR_SUCCESS;
	}

	AcquireSRWLockExclusive(&install_wait_lock);
	install_state.store(
		installed
			? kInstallStateReady
			: kInstallStateFailed,
		std::memory_order_release);
	WakeAllConditionVariable(&install_condition);
	ReleaseSRWLockExclusive(&install_wait_lock);
	return installed;
}

void InvalidateAssetPathTextureResource(uintptr_t resource)
{
	if (!resource)
		return;

	AcquireSRWLockExclusive(&producer_identity_lock);
	auto pending_owner = pending_resource_wrappers.find(resource);
	if (pending_owner != pending_resource_wrappers.end()) {
		auto identity = pending_producer_wrappers.find(
			pending_owner->second);
		if (identity != pending_producer_wrappers.end() &&
				identity->second.resource == resource)
			ErasePendingProducerWrapper(identity);
		else
			pending_resource_wrappers.erase(pending_owner);
	}
	auto resource_owner = producer_resource_wrappers.find(resource);
	if (resource_owner != producer_resource_wrappers.end()) {
		auto identity = producer_wrapper_paths.find(
			resource_owner->second);
		if (identity != producer_wrapper_paths.end() &&
				identity->second.resource == resource)
			EraseProducerWrapperIdentity(identity);
		else
			producer_resource_wrappers.erase(resource_owner);
	}
	ReleaseSRWLockExclusive(&producer_identity_lock);
}

bool CaptureUnrealTextureAssetPathAtCreation(
	std::wstring *asset_path)
{
	if (!asset_path || !AssetPathBridgeReady())
		return false;
	if (!active_producer_asset_path.empty()) {
		*asset_path = active_producer_asset_path;
		return true;
	}

	uintptr_t module_base = 0;
	uintptr_t module_end = 0;
	if (!asset_path_runtime.free_memory ||
			!GetModuleRange(&module_base, &module_end)) {
		static LONG unsupported_logged = 0;
		if (!InterlockedCompareExchange(&unsupported_logged, 1, 0))
			LogInfo(
				"Asset Path texture identity disabled: unsupported game executable\n");
		return false;
	}

	CONTEXT context = {};
	RtlCaptureContext(&context);
	for (unsigned depth = 0; depth < 32 && context.Rip; ++depth) {
		uintptr_t function_begin = 0;
		uintptr_t function_end = 0;
		if (TextureCreationFrameCandidate(
				context.Rip,
				&function_begin,
				&function_end)) {
			CONTEXT search_context = context;
			if (!UnwindContext(&search_context))
				break;
			for (unsigned frame = 0;
					frame < kTextureObjectSearchFrames;
					++frame) {
				if (!UnwindContext(&search_context))
					break;
				uintptr_t registers[] = {
					static_cast<uintptr_t>(search_context.Rbx),
					static_cast<uintptr_t>(search_context.Rsi),
					static_cast<uintptr_t>(search_context.Rdi),
					static_cast<uintptr_t>(search_context.R12),
					static_cast<uintptr_t>(search_context.R13),
					static_cast<uintptr_t>(search_context.R14),
					static_cast<uintptr_t>(search_context.R15),
					static_cast<uintptr_t>(search_context.Rbp),
				};
				for (uintptr_t object : registers) {
					std::wstring candidate;
					if (TryUnrealTextureObject(
							object,
							module_base,
							module_end,
							&candidate)) {
						SelectTextureCreationFrame(
							function_begin,
							function_end);
						*asset_path = std::move(candidate);
						return true;
					}
				}
			}
		}
		if (!UnwindContext(&context))
			break;
	}
	return false;
}

bool LooksLikeRhiTextureLabelFunction(uintptr_t target)
{
	constexpr size_t kValidationBytes = 96;
	if (!ExecutableMemory(target, kValidationBytes))
		return false;

	const uint8_t *code = reinterpret_cast<const uint8_t*>(target);
	bool moves_name_argument = false;
	bool moves_texture_argument = false;
	bool stores_name_number = false;
	bool stores_name_suffix = false;
	int32_t number_offset = 0;
	int32_t suffix_offset = 0;
	for (size_t i = 0; i + 8 < kValidationBytes; ++i) {
		if (code[i] == 0x49 &&
				code[i + 1] == 0x8b &&
				code[i + 2] == 0xc0)
			moves_name_argument = true;
		if (code[i] == 0x48 &&
				code[i + 1] == 0x8b &&
				code[i + 2] == 0xda)
			moves_texture_argument = true;
		if (code[i] == 0xf2 &&
				code[i + 1] == 0x0f &&
				code[i + 2] == 0x11 &&
				code[i + 3] == 0x83) {
			memcpy(
				&number_offset,
				code + i + 4,
				sizeof(number_offset));
			stores_name_number = true;
		}
		if (code[i] == 0x89 &&
				code[i + 1] == 0x83) {
			memcpy(
				&suffix_offset,
				code + i + 2,
				sizeof(suffix_offset));
			stores_name_suffix = true;
		}
	}
	return moves_name_argument &&
		moves_texture_argument &&
		stores_name_number &&
		stores_name_suffix &&
		suffix_offset == number_offset + 8;
}

void ObserveUnrealRhiTextureCreation()
{
	if (rhi_probe_state.load(std::memory_order_acquire) != 0)
		return;

	LONG attempt = rhi_probe_attempts.fetch_add(
		1,
		std::memory_order_acq_rel) + 1;
	if (attempt > kRhiProbeAttemptLimit)
		return;

	uintptr_t module_base = 0;
	uintptr_t module_end = 0;
	if (!GetModuleRange(&module_base, &module_end))
		return;

	CONTEXT context = {};
	RtlCaptureContext(&context);
	for (unsigned depth = 0; depth < 32 && context.Rip; ++depth) {
		uintptr_t function_begin = 0;
		uintptr_t function_end = 0;
		DWORD64 image_base = 0;
		PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
			context.Rip,
			&image_base,
			nullptr);
		if (function) {
			function_begin =
				static_cast<uintptr_t>(image_base + function->BeginAddress);
			function_end =
				static_cast<uintptr_t>(image_base + function->EndAddress);
		}

		if (function_begin >= module_base &&
				function_begin < module_end &&
				function_end <= module_end &&
				function_end > function_begin) {
			uintptr_t saved_this = 0;
			if (ReadableMemory(function_begin, 128)) {
				const uint8_t *code =
					reinterpret_cast<const uint8_t*>(function_begin);
				for (unsigned i = 0; i + 7 < 128; ++i) {
					if (code[i] == 0x48 &&
							code[i + 1] == 0x89 &&
							code[i + 2] == 0x4d) {
						int8_t displacement =
							static_cast<int8_t>(code[i + 3]);
						saved_this = ReadPointer(
							static_cast<uintptr_t>(context.Rbp) +
							displacement);
						break;
					}
					if (code[i] == 0x48 &&
							code[i + 1] == 0x89 &&
							code[i + 2] == 0x8d) {
						int32_t displacement = 0;
						memcpy(
							&displacement,
							code + i + 3,
							sizeof(displacement));
						saved_this = ReadPointer(
							static_cast<uintptr_t>(context.Rbp) +
							displacement);
						break;
					}
				}
			}

			uintptr_t objects[] = {saved_this};
			unsigned object_count = ARRAYSIZE(objects);
			for (unsigned register_index = 0;
					register_index < object_count;
					++register_index) {
				uintptr_t object = objects[register_index];
				uintptr_t vtable = ReadPointer(object);
				if (vtable < module_base ||
						vtable >= module_end ||
						!ReadableMemory(
							vtable,
							kRhiTextureCreationVtableLimit *
								sizeof(uintptr_t)))
					continue;

				for (unsigned slot = kRhiTextureCreationSlotMinimum;
						slot <= kRhiTextureCreationSlotMaximum;
						++slot) {
					uintptr_t entry = ReadPointer(
						vtable + slot * sizeof(uintptr_t));
					if (entry != function_begin)
						continue;

					unsigned label_slot =
						slot + kRhiTextureLabelSlotDelta;
					if (label_slot >= kRhiTextureCreationVtableLimit)
						continue;
					uintptr_t label_target = ReadPointer(
						vtable + label_slot * sizeof(uintptr_t));
					if (label_target < module_base ||
							label_target >= module_end ||
							!LooksLikeRhiTextureLabelFunction(label_target))
						continue;

					LONG expected = 0;
					if (!rhi_probe_state.compare_exchange_strong(
							expected,
							1,
							std::memory_order_acq_rel,
							std::memory_order_acquire))
						return;

						AcquireSRWLockExclusive(&rhi_label_report_lock);
						FILE *report =
							OpenRhiTextureLabelProbeReport(L"wb");
						if (report) {
							fprintf(
								report,
								"mode=independent-rhi-label attempt=%ld depth=%u "
								"source=%s index=%u "
								"object=0x%llx vtable=0x%llx "
							"creation_rva=0x%llx creation_slot=%u "
							"label_slot=%u label_rva=0x%llx\n",
							attempt,
							depth,
							"saved_rcx",
							0,
							static_cast<unsigned long long>(object),
							static_cast<unsigned long long>(vtable),
							static_cast<unsigned long long>(
								function_begin - module_base),
							slot,
							label_slot,
							static_cast<unsigned long long>(
								label_target - module_base));
						fprintf(report, "label_bytes=");
						const uint8_t *bytes =
							reinterpret_cast<const uint8_t*>(
								label_target);
						for (unsigned i = 0; i < 64; ++i)
							fprintf(report, "%02x", bytes[i]);
							fprintf(report, "\n");
							fclose(report);
						}
						ReleaseSRWLockExclusive(&rhi_label_report_lock);

						SIZE_T hook_id = 0;
						DWORD hook_error = cHookMgr.Hook(
							&hook_id,
							reinterpret_cast<void**>(
								&original_rhi_texture_label),
							reinterpret_cast<void*>(label_target),
							reinterpret_cast<void*>(RhiTextureLabelHook));
						AcquireSRWLockExclusive(&rhi_label_report_lock);
						report = OpenRhiTextureLabelProbeReport(L"ab");
						if (report) {
							fprintf(
								report,
								"label_hook error=0x%x target=0x%llx\n",
								hook_error,
								static_cast<unsigned long long>(
									label_target));
							fclose(report);
						}
						ReleaseSRWLockExclusive(&rhi_label_report_lock);
						if (hook_error)
							rhi_probe_state.store(
								-1,
								std::memory_order_release);
						return;
				}
			}
		}
		if (!UnwindContext(&context))
			break;
	}

	if (attempt == kRhiProbeAttemptLimit) {
		LONG expected = 0;
		if (rhi_probe_state.compare_exchange_strong(
				expected,
				-1,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			FILE *report = OpenRhiTextureLabelProbeReport(L"wb");
			if (report) {
				fprintf(
					report,
					"mode=discovery-failed attempts=%ld\n",
					attempt);
				fclose(report);
			}
		}
	}
}
