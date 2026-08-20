#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

#include "../DirectX11/AssetHashIniDocument.h"

namespace
{
const wchar_t *kIdentity = L"match_asset_path=/game/test/texture.texture";

std::wstring SourceDocument()
{
	return
		L"[TextureOverrideTest]\r\n"
		L"match_asset_path = /Game/Test/Texture.Texture\r\n"
		L"this = ResourceTest\r\n";
}

AssetHashObservationMap Observations(
	std::initializer_list<AssetHashObservation> hashes)
{
	AssetHashObservationMap observations;
	observations[kIdentity] = hashes;
	return observations;
}

std::wstring Transform(
	const std::wstring& previous,
	std::initializer_list<AssetHashObservation> hashes,
	const std::set<uint32_t>& ambiguous = {})
{
	return TransformAssetHashIniDocument(
		SourceDocument(),
		previous,
		Observations(hashes),
		{},
		ambiguous,
		L"test-version");
}

std::wstring TransformWithoutObservations(const std::wstring& previous)
{
	return TransformAssetHashIniDocument(
		SourceDocument(),
		previous,
		{},
		{},
		{},
		L"test-version");
}

std::wstring TransformAggressive(
	const std::wstring& current,
	std::initializer_list<AssetHashObservation> hashes)
{
	return TransformAssetHashIniDocument(
		current,
		current,
		Observations(hashes),
		{},
		{},
		L"test-version");
}

std::wstring HashLine(uint32_t hash)
{
	wchar_t text[32];
	swprintf(text, 32, L"hash = %08x", hash);
	return text;
}

AssetHashPathIdentityMap RuntimeIdentities(
	const std::wstring& path,
	std::initializer_list<AssetHashObservation> hashes)
{
	AssetHashPathIdentityMap identities;
	std::vector<AssetHashObservation> observations(hashes);
	for (const AssetHashObservation& observation : observations) {
		identities.emplace(
			observation.hash,
			AssetHashPathIdentity{path, observations});
	}
	return identities;
}

size_t Count(const std::wstring& text, const std::wstring& needle)
{
	size_t count = 0;
	for (size_t offset = 0;
			(offset = text.find(needle, offset)) != std::wstring::npos;
			offset += needle.size())
		++count;
	return count;
}

void Require(bool condition, const char *message)
{
	if (condition)
		return;
	std::cerr << "FAILED: " << message << '\n';
	std::exit(1);
}

void RequireHash(const std::wstring& text, uint32_t hash, bool present)
{
	const bool found = text.find(HashLine(hash)) != std::wstring::npos;
	if (found == present)
		return;
	std::wcerr << (present ? L"FAILED: expected hash missing: " :
		L"FAILED: stale hash retained: ") << HashLine(hash) << L'\n';
	std::exit(1);
}

void TestSingleMipReplacementPreservesUnseenMip()
{
	const std::wstring initial = Transform(
		L"",
		{{0x11111111, 1024, 1024}, {0x22222222, 512, 512}});
	const std::wstring updated = Transform(
		initial,
		{{0x33333333, 1024, 1024}});
	RequireHash(updated, 0x11111111, false);
	RequireHash(updated, 0x22222222, true);
	RequireHash(updated, 0x33333333, true);
	Require(
		updated.find(L"asset_hash_mip_multiplicity") == std::wstring::npos,
		"single-hash mip must not be marked as multi-hash");
}

void TestMultiMipDiscoveryAndCompleteReplacement()
{
	const std::wstring initial = Transform(
		L"",
		{{0x11111111, 1024, 1024}});
	const std::wstring multi = Transform(
		initial,
		{{0x33333333, 1024, 1024}, {0x44444444, 1024, 1024}});
	RequireHash(multi, 0x11111111, false);
	RequireHash(multi, 0x33333333, true);
	RequireHash(multi, 0x44444444, true);
	Require(
		Count(multi, L"; asset_hash_mip_multiplicity = 2") == 1,
		"multi-hash mip marker must be emitted exactly once");

	const std::wstring incomplete = Transform(
		multi,
		{{0x55555555, 1024, 1024}});
	RequireHash(incomplete, 0x33333333, true);
	RequireHash(incomplete, 0x44444444, true);
	RequireHash(incomplete, 0x55555555, true);
	Require(
		Count(incomplete, L"; asset_hash_mip_multiplicity = 2") == 1,
		"incomplete capture must retain the required multiplicity");

	const std::wstring complete = Transform(
		incomplete,
		{{0x55555555, 1024, 1024}, {0x66666666, 1024, 1024}});
	RequireHash(complete, 0x33333333, false);
	RequireHash(complete, 0x44444444, false);
	RequireHash(complete, 0x55555555, true);
	RequireHash(complete, 0x66666666, true);
	Require(
		Count(complete, L"; asset_hash_mip_multiplicity = 2") == 1,
		"complete capture must preserve the discovered multiplicity");
}

void TestUnmarkedAdditiveHistoryCollapsesToCurrentHash()
{
	std::wstring old_additive = Transform(
		L"",
		{{0x11111111, 1024, 1024}, {0x22222222, 1024, 1024}});
	const std::wstring marker = L"; asset_hash_mip_multiplicity = 2\r\n";
	size_t marker_offset = old_additive.find(marker);
	Require(marker_offset != std::wstring::npos, "test fixture marker missing");
	old_additive.erase(marker_offset, marker.size());

	const std::wstring updated = Transform(
		old_additive,
		{{0x33333333, 1024, 1024}});
	RequireHash(updated, 0x11111111, false);
	RequireHash(updated, 0x22222222, false);
	RequireHash(updated, 0x33333333, true);
}

void TestMultiplicityExpansionAndUnknownResolution()
{
	const std::wstring multi = Transform(
		L"",
		{{0x11111111, 1024, 1024}, {0x22222222, 1024, 1024}});
	const std::wstring expanded = Transform(
		multi,
		{{0x33333333, 1024, 1024},
		 {0x44444444, 1024, 1024},
		 {0x55555555, 1024, 1024}});
	Require(
		Count(expanded, L"; asset_hash_mip_multiplicity = 3") == 1,
		"larger complete set must refresh multiplicity");

	const std::wstring unknown_multi = Transform(
		L"",
		{{0x77777777, 0, 0}, {0x88888888, 0, 0}});
	const std::wstring unknown_incomplete = Transform(
		unknown_multi,
		{{0x99999999, 0, 0}});
	RequireHash(unknown_incomplete, 0x77777777, true);
	RequireHash(unknown_incomplete, 0x88888888, true);
	RequireHash(unknown_incomplete, 0x99999999, true);
	Require(
		Count(unknown_incomplete, L"; asset_hash_mip_multiplicity = 2") == 1,
		"unknown-resolution multiplicity must round-trip");
}

void TestAggressiveGeneratedSourceUsesTheSamePolicy()
{
	const std::wstring multi = Transform(
		L"",
		{{0x11111111, 1024, 1024}, {0x22222222, 1024, 1024}});
	const std::wstring incomplete = TransformAggressive(
		multi,
		{{0x33333333, 1024, 1024}});
	RequireHash(incomplete, 0x11111111, true);
	RequireHash(incomplete, 0x22222222, true);
	RequireHash(incomplete, 0x33333333, true);

	const std::wstring complete = TransformAggressive(
		incomplete,
		{{0x33333333, 1024, 1024}, {0x44444444, 1024, 1024}});
	RequireHash(complete, 0x11111111, false);
	RequireHash(complete, 0x22222222, false);
	RequireHash(complete, 0x33333333, true);
	RequireHash(complete, 0x44444444, true);
}

void TestAmbiguousObservationDoesNotDeleteStoredHash()
{
	const std::wstring initial = Transform(
		L"",
		{{0x11111111, 1024, 1024}});
	const std::wstring updated = Transform(
		initial,
		{{0x22222222, 1024, 1024}},
		{0x22222222});
	RequireHash(updated, 0x11111111, true);
	RequireHash(updated, 0x22222222, false);
}

void TestNoObservationRoundTripIsStable()
{
	const std::wstring initial = Transform(
		L"",
		{{0x11111111, 1024, 1024}, {0x22222222, 1024, 1024}});
	Require(
		TransformWithoutObservations(initial) == initial,
		"generated document must be stable without new observations");
}

void TestLegacyHashesConvertToMarkedPathAndLeaveUnverifiedHash()
{
	const std::wstring source =
		L"[TextureOverrideGoodA]\r\n"
		L"hash = 11111111\r\n"
		L"match_priority = 0\r\n"
		L"this = ResourceTest\r\n"
		L"\r\n"
		L"[TextureOverrideGoodB]\r\n"
		L"hash = 22222222\r\n"
		L"match_priority = 0\r\n"
		L"this = ResourceTest\r\n"
		L"\r\n"
		L"[TextureOverrideUnverified]\r\n"
		L"hash = deadbeef\r\n"
		L"match_priority = 0\r\n"
		L"this = ResourceTest\r\n";
	const auto identities = RuntimeIdentities(
		L"/Game/Test/Texture.Texture",
		{{0x11111111, 1024, 1024}, {0x22222222, 512, 512}});
	const std::wstring converted = TransformAssetHashIniDocumentToPaths(
		source,
		{},
		identities,
		{},
		L"test-version");
	RequireHash(converted, 0x11111111, false);
	RequireHash(converted, 0x22222222, false);
	RequireHash(converted, 0xdeadbeef, true);
	Require(
		Count(converted, L"match_asset_path = /Game/Test/Texture.Texture") == 2,
		"path output must contain one comment marker and one active matcher");
	Require(
		Count(converted, L"; <asset-hash-stream>") == 1,
		"path output must use the shared generated marker");
	Require(
		Count(converted, L"; asset_hash_compiler_version = Ver1.1") == 1,
		"path output must use compiler contract Ver1.1");
	Require(
		converted.find(L"match_priority = 0") != std::wstring::npos,
		"unverified hash must retain its original priority line");
	const size_t path_begin = converted.find(L"; <asset-hash-stream>");
	const size_t path_end = converted.find(L"; </asset-hash-stream>", path_begin);
	Require(
		converted.substr(path_begin, path_end - path_begin).find(
			L"match_priority = 0") == std::wstring::npos,
		"path override must omit match_priority = 0");
}

void TestGeneratedHashesRequireRuntimePathValidation()
{
	const std::wstring generated = Transform(
		L"",
		{{0x11111111, 1024, 1024}, {0x22222222, 512, 512}});
	const auto identities = RuntimeIdentities(
		L"/Game/Test/Texture.Texture",
		{{0x11111111, 1024, 1024}, {0x33333333, 512, 512}});
	Require(
		TransformAssetHashIniDocumentToPaths(
			generated,
			{},
			identities,
			{},
			L"test-version") == generated,
		"commented path must not be trusted without a runtime observation");
	Require(
		TransformAssetHashIniDocumentToCleanPaths(
			generated,
			{},
			identities,
			{},
			L"test-version") == generated,
		"clean path conversion must also require runtime path validation");

	const std::wstring converted = TransformAssetHashIniDocumentToPaths(
		generated,
		Observations(
			{{0x11111111, 1024, 1024}, {0x33333333, 512, 512}}),
		{},
		{},
		L"test-version");
	RequireHash(converted, 0x11111111, false);
	RequireHash(converted, 0x22222222, true);
	Require(
		Count(converted, L"match_asset_path = /Game/Test/Texture.Texture") == 2,
		"validated generated block must become one active path override");

	const std::wstring cleaned = TransformAssetHashIniDocumentToCleanPaths(
		generated,
		Observations({{0x44444444, 256, 256}}),
		{},
		{},
		L"test-version");
	RequireHash(cleaned, 0x11111111, false);
	RequireHash(cleaned, 0x22222222, false);
	RequireHash(cleaned, 0x44444444, false);
	Require(
		Count(cleaned, L"match_asset_path = /Game/Test/Texture.Texture") == 2,
		"clean conversion needs only live Path evidence, not an old Hash match");

	const std::wstring completed = TransformAssetHashIniDocumentToPaths(
		converted,
		Observations(
			{{0x11111111, 1024, 1024}, {0x22222222, 512, 512}}),
		{},
		{},
		L"test-version");
	RequireHash(completed, 0x22222222, false);
	Require(
		Count(completed, L"; <asset-hash-stream>") == 1,
		"newly validated residual must merge into the existing path block");
}

void TestGeneratedPathUsesPathToCurrentHashes()
{
	const std::wstring generated = Transform(
		L"",
		{{0x11111111, 1024, 1024}});
	const auto identities = RuntimeIdentities(
		L"/Game/Other/Texture.Texture",
		{{0x11111111, 1024, 1024}});
	const std::wstring converted = TransformAssetHashIniDocumentToPaths(
			generated,
			Observations({{0x11111111, 1024, 1024}}),
			identities,
			{},
			L"test-version");
	RequireHash(converted, 0x11111111, false);
	Require(
		converted.find(
			L"match_asset_path = /Game/Test/Texture.Texture") !=
			std::wstring::npos,
		"commented path must query current hashes without reverse Hash-to-Path proof");
}

void TestCtrlResidualMipHashesStayInStreamAndRepair()
{
	const std::wstring generated = Transform(
		L"",
		{{0x11111111, 1024, 1024}, {0x22222222, 1024, 1024}});
	const std::wstring path_with_residuals =
		TransformAssetHashIniDocumentToPaths(
			generated,
			Observations({{0x33333333, 1024, 1024}}),
			{},
			{},
			L"test-version");
	const size_t stream_end = path_with_residuals.find(
		L"; </asset-hash-stream>");
	Require(
		path_with_residuals.find(HashLine(0x11111111)) < stream_end &&
		path_with_residuals.find(HashLine(0x22222222)) < stream_end,
		"Ctrl+F7 residual hashes must remain inside their Path stream block");
	Require(
		Count(
			path_with_residuals,
			L"; asset_hash_mip_multiplicity = 2") == 1,
		"Ctrl+F7 residual hashes must retain their Mip multiplicity");

	const std::wstring incomplete = TransformAggressive(
		path_with_residuals,
		{{0x33333333, 1024, 1024}});
	RequireHash(incomplete, 0x11111111, true);
	RequireHash(incomplete, 0x22222222, true);
	RequireHash(incomplete, 0x33333333, true);

	const std::wstring repaired = TransformAggressive(
		incomplete,
		{{0x33333333, 1024, 1024}, {0x44444444, 1024, 1024}});
	RequireHash(repaired, 0x11111111, false);
	RequireHash(repaired, 0x22222222, false);
	RequireHash(repaired, 0x33333333, true);
	RequireHash(repaired, 0x44444444, true);

	const std::wstring cleaned = TransformAssetHashIniDocumentToCleanPaths(
		path_with_residuals,
		Observations({{0x55555555, 256, 256}}),
		{},
		{},
		L"test-version");
	RequireHash(cleaned, 0x11111111, false);
	RequireHash(cleaned, 0x22222222, false);
}

void TestMarkedPathCanReturnToGeneratedHashes()
{
	const std::wstring legacy =
		L"[TextureOverrideTest]\r\n"
		L"hash = 11111111\r\n"
		L"this = ResourceTest\r\n";
	const auto identities = RuntimeIdentities(
		L"/Game/Test/Texture.Texture",
		{{0x11111111, 1024, 1024}});
	const std::wstring path_document = TransformAssetHashIniDocumentToPaths(
		legacy,
		{},
		identities,
		{},
		L"test-version");
	const std::wstring hashes = TransformAssetHashIniDocument(
		path_document,
		path_document,
		Observations({{0x44444444, 1024, 1024}}),
		{},
		{},
		L"next-version");
	RequireHash(hashes, 0x44444444, true);
	Require(
		Count(hashes, L"match_asset_path = /Game/Test/Texture.Texture") == 1,
		"generated hash block must keep only the commented path marker");
	Require(
		TransformAssetHashIniDocumentToPaths(
			path_document,
			Observations({{0x11111111, 1024, 1024}}),
			{},
			{},
			L"test-version") == path_document,
		"validated marked path block must be idempotent");
}

void TestShortIdentityAliasesCanonicalize()
{
	const std::wstring short_name =
		L"[TextureOverrideNum]\r\n"
		L"name = T_R2T1DaniyaMd10011Bangs02_D\r\n"
		L"handling = skip\r\n";
	const std::set<std::wstring> identities =
		CollectAssetHashIniIdentities(short_name);
	Require(
		identities.find(
			L"match_asset_name=t_r2t1daniyamd10011bangs02_d") !=
			identities.end(),
		"short name alias must register the canonical runtime identity");
	Require(
		AssetHashIniUsesIdentityAliases(short_name),
		"short name alias must request canonicalization on F7");

	const std::wstring canonical = TransformAssetHashIniDocument(
		short_name,
		short_name,
		{},
		{},
		{},
		L"test-version");
	Require(
		canonical.find(L"match_asset_name = T_R2T1DaniyaMd10011Bangs02_D") !=
			std::wstring::npos,
		"F7 transform must expand name to match_asset_name");
	Require(
		canonical.find(L"\nname = ") == std::wstring::npos,
		"F7 transform must remove the short name spelling");
	Require(
		canonical.find(L"handling = skip") != std::wstring::npos,
		"canonicalization must preserve the override body");

	const std::wstring short_path =
		L"[TextureOverridePath]\r\n"
		L"path = /Game/Test/Texture.Texture\r\n"
		L"this = ResourceTest\r\n";
	const std::wstring path_canonical = TransformAssetHashIniDocumentToPaths(
		short_path,
		{},
		{},
		{},
		L"test-version");
	Require(
		path_canonical.find(
			L"match_asset_path = /Game/Test/Texture.Texture") !=
			std::wstring::npos,
		"Ctrl+F7 transform must expand path even without a runtime match");

	const std::wstring combined_identities =
		L"[TextureOverrideCombined]\r\n"
		L"name = Texture\r\n"
		L"path = /Game/Test/Texture.Texture\r\n"
		L"handling = skip\r\n";
	const std::set<std::wstring> combined_keys =
		CollectAssetHashIniIdentities(combined_identities);
	Require(
		combined_keys.find(L"match_asset_path=/game/test/texture.texture") !=
			combined_keys.end() &&
		combined_keys.find(L"match_asset_name=texture") !=
			combined_keys.end(),
		"path and name in one section must both register as identities");
	const std::wstring combined_canonical = TransformAssetHashIniDocument(
		combined_identities,
		combined_identities,
		{},
		{},
		{},
		L"test-version");
	Require(
		combined_canonical.find(
			L"match_asset_path = /Game/Test/Texture.Texture") !=
			std::wstring::npos &&
		combined_canonical.find(L"match_asset_name = Texture") !=
			std::wstring::npos,
		"F7 canonicalization must preserve both path and name identities");

	const std::wstring unrelated_name =
		L"[ResourceMetadata]\r\n"
		L"name = NotAnAssetIdentity\r\n";
	Require(
		CollectAssetHashIniIdentities(unrelated_name).empty() &&
			!AssetHashIniUsesIdentityAliases(unrelated_name),
		"name outside TextureOverride must not become an asset identity alias");
}

void TestGeneratedSectionsUseCanonicalNames()
{
	const std::wstring source =
		L"; <asset-hash-stream>\r\n"
		L"; match_asset_path = /Game/Test/A.A\r\n"
		L"[TextureOverrideTextureAuthorLabelA]\r\n"
		L"hash = 11111111\r\n"
		L"this = ResourceA\r\n"
		L"; </asset-hash-stream>\r\n"
		L"; <asset-hash-stream>\r\n"
		L"; match_asset_path = /Game/Test/B.B\r\n"
		L"[TextureOverride_AnotherAuthorLabelB]\r\n"
		L"hash = 22222222\r\n"
		L"this = ResourceB\r\n"
		L"; </asset-hash-stream>\r\n";
	AssetHashObservationMap observations;
	observations[L"match_asset_path=/game/test/a.a"] =
		{{0x11111111, 1024, 1024}};
	observations[L"match_asset_path=/game/test/b.b"] =
		{{0x22222222, 1024, 1024}};

	const std::wstring paths = TransformAssetHashIniDocumentToCleanPaths(
		source,
		observations,
		{},
		{},
		L"test-version");
	Require(
		Count(paths, L"[TextureOverride_") == 2,
		"different paths sharing one legacy base must receive path identities");
	Require(
		paths.find(L"[TextureOverride_A]") != std::wstring::npos &&
			paths.find(L"[TextureOverride_B]") != std::wstring::npos,
		"Path mode must replace every author suffix with the object name");
	Require(
		paths.find(L"AuthorLabel") == std::wstring::npos,
		"Path mode must not retain custom author section labels");
	size_t first = paths.find(L"[TextureOverride_");
	size_t first_end = paths.find(L']', first);
	size_t second = paths.find(
		L"[TextureOverride_",
		first_end);
	size_t second_end = paths.find(L']', second);
	Require(
		paths.substr(first, first_end - first) !=
			paths.substr(second, second_end - second),
		"different asset paths must never collapse to the same section name");
	Require(
		paths.find(L"[TextureOverride_Texture]\r\n") == std::wstring::npos,
		"path conversion must not emit the colliding legacy base name");

	const std::wstring hashes = TransformAssetHashIniDocument(
		paths,
		paths,
		observations,
		{},
		{},
		L"test-version");
	Require(
		hashes.find(L"[TextureOverride_Texture_11111111]") !=
			std::wstring::npos &&
			hashes.find(L"[TextureOverride_Texture_22222222]") !=
			std::wstring::npos,
		"Hash mode must replace every suffix with Texture_<hash>");
	Require(
		Count(hashes, L"match_priority = 0") == 2,
		"Hash mode must add match_priority = 0 to every generated section");
	Require(
		hashes.find(L"[TextureOverride_A_11111111]") == std::wstring::npos &&
			hashes.find(L"[TextureOverride_B_22222222]") == std::wstring::npos,
		"Hash mode must not retain Path or author suffixes");
	const std::wstring round_trip = TransformAssetHashIniDocumentToCleanPaths(
		hashes,
		observations,
		{},
		{},
		L"test-version");
	Require(
		Count(round_trip, L"[TextureOverride_") == 2 &&
			Count(round_trip, L"_assetpath_") == 0,
		"Path to Hash to Path must preserve one stable suffix per resource");

	const std::wstring legacy_path =
		L"; <asset-hash-stream>\r\n"
		L"; match_asset_path = /Game/Test/A.A\r\n"
		L"[TextureOverride_Texture]\r\n"
		L"match_asset_path = /Game/Test/A.A\r\n"
		L"this = ResourceA\r\n"
		L"; </asset-hash-stream>\r\n";
	Require(
		AssetHashIniNeedsCanonicalization(legacy_path),
		"legacy generated path blocks must request automatic repair");
	const std::wstring repaired = TransformAssetHashIniDocumentToPaths(
		legacy_path,
		{},
		{},
		{},
		L"test-version");
	Require(
		repaired.find(L"[TextureOverride_A]") !=
			std::wstring::npos &&
			!AssetHashIniNeedsCanonicalization(repaired),
		"legacy path blocks must repair without trusting the commented path");
}

std::wstring VbSourceDocument()
{
	return
		L"[TextureOverrideComponent0_ib0]\r\n"
		L"hash = aaaaaaaa\r\n"
		L"match_first_index = 0\r\n"
		L"match_index_count = 100\r\n"
		L"$object_detected_ib0 = 1\r\n"
		L"\r\n"
		L"[TextureOverrideComponent1_ib0]\r\n"
		L"hash = aaaaaaaa\r\n"
		L"match_first_index = 100\r\n"
		L"match_index_count = 50\r\n"
		L"$object_detected_ib0 = 1\r\n"
		L"\r\n"
		L"[TextureOverrideFoldHost_ib0]\r\n"
		L"hash = cccccccc\r\n"
		L"match_first_index = 0\r\n"
		L"match_index_count = 100\r\n"
		L"$object_detected_ib0 = 1\r\n"
		L"\r\n"
		L"[TextureOverrideComponent_ib1]\r\n"
		L"hash = bbbbbbbb\r\n"
		L"match_first_index = 0\r\n"
		L"match_index_count = 80\r\n"
		L"$object_detected_ib1 = 1\r\n"
		L"\r\n"
		L"; <asset-hash-stream>\r\n"
		L"; match_asset_path = /Game/Test/Body.Body\r\n"
		L"[TextureOverride_Texture_01010101]\r\n"
		L"hash = 01010101\r\n"
		L"if $object_detected_ib0\r\n"
		L"    this = ResourceBody\r\n"
		L"endif\r\n"
		L"; </asset-hash-stream>\r\n"
		L"\r\n"
		L"; <asset-hash-stream>\r\n"
		L"; match_asset_path = /Game/Test/Shared.Shared\r\n"
		L"[TextureOverride_Texture_02020202]\r\n"
		L"hash = 02020202\r\n"
		L"if $object_detected_ib0 || $object_detected_ib1\r\n"
		L"    this = ResourceShared\r\n"
		L"endif\r\n"
		L"; </asset-hash-stream>\r\n";
}

void TestVbHashReplacementUsesPathGroupAndCompleteDrawSignature()
{
	const std::wstring source = VbSourceDocument();
	const std::set<uint32_t> candidates = CollectVbHashIniCandidates(source);
	Require(
		candidates.size() == 3 &&
			candidates.find(0xaaaaaaaa) != candidates.end() &&
			candidates.find(0xbbbbbbbb) != candidates.end() &&
			candidates.find(0xcccccccc) != candidates.end(),
		"only host families linked to Path object variables must gate VB0 capture");
	const VbHashObservationList observations = {
		{L"/Game/Test/Body.Body", 0x11111111, 0, 100},
		{L"/Game/Test/Shared.Shared", 0x11111111, 100, 50},
		{L"/Game/Test/Shared.Shared", 0x22222222, 0, 80}};
	const std::wstring updated =
		TransformVbHashIniDocument(source, observations);
	Require(
		Count(updated, L"hash = 11111111") == 2,
		"one complete ib0 draw-signature family must replace every old host hash");
	Require(
		updated.find(L"hash = aaaaaaaa") == std::wstring::npos,
		"the replaced ib0 host hash must not remain");
	Require(
		updated.find(L"hash = 22222222") != std::wstring::npos &&
			updated.find(L"hash = bbbbbbbb") == std::wstring::npos,
		"a shared Path must still resolve ib1 through its distinct draw signature");
	Require(
		updated.find(L"hash = cccccccc") != std::wstring::npos,
		"a partial alternative host family must not be replaced by a superset");
	Require(
		Count(updated, L"; <asset-hash-stream>") == 2 &&
			Count(updated, L"; </asset-hash-stream>") == 2,
		"VB0 replacement must not add or remove Path comments or markers");
}

void TestFirstPassGeneratedPathCanRepairVbFamily()
{
	const std::wstring source =
		L"[TextureOverrideComponent0]\r\n"
		L"hash = aaaaaaaa\r\n"
		L"match_first_index = 0\r\n"
		L"match_index_count = 100\r\n"
		L"$object_detected = 1\r\n"
		L"\r\n[TextureOverrideLegacyTexture]\r\n"
		L"hash = 01010101\r\n"
		L"if $object_detected\r\n"
		L"    this = ResourceTexture\r\n"
		L"endif\r\n";
	const AssetHashPathIdentityMap legacy_identities = {
		{0x01010101, {
			L"/Game/Test/Body.Body",
			{{0x01010101, 1024, 1024}}}}};
	const std::wstring with_path = TransformAssetHashIniDocument(
		source,
		L"",
		{},
		legacy_identities,
		{},
		L"test");
	const VbHashObservationList observations = {
		{L"/Game/Test/Body.Body", 0x11111111, 0, 100, 1000}};
	const std::wstring updated =
		TransformVbHashIniDocument(with_path, observations);
	Require(
		updated.find(L"match_asset_path = /Game/Test/Body.Body") !=
				std::wstring::npos &&
		updated.find(L"hash = 11111111") != std::wstring::npos &&
		updated.find(L"hash = aaaaaaaa") == std::wstring::npos,
		"a Path generated during the current F7 pass must repair its VB family");
}

void TestVbHashReplacementRejectsAmbiguity()
{
	const std::wstring source = VbSourceDocument();
	const VbHashObservationList observations = {
		{L"/Game/Test/Body.Body", 0x11111111, 0, 100},
		{L"/Game/Test/Shared.Shared", 0x11111111, 100, 50},
		{L"/Game/Test/Body.Body", 0x33333333, 0, 100},
		{L"/Game/Test/Shared.Shared", 0x33333333, 100, 50}};
	const std::wstring updated =
		TransformVbHashIniDocument(source, observations);
	Require(
		Count(updated, L"hash = aaaaaaaa") == 2 &&
			updated.find(L"hash = 11111111") == std::wstring::npos &&
			updated.find(L"hash = 33333333") == std::wstring::npos,
		"two complete new VB0 candidates must leave the old family unchanged");
}

void TestVbRangeAndShapeKeyReplacementUsesUniqueStructure()
{
	const std::wstring source =
		VbSourceDocument() +
		L"\r\n[TextureOverrideShapeKeyOffsets_ib0]\r\n"
		L"hash = 7fe8c94e\r\n"
		L"override_byte_stride = 24\r\n"
		L"override_vertex_count = $mesh_vertex_count_ib0\r\n"
		L"\r\n[TextureOverrideShapeKeyScale_ib0]\r\n"
		L"hash = 81378bbb\r\n"
		L"override_byte_stride = 4\r\n"
		L"override_vertex_count = $mesh_vertex_count_ib0\r\n"
		L"\r\n[TextureOverrideShapeKeyLoaderCallback_ib0]\r\n"
		L"hash = 7fe8c94e\r\n"
		L"if cs == 3381.3333\r\n"
		L"    handling = skip\r\n"
		L"endif\r\n"
		L"\r\n[TextureOverrideShapeKeyMultiplierCallback_ib0]\r\n"
		L"hash = 7fe8c94e\r\n"
		L"if cs == 3381.4444\r\n"
		L"    handling = skip\r\n"
		L"endif\r\n";
	const VbHashObservationList vb_observations = {
		{L"/Game/Test/Body.Body", 0x11111111, 144069, 83268, 1000},
		{L"/Game/Test/Shared.Shared", 0x11111111, 227337, 34740, 1000}};
	const ShapeKeyHashObservationList shape_observations = {
		{0x1a646636, 24000, 0, 3333, 0, true},
		{0x1a646636, 24000, 0, 4444, 0, true},
		{0x06fe8141, 4000, 0, 3333, 2, false},
		{0x06fe8141, 4000, 0, 4444, 2, false}};
	const std::wstring updated = TransformVbHashIniDocument(
		source, vb_observations, shape_observations);
	Require(
		Count(updated, L"hash = 11111111") == 2 &&
		updated.find(L"match_first_index = 144069") != std::wstring::npos &&
		updated.find(L"match_index_count = 83268") != std::wstring::npos &&
		updated.find(L"match_first_index = 227337") != std::wstring::npos &&
		updated.find(L"match_index_count = 34740") != std::wstring::npos,
		"a unique contiguous component sequence must update VB0 and both draw ranges");
	Require(
		Count(updated, L"hash = 1a646636") == 3 &&
		updated.find(L"hash = 7fe8c94e") == std::wstring::npos,
		"the offsets hash must update every semantic ShapeKey reference");
	Require(
		updated.find(L"hash = 06fe8141") != std::wstring::npos &&
		updated.find(L"hash = 81378bbb") == std::wstring::npos,
		"the uniquely sized ShapeKey scale buffer must update without markers");
}

void TestVbRangeReplacementRejectsTwoContiguousCandidates()
{
	const VbHashObservationList observations = {
		{L"/Game/Test/Body.Body", 0x11111111, 1000, 40, 1000},
		{L"/Game/Test/Shared.Shared", 0x11111111, 1040, 20, 1000},
		{L"/Game/Test/Body.Body", 0x22222222, 2000, 80, 2000},
		{L"/Game/Test/Shared.Shared", 0x22222222, 2080, 30, 2000}};
	const std::wstring updated =
		TransformVbHashIniDocument(VbSourceDocument(), observations);
	Require(
		Count(updated, L"hash = aaaaaaaa") == 2 &&
		updated.find(L"match_first_index = 0") != std::wstring::npos,
		"two structurally valid component families must not be guessed by order");
}

void TestVbRangeReplacementWorksWhenHashIsUnchanged()
{
	const VbHashObservationList observations = {
		{L"/Game/Test/Body.Body", 0xaaaaaaaa, 500, 120, 1000},
		{L"/Game/Test/Shared.Shared", 0xaaaaaaaa, 620, 60, 1000}};
	const std::wstring updated =
		TransformVbHashIniDocument(VbSourceDocument(), observations);
	Require(
		Count(updated, L"hash = aaaaaaaa") == 2 &&
		updated.find(L"match_first_index = 500") != std::wstring::npos &&
		updated.find(L"match_index_count = 120") != std::wstring::npos &&
		updated.find(L"match_first_index = 620") != std::wstring::npos &&
		updated.find(L"match_index_count = 60") != std::wstring::npos,
		"component ranges must repair even when the VB0 hash itself is unchanged");
}

void TestShapeKeyReplacementRejectsAmbiguousBufferSize()
{
	const std::wstring source =
		VbSourceDocument() +
		L"\r\n[TextureOverrideShapeKeyOffsets_ib0]\r\n"
		L"hash = 7fe8c94e\r\n"
		L"override_byte_stride = 24\r\n"
		L"override_vertex_count = $mesh_vertex_count_ib0\r\n"
		L"\r\n[TextureOverrideShapeKeyScale_ib0]\r\n"
		L"hash = 81378bbb\r\n"
		L"override_byte_stride = 4\r\n"
		L"override_vertex_count = $mesh_vertex_count_ib0\r\n";
	const VbHashObservationList vb_observations = {
		{L"/Game/Test/Body.Body", 0xaaaaaaaa, 0, 100, 1000},
		{L"/Game/Test/Shared.Shared", 0xaaaaaaaa, 100, 50, 1000}};
	const ShapeKeyHashObservationList shape_observations = {
		{0x1a646636, 24000, 0, 3333, 0, true},
		{0x1a646636, 24000, 0, 4444, 0, true},
		{0x06fe8141, 4000, 0, 3333, 2, false},
		{0x12345678, 4000, 0, 4444, 3, false}};
	const std::wstring updated = TransformVbHashIniDocument(
		source, vb_observations, shape_observations);
	Require(
		updated.find(L"hash = 1a646636") != std::wstring::npos &&
		updated.find(L"hash = 81378bbb") != std::wstring::npos &&
		updated.find(L"hash = 06fe8141") == std::wstring::npos &&
		updated.find(L"hash = 12345678") == std::wstring::npos,
		"two scale-sized buffers must preserve the old scale hash while offsets remain repairable");
}

void TestMeshVertexCountSelectsOneIniWithoutComponentMatching()
{
	const std::wstring source =
		L"[Constants]\r\n"
		L"global $mesh_vertex_count = 78034\r\n\r\n" +
		VbSourceDocument();
	uint32_t vertex_count = 0;
	Require(
		CollectVbHashIniMeshVertexCount(source, &vertex_count) &&
		vertex_count == 78034,
		"the lightweight current-model identity must read mesh_vertex_count");
}

void TestDrawSignaturesDoNotDependOnOldVbHashOrMeshVertexCount()
{
	const std::wstring source =
		L"[Constants]\r\n"
		L"global $mesh_vertex_count = 78034\r\n\r\n"
		L"[TextureOverrideComponent0]\r\n"
		L"hash = 15fb50a9\r\n"
		L"match_first_index = 0\r\n"
		L"match_index_count = 13842\r\n\r\n"
		L"[TextureOverrideComponent1]\r\n"
		L"hash = 15fb50a9\r\n"
		L"match_first_index = 13842\r\n"
		L"match_index_count = 33159\r\n";
	const std::set<std::pair<uint32_t, uint32_t>> signatures =
		CollectVbHashIniDrawSignatures(source);
	Require(
		signatures.size() == 2 &&
		signatures.count({0, 13842}) == 1 &&
		signatures.count({13842, 33159}) == 1,
		"current-model draw signatures must exclude the stale VB hash and exported mesh vertex count");
}

void TestPathlessCurrentModelObservationRepairsOnlyWhenEnabled()
{
	const VbHashObservationList observations = {
		{L"", 0x11111111, 500, 120, 1000},
		{L"", 0x11111111, 620, 60, 1000}};
	const std::wstring unchanged = TransformVbHashIniDocument(
		VbSourceDocument(), observations);
	const std::wstring updated = TransformVbHashIniDocument(
		VbSourceDocument(),
		observations,
		ShapeKeyHashObservationList(),
		true);
	Require(
		Count(unchanged, L"hash = aaaaaaaa") == 2,
		"pathless observations must not affect non-target documents");
	Require(
		Count(updated, L"hash = 11111111") == 2 &&
		updated.find(L"match_first_index = 500") != std::wstring::npos &&
		updated.find(L"match_index_count = 120") != std::wstring::npos &&
		updated.find(L"match_first_index = 620") != std::wstring::npos &&
		updated.find(L"match_index_count = 60") != std::wstring::npos,
		"the selected current-model document must accept a pathless draw family");
}
}

int main()
{
	TestSingleMipReplacementPreservesUnseenMip();
	TestMultiMipDiscoveryAndCompleteReplacement();
	TestUnmarkedAdditiveHistoryCollapsesToCurrentHash();
	TestMultiplicityExpansionAndUnknownResolution();
	TestAggressiveGeneratedSourceUsesTheSamePolicy();
	TestAmbiguousObservationDoesNotDeleteStoredHash();
	TestNoObservationRoundTripIsStable();
	TestLegacyHashesConvertToMarkedPathAndLeaveUnverifiedHash();
	TestGeneratedHashesRequireRuntimePathValidation();
	TestGeneratedPathUsesPathToCurrentHashes();
	TestCtrlResidualMipHashesStayInStreamAndRepair();
	TestMarkedPathCanReturnToGeneratedHashes();
	TestShortIdentityAliasesCanonicalize();
	TestGeneratedSectionsUseCanonicalNames();
	TestVbHashReplacementUsesPathGroupAndCompleteDrawSignature();
	TestFirstPassGeneratedPathCanRepairVbFamily();
	TestVbHashReplacementRejectsAmbiguity();
	TestVbRangeAndShapeKeyReplacementUsesUniqueStructure();
	TestVbRangeReplacementRejectsTwoContiguousCandidates();
	TestVbRangeReplacementWorksWhenHashIsUnchanged();
	TestShapeKeyReplacementRejectsAmbiguousBufferSize();
	TestMeshVertexCountSelectsOneIniWithoutComponentMatching();
	TestDrawSignaturesDoNotDependOnOldVbHashOrMeshVertexCount();
	TestPathlessCurrentModelObservationRepairsOnlyWhenEnabled();
	std::cout << "asset_hash_ini_document_tests: PASS\n";
	return 0;
}
