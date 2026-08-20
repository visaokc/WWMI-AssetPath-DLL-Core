#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

struct AssetHashObservation
{
	uint32_t hash;
	uint32_t width;
	uint32_t height;
};

typedef std::map<std::wstring, std::vector<AssetHashObservation>>
	AssetHashObservationMap;

struct AssetHashPathIdentity
{
	std::wstring asset_path;
	std::vector<AssetHashObservation> hashes;
};

typedef std::map<uint32_t, AssetHashPathIdentity>
	AssetHashPathIdentityMap;

struct VbHashObservation
{
	std::wstring asset_path;
	uint32_t hash;
	uint32_t first_index;
	uint32_t index_count;
	uint32_t vertex_count;
};

typedef std::vector<VbHashObservation> VbHashObservationList;

struct ShapeKeyHashObservation
{
	uint32_t hash;
	uint32_t byte_width;
	uint32_t structure_byte_stride;
	uint32_t filter_index;
	uint32_t slot;
	bool unordered_access;
};

typedef std::vector<ShapeKeyHashObservation> ShapeKeyHashObservationList;

std::set<std::wstring> CollectAssetHashIniIdentities(
	const std::wstring& document);

bool AssetHashIniUsesIdentityAliases(const std::wstring& document);

bool AssetHashIniNeedsCanonicalization(const std::wstring& document);

std::set<uint32_t> CollectAssetHashIniLegacyHashes(
	const std::wstring& document);

std::wstring TransformAssetHashIniDocument(
	const std::wstring& source,
	const std::wstring& previous_output,
	const AssetHashObservationMap& observations,
	const AssetHashPathIdentityMap& legacy_hash_identities,
	const std::set<uint32_t>& ambiguous_hashes,
	const std::wstring& game_version);

std::wstring TransformAssetHashIniDocumentToPaths(
	const std::wstring& source,
	const AssetHashObservationMap& observations,
	const AssetHashPathIdentityMap& legacy_hash_identities,
	const std::set<uint32_t>& ambiguous_hashes,
	const std::wstring& game_version);

std::wstring TransformAssetHashIniDocumentToCleanPaths(
	const std::wstring& source,
	const AssetHashObservationMap& observations,
	const AssetHashPathIdentityMap& legacy_hash_identities,
	const std::set<uint32_t>& ambiguous_hashes,
	const std::wstring& game_version);

std::wstring TransformVbHashIniDocument(
	const std::wstring& source,
	const VbHashObservationList& observations,
	const ShapeKeyHashObservationList& shape_key_observations = {},
	bool allow_pathless_observations = false,
	uint32_t selected_target_hash = 0,
	uint32_t selected_target_vertex_count = 0);

std::set<uint32_t> CollectVbHashIniCandidates(
	const std::wstring& source);

bool CollectVbHashIniMeshVertexCount(
	const std::wstring& source,
	uint32_t *vertex_count);

std::set<std::tuple<uint32_t, uint32_t, uint32_t>>
CollectVbHashIniSignatures(const std::wstring& source);

std::set<std::pair<uint32_t, uint32_t>>
CollectVbHashIniDrawSignatures(const std::wstring& source);

std::set<uint32_t> CollectShapeKeyHashIniCandidates(
	const std::wstring& source);
