#include "resource.h"
#include "gens.h"
#include "save.h"
#include "g_main.h"
#include "luascript.h"
#include <assert.h>
#include <vector>
#include <map>
#include <string>
#include <algorithm>

char Recent_Scripts[MAX_RECENT_SCRIPTS][1024];

extern std::vector<HWND> LuaScriptHWnds;
struct LuaPerWindowInfo {
	std::string filename;
	HANDLE fileWatcherThread;
	bool started;
	bool closeOnStop;
	LuaPerWindowInfo() : fileWatcherThread(NULL), started(false), closeOnStop(false) {}
};
std::map<HWND, LuaPerWindowInfo> LuaWindowInfo;
char Lua_Dir[1024]="";


int WINAPI FileSysWatcher (LPVOID arg)
{
	HWND hDlg = (HWND)arg;
	LuaPerWindowInfo& info = LuaWindowInfo[hDlg];

	while(true)
	{
		char filename [1024], directory [1024];

		strncpy(filename, info.filename.c_str(), 1024);
		filename[1023] = 0;
		strcpy(directory, filename);
		char* slash = strrchr(directory, '/');
		slash = max(slash, strrchr(directory, '\\'));
		if(slash)
			*slash = 0;

		WIN32_FILE_ATTRIBUTE_DATA origData;
		GetFileAttributesEx (filename,  GetFileExInfoStandard,  (LPVOID)&origData);

		HANDLE hNotify = FindFirstChangeNotification(directory, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);

		if(hNotify)
		{
			DWORD dwWaitResult = WaitForSingleObject(hNotify, 500);

			if(dwWaitResult != STATUS_TIMEOUT)
			{
				if(dwWaitResult == WAIT_ABANDONED)
					return dwWaitResult;

				WIN32_FILE_ATTRIBUTE_DATA data;
				GetFileAttributesEx (filename,  GetFileExInfoStandard,  (LPVOID)&data);

				// at this point it could be any file in the directory that changed
				// so check to make sure it was the file we care about
				if(memcmp(&origData.ftLastWriteTime, &data.ftLastWriteTime, sizeof(FILETIME)))
				{
					RequestAbortLuaScript((int)hDlg, "terminated to reload the script");
					PostMessage(hDlg, WM_COMMAND, IDC_BUTTON_LUARUN, 0);
				}
			}
		}
		else
		{
			Sleep(500);
		}

		//FindNextChangeNotification(hNotify);
	}

	return 0;
}

void RegisterWatcherThread (HWND hDlg)
{
	HANDLE thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) FileSysWatcher, (LPVOID) hDlg, CREATE_SUSPENDED, NULL);
	SetThreadPriority(thread, THREAD_PRIORITY_LOWEST);

	LuaPerWindowInfo& info = LuaWindowInfo[hDlg];
	info.fileWatcherThread = thread;

	ResumeThread(thread);
}
void KillWatcherThread (HWND hDlg)
{
	LuaPerWindowInfo& info = LuaWindowInfo[hDlg];
	TerminateThread(info.fileWatcherThread, 0);
	info.fileWatcherThread = NULL;
}




void Update_Recent_Script(const char* Path)
{
	FILE* file = fopen(Path, "rb");
	if(file)
		fclose(file);
	else
		return;

	int i;
	for(i = 0; i < MAX_RECENT_SCRIPTS; i++)
	{
		if (!(strcmp(Recent_Scripts[i], Path)))
		{
			// move recent item to the top of the list
			if(i == 0)
				return;
			char temp [1024];
			strcpy(temp, Recent_Scripts[i]);
			int j;
			for(j = i; j > 0; j--)
				strcpy(Recent_Scripts[j], Recent_Scripts[j-1]);
			strcpy(Recent_Scripts[0], temp);
			MustUpdateMenu = 1;
			return;
		}
	}
		
	for(i = MAX_RECENT_SCRIPTS-1; i > 0; i--)
		strcpy(Recent_Scripts[i], Recent_Scripts[i - 1]);

	strcpy(Recent_Scripts[0], Path);

	MustUpdateMenu = 1;
}

bool IsScriptFileOpen(const char* Path)
{
	for(std::map<HWND, LuaPerWindowInfo>::iterator iter = LuaWindowInfo.begin(); iter != LuaWindowInfo.end(); ++iter)
	{
		LuaPerWindowInfo& info = iter->second;
		const char* filename = info.filename.c_str();
		const char* pathPtr = Path;

		// case-insensitive slash-direction-insensitive compare
		bool same = true;
		while(*filename || *pathPtr)
		{
			if((*filename == '/' || *filename == '\\') && (*pathPtr == '/' || *pathPtr == '\\'))
			{
				do {filename++;} while(*filename == '/' || *filename == '\\');
				do {pathPtr++;} while(*pathPtr == '/' || *pathPtr == '\\');
			}
			else if(tolower(*filename) != tolower(*pathPtr))
			{
				same = false;
				break;
			}
			else
			{
				filename++;
				pathPtr++;
			}
		}

		if(same)
			return true;
	}
	return false;
}


void PrintToWindowConsole(int hDlgAsInt, const char* str)
{
	HWND hDlg = (HWND)hDlgAsInt;
	HWND hConsole = GetDlgItem(hDlg, IDC_LUACONSOLE);
	int length = GetWindowTextLength(hConsole);
	if(length > 250000)
	{
		// discard first half of text if it's getting too long
		SendMessage(hConsole, EM_SETSEL, 0, length/2);
		SendMessage(hConsole, EM_REPLACESEL, false, (LPARAM)"");
		length = GetWindowTextLength(hConsole);
	}
	SendMessage(hConsole, EM_SETSEL, length, length);

	LuaPerWindowInfo& info = LuaWindowInfo[hDlg];

	{
		SendMessage(hConsole, EM_REPLACESEL, false, (LPARAM)str);
	}
}

extern int Show_Genesis_Screen(HWND hWnd);
void OnStart(int hDlgAsInt)
{
	HWND hDlg = (HWND)hDlgAsInt;
	LuaPerWindowInfo& info = LuaWindowInfo[hDlg];
	info.started = true;
	EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_LUABROWSE), false); // disable browse while running because it misbehaves if clicked in a frameadvance loop
	EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_LUASTOP), true);
	SetWindowText(GetDlgItem(hDlg, IDC_BUTTON_LUARUN), "Restart");
	SetWindowText(GetDlgItem(hDlg, IDC_LUACONSOLE), ""); // clear the console
	Show_Genesis_Screen(HWnd); // otherwise we might never show the first thing the script draws
}

void OnStop(int hDlgAsInt, bool statusOK)
{
	HWND hDlg = (HWND)hDlgAsInt;
	LuaPerWindowInfo& info = LuaWindowInfo[hDlg];
	info.started = false;
	EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_LUABROWSE), true);
	EnableWindow(GetDlgItem(hDlg, IDC_BUTTON_LUASTOP), false);
	SetWindowText(GetDlgItem(hDlg, IDC_BUTTON_LUARUN), "Run");
	if(statusOK)
		Show_Genesis_Screen(HWnd); // otherwise we might never show the last thing the script draws
	if(info.closeOnStop)
		PostMessage(hDlg, WM_CLOSE, 0, 0);
}

extern "C" int Clear_Sound_Buffer(void);

LRESULT CALLBACK LuaScriptProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	RECT r;
	RECT r2;
	int dx1, dy1, dx2, dy2;


	switch(uMsg)
	{
		case WM_INITDIALOG: {
			if(std::find(LuaScriptHWnds.begin(), LuaScriptHWnds.end(), hDlg) == LuaScriptHWnds.end())
			{
				LuaScriptHWnds.push_back(hDlg);
				Build_Main_Menu();
			}
			if (Full_Screen)
			{
				while (ShowCursor(false) >= 0);
				while (ShowCursor(true) < 0);
			}

			GetWindowRect(HWnd, &r);
			dx1 = (r.right - r.left) / 2;
			dy1 = (r.bottom - r.top) / 2;

			GetWindowRect(hDlg, &r2);
			dx2 = (r2.right - r2.left) / 2;
			dy2 = (r2.bottom - r2.top) / 2;

			int windowIndex = std::find(LuaScriptHWnds.begin(), LuaScriptHWnds.end(), hDlg) - LuaScriptHWnds.begin();
			int staggerOffset = windowIndex * 16;
			r.left += staggerOffset;
			r.right += staggerOffset;
			r.top += staggerOffset;
			r.bottom += staggerOffset;

			// push it away from the main window if we can
			const int width = (r.right-r.left); 
			const int width2 = (r2.right-r2.left); 
			if(r.left+width2 + width < GetSystemMetrics(SM_CXSCREEN))
			{
				r.right += width;
				r.left += width;
			}
			else if((int)r.left - (int)width2 > 0)
			{
				r.right -= width2;
				r.left -= width2;
			}

			SetWindowPos(hDlg, NULL, r.left, r.top, NULL, NULL, SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);

			LuaPerWindowInfo info;
			LuaWindowInfo[hDlg] = info;
			RegisterWatcherThread(hDlg);

			OpenLuaContext((int)hDlg, PrintToWindowConsole, OnStart, OnStop);

			return true;
		}	break;

		case WM_MENUSELECT:
 		case WM_ENTERSIZEMOVE:
			Clear_Sound_Buffer();
			break;

		case WM_COMMAND:
		{
			switch(LOWORD(wParam))
			{
				case IDC_BUTTON_LUABROWSE:
				{
					LuaPerWindowInfo& info = LuaWindowInfo[hDlg];
					strcpy(Str_Tmp,info.filename.c_str());
					SendDlgItemMessage(hDlg,IDC_EDIT_LUAPATH,WM_GETTEXT,(WPARAM)512,(LPARAM)Str_Tmp);
					DialogsOpen++;
					Clear_Sound_Buffer();
					if(Change_File_L(Str_Tmp, Lua_Dir, "Load Lua Script", "Gens Lua Script\0*.lua*\0All Files\0*.*\0\0", "lua", hDlg))
					{
						SendDlgItemMessage(hDlg,IDC_EDIT_LUAPATH,WM_SETTEXT,0,(LPARAM)Str_Tmp);
					}
					DialogsOpen--;

				}	break;
				case IDC_EDIT_LUAPATH:
				{
					switch(HIWORD(wParam))
					{
						case EN_CHANGE:
						{
							char local_str_tmp [1024];
							SendDlgItemMessage(hDlg,IDC_EDIT_LUAPATH,WM_GETTEXT,(WPARAM)512,(LPARAM)local_str_tmp);

							FILE* ftemp = fopen(local_str_tmp, "rb");
							if(ftemp)
							{
								fclose(ftemp);

								LuaPerWindowInfo& info = LuaWindowInfo[hDlg];
								info.filename = local_str_tmp;

								char* slash = strrchr(local_str_tmp, '/');
								slash = max(slash, strrchr(local_str_tmp, '\\'));
								if(slash)
									slash++;
								else
									slash = local_str_tmp;
								SetWindowText(hDlg, slash);
								Build_Main_Menu();

								PostMessage(hDlg, WM_COMMAND, IDC_BUTTON_LUARUN, 0);
							}
						}	break;
					}
				}	break;
				case IDC_BUTTON_LUARUN:
				{
					SetActiveWindow(HWnd);
					LuaPerWindowInfo& info = LuaWindowInfo[hDlg];
					strcpy(Str_Tmp,info.filename.c_str());
					Update_Recent_Script(Str_Tmp);
					RunLuaScriptFile((int)hDlg, Str_Tmp);
				}	break;
				case IDC_BUTTON_LUASTOP:
				{
					PrintToWindowConsole((int)hDlg, "user clicked stop button\r\n");
					SetActiveWindow(HWnd);
					StopLuaScript((int)hDlg);
				}	break;
				//case IDOK:
				case IDCANCEL:
				{	LuaPerWindowInfo& info = LuaWindowInfo[hDlg];
					if(info.filename.empty())
					{
						if (Full_Screen)
						{
							while (ShowCursor(true) < 0);
							while (ShowCursor(false) >= 0);
						}
						DialogsOpen--;
						KillWatcherThread(hDlg);
						LuaScriptHWnds.erase(remove(LuaScriptHWnds.begin(), LuaScriptHWnds.end(), hDlg), LuaScriptHWnds.end());
						LuaWindowInfo.erase(hDlg);
						CloseLuaContext((int)hDlg);
						Build_Main_Menu();
						EndDialog(hDlg, true);
					}
				}	return true;
			}

			return false;
		}	break;

		case WM_CLOSE:
		{
			LuaPerWindowInfo& info = LuaWindowInfo[hDlg];

			PrintToWindowConsole((int)hDlg, "user closed script window\r\n");
			StopLuaScript((int)hDlg);
			if(info.started)
			{
				// not stopped yet, wait to close until we are, otherwise we'll crash
				info.closeOnStop = true;
				return false;
			}

			if (Full_Screen)
			{
				while (ShowCursor(true) < 0);
				while (ShowCursor(false) >= 0);
			}
			DialogsOpen--;
			KillWatcherThread(hDlg);
			LuaScriptHWnds.erase(remove(LuaScriptHWnds.begin(), LuaScriptHWnds.end(), hDlg), LuaScriptHWnds.end());
			LuaWindowInfo.erase(hDlg);
			CloseLuaContext((int)hDlg);
			Build_Main_Menu();
			EndDialog(hDlg, true);
		}	return true;
	}

	return false;
}

