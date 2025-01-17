#pragma once


#include "engine/array.h"
#include "engine/log.h"
#include "engine/mt/sync.h"
#include "engine/string.h"


namespace Lumix
{


class LUMIX_EDITOR_API LogUI
{
	public:
		explicit LogUI(IAllocator& allocator);
		~LogUI();

		void onGUI();
		void update(float time_delta);
		int addNotification(const char* text);
		void setNotificationTime(int uid, float time);
		int getUnreadErrorCount() const;

	public:
		bool m_is_open;

	private:
		struct Notification
		{
			explicit Notification(IAllocator& alloc)
				: message(alloc)
			{
			}
			float time;
			int uid;
			String message;
		};

	private:
		void onLog(LogLevel level, const char* system, const char* message);
		void push(LogLevel level, const char* message);
		void showNotifications();

	private:
		struct Message
		{
			Message(IAllocator& allocator)
				: text(allocator)
			{}

			String text;
			LogLevel level;
		};

		IAllocator& m_allocator;
		Array<Message> m_messages;
		Array<Notification> m_notifications;
		int m_new_message_count[(int)LogLevel::COUNT];
		u8 m_level_filter;
		int m_last_uid;
		bool m_move_notifications_to_front;
		bool m_are_notifications_hovered;
		MT::SpinMutex m_guard;
};


} // namespace Lumix
