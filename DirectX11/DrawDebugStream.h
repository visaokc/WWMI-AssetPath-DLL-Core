#pragma once

#include <stdint.h>

void ConfigureDrawDebugStream(bool enabled, unsigned max_records);
void SetDrawDebugControlAllowed(bool allowed);
void StartDrawDebugStream();
void StopDrawDebugStream();
bool IsDrawDebugStreamActive();
void DrawDebugStreamFrameBoundary();
void DrawDebugStreamRecord(const char *call, uint64_t arg0, uint64_t arg1,
	uint64_t arg2, uint64_t arg3, uint64_t vs, uint64_t ps, uint64_t cs,
	uint64_t gs, uint64_t hs, uint64_t ds, uint32_t ib, uint32_t vb0);
void DrawDebugStreamMark(const char *label);
bool ConsumeDrawDebugStartRequest();
bool ConsumeDrawDebugStopRequest();
bool ConsumeDrawDebugSnapshotRequest();
