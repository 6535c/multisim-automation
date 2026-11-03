// Calls only the helper hook installed in the explicitly supplied Multisim PID.
#include <windows.h>
#include <cstdio>
#include <string>
#include <regex>
struct MutexGuard {
    HANDLE handle = nullptr;
    bool acquired = false;
    ~MutexGuard() { if (acquired) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
};
static std::string readReport(const wchar_t* path) {
    FILE* report = nullptr;
    if (_wfopen_s(&report, path, L"rb") || !report) return "";
    std::string text;
    char buffer[4096]; size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), report)) != 0 && text.size() < 4 * 1024 * 1024) text.append(buffer, count);
    fclose(report);
    return text;
}
struct WindowChoice { DWORD pid; HWND window; LONG area; };
static void dumpMenu(HMENU menu, int depth) {
    if (!menu || depth > 8) return;
    for (int position = 0; position < GetMenuItemCount(menu); ++position) {
        wchar_t label[512] = {};
        MENUITEMINFOW item = {sizeof(item)};
        item.fMask = MIIM_ID | MIIM_STRING | MIIM_SUBMENU | MIIM_STATE;
        item.dwTypeData = label;
        item.cch = 511;
        if (!GetMenuItemInfoW(menu, position, TRUE, &item)) continue;
        char text[2048];
        WideCharToMultiByte(CP_UTF8, 0, label, -1, text, sizeof(text), nullptr, nullptr);
        printf("depth=%d id=%u state=%u label=%s\n", depth, item.wID, item.fState, text);
        if (item.hSubMenu) dumpMenu(item.hSubMenu, depth + 1);
    }
}
static BOOL CALLBACK chooseWindow(HWND window, LPARAM parameter) {
    WindowChoice* choice = (WindowChoice*)parameter;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != choice->pid) return TRUE;
    RECT rectangle = {};
    GetClientRect(window, &rectangle);
    LONG area = (rectangle.right - rectangle.left) * (rectangle.bottom - rectangle.top);
    if (!choice->window || area > choice->area) {
        choice->window = window;
        choice->area = area;
    }
    return TRUE;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4) { fwprintf(stderr, L"Usage: native_host.exe OWNED_MULTISIM_PID [inspect|save|new|batch REQUEST.tsv]\n"); return 2; }
    if (!wcscmp(argv[1], L"--string-test")) {
        HMODULE mfc = LoadLibraryW(L"C:\\Windows\\SysWOW64\\MFC71U.DLL");
        typedef void* (__thiscall* Constructor)(void*, const wchar_t*);
        typedef void (__thiscall* Destructor)(void*);
        Constructor construct = (Constructor)GetProcAddress(mfc, MAKEINTRESOURCEA(283));
        Destructor destroy = (Destructor)GetProcAddress(mfc, MAKEINTRESOURCEA(577));
        void* data = nullptr;
        if (!construct || !destroy) return 10;
        construct(&data, L"native-1k");
        int characters = *((int*)data - 3);
        bool matches = characters == 9 && !memcmp(data, L"native-1k", 18);
        printf("{\"native_wide_string\":%s,\"characters\":%d}\n", matches ? "true" : "false", characters);
        destroy(&data);
        return matches ? 0 : 11;
    }
    DWORD pid = wcstoul(argv[1], nullptr, 10);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t executable[MAX_PATH]; DWORD length = MAX_PATH;
    if (!process || !QueryFullProcessImageNameW(process, 0, executable, &length)) return 3;
    CloseHandle(process);
    if (_wcsicmp(executable, L"D:\\Multisim10\\Circuit Design Suite 10.0\\multisim.exe")) return 4;
    MutexGuard guard;
    guard.handle = CreateMutexW(nullptr, FALSE, L"Local\\Codex.Multisim10.NativeBridge.Request.v1");
    DWORD locked = guard.handle ? WaitForSingleObject(guard.handle, 30000) : WAIT_FAILED;
    guard.acquired = locked == WAIT_OBJECT_0 || locked == WAIT_ABANDONED;
    if (!guard.acquired) return 21;
    WindowChoice choice = {pid, nullptr, -1};
    EnumWindows(chooseWindow, (LPARAM)&choice);
    if (!choice.window) { fprintf(stderr, "Owned Multisim has no hookable window\n"); return 5; }
    if (argc == 3 && !wcscmp(argv[2], L"menus")) {
        HMENU menu = GetMenu(choice.window);
        printf("menu=%p window=%p\n", menu, choice.window);
        dumpMenu(menu, 0);
        return 0;
    }
    wchar_t library[MAX_PATH];
    GetModuleFileNameW(nullptr, library, MAX_PATH);
    wchar_t* separator = wcsrchr(library, L'\\');
    wcscpy_s(separator + 1, MAX_PATH - (separator + 1 - library), L"native_bridge.dll");
    HMODULE module = LoadLibraryW(library);
    if (!module) { fprintf(stderr, "LoadLibrary error %lu\n", GetLastError()); return 6; }
    HOOKPROC callback = (HOOKPROC)GetProcAddress(module, "_MultisimHook@12");
    DWORD thread = GetWindowThreadProcessId(choice.window, nullptr);
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, callback, module, thread);
    if (!hook) { fprintf(stderr, "Hook error %lu\n", GetLastError()); return 7; }
    DWORD_PTR result = 0;
    WPARAM action = argc >= 3 ? (!wcscmp(argv[2], L"factory") ? 1 : (!wcscmp(argv[2], L"place") ? 2 : (!wcscmp(argv[2], L"save") ? 3 : (!wcscmp(argv[2], L"connect") ? 4 : (!wcscmp(argv[2], L"saveas") ? 5 : (!wcscmp(argv[2], L"new") ? 6 : (!wcscmp(argv[2], L"batch") ? 7 : 0))))))) : 0;
    std::wstring directory(library, separator + 1);
    std::wstring faultFile = directory + L"native-fault.json";
    std::wstring outputFile = directory + (action == 0 ? L"native-snapshot.json" : L"native-operation.json");
    DeleteFileW(faultFile.c_str());
    DeleteFileW(outputFile.c_str());
    if (action == 7 && (argc != 4 || !CopyFileW(argv[3], (directory + L"native-request.tsv").c_str(), FALSE))) {
        UnhookWindowsHookEx(hook); FreeLibrary(module); return 22;
    }
    LRESULT sent = SendMessageTimeoutW(choice.window, RegisterWindowMessageW(L"Codex.Multisim10.NativeBridge.v1"), action, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 15000, &result);
    DWORD error = GetLastError();
    UnhookWindowsHookEx(hook);
    FreeLibrary(module);
    printf("{\"pid\":%lu,\"thread\":%lu,\"window\":\"%p\",\"sent\":%s,\"error\":%lu}\n", pid, thread, choice.window, sent ? "true" : "false", error);
    std::string fault = readReport(faultFile.c_str());
    if (!fault.empty()) { fprintf(stderr, "%s", fault.c_str()); return 20; }
    std::string contents = readReport(outputFile.c_str());
    if (sent && contents.empty()) return 23;
    std::smatch reportOwner;
    if (sent && (!std::regex_search(contents, reportOwner, std::regex("\\\"pid\\\":([0-9]+)")) || std::stoul(reportOwner[1]) != pid)) return 24;
    if (sent && (action == 2 || action == 3 || action == 4 || action == 5 || action == 6)) {
        wcscpy_s(separator + 1, MAX_PATH - (separator + 1 - library), L"native-operation.json");
        std::smatch owner, command;
        if (!std::regex_search(contents, owner, std::regex("\\\"pid\\\":([0-9]+)")) || std::stoul(owner[1]) != pid) return 13;
        if (!std::regex_search(contents, command, std::regex("\\\"save_command_id\\\":([0-9]+)"))) return 14;
        UINT id = (UINT)std::stoul(command[1]);
        if (!id || id >= 0xFFFF) { printf("{\"save_command_sent\":false,\"reason\":\"not_mapped\"}\n"); return 0; }
        std::smatch frameMatch;
        if (!std::regex_search(contents, frameMatch, std::regex("\\\"frame_hwnd\\\":\\\"([0-9A-Fa-f]+)\\\""))) return 17;
        HWND frame = (HWND)(ULONG_PTR)std::stoul(frameMatch[1], nullptr, 16);
        DWORD frameOwner = 0;
        GetWindowThreadProcessId(frame, &frameOwner);
        if (frameOwner != pid) return 18;
        LRESULT saved = SendMessageTimeoutW(frame, WM_COMMAND, MAKEWPARAM(id, 0), 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 15000, &result);
        printf("{\"save_command_id\":%u,\"save_command_sent\":%s}\n", id, saved ? "true" : "false");
        if (!saved) return 16;
    }
    if (!contents.empty()) printf("%s", contents.c_str());
    if (action == 7 && contents.find("\"ok\":false") != std::string::npos) return 25;
    return sent ? 0 : 8;
}
