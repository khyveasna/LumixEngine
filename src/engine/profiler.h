#pragma once


#include "engine/lumix.h"


namespace Lumix
{


namespace Profiler
{

// writing API

LUMIX_ENGINE_API void pause(bool paused);

LUMIX_ENGINE_API void setThreadName(const char* name);
LUMIX_ENGINE_API void showInProfiler(bool show);

LUMIX_ENGINE_API void beginBlock(const char* name_literal);
LUMIX_ENGINE_API void blockColor(u8 r, u8 g, u8 b);
LUMIX_ENGINE_API void endBlock();
LUMIX_ENGINE_API void frame();
LUMIX_ENGINE_API void pushJobInfo(u32 signal_on_finish, u32 precondition);
LUMIX_ENGINE_API void pushString(const char* value);
LUMIX_ENGINE_API void pushInt(const char* key_literal, int value);

LUMIX_ENGINE_API void beginGPUBlock(const char* name, u64 timestamp, i64 profiler_link);
LUMIX_ENGINE_API void endGPUBlock(u64 timestamp);
LUMIX_ENGINE_API void gpuFrame();
LUMIX_ENGINE_API void link(i64 link);
LUMIX_ENGINE_API i64 createNewLinkID();

struct FiberSwitchData {
	i32 id;
	const char* blocks[16];
	int count;
};

LUMIX_ENGINE_API void beforeFiberSwitch();
LUMIX_ENGINE_API FiberSwitchData beginFiberWait(u32 job_system_signal);
LUMIX_ENGINE_API void endFiberWait(u32 job_system_signal, const FiberSwitchData& switch_data);
LUMIX_ENGINE_API float getLastFrameDuration();

struct Scope
{
	explicit Scope(const char* name_literal) { beginBlock(name_literal); }
	~Scope() { endBlock(); }
};


// reading API

LUMIX_ENGINE_API bool contextSwitchesEnabled();
LUMIX_ENGINE_API u64 frequency();

struct ContextSwitchRecord
{
	u32 old_thread_id;
	u32 new_thread_id;
	u64 timestamp;
	i8 reason;
};


struct IntRecord
{
	const char* key;
	int value;
};


struct JobRecord
{
	u32 signal_on_finish;
	u32 precondition;
};


struct FiberWaitRecord
{
	i32 id;
	u32 job_system_signal;
};


struct GPUBlock
{
	char name[32];
	u64 timestamp;
	i64 profiler_link;
};


enum class EventType : u8
{
	BEGIN_BLOCK,
	BLOCK_COLOR,
	END_BLOCK,
	FRAME,
	STRING,
	INT,
	BEGIN_FIBER_WAIT,
	END_FIBER_WAIT,
	CONTEXT_SWITCH,
	JOB_INFO,
	BEGIN_GPU_BLOCK,
	END_GPU_BLOCK,
	GPU_FRAME,
	LINK
};

#pragma pack(1)
struct EventHeader
{
	u16 size;
	EventType type;
	u64 time;
};
#pragma pack()


LUMIX_ENGINE_API struct GlobalState {
	GlobalState();
	~GlobalState();
	
	int threadsCount() const;
	const char* getThreadName(int idx) const;

	int local_readers_count = 0;
};


LUMIX_ENGINE_API struct ThreadState {
	ThreadState(GlobalState& reader, int thread_idx);
	~ThreadState();

	GlobalState& reader;
	int thread_idx;

	const char* name;
	const u8* buffer;
	uint buffer_size;
	uint begin;
	uint end;
	u32 thread_id;

	bool show;
	bool open;
	uint rows;
};


} // namespace Profiler

#define LUMIX_CONCAT2(a, b) a ## b
#define LUMIX_CONCAT(a, b) LUMIX_CONCAT2(a, b)

#define PROFILE_FUNCTION() Profiler::Scope profile_scope(__FUNCTION__);
#define PROFILE_BLOCK(name) Profiler::Scope LUMIX_CONCAT(profile_scope, __LINE__)(name);


} // namespace Lumix
