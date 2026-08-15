#include "DrawDebugStream.h"

#include <Windows.h>
#include <Strsafe.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

extern HINSTANCE migoto_handle;

namespace {

CRITICAL_SECTION queue_lock;
INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
HANDLE wake_event = NULL;
HANDLE writer_thread = NULL;
HANDLE pipe_thread = NULL;
HANDLE output_file = INVALID_HANDLE_VALUE;
std::deque<std::string> queue;
std::wstring output_path;
std::atomic<bool> configured(false);
std::atomic<bool> control_allowed(false);
std::atomic<bool> active(false);
std::atomic<bool> start_requested(false);
std::atomic<bool> stop_requested(false);
std::atomic<bool> snapshot_requested(false);
std::atomic<bool> targeted(false);
std::atomic<bool> armed(false);
std::atomic<unsigned long long> sequence(0);
std::atomic<unsigned long long> frame(0);
std::atomic<unsigned long long> written(0);
std::atomic<unsigned long long> dropped(0);
unsigned queue_limit = 65536;
std::atomic<unsigned> draw_filter_count(0);
std::atomic<unsigned long long> draw_filters[128];
std::atomic<unsigned> learned_shader_count(0);
std::atomic<unsigned long long> learned_shaders[128];

bool MatchesTarget(const char *call, uint64_t arg0, uint64_t arg1, uint64_t vs, uint64_t ps)
{
	if (!targeted.load(std::memory_order_relaxed))
		return true;
	bool direct = false;
	if (!strcmp(call, "DrawIndexed")) {
		unsigned long long key = (arg0 << 32) | (arg1 & 0xffffffffull);
		unsigned count = draw_filter_count.load(std::memory_order_acquire);
		for (unsigned i = 0; i < count; ++i) {
			if (draw_filters[i].load(std::memory_order_relaxed) == key) {
				direct = true;
				break;
			}
		}
	}
	if (direct) {
		unsigned idx = learned_shader_count.fetch_add(2, std::memory_order_relaxed);
		if (idx + 1 < _countof(learned_shaders)) {
			learned_shaders[idx].store(vs, std::memory_order_relaxed);
			learned_shaders[idx + 1].store(ps, std::memory_order_relaxed);
		} else {
			learned_shader_count.store((unsigned)_countof(learned_shaders));
		}
		if (armed.exchange(false)) {
			snapshot_requested.store(true);
			DrawDebugStreamMark("target_detected_auto_snapshot");
		}
		return true;
	}
	unsigned learned = learned_shader_count.load(std::memory_order_acquire);
	if (learned > _countof(learned_shaders))
		learned = (unsigned)_countof(learned_shaders);
	for (unsigned i = 0; i < learned; ++i) {
		unsigned long long shader = learned_shaders[i].load(std::memory_order_relaxed);
		if (shader && (shader == vs || shader == ps))
			return true;
	}
	return false;
}

void Enqueue(std::string &&line)
{
	if (!active.load(std::memory_order_relaxed))
		return;
	if (!TryEnterCriticalSection(&queue_lock)) {
		dropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (queue.size() >= queue_limit) {
		queue.pop_front();
		dropped.fetch_add(1, std::memory_order_relaxed);
	}
	queue.emplace_back(std::move(line));
	LeaveCriticalSection(&queue_lock);
	SetEvent(wake_event);
}

DWORD WINAPI WriterThreadProc(void *)
{
	std::deque<std::string> pending;
	for (;;) {
		WaitForSingleObject(wake_event, 250);
		EnterCriticalSection(&queue_lock);
		pending.swap(queue);
		LeaveCriticalSection(&queue_lock);

		if (output_file == INVALID_HANDLE_VALUE || pending.empty())
			continue;
		for (const std::string &line : pending) {
			DWORD bytes_written = 0;
			WriteFile(output_file, line.data(), (DWORD)line.size(), &bytes_written, NULL);
			written.fetch_add(1, std::memory_order_relaxed);
		}
		FlushFileBuffers(output_file);
		pending.clear();
	}
}

std::string StatusJson()
{
	char buf[1024];
	char path_utf8[MAX_PATH * 3] = {};
	WideCharToMultiByte(CP_UTF8, 0, output_path.c_str(), -1,
		path_utf8, sizeof(path_utf8), NULL, NULL);
	for (char *p = path_utf8; *p; ++p) {
		if (*p == '\\')
			*p = '/';
	}
	sprintf_s(buf,
		"{\"active\":%s,\"hunting_required\":true,\"control_allowed\":%s,"
		"\"frame\":%llu,\"sequence\":%llu,"
		"\"written\":%llu,\"dropped\":%llu,\"path\":\"%s\"}\n",
		active.load() ? "true" : "false",
		control_allowed.load() ? "true" : "false",
		frame.load(), sequence.load(),
		written.load(), dropped.load(), path_utf8);
	return std::string(buf);
}

DWORD WINAPI PipeThreadProc(void *)
{
	const wchar_t *pipe_name = L"\\\\.\\pipe\\wwmi-draw-debug";
	for (;;) {
		HANDLE pipe = CreateNamedPipeW(pipe_name,
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1, 4096, 4096, 0, NULL);
		if (pipe == INVALID_HANDLE_VALUE)
			return 1;
		if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
			CloseHandle(pipe);
			continue;
		}

		char command[4096] = {};
		DWORD read = 0;
		std::string response;
		if (ReadFile(pipe, command, sizeof(command) - 1, &read, NULL)) {
			command[read] = 0;
			while (read && (command[read - 1] == '\r' || command[read - 1] == '\n'))
				command[--read] = 0;
			if (!_stricmp(command, "PING")) {
				response = "PONG\n";
			} else if (!_stricmp(command, "STATUS")) {
				response = StatusJson();
			} else if (!_stricmp(command, "START")) {
				if (!control_allowed.load()) {
					response = "ERROR hunting mode required\n";
				} else {
					targeted.store(false);
					start_requested.store(true);
					response = "QUEUED START\n";
				}
			} else if (!_stricmp(command, "ARM")) {
				if (!control_allowed.load()) {
					response = "ERROR hunting mode required\n";
				} else {
					targeted.store(true);
					armed.store(true);
					learned_shader_count.store(0);
					start_requested.store(true);
					response = "QUEUED ARM\n";
				}
			} else if (!_stricmp(command, "STOP")) {
				stop_requested.store(true);
				response = "QUEUED STOP\n";
			} else if (!_stricmp(command, "SNAPSHOT")) {
				if (!control_allowed.load()) {
					response = "ERROR hunting mode required\n";
				} else {
					snapshot_requested.store(true);
					response = "QUEUED SNAPSHOT\n";
				}
			} else if (!_strnicmp(command, "MARK ", 5)) {
				DrawDebugStreamMark(command + 5);
				response = "OK MARK\n";
			} else if (!_stricmp(command, "FILTER CLEAR")) {
				draw_filter_count.store(0, std::memory_order_release);
				learned_shader_count.store(0, std::memory_order_release);
				response = "OK FILTER CLEAR\n";
			} else if (!_strnicmp(command, "FILTER DRAW ", 12)) {
				unsigned long long count = 0, first = 0;
				if (sscanf_s(command + 12, "%llu %llu", &count, &first) == 2) {
					unsigned idx = draw_filter_count.load(std::memory_order_relaxed);
					if (idx < _countof(draw_filters)) {
						draw_filters[idx].store((count << 32) | (first & 0xffffffffull));
						draw_filter_count.store(idx + 1, std::memory_order_release);
						response = "OK FILTER DRAW\n";
					} else {
						response = "ERROR filter limit\n";
					}
				} else {
					response = "ERROR FILTER DRAW syntax\n";
				}
			} else {
				response = "ERROR unknown command\n";
			}
		}
		DWORD sent = 0;
		WriteFile(pipe, response.data(), (DWORD)response.size(), &sent, NULL);
		FlushFileBuffers(pipe);
		DisconnectNamedPipe(pipe);
		CloseHandle(pipe);
	}
}

BOOL CALLBACK InitState(PINIT_ONCE, PVOID, PVOID *)
{
	InitializeCriticalSection(&queue_lock);
	wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	writer_thread = CreateThread(NULL, 0, WriterThreadProc, NULL, 0, NULL);
	pipe_thread = CreateThread(NULL, 0, PipeThreadProc, NULL, 0, NULL);
	return TRUE;
}

void EnsureInitialized()
{
	InitOnceExecuteOnce(&init_once, InitState, NULL, NULL);
}

} // namespace

void ConfigureDrawDebugStream(bool enabled, unsigned max_records)
{
	configured.store(enabled);
	if (!enabled) {
		control_allowed.store(false);
		return;
	}
	queue_limit = max_records ? max_records : 65536;
	EnsureInitialized();
}

void SetDrawDebugControlAllowed(bool allowed)
{
	control_allowed.store(allowed, std::memory_order_release);
}

void StartDrawDebugStream()
{
	if (!configured.load() || active.load())
		return;
	EnsureInitialized();
	sequence.store(0);
	frame.store(0);
	written.store(0);
	dropped.store(0);

	wchar_t module_path[MAX_PATH];
	wchar_t directory[MAX_PATH];
	time_t now = time(NULL);
	struct tm local_time;
	_localtime64_s(&local_time, &now);
	wchar_t stamp[64];
	wcsftime(stamp, _countof(stamp), L"DrawDebug-%Y-%m-%d-%H%M%S", &local_time);
	GetModuleFileNameW(migoto_handle, module_path, MAX_PATH);
	wcscpy_s(directory, module_path);
	wchar_t *slash = wcsrchr(directory, L'\\');
	if (slash)
		slash[1] = 0;
	wcscat_s(directory, stamp);
	CreateDirectoryW(directory, NULL);
	output_path = directory;
	output_path += L"\\stream.jsonl";
	output_file = CreateFileW(output_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
		NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	active.store(true, std::memory_order_release);
	DrawDebugStreamMark("capture_started");
}

void StopDrawDebugStream()
{
	if (!active.exchange(false))
		return;
	char buf[256];
	sprintf_s(buf,
		"{\"type\":\"capture_stopped\",\"frame\":%llu,\"sequence\":%llu,"
		"\"written\":%llu,\"dropped\":%llu}\n",
		frame.load(), sequence.load(), written.load(), dropped.load());
	EnterCriticalSection(&queue_lock);
	queue.emplace_back(buf);
	LeaveCriticalSection(&queue_lock);
	SetEvent(wake_event);
}

bool IsDrawDebugStreamActive()
{
	return active.load(std::memory_order_relaxed);
}

void DrawDebugStreamFrameBoundary()
{
	if (active.load(std::memory_order_relaxed))
		frame.fetch_add(1, std::memory_order_relaxed);
}

void DrawDebugStreamRecord(const char *call, uint64_t arg0, uint64_t arg1,
	uint64_t arg2, uint64_t arg3, uint64_t vs, uint64_t ps, uint64_t cs,
	uint64_t gs, uint64_t hs, uint64_t ds, uint32_t ib, uint32_t vb0)
{
	if (!active.load(std::memory_order_relaxed))
		return;
	if (!MatchesTarget(call, arg0, arg1, vs, ps))
		return;
	char buf[768];
	unsigned long long seq = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	sprintf_s(buf,
		"{\"type\":\"draw\",\"seq\":%llu,\"frame\":%llu,\"call\":\"%s\","
		"\"args\":[%llu,%llu,%llu,%llu],\"vs\":\"%016llx\",\"ps\":\"%016llx\","
		"\"cs\":\"%016llx\",\"gs\":\"%016llx\",\"hs\":\"%016llx\","
		"\"ds\":\"%016llx\",\"ib\":\"%08x\",\"vb0\":\"%08x\"}\n",
		seq, frame.load(std::memory_order_relaxed), call, arg0, arg1, arg2, arg3,
		vs, ps, cs, gs, hs, ds, ib, vb0);
	Enqueue(std::string(buf));
}

void DrawDebugStreamMark(const char *label)
{
	if (!active.load(std::memory_order_relaxed))
		return;
	char safe[256];
	size_t j = 0;
	for (size_t i = 0; label[i] && j + 1 < sizeof(safe); ++i) {
		char c = label[i];
		safe[j++] = (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? '_' : c;
	}
	safe[j] = 0;
	char buf[512];
	sprintf_s(buf, "{\"type\":\"mark\",\"frame\":%llu,\"label\":\"%s\"}\n",
		frame.load(), safe);
	Enqueue(std::string(buf));
}

bool ConsumeDrawDebugStartRequest() { return start_requested.exchange(false); }
bool ConsumeDrawDebugStopRequest() { return stop_requested.exchange(false); }
bool ConsumeDrawDebugSnapshotRequest() { return snapshot_requested.exchange(false); }
