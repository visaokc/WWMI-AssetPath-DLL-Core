#include "DrawDebugStream.h"
#include "log.h"

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
HANDLE writer_stop_event = NULL;
HANDLE pipe_stop_event = NULL;
HANDLE writer_thread = NULL;
HANDLE pipe_thread = NULL;
HANDLE output_file = INVALID_HANDLE_VALUE;
std::deque<std::string> queue;
std::deque<std::string> agent_dump_requests;
std::wstring output_path;
std::wstring last_dump_path;
std::string last_dump_error;
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
std::atomic<unsigned> pending_dumps(0);
std::atomic<unsigned long long> completed_dumps(0);
std::atomic<bool> last_dump_ok(false);
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
	HANDLE waits[] = { writer_stop_event, wake_event };
	for (;;) {
		DWORD wait_result = WaitForMultipleObjects(_countof(waits), waits, FALSE, INFINITE);
		EnterCriticalSection(&queue_lock);
		pending.swap(queue);
		LeaveCriticalSection(&queue_lock);

		if (output_file != INVALID_HANDLE_VALUE) {
			for (const std::string &line : pending) {
				DWORD bytes_written = 0;
				WriteFile(output_file, line.data(), (DWORD)line.size(), &bytes_written, NULL);
				written.fetch_add(1, std::memory_order_relaxed);
			}
			if (!pending.empty())
				FlushFileBuffers(output_file);
		}
		pending.clear();
		if (wait_result == WAIT_OBJECT_0)
			break;
	}
	return 0;
}

std::string StatusJson()
{
	char buf[2048];
	char path_utf8[MAX_PATH * 3] = {};
	char dump_path_utf8[MAX_PATH * 3] = {};
	char dump_error[512] = {};
	EnterCriticalSection(&queue_lock);
	std::wstring path = output_path;
	std::wstring dump_path = last_dump_path;
	strncpy_s(dump_error, last_dump_error.c_str(), _TRUNCATE);
	LeaveCriticalSection(&queue_lock);
	WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
		path_utf8, sizeof(path_utf8), NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, dump_path.c_str(), -1,
		dump_path_utf8, sizeof(dump_path_utf8), NULL, NULL);
	for (char *p = path_utf8; *p; ++p) {
		if (*p == '\\')
			*p = '/';
	}
	for (char *p = dump_path_utf8; *p; ++p) {
		if (*p == '\\')
			*p = '/';
	}
	for (char *p = dump_error; *p; ++p) {
		if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20)
			*p = '_';
	}
	sprintf_s(buf,
		"{\"active\":%s,\"agent_hunting_required\":false,\"control_allowed\":%s,"
		"\"frame\":%llu,\"sequence\":%llu,"
		"\"written\":%llu,\"dropped\":%llu,\"path\":\"%s\","
		"\"pending_dumps\":%u,\"completed_dumps\":%llu,\"last_dump_ok\":%s,"
		"\"last_dump_path\":\"%s\",\"last_dump_error\":\"%s\"}\n",
		active.load() ? "true" : "false",
		control_allowed.load() ? "true" : "false",
		frame.load(), sequence.load(),
		written.load(), dropped.load(), path_utf8,
		pending_dumps.load(), completed_dumps.load(),
		last_dump_ok.load() ? "true" : "false",
		dump_path_utf8, dump_error);
	return std::string(buf);
}

bool CompletePipeOperation(HANDLE pipe, OVERLAPPED *overlapped, BOOL completed,
	DWORD *bytes_transferred)
{
	DWORD ignored = 0;
	if (!bytes_transferred)
		bytes_transferred = &ignored;
	if (completed)
		return true;
	if (GetLastError() != ERROR_IO_PENDING)
		return false;
	HANDLE waits[] = { pipe_stop_event, overlapped->hEvent };
	if (WaitForMultipleObjects(_countof(waits), waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
		CancelIoEx(pipe, overlapped);
		WaitForSingleObject(overlapped->hEvent, INFINITE);
		return false;
	}
	return !!GetOverlappedResult(pipe, overlapped, bytes_transferred, FALSE);
}

DWORD WINAPI PipeThreadProc(void *)
{
	const wchar_t *pipe_name = L"\\\\.\\pipe\\wwmi-draw-debug";
	while (WaitForSingleObject(pipe_stop_event, 0) != WAIT_OBJECT_0) {
		HANDLE pipe = CreateNamedPipeW(pipe_name,
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1, 4096, 4096, 0, NULL);
		if (pipe == INVALID_HANDLE_VALUE)
			return 1;
		OVERLAPPED operation = {};
		operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (!operation.hEvent) {
			CloseHandle(pipe);
			return 1;
		}
		BOOL connected = ConnectNamedPipe(pipe, &operation);
		if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) {
			connected = TRUE;
		} else if (!CompletePipeOperation(pipe, &operation, connected, NULL)) {
			CloseHandle(operation.hEvent);
			CloseHandle(pipe);
			continue;
		}

		char command[4096] = {};
		DWORD read = 0;
		std::string response;
		ResetEvent(operation.hEvent);
		BOOL read_completed = ReadFile(pipe, command, sizeof(command) - 1,
			&read, &operation);
		if (CompletePipeOperation(pipe, &operation, read_completed, &read)) {
			command[read] = 0;
			while (read && (command[read - 1] == '\r' || command[read - 1] == '\n'))
				command[--read] = 0;
			if (!_stricmp(command, "PING")) {
				response = "PONG\n";
			} else if (!_stricmp(command, "STATUS")) {
				response = StatusJson();
			} else if (!_stricmp(command, "START")) {
				targeted.store(false);
				start_requested.store(true);
				response = "QUEUED START\n";
			} else if (!_stricmp(command, "ARM")) {
				targeted.store(true);
				armed.store(true);
				learned_shader_count.store(0);
				start_requested.store(true);
				response = "QUEUED ARM\n";
			} else if (!_stricmp(command, "STOP")) {
				stop_requested.store(true);
				response = "QUEUED STOP\n";
			} else if (!_stricmp(command, "SNAPSHOT")) {
				snapshot_requested.store(true);
				response = "QUEUED SNAPSHOT\n";
			} else if (!_strnicmp(command, "DUMP ", 5)) {
				EnterCriticalSection(&queue_lock);
				if (agent_dump_requests.size() < 128) {
					agent_dump_requests.emplace_back(command + 5);
					pending_dumps.store((unsigned)agent_dump_requests.size());
					response = "QUEUED DUMP\n";
				} else {
					response = "ERROR dump queue limit\n";
				}
				LeaveCriticalSection(&queue_lock);
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
		ResetEvent(operation.hEvent);
		BOOL write_completed = WriteFile(pipe, response.data(),
			(DWORD)response.size(), &sent, &operation);
		CompletePipeOperation(pipe, &operation, write_completed, &sent);
		DisconnectNamedPipe(pipe);
		CloseHandle(operation.hEvent);
		CloseHandle(pipe);
	}
	return 0;
}

BOOL CALLBACK InitState(PINIT_ONCE, PVOID, PVOID *)
{
	InitializeCriticalSection(&queue_lock);
	wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	writer_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	pipe_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	return wake_event && writer_stop_event && pipe_stop_event;
}

bool EnsureInitialized()
{
	return !!InitOnceExecuteOnce(&init_once, InitState, NULL, NULL);
}

void StartDrawDebugControlServer()
{
	if (pipe_thread || !EnsureInitialized())
		return;
	ResetEvent(pipe_stop_event);
	pipe_thread = CreateThread(NULL, 0, PipeThreadProc, NULL, 0, NULL);
}

void StopDrawDebugControlServer()
{
	if (!pipe_thread)
		return;
	SetEvent(pipe_stop_event);
	DWORD wait_result = WaitForSingleObject(pipe_thread, 5000);
	if (wait_result == WAIT_TIMEOUT) {
		LogInfo("Draw Debug pipe thread did not stop within five seconds\n");
		return;
	}
	CloseHandle(pipe_thread);
	pipe_thread = NULL;
}

} // namespace

void ConfigureDrawDebugStream(bool enabled, unsigned max_records)
{
	configured.store(enabled);
	queue_limit = max_records ? max_records : 65536;
	if (!enabled) {
		SetDrawDebugControlAllowed(false);
		StopDrawDebugStream();
		return;
	}
}

void SetDrawDebugControlAllowed(bool allowed)
{
	bool effective = configured.load(std::memory_order_acquire) && allowed;
	bool previous = control_allowed.exchange(effective, std::memory_order_acq_rel);
	if (effective && !previous)
		StartDrawDebugControlServer();
	else if (!effective && previous)
		StopDrawDebugControlServer();
}

void StartDrawDebugStream()
{
	if (!configured.load() || active.load() || writer_thread || !EnsureInitialized())
		return;
	sequence.store(0);
	frame.store(0);
	written.store(0);
	dropped.store(0);
	EnterCriticalSection(&queue_lock);
	queue.clear();
	LeaveCriticalSection(&queue_lock);

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
	std::wstring path = directory;
	path += L"\\stream.jsonl";
	output_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
		NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (output_file == INVALID_HANDLE_VALUE)
		return;
	EnterCriticalSection(&queue_lock);
	output_path = path;
	LeaveCriticalSection(&queue_lock);
	ResetEvent(writer_stop_event);
	active.store(true, std::memory_order_release);
	writer_thread = CreateThread(NULL, 0, WriterThreadProc, NULL, 0, NULL);
	if (!writer_thread) {
		active.store(false, std::memory_order_release);
		CloseHandle(output_file);
		output_file = INVALID_HANDLE_VALUE;
		return;
	}
	DrawDebugStreamMark("capture_started");
}

void StopDrawDebugStream()
{
	if (!active.exchange(false) && !writer_thread)
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
	SetEvent(writer_stop_event);
	if (writer_thread) {
		DWORD wait_result = WaitForSingleObject(writer_thread, 5000);
		if (wait_result == WAIT_TIMEOUT) {
			LogInfo("Draw Debug writer thread did not stop within five seconds\n");
			return;
		}
		CloseHandle(writer_thread);
		writer_thread = NULL;
	}
	if (output_file != INVALID_HANDLE_VALUE) {
		FlushFileBuffers(output_file);
		CloseHandle(output_file);
		output_file = INVALID_HANDLE_VALUE;
	}
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

bool ConsumeAgentDumpRequest(std::string *request)
{
	if (!request || !EnsureInitialized())
		return false;
	EnterCriticalSection(&queue_lock);
	if (agent_dump_requests.empty()) {
		LeaveCriticalSection(&queue_lock);
		return false;
	}
	*request = std::move(agent_dump_requests.front());
	agent_dump_requests.pop_front();
	pending_dumps.store((unsigned)agent_dump_requests.size());
	LeaveCriticalSection(&queue_lock);
	return true;
}

bool DrawDebugStreamIsLearnedShader(uint64_t hash)
{
	unsigned count = learned_shader_count.load(std::memory_order_acquire);
	if (count > _countof(learned_shaders))
		count = (unsigned)_countof(learned_shaders);
	for (unsigned i = 0; i < count; ++i) {
		if (learned_shaders[i].load(std::memory_order_relaxed) == hash)
			return true;
	}
	return false;
}

void SetAgentDumpResult(bool success, const std::wstring& path,
	const std::string& error)
{
	if (!EnsureInitialized())
		return;
	EnterCriticalSection(&queue_lock);
	last_dump_path = path;
	last_dump_error = error;
	last_dump_ok.store(success);
	completed_dumps.fetch_add(1);
	LeaveCriticalSection(&queue_lock);
}
