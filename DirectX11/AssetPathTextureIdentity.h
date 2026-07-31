#pragma once

#include <cstdint>
#include <string>

bool AssetPathTextureIdentityRequired();

void SetAssetPathTextureOverridesEnabled(bool enabled);

void SetAssetPathFrameAnalysisEnabled(bool enabled);

std::wstring ExtractUnrealAssetName(const std::wstring& asset_path);

void ObserveAssetPathNameMatch(
	const std::wstring& asset_name,
	const std::wstring& asset_path);

bool InstallAssetPathTextureIdentityBridge();

void StartAssetPathTextureIdentityBridgeWorker();

void ObserveUnrealRhiTextureCreation();

void InvalidateAssetPathTextureResource(uintptr_t resource);

bool CaptureUnrealTextureAssetPathAtCreation(
	std::wstring *asset_path);
