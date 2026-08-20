#pragma once

#include <cstdint>
#include <string>

class HackerDevice;

void ToggleAssetHashCapture(HackerDevice *device, void *private_data);
void ToggleAggressiveAssetHashCapture(
	HackerDevice *device,
	void *private_data);
void ToggleAssetHashPathConversion(
	HackerDevice *device,
	void *private_data);
void ToggleAssetHashCleanPathConversion(
	HackerDevice *device,
	void *private_data);
void RefreshAssetHashCaptureSources();
void ObserveAssetHashForAuthoring(
	uintptr_t resource_address,
	const std::wstring& asset_path,
	uint32_t hash,
	uint32_t width,
	uint32_t height);
void ObserveVbHashForAuthoring(
	const std::wstring& asset_path,
	uint32_t hash,
	uint32_t first_index,
	uint32_t index_count);
void RetireAssetHashForAuthoring(uintptr_t resource_address);
bool AssetHashCaptureStatusVisible();
bool AssetHashCaptureEnabled();
bool AssetHashCaptureNeedsVbObservation(
	uint32_t hash,
	uint32_t first_index,
	uint32_t index_count);
const wchar_t *AssetHashCaptureStatusText();
