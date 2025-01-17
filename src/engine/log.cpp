#include "engine/delegate_list.h"
#include "engine/log.h"
#include "engine/path.h"
#include "engine/string.h"
#include <cstdlib>


namespace Lumix
{

void fatal(bool cond, const char* msg)
{
	if (!cond) {
		logError("FATAL") << msg << " is false.";
		abort();
	}
}


static DefaultAllocator g_allocator;
static LogCallback g_callback(g_allocator);


struct Log {
	Log(LogLevel level) 
		: level(level) 
		, message(g_allocator)
	{}
	LogLevel level;
	String message;
};

thread_local Log g_log_info(LogLevel::INFO);
thread_local Log g_log_warning(LogLevel::WARNING);
thread_local Log g_log_error(LogLevel::ERROR);

LogCallback& getLogCallback() { return g_callback; }
LogProxy logInfo(const char* system) { return LogProxy(&g_log_info, system); }
LogProxy logWarning(const char* system) { return LogProxy(&g_log_warning, system); }
LogProxy logError(const char* system) { return LogProxy(&g_log_error, system); }


LogProxy::LogProxy(Log* log, const char* system)
	: system(system)
	, log(log)
{
}

LogProxy::~LogProxy()
{
	g_callback.invoke(log->level, system, log->message.c_str());
	log->message = "";
}


LogProxy& LogProxy::operator<<(const char* message)
{
	log->message.cat(message);
	return *this;
}

LogProxy& LogProxy::operator<<(float message)
{
	log->message.cat(message);
	return *this;
}

LogProxy& LogProxy::operator<<(u32 message)
{
	log->message.cat(message);
	return *this;
}

LogProxy& LogProxy::operator<<(u64 message)
{
	log->message.cat(message);
	return *this;
}

LogProxy& LogProxy::operator<<(i32 message)
{
	log->message.cat(message);
	return *this;
}

LogProxy& LogProxy::operator<<(const String& path)
{
	log->message.cat(path.c_str());
	return *this;
}

LogProxy& LogProxy::operator<<(const Path& path)
{
	log->message.cat(path.c_str());
	return *this;
}


} // namespace Lumix
