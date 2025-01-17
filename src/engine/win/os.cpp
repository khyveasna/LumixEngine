#include "engine/allocator.h"
#include "engine/lumix.h"
#include "engine/os.h"
#include "engine/path_utils.h"
#include "engine/string.h"
#define UNICODE
#include <cASSERT>
#pragma warning(push)
#pragma warning(disable : 4091)
#include <ShlObj.h>
#pragma warning(pop)
#include <Windows.h>

namespace Lumix::OS
{


static struct
{
	bool finished = false;
	Interface* iface = nullptr;
	Point relative_mode_pos = {};
	bool relative_mouse = false;
	WindowHandle win = INVALID_WINDOW;
} G;


InputFile::InputFile()
{
	m_handle = (void*)INVALID_HANDLE_VALUE;
	static_assert(sizeof(m_handle) >= sizeof(HANDLE), "");
}


OutputFile::OutputFile()
{
    m_is_error = false;
	m_handle = (void*)INVALID_HANDLE_VALUE;
	static_assert(sizeof(m_handle) >= sizeof(HANDLE), "");
}


InputFile::~InputFile()
{
	ASSERT((HANDLE)m_handle == INVALID_HANDLE_VALUE);
}


OutputFile::~OutputFile()
{
	ASSERT((HANDLE)m_handle == INVALID_HANDLE_VALUE);
}


bool OutputFile::open(const char* path)
{
	m_handle = (HANDLE)::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	m_is_error = INVALID_HANDLE_VALUE == m_handle;
    return !m_is_error;
}


bool InputFile::open(const char* path)
{
	m_handle = (HANDLE)::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	return INVALID_HANDLE_VALUE != m_handle;
}


void OutputFile::flush()
{
	ASSERT(nullptr != m_handle);
	FlushFileBuffers((HANDLE)m_handle);
}


void OutputFile::close()
{
	if (INVALID_HANDLE_VALUE != (HANDLE)m_handle)
	{
		::CloseHandle((HANDLE)m_handle);
		m_handle = (void*)INVALID_HANDLE_VALUE;
	}
}


void InputFile::close()
{
	if (INVALID_HANDLE_VALUE != (HANDLE)m_handle)
	{
		::CloseHandle((HANDLE)m_handle);
		m_handle = (void*)INVALID_HANDLE_VALUE;
	}
}


bool OutputFile::write(const void* data, u64 size)
{
	ASSERT(INVALID_HANDLE_VALUE != (HANDLE)m_handle);
	u64 written = 0;
	::WriteFile((HANDLE)m_handle, data, (DWORD)size, (LPDWORD)&written, nullptr);
	m_is_error = m_is_error || size != written;
    return !m_is_error;
}

bool InputFile::read(void* data, u64 size)
{
	ASSERT(INVALID_HANDLE_VALUE != m_handle);
	DWORD readed = 0;
	BOOL success = ::ReadFile((HANDLE)m_handle, data, (DWORD)size, (LPDWORD)&readed, nullptr);
	return success && size == readed;
}

u64 InputFile::size() const
{
	ASSERT(INVALID_HANDLE_VALUE != m_handle);
	return ::GetFileSize((HANDLE)m_handle, 0);
}


u64 OutputFile::pos()
{
	ASSERT(INVALID_HANDLE_VALUE != m_handle);
	return ::SetFilePointer((HANDLE)m_handle, 0, nullptr, FILE_CURRENT);
}


u64 InputFile::pos()
{
	ASSERT(INVALID_HANDLE_VALUE != m_handle);
	return ::SetFilePointer((HANDLE)m_handle, 0, nullptr, FILE_CURRENT);
}


bool InputFile::seek(u64 pos)
{
	ASSERT(INVALID_HANDLE_VALUE != m_handle);
	LARGE_INTEGER dist;
	dist.QuadPart = pos;
	return ::SetFilePointer((HANDLE)m_handle, dist.u.LowPart, &dist.u.HighPart, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
}


OutputFile& OutputFile::operator <<(const char* text)
{
	write(text, stringLength(text));
	return *this;
}


OutputFile& OutputFile::operator <<(i32 value)
{
	char buf[20];
	toCString(value, buf, lengthOf(buf));
	write(buf, stringLength(buf));
	return *this;
}


OutputFile& OutputFile::operator <<(u32 value)
{
	char buf[20];
	toCString(value, buf, lengthOf(buf));
	write(buf, stringLength(buf));
	return *this;
}


OutputFile& OutputFile::operator <<(u64 value)
{
	char buf[30];
	toCString(value, buf, lengthOf(buf));
	write(buf, stringLength(buf));
	return *this;
}


OutputFile& OutputFile::operator <<(float value)
{
	char buf[128];
	toCString(value, buf, lengthOf(buf), 7);
	write(buf, stringLength(buf));
	return *this;
}


static void fromWChar(char* out, int size, const WCHAR* in)
{
	const WCHAR* c = in;
	char* cout = out;
	while (*c && c - in < size - 1)
	{
		*cout = (char)*c;
		++cout;
		++c;
	}
	*cout = 0;
}


template <int N> static void toWChar(WCHAR (&out)[N], const char* in)
{
	const char* c = in;
	WCHAR* cout = out;
	while (*c && c - in < N - 1)
	{
		*cout = *c;
		++cout;
		++c;
	}
	*cout = 0;
}


template <int N>
struct WCharStr
{
	WCharStr(const char* rhs)
	{
		toWChar(data, rhs);
	}

	operator const WCHAR*() const
	{
		return data;
	}

	WCHAR data[N];
};


void getDropFile(const Event& event, int idx, char* out, int max_size)
{
	ASSERT(max_size > 0);
	HDROP drop = (HDROP)event.file_drop.handle;
	WCHAR buffer[MAX_PATH];
	if (DragQueryFile(drop, idx, buffer, MAX_PATH))
	{
		fromWChar(out, max_size, buffer);
	}
	else
	{
		ASSERT(false);
	}
}


int getDropFileCount(const Event& event)
{
	HDROP drop = (HDROP)event.file_drop.handle;
	return (int)DragQueryFile(drop, 0xFFFFFFFF, NULL, 0);
}


void finishDrag(const Event& event)
{
	HDROP drop = (HDROP)event.file_drop.handle;
	DragFinish(drop);
}


static void processEvents()
{
	MSG msg;
	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
		Event e;
		e.window = msg.hwnd;
		switch (msg.message) {
			case WM_DROPFILES:
				e.type = Event::Type::DROP_FILE;
				e.file_drop.handle = (HDROP)msg.wParam;
				G.iface->onEvent(e);
				break;
			case WM_QUIT: 
				e.type = Event::Type::QUIT; 
				G.iface->onEvent(e);
				break;
			case WM_CLOSE: 
				e.type = Event::Type::WINDOW_CLOSE; 
				G.iface->onEvent(e);
				break;
			case WM_KEYDOWN:
				e.type = Event::Type::KEY;
				e.key.down = true;
				e.key.keycode = (Keycode)msg.wParam;
				G.iface->onEvent(e);
				break;
			case WM_KEYUP:
				e.type = Event::Type::KEY;
				e.key.down = false;
				e.key.keycode = (Keycode)msg.wParam;
				G.iface->onEvent(e);
				break;
			case WM_CHAR:
				e.type = Event::Type::CHAR;
				e.text_input.utf32 = (u32)msg.wParam;
				// TODO msg.wParam is utf16, convert
				// e.g. https://github.com/SFML/SFML/blob/master/src/SFML/Window/Win32/WindowImplWin32.cpp#L694
				G.iface->onEvent(e);
				break;
			case WM_INPUT: {
				HRAWINPUT hRawInput = (HRAWINPUT)msg.lParam;
				UINT dataSize;
				GetRawInputData(hRawInput, RID_INPUT, NULL, &dataSize, sizeof(RAWINPUTHEADER));
				char dataBuf[1024];
				if (dataSize == 0 || dataSize > sizeof(dataBuf)) break;

				GetRawInputData(hRawInput, RID_INPUT, dataBuf, &dataSize, sizeof(RAWINPUTHEADER));

				const RAWINPUT* raw = (const RAWINPUT*)dataBuf;
				if (raw->header.dwType != RIM_TYPEMOUSE) break;

				const HANDLE deviceHandle = raw->header.hDevice;
				const RAWMOUSE& mouseData = raw->data.mouse;
				const USHORT flags = mouseData.usButtonFlags;
				const short wheel_delta = (short)mouseData.usButtonData;
				const LONG x = mouseData.lLastX, y = mouseData.lLastY;

				if (wheel_delta) {
					e.mouse_wheel.amount = (float)wheel_delta / WHEEL_DELTA;
					e.type = Event::Type::MOUSE_WHEEL;
					G.iface->onEvent(e);
				}

				if(flags & RI_MOUSE_LEFT_BUTTON_DOWN) {
					e.type = Event::Type::MOUSE_BUTTON;
					e.mouse_button.button = MouseButton::LEFT;
					e.mouse_button.down = true;
					G.iface->onEvent(e);
				}
				if(flags & RI_MOUSE_LEFT_BUTTON_UP) {
					e.type = Event::Type::MOUSE_BUTTON;
					e.mouse_button.button = MouseButton::LEFT;
					e.mouse_button.down = false;
					G.iface->onEvent(e);
				}
					
				if(flags & RI_MOUSE_RIGHT_BUTTON_UP) {
					e.type = Event::Type::MOUSE_BUTTON;
					e.mouse_button.button = MouseButton::RIGHT;
					e.mouse_button.down = false;
					G.iface->onEvent(e);
				}
				if(flags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
					e.type = Event::Type::MOUSE_BUTTON;
					e.mouse_button.button = MouseButton::RIGHT;
					e.mouse_button.down = true;
					G.iface->onEvent(e);
				}

				if(flags & RI_MOUSE_MIDDLE_BUTTON_UP) {
					e.type = Event::Type::MOUSE_BUTTON;
					e.mouse_button.button = MouseButton::MIDDLE;
					e.mouse_button.down = false;
					G.iface->onEvent(e);
				}
				if(flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) {
					e.type = Event::Type::MOUSE_BUTTON;
					e.mouse_button.button = MouseButton::MIDDLE;
					e.mouse_button.down = true;
					G.iface->onEvent(e);
				}

				if (x != 0 || y != 0) {
					e.type = Event::Type::MOUSE_MOVE;
					e.mouse_move.xrel = x;
					e.mouse_move.yrel = y;
					G.iface->onEvent(e);
				}
				break;
			}
		}

		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}


void destroyWindow(WindowHandle window)
{
	DestroyWindow((HWND)window);
	G.win = INVALID_WINDOW;
}

void UTF32ToUTF8(u32 utf32, char* utf8)
{
    if (utf32 <= 0x7F) {
        utf8[0] = (char) utf32;
        utf8[1] = '\0';
    } else if (utf32 <= 0x7FF) {
        utf8[0] = 0xC0 | (char) ((utf32 >> 6) & 0x1F);
        utf8[1] = 0x80 | (char) (utf32 & 0x3F);
        utf8[2] = '\0';
    } else if (utf32 <= 0xFFFF) {
        utf8[0] = 0xE0 | (char) ((utf32 >> 12) & 0x0F);
        utf8[1] = 0x80 | (char) ((utf32 >> 6) & 0x3F);
        utf8[2] = 0x80 | (char) (utf32 & 0x3F);
        utf8[3] = '\0';
    } else if (utf32 <= 0x10FFFF) {
        utf8[0] = 0xF0 | (char) ((utf32 >> 18) & 0x0F);
        utf8[1] = 0x80 | (char) ((utf32 >> 12) & 0x3F);
        utf8[2] = 0x80 | (char) ((utf32 >> 6) & 0x3F);
        utf8[3] = 0x80 | (char) (utf32 & 0x3F);
        utf8[4] = '\0';
	}
	else {
		ASSERT(false);
	}
}


WindowHandle createWindow(const InitWindowArgs& args)
{


	WNDCLASS wc = {};

	auto WndProc = [](HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
		Event e;
		e.window = hWnd;
		switch (Msg)
		{
			case WM_MOVE:
				e.type = Event::Type::WINDOW_MOVE;
				e.win_move.x = LOWORD(lParam);
				e.win_move.y = HIWORD(lParam);
				G.iface->onEvent(e);
				return 0;
			case WM_SIZE:
				e.type = Event::Type::WINDOW_SIZE;
				e.win_size.w = LOWORD(lParam);
				e.win_size.h = HIWORD(lParam);
				G.iface->onEvent(e);
				return 0;
			case WM_CLOSE:
				e.type = Event::Type::WINDOW_CLOSE;
				G.iface->onEvent(e);
				return 0;
			case WM_ACTIVATE:
				if (wParam == WA_INACTIVE) {
					showCursor(true);
					unclipCursor();
				}
				e.type = Event::Type::FOCUS;
				e.focus.gained = wParam != WA_INACTIVE;
				G.iface->onEvent(e);
				break;
		}
		return DefWindowProc(hWnd, Msg, wParam, lParam);
	};

	wc.style = 0;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = GetModuleHandle(NULL);
	wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = NULL;
	WCharStr<MAX_PATH_LENGTH> wname(args.name);
	wc.lpszClassName = wname;

	if (!RegisterClass(&wc)) return INVALID_WINDOW;

	const HWND hwnd = CreateWindow(wname,
		wname,
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		NULL,
		NULL,
		wc.hInstance,
		NULL);

	if (args.handle_file_drops)
	{
		DragAcceptFiles(hwnd, TRUE);
	}

	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	if (!G.win) {
		RAWINPUTDEVICE device;
		device.usUsagePage = 0x01;
		device.usUsage = 0x02;
		device.dwFlags = RIDEV_INPUTSINK;
		device.hwndTarget = hwnd;
		BOOL status = RegisterRawInputDevices(&device, 1, sizeof(device));
		ASSERT(status);
	}

	G.win = hwnd;
	return hwnd;
}


void quit()
{
	G.finished = true;
}


bool isKeyDown(Keycode keycode)
{
	const SHORT res = GetAsyncKeyState((int)keycode);
	return (res & 0x8000) != 0;
}


void getKeyName(Keycode keycode, char* out, int size)
{
	LONG scancode = MapVirtualKey((UINT)keycode, MAPVK_VK_TO_VSC);

	// because MapVirtualKey strips the extended bit for some keys
	switch ((UINT)keycode)
	{
		case VK_LEFT:
		case VK_UP:
		case VK_RIGHT:
		case VK_DOWN:
		case VK_PRIOR:
		case VK_NEXT:
		case VK_END:
		case VK_HOME:
		case VK_INSERT:
		case VK_DELETE:
		case VK_DIVIDE:
		case VK_NUMLOCK: scancode |= 0x100; break;
	}

	WCHAR tmp[256];
	ASSERT(size <= 256 && size > 0);
	int res = GetKeyNameText(scancode << 16, tmp, size);
	if (res == 0)
	{
		*out = 0;
	}
	else
	{
		fromWChar(out, size, tmp);
	}
}


void showCursor(bool show)
{
	if (show) {
		while(ShowCursor(show) < 0);
	}
	else {
		while(ShowCursor(show) >= 0);
	}
}


void setWindowTitle(WindowHandle win, const char* title)
{
	WCharStr<256> tmp(title);
	SetWindowText((HWND)win, tmp);
}


Rect getWindowScreenRect(WindowHandle win)
{
	RECT rect;
	GetWindowRect((HWND)win, &rect);
	return {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
}


Point getWindowClientSize(WindowHandle win)
{
	RECT rect;
	BOOL status = GetClientRect((HWND)win, &rect);
	ASSERT(status);
	return {rect.right - rect.left, rect.bottom - rect.top};
}


void setWindowScreenRect(WindowHandle win, const Rect& rect)
{
	MoveWindow((HWND)win, rect.left, rect.top, rect.width, rect.height, TRUE);
}


void setMouseScreenPos(int x, int y)
{
	SetCursorPos(x, y);
}

Point getMousePos(WindowHandle win)
{
	POINT p;
	BOOL b = GetCursorPos(&p);
	ScreenToClient((HWND)win, &p);
	ASSERT(b);
	return {p.x, p.y};
}

Point getMouseScreenPos()
{
	POINT p;
	BOOL b = GetCursorPos(&p);
	ASSERT(b);
	return {p.x, p.y};
}


WindowHandle getFocused()
{
	return GetActiveWindow();
}


bool isMaximized(WindowHandle win)
{
	WINDOWPLACEMENT placement;
	BOOL res = GetWindowPlacement((HWND)win, &placement);
	ASSERT(res);
	return placement.showCmd == SW_SHOWMAXIMIZED;
}


void maximizeWindow(WindowHandle win)
{
	ShowWindow((HWND)win, SW_SHOWMAXIMIZED);
}


bool isRelativeMouseMode()
{
	return G.relative_mouse;
}


void run(Interface& iface)
{
	G.iface = &iface;
	G.iface->onInit();
	while (!G.finished)
	{
		processEvents();
		G.iface->onIdle();
	}
}


int getDPI()
{
	const HDC hdc = GetDC(NULL);
    return GetDeviceCaps(hdc, LOGPIXELSX);
}


void* memReserve(size_t size) {
	return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
}

void memCommit(void* ptr, size_t size) {
	VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE);
}

void memRelease(void* ptr) {
	VirtualFree(ptr, 0, MEM_RELEASE);
}

struct FileIterator
{
	HANDLE handle;
	IAllocator* allocator;
	WIN32_FIND_DATA ffd;
	bool is_valid;
};


FileIterator* createFileIterator(const char* path, IAllocator& allocator)
{
	char tmp[MAX_PATH_LENGTH];
	copyString(tmp, path);
	catString(tmp, "/*");
	
	WCharStr<MAX_PATH_LENGTH> wtmp(tmp);
	auto* iter = LUMIX_NEW(allocator, FileIterator);
	iter->allocator = &allocator;
	iter->handle = FindFirstFile(wtmp, &iter->ffd);
	iter->is_valid = iter->handle != INVALID_HANDLE_VALUE;
	return iter;
}


void destroyFileIterator(FileIterator* iterator)
{
	FindClose(iterator->handle);
	LUMIX_DELETE(*iterator->allocator, iterator);
}


bool getNextFile(FileIterator* iterator, FileInfo* info)
{
	if (!iterator->is_valid) return false;

	info->is_directory = (iterator->ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	fromWChar(info->filename, lengthOf(info->filename), iterator->ffd.cFileName);

	iterator->is_valid = FindNextFile(iterator->handle, &iterator->ffd) != FALSE;
	return true;
}


void setCurrentDirectory(const char* path)
{
	WCharStr<MAX_PATH_LENGTH> tmp(path);
	SetCurrentDirectory(tmp);
}


void getCurrentDirectory(char* buffer, int buffer_size)
{
	WCHAR tmp[MAX_PATH_LENGTH];
	GetCurrentDirectory(lengthOf(tmp), tmp);
	fromWChar(buffer, buffer_size, tmp);
}


bool getSaveFilename(char* out, int max_size, const char* filter, const char* default_extension)
{
	WCharStr<MAX_PATH_LENGTH> wtmp("");
	WCHAR wfilter[MAX_PATH_LENGTH];
	
	const char* c = filter;
	WCHAR* cout = wfilter;
	while ((*c || *(c + 1)) && (c - filter) < MAX_PATH_LENGTH - 2) {
		*cout = *c;
		++cout;
		++c;
	}
	*cout = 0;
	++cout;
	*cout = 0;

	WCharStr<MAX_PATH_LENGTH> wdefault_extension(default_extension ? default_extension : "");
	OPENFILENAME ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = NULL;
	ofn.lpstrFile = wtmp.data;
	ofn.lpstrFile[0] = '\0';
	ofn.nMaxFile = lengthOf(wtmp.data);
	ofn.lpstrFilter = wfilter;
	ofn.nFilterIndex = 1;
	ofn.lpstrDefExt = default_extension ? wdefault_extension.data : nullptr;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_NONETWORKBUTTON;

	bool res = GetSaveFileName(&ofn) != FALSE;

	char tmp[MAX_PATH_LENGTH];
	fromWChar(tmp, lengthOf(tmp), wtmp);
	if (res) PathUtils::normalize(tmp, out, max_size);
	return res;
}


bool getOpenFilename(char* out, int max_size, const char* filter, const char* starting_file)
{
	OPENFILENAME ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = NULL;
	if (starting_file)
	{
		char* to = out;
		for (const char *from = starting_file; *from; ++from, ++to)
		{
			if (to - out > max_size - 1) break;
			*to = *to == '/' ? '\\' : *from;
		}
		*to = '\0';
	}
	else
	{
		out[0] = '\0';
	}
	WCHAR wout[MAX_PATH_LENGTH] = {};
	WCHAR wfilter[MAX_PATH_LENGTH];
	
	const char* c = filter;
	WCHAR* cout = wfilter;
	while ((*c || *(c + 1)) && (c - filter) < MAX_PATH_LENGTH - 2) {
		*cout = *c;
		++cout;
		++c;
	}
	*cout = 0;
	++cout;
	*cout = 0;

	ofn.lpstrFile = wout;
	ofn.nMaxFile = sizeof(wout);
	ofn.lpstrFilter = wfilter;
	ofn.nFilterIndex = 1;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = nullptr;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_NONETWORKBUTTON;

	const bool res = GetOpenFileName(&ofn) != FALSE;
	if (res) {
		char tmp[MAX_PATH_LENGTH];
		fromWChar(tmp, lengthOf(tmp), wout);
		PathUtils::normalize(tmp, out, max_size);
	}
	else {
		auto err = CommDlgExtendedError();
		ASSERT(err == 0);
	}
	return res;
}


bool getOpenDirectory(char* out, int max_size, const char* starting_dir)
{
	bool ret = false;
	IFileDialog* pfd;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
	{
		if (starting_dir)
		{
			PIDLIST_ABSOLUTE pidl;
			WCHAR wstarting_dir[MAX_PATH];
			WCHAR* wc = wstarting_dir;
			for (const char *c = starting_dir; *c && wc - wstarting_dir < MAX_PATH - 1; ++c, ++wc)
			{
				*wc = *c == '/' ? '\\' : *c;
			}
			*wc = 0;

			HRESULT hresult = ::SHParseDisplayName(wstarting_dir, 0, &pidl, SFGAO_FOLDER, 0);
			if (SUCCEEDED(hresult))
			{
				IShellItem* psi;
				hresult = ::SHCreateShellItem(NULL, NULL, pidl, &psi);
				if (SUCCEEDED(hresult))
				{
					pfd->SetFolder(psi);
				}
				ILFree(pidl);
			}
		}

		DWORD dwOptions;
		if (SUCCEEDED(pfd->GetOptions(&dwOptions)))
		{
			pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);
		}
		if (SUCCEEDED(pfd->Show(NULL)))
		{
			IShellItem* psi;
			if (SUCCEEDED(pfd->GetResult(&psi)))
			{
				WCHAR* tmp;
				if (SUCCEEDED(psi->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &tmp)))
				{
					char* c = out;
					while (*tmp && c - out < max_size - 1)
					{
						*c = (char)*tmp;
						++c;
						++tmp;
					}
					*c = '\0';
					if (!endsWith(out, "/") && !endsWith(out, "\\") && c - out < max_size - 1)
					{
						*c = '/';
						++c;
						*c = '\0';
					}
					ret = true;
				}
				psi->Release();
			}
		}
		pfd->Release();
	}
	return ret;
}


void copyToClipboard(const char* text)
{
	if (!OpenClipboard(NULL)) return;
	int len = stringLength(text);
	HGLOBAL mem_handle = GlobalAlloc(GMEM_MOVEABLE, len * sizeof(char));
	if (!mem_handle) return;

	char* mem = (char*)GlobalLock(mem_handle);
	copyString(mem, len, text);
	GlobalUnlock(mem_handle);
	EmptyClipboard();
	SetClipboardData(CF_TEXT, mem_handle);
	CloseClipboard();
}


ExecuteOpenResult shellExecuteOpen(const char* path)
{
	const WCharStr<MAX_PATH_LENGTH> wpath(path);
	const uintptr_t res = (uintptr_t)ShellExecute(NULL, NULL, wpath, NULL, NULL, SW_SHOW);
	if (res > 32) return ExecuteOpenResult::SUCCESS;
	if (res == SE_ERR_NOASSOC) return ExecuteOpenResult::NO_ASSOCIATION;
	return ExecuteOpenResult::OTHER_ERROR;
}


bool deleteFile(const char* path)
{
	const WCharStr<MAX_PATH_LENGTH> wpath(path);
	return DeleteFile(wpath) != FALSE;
}


bool moveFile(const char* from, const char* to)
{
	const WCharStr<MAX_PATH_LENGTH> wfrom(from);
	const WCharStr<MAX_PATH_LENGTH> wto(to);
	return MoveFile(wfrom, wto) != FALSE;
}


size_t getFileSize(const char* path)
{
	WIN32_FILE_ATTRIBUTE_DATA fad;
	const WCharStr<MAX_PATH_LENGTH> wpath(path);
	if (!GetFileAttributesEx(wpath, GetFileExInfoStandard, &fad)) return -1;
	LARGE_INTEGER size;
	size.HighPart = fad.nFileSizeHigh;
	size.LowPart = fad.nFileSizeLow;
	return (size_t)size.QuadPart;
}


bool fileExists(const char* path)
{
	const WCharStr<MAX_PATH_LENGTH> wpath(path);
	DWORD dwAttrib = GetFileAttributes(wpath);
	return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}


bool dirExists(const char* path)
{
	const WCharStr<MAX_PATH_LENGTH> wpath(path);
	DWORD dwAttrib = GetFileAttributes(wpath);
	return (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}


u64 getLastModified(const char* path)
{
	const WCharStr<MAX_PATH_LENGTH> wpath(path);
	FILETIME ft;
	HANDLE handle = CreateFile(wpath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) return 0;
	if (GetFileTime(handle, NULL, NULL, &ft) == FALSE) {
		CloseHandle(handle);
		return 0;
	}
	CloseHandle(handle);

	ULARGE_INTEGER i;
	i.LowPart = ft.dwLowDateTime;
	i.HighPart = ft.dwHighDateTime;
	return i.QuadPart;
}


bool makePath(const char* path)
{
	char tmp[MAX_PATH];
	char* out = tmp;
	const char* in = path;
	while (*in && out - tmp < lengthOf(tmp) - 1)
	{
		*out = *in == '/' ? '\\' : *in;
		++out;
		++in;
	}
	*out = '\0';

	const WCharStr<MAX_PATH_LENGTH> wpath(tmp);
	int error_code = SHCreateDirectoryEx(NULL, wpath, NULL);
	return error_code == ERROR_SUCCESS;
}


void clipCursor(WindowHandle win, int x, int y, int w, int h)
{
	POINT min;
	POINT max;
	min.x = LONG(x);
	min.y = LONG(y);
	max.x = LONG(x + w);
	max.y = LONG(y + h);

	ClientToScreen((HWND)win, &min);
	ClientToScreen((HWND)win, &max);
	RECT rect;
	rect.left = min.x;
	rect.right = max.x;
	rect.top = min.y;
	rect.bottom = max.y;
	ClipCursor(&rect);
}


void unclipCursor()
{
	ClipCursor(NULL);
}


bool copyFile(const char* from, const char* to)
{
	WCHAR tmp_from[MAX_PATH];
	WCHAR tmp_to[MAX_PATH];
	toWChar(tmp_from, from);
	toWChar(tmp_to, to);
	
	if (CopyFile(tmp_from, tmp_to, FALSE) == FALSE) return false;

	FILETIME ft;
	SYSTEMTIME st;

	GetSystemTime(&st);
	SystemTimeToFileTime(&st, &ft);
	HANDLE handle = CreateFile(tmp_to, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	bool f = SetFileTime(handle, (LPFILETIME)NULL, (LPFILETIME)NULL, &ft) != FALSE;
	ASSERT(f);
	CloseHandle(handle);

	return true;
}


void getExecutablePath(char* buffer, int buffer_size)
{
	WCHAR tmp[MAX_PATH];
	toWChar(tmp, buffer);
	GetModuleFileName(NULL, tmp, buffer_size);
}


void messageBox(const char* text)
{
	WCHAR tmp[2048];
	toWChar(tmp, text);
	MessageBox(NULL, tmp, L"Message", MB_OK);
}

	
void setCommandLine(int, char**)
{
	ASSERT(false);
}
	

bool getCommandLine(char* output, int max_size)
{
	WCHAR* cl = GetCommandLine();
	fromWChar(output, max_size, cl);
	return true;
}


void* loadLibrary(const char* path)
{
	WCHAR tmp[MAX_PATH];
	toWChar(tmp, path);
	return LoadLibrary(tmp);
}


void unloadLibrary(void* handle)
{
	FreeLibrary((HMODULE)handle);
}


void* getLibrarySymbol(void* handle, const char* name)
{
	return (void*)GetProcAddress((HMODULE)handle, name);
}

Timer::Timer()
{
	LARGE_INTEGER f, n;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&n);
	first_tick = last_tick = n.QuadPart;
	frequency = f.QuadPart;

}


float Timer::getTimeSinceStart()
{
	LARGE_INTEGER n;
	QueryPerformanceCounter(&n);
	const u64 tick = n.QuadPart;
	float delta = static_cast<float>((double)(tick - first_tick) / (double)frequency);
	return delta;
}


float Timer::getTimeSinceTick()
{
	LARGE_INTEGER n;
	QueryPerformanceCounter(&n);
	const u64 tick = n.QuadPart;
	float delta = static_cast<float>((double)(tick - last_tick) / (double)frequency);
	return delta;
}


float Timer::tick()
{
	LARGE_INTEGER n;
	QueryPerformanceCounter(&n);
	const u64 tick = n.QuadPart;
	float delta = static_cast<float>((double)(tick - last_tick) / (double)frequency);
	last_tick = tick;
	return delta;
}


u64 Timer::getFrequency()
{
	static u64 freq = []() {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return f.QuadPart;
	}();
	return freq;
}


u64 Timer::getRawTimestamp()
{
	LARGE_INTEGER tick;
	QueryPerformanceCounter(&tick);
	return tick.QuadPart;
}


} // namespace Lumix::OS
