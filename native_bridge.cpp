// Native editing and inspection inside an owned Multisim 10 process.
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <cwchar>
#include <cmath>

static HMODULE selfModule;
static const wchar_t* messageName = L"Codex.Multisim10.NativeBridge.v1";
static int runtimeSlot = 0;
static FILE* currentOutput;
static const char* operationStage = "idle";
static int methodSlot = -1;
static unsigned syncManagers, syncMatches;
static void* syncTree;
static void* faultAddress;
static ULONG_PTR accessAddress;
static DWORD faultEcx;
static LONG captureFault(EXCEPTION_POINTERS* fault) {
    faultAddress = fault->ExceptionRecord->ExceptionAddress;
    accessAddress = fault->ExceptionRecord->NumberParameters > 1 ? fault->ExceptionRecord->ExceptionInformation[1] : 0;
    faultEcx = fault->ContextRecord->Ecx;
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool readable(const void* pointer, size_t bytes) {
    MEMORY_BASIC_INFORMATION memory = {};
    if (!pointer || VirtualQuery(pointer, &memory, sizeof(memory)) != sizeof(memory)) return false;
    if (memory.State != MEM_COMMIT || memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const char*)pointer + bytes <= (const char*)memory.BaseAddress + memory.RegionSize;
}
static bool executable(const void* pointer) {
    MEMORY_BASIC_INFORMATION memory = {};
    if (!pointer || !VirtualQuery(pointer, &memory, sizeof(memory))) return false;
    return memory.State == MEM_COMMIT && (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
static void* runtimeClass(void* object) {
    __try {
        if (!readable(object, 4)) return nullptr;
        void** table = *(void***)object;
        if (runtimeSlot < 0 || !readable(table, 4 * (runtimeSlot + 1)) || !executable(table[runtimeSlot])) return nullptr;
        // Read the compiler's constant-return GetRuntimeClass stub. Never call
        // a speculative vtable entry while inspecting an arbitrary field.
        unsigned char* code = (unsigned char*)table[runtimeSlot];
        for (int jump = 0; jump < 6 && readable(code, 6); ++jump) {
            if (code[0] == 0xE9) code += 5 + *(int*)(code + 1);
            else if (code[0] == 0xFF && code[1] == 0x25 && readable(*(void**)(code + 2), 4)) code = **(unsigned char***)(code + 2);
            else break;
        }
        if (!readable(code, 6) || code[0] != 0xB8 || code[5] != 0xC3) return nullptr;
        void* klass = *(void**)(code + 1);
        if (!readable(klass, 20)) return nullptr;
        const char* name = *(const char**)klass;
        if (!readable(name, 64) || name[0] != 'C') return nullptr;
        for (int i = 0; i < 64; ++i) {
            if (!name[i]) return klass;
            if (name[i] < 32 || name[i] > 126) return nullptr;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
static const char* runtimeName(void* object) {
    void* klass = runtimeClass(object);
    return klass ? *(const char**)klass : nullptr;
}
static FARPROC symbol(const char* module, const char* name) {
    HMODULE handle = GetModuleHandleA(module);
    return handle ? GetProcAddress(handle, name) : nullptr;
}
static void* globalObject(const char* name) {
    typedef void* (__cdecl* GetObject)();
    GetObject fn = (GetObject)symbol("DrwEngine.dll", name);
    return fn ? fn() : nullptr;
}
static std::wstring siblingPath(const wchar_t* filename) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(selfModule, path, MAX_PATH);
    wchar_t* separator = wcsrchr(path, L'\\');
    return separator ? std::wstring(path, separator + 1) + filename : filename;
}
struct NativeString {
    void* data = nullptr;
    NativeString() = default;
    NativeString(const wchar_t* value) {
        typedef void* (__thiscall* Constructor)(void*, const wchar_t*);
        Constructor fn = (Constructor)GetProcAddress(GetModuleHandleA("MFC71U.DLL"), MAKEINTRESOURCEA(283));
        fn(this, value);
    }
    ~NativeString() {
        typedef void (__thiscall* Destructor)(void*);
        Destructor fn = (Destructor)GetProcAddress(GetModuleHandleA("MFC71U.DLL"), MAKEINTRESOURCEA(577));
        if (data) fn(this);
    }
    NativeString(const NativeString&) = delete;
};
static void* currentDocument(void* sheet) {
    void* metadata = runtimeClass(sheet);
    if (!metadata) return nullptr;
    unsigned bytes = *(unsigned*)((char*)metadata + 4);
    for (unsigned offset = 4; offset + 4 <= bytes && offset < 0x400; offset += 4) {
        void* candidate = *(void**)((char*)sheet + offset);
        const char* name = runtimeName(candidate);
        if (name && !strcmp(name, "CIITHierDoc")) return candidate;
    }
    return nullptr;
}
static void* currentDiagram(void* sheet) {
    typedef void* (__thiscall* Getter)(void*);
    Getter fn = (Getter)symbol("SCH_CAP.dll", "?GetDiagram@CSCH_CAPDoc@@QBEPAVCODDiagram@@XZ");
    void* document = currentDocument(sheet);
    return document && fn ? fn(document) : nullptr;
}
static void* dynamicMethod(void* object, const char* baseTableName, const char* methodName, const char* module = "Od101dll.dll") {
    void** base = (void**)symbol(module, baseTableName);
    void* method = (void*)symbol(module, methodName);
    if (!base || !method || !object) return nullptr;
    for (int i = 0; i < 384 && readable(base + i, 4); ++i) {
        if (base[i] == method) { methodSlot = i; return (*(void***)object)[i]; }
    }
    return nullptr;
}
static void* findTypeDescriptor(const char* name) {
    unsigned char* module = (unsigned char*)GetModuleHandleA("SCH_CAP.dll");
    if (!module) return nullptr;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)module;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(module + dos->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    size_t length = strlen(name) + 1;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(section[i].Name, ".rdata", 6) && memcmp(section[i].Name, ".data", 5)) continue;
        unsigned char* first = module + section[i].VirtualAddress;
        size_t bytes = section[i].Misc.VirtualSize;
        for (size_t offset = 0; offset < bytes; offset += 2048) {
            size_t chunk = (std::min)(bytes - offset, (size_t)2048 + length);
            if (!readable(first + offset, chunk)) continue;
            unsigned char* found = std::search(first + offset, first + offset + chunk, name, name + length);
            if (found != first + offset + chunk && found >= first + 8) return found - 8;
        }
    }
    return nullptr;
}
static void* nativeView(const char* target = ".?AVCIITView@@") {
    void* base = globalObject("?GetSheetView@@YAPAVCBaseView@@XZ");
    typedef void* (__cdecl* Cast)(void*, long, void*, void*, int);
    Cast cast = (Cast)symbol("MSVCR71.dll", "__RTDynamicCast");
    void* from = findTypeDescriptor(".?AVCBaseView@@");
    void* to = findTypeDescriptor(target);
    return base && cast && from && to ? cast(base, 0, from, to, 0) : nullptr;
}
static std::string utf8(const wchar_t* text) {
    if (!text || !readable(text, 2)) return "";
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length < 1 || length > 65536) return "";
    std::string output(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &output[0], length, nullptr, nullptr);
    output.resize(length - 1);
    return output;
}
static void jsonString(FILE* file, const std::string& text) {
    fputc('"', file);
    for (unsigned char character : text) {
        if (character == '"' || character == '\\') fprintf(file, "\\%c", character);
        else if (character < 32) fprintf(file, "\\u%04x", character);
        else fputc(character, file);
    }
    fputc('"', file);
}
static void pointerDiagnostic(FILE* file, void* object) {
    fprintf(file, ",\"component_address\":\"%p\"", object);
    if (!readable(object, 4)) return;
    void** table = *(void***)object;
    if (!readable(table - 1, 12)) return;
    unsigned char* method = (unsigned char*)table[0];
    fprintf(file, ",\"class_stub_hex\":\"");
    if (readable(method, 24)) for (int i = 0; i < 24; ++i) fprintf(file, "%02x", method[i]);
    fprintf(file, "\"");
    void* locator = table[-1];
    if (readable(locator, 20)) {
        void* type = *(void**)((char*)locator + 12);
        if (readable(type, 100)) {
            const char* typeName = (const char*)type + 8;
            if (typeName[0] == '.') {
                fprintf(file, ",\"complete_type\":\"%.90s\",\"base_offset\":%u", typeName, *(unsigned*)((char*)locator + 4));
            }
        }
    }
}
static std::string referenceString(void* object, const char* name, const char* module = "Od101dll.dll") {
    typedef void* (__thiscall* Getter)(void*);
    Getter fn = (Getter)symbol(module, name);
    if (!fn) return "";
    void* string = fn(object);
    return readable(string, 4) ? utf8(*(const wchar_t**)string) : "";
}
static std::string valueString(void* object, const char* name, const char* module = "Od101dll.dll") {
    // The old nontrivial CString return uses a caller-owned hidden result slot.
    typedef void* (__thiscall* Getter)(void*, void*);
    Getter fn = (Getter)symbol(module, name);
    NativeString result;
    if (fn) fn(object, &result);
    return utf8((const wchar_t*)result.data);
}
static void* electricalData(void* object);
static void* pinNode(void* pin) {
    typedef void* (__thiscall* PortGetter)(void*);
    typedef void* (__thiscall* NodeGetter)(void*, int, bool);
    PortGetter getPort = (PortGetter)symbol("Od101dll.dll", "?GetCiPort@CIITPinSymbolComp@@QAEPAVCiPort@@XZ");
    NodeGetter getNode = (NodeGetter)symbol("Circexp.dll", "?GetNode@CiPort@@QBEPAVCiNode@@H_N@Z");
    void* port = pin && getPort ? getPort(pin) : nullptr;
    return port && getNode ? getNode(port, 0, false) : nullptr;
}
static void* diagramRoot(void* diagram) {
    typedef void* (__thiscall* Getter)(void*);
    Getter getRoot = (Getter)symbol("Od101dll.dll", "?GetComponent@CODDiagram@@QBEPAVCODComponent@@XZ");
    return diagram && getRoot ? getRoot(diagram) : nullptr;
}
static void* findSymbol(void* diagram, const char* refdes) {
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*, void**);
    Getter first = (Getter)symbol("Od101dll.dll", "?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    Next next = (Next)symbol("Od101dll.dll", "?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    void* root = diagramRoot(diagram);
    void* position = root && first ? first(root) : nullptr;
    for (unsigned i = 0; position && next && i < 16384; ++i) {
        void* object = next(root, &position);
        const char* name = runtimeName(object);
        if (name && (!strcmp(name, "CIITSymbolComp") || !strcmp(name, "CIITAnimationSymbolComp"))) {
            if (referenceString((char*)object + 0x40, "?GetMultisectionInstanceRefDes@CIITSymbolComp@@UAEAAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ") == refdes) return object;
            void* data = electricalData(object);
            if (data && valueString(data, "?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ", "Circexp.dll") == refdes) return object;
        }
    }
    return nullptr;
}
static void* electricalData(void* object) {
    typedef void* (__cdecl* Getter)(void*);
    Getter fn = (Getter)symbol("SCH_CAP.dll", "?GetSymbolComponentData@CComponentBridge@@SAPAVCiComponent@@PAVCODComponent@@@Z");
    return object && fn ? fn(object) : nullptr;
}
static bool isInstrumentSymbol(void* object) {
    typedef bool (__cdecl* IsInstrument)(void*);
    IsInstrument test=(IsInstrument)symbol("SCH_CAP.dll","?IsInstrument@CComponentBridge@@SA_NPAVCODSymbolComp@@@Z");
    return object && test && test(object);
}
static void* pinByNumber(void* object, const char* number) {
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*, void**);
    Getter first = (Getter)symbol("Od101dll.dll", "?GetFirstPinPosition@CIITSymbolComp@@QAEPAU__POSITION@@XZ");
    Next next = (Next)symbol("Od101dll.dll", "?GetNextPin@CIITSymbolComp@@QAEPAVCIITPinSymbolComp@@AAPAU__POSITION@@@Z");
    void* position = first && object ? first(object) : nullptr;
    bool instrument=isInstrumentSymbol(object);
    void* found=nullptr;
    for (unsigned i = 0; position && next && i < 256; ++i) {
        void* pin = next(object, &position);
        const char* getter=instrument ? "?GetPinName@CIITPinSymbolComp@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ" : "?GetPinNumber@CIITPinSymbolComp@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ";
        if (valueString(pin,getter)==number) {
            if (!instrument) return pin;
            if (found) return nullptr;
            found=pin;
        }
    }
    return found;
}
struct DiagramPoint { LONG x; LONG y; };
#include "native_endpoints.h"
static void* currentCircuit(void* sheet);
static void* makeProbe(void* sheet,void* diagram,const wchar_t* name,unsigned kind,DiagramPoint at);
static bool bindProbe(void* object,void* node);
static bool setProbeDirection(void* object,DiagramPoint at,DiagramPoint positive);
static int netNamePriority(void* node);
static bool propagateMergedNetNames(void* diagram,const std::string& oldA,const std::string& oldB,void* winner);
static void* makeWire(void* diagram, void* sourcePin, void* targetPin) {
    operationStage = "wire_pin_points";
    typedef void* (__thiscall* Getter)(void*);
    DiagramPoint endpoints[2] = {};
    if (!sourcePin || !targetPin) return nullptr;
    void* source = terminalConnector(sourcePin, &endpoints[0]);
    void* target = terminalConnector(targetPin, &endpoints[1]);
    DiagramPoint vertices[4] = {endpoints[0], {endpoints[0].x, endpoints[0].y - 120}, {endpoints[1].x, endpoints[0].y - 120}, endpoints[1]};
    typedef void* (__cdecl* Allocate)(unsigned);
    Allocate allocate = (Allocate)symbol("MSVCR71.dll", "??2@YAPAXI@Z");
    void* klass = (void*)symbol("SCH_CAP.dll", "?classCIITLinkComp@CIITLinkComp@@2UCRuntimeClass@@A");
    unsigned bytes = klass && readable(klass, 8) ? *(unsigned*)((char*)klass + 4) : 0;
    if (!source || !target || !allocate || bytes != 0x74) return nullptr;
    typedef void* (__thiscall* Constructor)(void*, DiagramPoint*, int, void*);
    Constructor construct = (Constructor)symbol("SCH_CAP.dll", "??0CIITLinkComp@@QAE@PAVCPoint@@HPAVCODGraphic@@@Z");
    operationStage = "wire_construct";
    if (!construct) return nullptr;
    void* link = construct(allocate(bytes), vertices, 4, nullptr);
    typedef void (__thiscall* DiagramSetter)(void*, void*);
    DiagramSetter setDiagram = (DiagramSetter)symbol("Od101dll.dll", "?SetDiagram@CODComponent@@QAEXPAVCODDiagram@@@Z");
    if (!setDiagram) return nullptr;
    setDiagram(link, diagram);
    typedef void (__thiscall* ConnectorGetter)(void*, void**, void**);
    ConnectorGetter connectorGetter = (ConnectorGetter)symbol("Od101dll.dll", "?GetConnectors@CODLinkComp@@QAEXAAPAVCODConnector@@0@Z");
    void* start = nullptr;
    void* end = nullptr;
    if (!connectorGetter) return nullptr;
    connectorGetter(link, &start, &end);
    operationStage = "wire_connect_graphics";
    typedef void (__thiscall* Connect)(void*, void*);
    Connect connectStart = (Connect)dynamicMethod(start, "??_7CODConnector@@6B@", "?Connect@CODConnector@@UAEXPAV1@@Z");
    Connect connectEnd = (Connect)dynamicMethod(end, "??_7CODConnector@@6B@", "?Connect@CODConnector@@UAEXPAV1@@Z");
    fprintf(currentOutput, ",\"wire_debug\":{\"source\":\"%p\",\"target\":\"%p\",\"start\":\"%p\",\"end\":\"%p\",\"connect_start\":\"%p\",\"connect_end\":\"%p\"}", source, target, start, end, connectStart, connectEnd);
    fflush(currentOutput);
    if (!connectStart || !connectEnd) return nullptr;
    void* root = diagramRoot(diagram);
    typedef void* (__thiscall* BoxGetter)(void*, RECT*, void*);
    BoxGetter boxGetter = (BoxGetter)dynamicMethod(link, "??_7CODComponent@@6B@", "?GetBox@CODComponent@@UAE?AVCRect@@AAUODInstContext@@@Z");
    void* context = (void*)symbol("Od101dll.dll", "?DefIC@@3UODInstContext@@A");
    void* view = nativeView(".?AVCODView@@");
    typedef void (__thiscall* Add)(void*, void*, DiagramPoint, void*);
    Add add = (Add)dynamicMethod(diagram, "??_7CODDiagram@@6B@", "?AddComponent@CODDiagram@@UAEXPAVCODComponent@@VCPoint@@PAVCODView@@@Z");
    if (!boxGetter || !context || !view || !add) return nullptr;
    RECT box = {}; boxGetter(link, &box, context);
    operationStage = "wire_append";
    DiagramPoint anchor = {box.left, box.top};
    add(diagram, link, anchor, view);
    typedef void* (__thiscall* ConnectConstructor)(void*, void*, void*, void*);
    ConnectConstructor connectConstructor = (ConnectConstructor)symbol("Od101dll.dll", "??0CODConnectAct@@QAE@PAVCODView@@PAVCODConnector@@1@Z");
    typedef long (__thiscall* Execute)(void*, void*);
    Execute execute = (Execute)dynamicMethod(diagram, "??_7CODDiagram@@6B@", "?ExecuteAct@CODDiagram@@UAEJPAVCODAction@@@Z");
    if (!connectConstructor || !execute) return nullptr;
    operationStage = "wire_connect_actions";
    void* actionA = connectConstructor(allocate(256), view, start, source);
    if (execute(diagram, actionA) != 0) return nullptr;
    void* actionB = connectConstructor(allocate(256), view, end, target);
    if (execute(diagram, actionB) != 0) return nullptr;
    typedef void* (__thiscall* AddConnection)(void*, void*, int, void*, bool);
    AddConnection attach = (AddConnection)symbol("Circexp.dll", "?AddConnection@CiCircuit@@QAEPAVCiNode@@PAVCiPort@@HPAV2@_N@Z");
    void* oldSourceNode = terminalNode(sourcePin);
    void* oldTargetNode = terminalNode(targetPin);
    std::string oldSourceName=oldSourceNode ? valueString(oldSourceNode,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll") : "";
    std::string oldTargetName=oldTargetNode ? valueString(oldTargetNode,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll") : "";
    void* node = oldTargetNode;
    operationStage = "wire_attach_electrical";
    if (!node) node = oldSourceNode;
    // Preserve ground and explicit names; target wins ties.
    if (netNamePriority(oldSourceNode)>netNamePriority(oldTargetNode)) node=oldSourceNode;
    if (!node) {
        void* circuit = currentCircuit(globalObject("?GetDrwSubSheet@@YAPAVCDrwSubSheet@@XZ"));
        void* nodeClass = (void*)symbol("Circexp.dll", "?classCiNode@CiNode@@2UCRuntimeClass@@A");
        unsigned nodeBytes = nodeClass ? *(unsigned*)((char*)nodeClass + 4) : 0;
        typedef void* (__thiscall* NodeConstructor)(void*);
        NodeConstructor nodeConstructor = (NodeConstructor)symbol("Circexp.dll", "??0CiNode@@QAE@XZ");
        if (!circuit || !nodeConstructor || nodeBytes < 16 || nodeBytes > 1024) return nullptr;
        node = nodeConstructor(allocate(nodeBytes));
        wchar_t nodeName[80];
        swprintf_s(nodeName, L"N_AUTO_%lu_%p", GetTickCount(), node);
        typedef int (__thiscall* NameSetter)(void*, const wchar_t*);
        NameSetter nameSetter = (NameSetter)symbol("Circexp.dll", "?SetLocalName@CiElement@@QAEHPB_W@Z");
        nameSetter(node, nodeName);
        typedef int (__thiscall* NodeAdder)(void*, void*);
        NodeAdder addNode = (NodeAdder)symbol("Circexp.dll", "?AddNodeEx@CiCircuit@@QAEHPAVCiNode@@@Z");
        addNode(circuit, node);
    }
    if (node) {
        typedef int (__thiscall* Merge)(void*,void*,bool);
        Merge merge = (Merge)symbol("Circexp.dll","?Merge@CiNode@@QAEHPAV1@_N@Z");
        if (!merge || !attach) return nullptr;
        for (void* prior : {oldSourceNode,oldTargetNode})
            if (prior && prior!=node && !merge(node,prior,false)) return nullptr;
        void* portA = terminalPort(sourcePin); void* portB = terminalPort(targetPin);
        if (portA) node = attach(nullptr, portA, 0, node, false);
        if (node && portB) node = attach(nullptr, portB, 0, node, false);
    }
    typedef bool (__cdecl* SetNode)(void*, void*);
    SetNode setNode = (SetNode)symbol("SCH_CAP.dll", "?SetLinkNodeData@CComponentBridge@@SA_NPAVCODComponent@@PAVCiNode@@@Z");
    if (node) {
        setNode(link, node);
        // AddConnection merges electrical ports, but legacy graphic links keep
        // their old node pointer unless the editor also updates those links.
        Getter linkNode = (Getter)symbol("SCH_CAP.dll", "?GetNode@CIITLinkComp@@QAEPAVCiNode@@XZ");
        Getter first = (Getter)symbol("Od101dll.dll", "?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
        typedef void* (__thiscall* Next)(void*, void**);
        Next next = (Next)symbol("Od101dll.dll", "?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
        void* position = first && next && linkNode ? first(root) : nullptr;
        for (unsigned i = 0; position && i < 16384; ++i) {
            void* item = next(root, &position);
            const char* type = runtimeName(item);
            if (!type || strcmp(type, "CIITLinkComp")) continue;
            void* prior = linkNode(item);
            if (prior && (prior == oldSourceNode || prior == oldTargetNode)) setNode(item, node);
        }
    }
    return node && propagateMergedNetNames(diagram,oldSourceName,oldTargetName,node) ? link : nullptr;
}
static void* currentProject(void* sheet) {
    typedef void* (__thiscall* Getter)(void*, void*);
    Getter fn = (Getter)symbol("DrwEngine.dll", "?GetProject@CWorkspaceContainer@@QAEPAVCProject@@PBVCDrwSubSheet@@@Z");
    void* workspace = globalObject("?GetWorkSpaceContainer@@YAPAVCWorkspaceContainer@@XZ");
    return workspace && sheet && fn ? fn(workspace, sheet) : nullptr;
}
static std::vector<void*> incidentWires(void* diagram, void* component) {
    std::vector<void*> result;
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*, void**);
    typedef bool (__thiscall* Connected)(void*, void*);
    Getter first = (Getter)symbol("Od101dll.dll", "?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    Next next = (Next)symbol("Od101dll.dll", "?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    Connected connected = (Connected)symbol("SCH_CAP.dll", "?IsConnectedToComponent@CIITLinkComp@@QAE_NPAVCODSymbolComp@@@Z");
    Getter source = (Getter)symbol("Od101dll.dll", "?GetSourceConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    Getter target = (Getter)symbol("Od101dll.dll", "?GetTargetConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    void* root = diagramRoot(diagram);
    void* position = root && first ? first(root) : nullptr;
    for (unsigned i = 0; position && next && i < 16384; ++i) {
        void* item = next(root, &position);
        const char* klass = runtimeName(item);
        if (!klass || strcmp(klass,"CIITLinkComp")) continue;
        if (isScalarConnector(component) ? (source && target && (source(item)==component || target(item)==component)) : (connected && connected(item,component))) result.push_back(item);
    }
    return result;
}
static void* findWire(void* diagram, void* a, void* b) {
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*, void**);
    Getter first = (Getter)symbol("Od101dll.dll", "?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    Next next = (Next)symbol("Od101dll.dll", "?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    Getter source = (Getter)symbol("Od101dll.dll", "?GetSourceConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    Getter target = (Getter)symbol("Od101dll.dll", "?GetTargetConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    if (!a || !b || !first || !next || !source || !target) return nullptr;
    DiagramPoint ignored;
    void* ac = terminalConnector(a, &ignored); void* bc = terminalConnector(b, &ignored);
    void* root = diagramRoot(diagram);
    void* position = first(root);
    for (unsigned i = 0; position && i < 16384; ++i) {
        void* item = next(root, &position);
        const char* klass = runtimeName(item);
        if (!klass || strcmp(klass, "CIITLinkComp")) continue;
        void* s = source(item); void* t = target(item);
        if ((s == ac && t == bc) || (s == bc && t == ac)) return item;
    }
    return nullptr;
}
static bool alignWireEndpoints(void* wire,void* a,void* b) {
    DiagramPoint desiredA={},desiredB={},oldA={},oldB={};
    if (!terminalConnector(a,&desiredA) || !terminalConnector(b,&desiredB)) return false;
    typedef void (__thiscall* Endpoints)(void*,DiagramPoint*,DiagramPoint*);
    Endpoints endpoints=(Endpoints)symbol("Od101dll.dll","?GetEndpoints@CODLinkComp@@QAEXAAVCPoint@@0@Z");
    typedef void (__thiscall* Vertices)(void*,DiagramPoint**,int*);
    Vertices vertices=(Vertices)symbol("Od101dll.dll","?GetVertices@CODLinkComp@@QAEXAAPAVCPoint@@AAH@Z");
    typedef void (__cdecl* Free)(void*);
    Free release=(Free)symbol("MSVCR71.dll","??_V@YAXPAX@Z");
    typedef void* (__thiscall* Getter)(void*);
    Getter multiline=(Getter)symbol("Od101dll.dll","?GetMultiLine@CODLinkComp@@QAEPAVCODMultiLine@@XZ");
    Getter graphic=(Getter)symbol("Od101dll.dll","?GetGraphic@CODComponent@@QAEPAVCODGraphic@@XZ");
    typedef void (__thiscall* Points)(void*,DiagramPoint*,int);
    Points setter=(Points)symbol("Od101dll.dll","?SetNewPointsArray@CODVertices@@QAEXPAVCPoint@@H@Z");
    typedef void (__thiscall* Connectors)(void*,void**,void**);
    Connectors connectors=(Connectors)symbol("Od101dll.dll","?GetConnectors@CODLinkComp@@QAEXAAPAVCODConnector@@0@Z");
    typedef void (__thiscall* Translate)(void*,float,float);
    Translate translate=(Translate)symbol("Od101dll.dll","?Translate@CODGraphic@@UAEXMM@Z");
    if (!endpoints || !vertices || !release || !multiline || !graphic || !setter || !connectors || !translate) return false;
    endpoints(wire,&oldA,&oldB);
    DiagramPoint* points=nullptr; int count=0;
    vertices(wire,&points,&count);
    if (!points || count<2 || count>65536) { if (points) release(points); return false; }
    points[0]=desiredA; points[count-1]=desiredB;
    setter(multiline(wire),points,count);
    release(points);
    void* start=nullptr; void* end=nullptr; connectors(wire,&start,&end);
    if (!start || !end) return false;
    translate(graphic(start),(float)(desiredA.x-oldA.x),(float)(desiredA.y-oldA.y));
    translate(graphic(end),(float)(desiredB.x-oldB.x),(float)(desiredB.y-oldB.y));
    return true;
}
#include "native_topology.h"
static bool deleteNative(void* diagram, const std::vector<void*>& objects) {
    void* view = nativeView(".?AVCODView@@");
    void* iitView = nativeView();
    typedef void* (__thiscall* Getter)(void*);
    Getter selectionGetter = (Getter)symbol("Od101dll.dll", "?GetSelection@CODView@@QAEPAVCODSelection@@XZ");
    typedef void (__thiscall* Clear)(void*);
    Clear clear = (Clear)symbol("Od101dll.dll", "?Clear@CODSelection@@QAEXXZ");
    // CODComposite.SelectComp clears the selection on every call. Append all
    // requested objects to its native CPtrList instead (same #998 import used
    // by CODComposite.SelectCompList) so incident wires are actually selected.
    typedef void* (__thiscall* Append)(void*,void*);
    Append append = (Append)GetProcAddress(GetModuleHandleA("MFC71U.dll"),MAKEINTRESOURCEA(998));
    typedef void (__thiscall* Delete)(void*, bool, bool);
    Delete remove = (Delete)symbol("SCH_CAP.dll", "?DeleteCompInSelection@CIITView@@QAEX_N0@Z");
    void* selection = view && selectionGetter ? selectionGetter(view) : nullptr;
    if (!selection || !iitView || !clear || !append || !remove) return false;
    DeletionTopology topology;
    if (!topology.capture(diagram,objects)) {
        fprintf(currentOutput, ",\"error\":\"Deletion requires topology support for bus or hidden ports\"");
        return false;
    }
    clear(selection);
    for (void* object : objects) append(selection,object);
    operationStage = "delete_native_selection";
    bool instrument=false;
    for (void* object : objects) {
        const char* type=runtimeName(object);
        if (type && !strcmp(type,"CIITSymbolComp") && isInstrumentSymbol(object)) instrument=true;
    }
    if (instrument) {
        typedef void* (__cdecl* UI)();
        typedef unsigned short (__thiscall* Command)(void*,unsigned short,const wchar_t*);
        UI uiGetter=(UI)symbol("PlugInManagement.dll","?GetUserInterfaceHandler@CUserInterfaceHandler@@SAPAV1@XZ");
        Getter frameGetter=(Getter)symbol("PlugInManagement.dll","?GetFrameWnd@CUserInterfaceHandler@@QAEPAVCFrameWnd@@XZ");
        Command command=(Command)symbol("PlugInManagement.dll","?GetCommandID@CUserInterfaceHandler@@QAEGGPB_W@Z");
        void* ui=uiGetter ? uiGetter() : nullptr;
        void* frame=ui && frameGetter ? frameGetter(ui) : nullptr;
        HWND window=readable(frame,0x24) ? *(HWND*)((char*)frame+0x20) : nullptr;
        unsigned short id=ui && command ? command(ui,21,L"EditDelete") : 0xffff;
        fprintf(currentOutput,",\"instrument_delete_command\":%u",id);
        DWORD owner=0;
        GetWindowThreadProcessId(window,&owner);
        if (owner!=GetCurrentProcessId() || !id || id==0xffff) { clear(selection); return false; }
        operationStage="dispatch_instrument_delete";
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,0),0);
    } else {
        remove(iitView, false, false);
    }
    clear(selection);
    return topology.repair();
}
static bool splitWire(void* diagram,void* a,void* b,const wchar_t* name,DiagramPoint point,int probeKind=-1) {
    if (!a || !b || a==b || !name || !*name || !wcsncmp(name,L"@junction:",10) || findEditable(diagram,utf8(name).c_str())) return false;
    void* wire=findWire(diagram,a,b);
    if (!wire) return false;
    DeletionTopology preflight;
    if (!preflight.capture(diagram,{wire})) return false;
    typedef void (__cdecl* Snap)(DiagramPoint*);
    Snap snap=(Snap)symbol("Od101dll.dll","?MovePointOnGrid@CODDiagram@@SAXAAVCPoint@@@Z");
    typedef void (__thiscall* Vertices)(void*,DiagramPoint**,int*);
    Vertices vertices=(Vertices)symbol("Od101dll.dll","?GetVertices@CODLinkComp@@QAEXAAPAVCPoint@@AAH@Z");
    typedef void (__cdecl* Free)(void*);
    Free release=(Free)symbol("MSVCR71.dll","??_V@YAXPAX@Z");
    typedef void* (__thiscall* Getter)(void*);
    Getter source=(Getter)symbol("Od101dll.dll","?GetSourceConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    Getter multiline=(Getter)symbol("Od101dll.dll","?GetMultiLine@CODLinkComp@@QAEPAVCODMultiLine@@XZ");
    typedef void (__thiscall* Points)(void*,DiagramPoint*,int);
    Points setter=(Points)symbol("Od101dll.dll","?SetNewPointsArray@CODVertices@@QAEXPAVCPoint@@H@Z");
    if (!snap || !vertices || !release || !source || !multiline || !setter) return false;
    snap(&point);
    DiagramPoint ignored={};
    if (source(wire)!=terminalConnector(a,&ignored)) std::swap(a,b);
    DiagramPoint* nativePoints=nullptr; int count=0;
    vertices(wire,&nativePoints,&count);
    if (!nativePoints || count<2 || count>65536) { if (nativePoints) release(nativePoints); return false; }
    std::vector<DiagramPoint> original(nativePoints,nativePoints+count);
    release(nativePoints);
    auto same=[](DiagramPoint p,DiagramPoint q){return p.x==q.x && p.y==q.y;};
    if (same(point,original.front()) || same(point,original.back())) return false;
    int segment=-1;
    for (int i=0;i<count-1;++i) {
        DiagramPoint p=original[i],q=original[i+1];
        if (same(p,q)) continue;
        long long cross=((long long)point.x-p.x)*((long long)q.y-p.y)-((long long)point.y-p.y)*((long long)q.x-p.x);
        if (!cross && point.x>=(std::min)(p.x,q.x) && point.x<=(std::max)(p.x,q.x) && point.y>=(std::min)(p.y,q.y) && point.y<=(std::max)(p.y,q.y)) { segment=i; break; }
    }
    if (segment<0) { fprintf(currentOutput,",\"error\":\"The snapped junction point must lie on the existing wire interior\""); return false; }
    std::vector<DiagramPoint> left(original.begin(),original.begin()+segment+1),right={point};
    if (!same(left.back(),point)) left.push_back(point);
    for (int i=segment+1;i<count;++i) if (!same(right.back(),original[i])) right.push_back(original[i]);
    if (left.size()<2 || right.size()<2) return false;
    void* junction=probeKind<0 ? makeJunction(diagram,name,point) : makeProbe(globalObject("?GetDrwSubSheet@@YAPAVCDrwSubSheet@@XZ"),diagram,name,(unsigned)probeKind,point);
    if (!junction) return false;
    void* first=makeWire(diagram,a,junction);
    void* second=first ? makeWire(diagram,junction,b) : nullptr;
    if (!first || !second) return false;
    setter(multiline(first),left.data(),(int)left.size());
    setter(multiline(second),right.data(),(int)right.size());
    if (probeKind>=0 && !setProbeDirection(junction,point,left[left.size()-2])) return false;
    if (!deleteNative(diagram,{wire})) return false;
    if (probeKind>=0) {
        void* object=findProbe(diagram,utf8(name).c_str());
        return object && bindProbe(object,terminalNode(object));
    }
    return findJunction(diagram,utf8(name).c_str())!=nullptr;
}
static bool rerouteIncident(void* diagram, void* component) {
    typedef void* (__thiscall* Getter)(void*);
    Getter sourceGetter = (Getter)symbol("Od101dll.dll", "?GetSourceConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    Getter targetGetter = (Getter)symbol("Od101dll.dll", "?GetTargetConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    Getter multiLineGetter = (Getter)symbol("Od101dll.dll", "?GetMultiLine@CODLinkComp@@QAEPAVCODMultiLine@@XZ");
    typedef void (__thiscall* Center)(void*, LONG*, LONG*);
    Center center = (Center)symbol("Od101dll.dll", "?GetCenter@CODConnector@@UAEXAAH0@Z");
    typedef void (__thiscall* PointsSetter)(void*, DiagramPoint*, int);
    PointsSetter pointsSetter = (PointsSetter)symbol("Od101dll.dll", "?SetNewPointsArray@CODVertices@@QAEXPAVCPoint@@H@Z");
    typedef void (__thiscall* ConnectorGetter)(void*, void**, void**);
    ConnectorGetter connectors = (ConnectorGetter)symbol("Od101dll.dll", "?GetConnectors@CODLinkComp@@QAEXAAPAVCODConnector@@0@Z");
    Getter parent = (Getter)symbol("Od101dll.dll", "?GetParent@CODComponent@@QAEPAV1@XZ");
    Getter graphicGetter = (Getter)symbol("Od101dll.dll", "?GetGraphic@CODComponent@@QAEPAVCODGraphic@@XZ");
    typedef void* (__thiscall* PinPoint)(void*, DiagramPoint*, int);
    PinPoint pinPoint = (PinPoint)symbol("Od101dll.dll", "?GetConnectorPoint@CIITPinSymbolComp@@QAEPAVCIITPinConnectorComp@@AAVCPoint@@H@Z");
    typedef void (__thiscall* Translate)(void*, float, float);
    Translate translate = (Translate)symbol("Od101dll.dll", "?Translate@CODGraphic@@UAEXMM@Z");
    typedef void (__thiscall* Endpoints)(void*, DiagramPoint*, DiagramPoint*);
    Endpoints endpoints = (Endpoints)symbol("Od101dll.dll", "?GetEndpoints@CODLinkComp@@QAEXAAVCPoint@@0@Z");
    if (!sourceGetter || !targetGetter || !multiLineGetter || !center || !pointsSetter || !connectors || !parent || !graphicGetter || !pinPoint || !translate) return false;
    struct PinLocation { void* connector; DiagramPoint point; };
    std::vector<PinLocation> locations;
    Getter first = (Getter)symbol("Od101dll.dll", "?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    typedef void* (__thiscall* Next)(void*, void**);
    Next next = (Next)symbol("Od101dll.dll", "?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    Getter firstPin = (Getter)symbol("Od101dll.dll", "?GetFirstPinPosition@CIITSymbolComp@@QAEPAU__POSITION@@XZ");
    Next nextPin = (Next)symbol("Od101dll.dll", "?GetNextPin@CIITSymbolComp@@QAEPAVCIITPinSymbolComp@@AAPAU__POSITION@@@Z");
    void* root = diagramRoot(diagram);
    void* position = first(root);
    for (unsigned i = 0; position && i < 16384; ++i) {
        void* item = next(root, &position);
        const char* klass = runtimeName(item);
        if (isScalarConnector(item)) {
            PinLocation location = {};
            location.connector = terminalConnector(item,&location.point);
            locations.push_back(location);
            continue;
        }
        if (!klass || (strcmp(klass, "CIITSymbolComp") && strcmp(klass, "CIITAnimationSymbolComp"))) continue;
        void* pins = firstPin(item);
        for (unsigned p = 0; pins && p < 256; ++p) {
            void* pin = nextPin(item, &pins);
            PinLocation location = {};
            location.connector = pinPoint(pin, &location.point, 0);
            locations.push_back(location);
        }
    }
    for (void* wire : incidentWires(diagram, component)) {
        void* source = sourceGetter(wire);
        void* target = targetGetter(wire);
        void* multiLine = multiLineGetter(wire);
        if (!source || !target || !multiLine) return false;
        DiagramPoint a = {}, b = {};
        center(source, &a.x, &a.y);
        center(target, &b.x, &b.y);
        for (const auto& location : locations) {
            if (location.connector == source) a = location.point;
            if (location.connector == target) b = location.point;
        }
        DiagramPoint oldA = {}, oldB = {};
        if (!endpoints) return false;
        endpoints(wire, &oldA, &oldB);
        std::vector<DiagramPoint> points={a, {a.x,a.y-120}, {b.x,a.y-120}, b};
        if (isProbe(source) || isProbe(target)) {
            // Both links at an inline probe must keep their local approach
            // directions. The generic U route points both links the same way.
            typedef void (__thiscall* Vertices)(void*,DiagramPoint**,int*);
            Vertices vertices=(Vertices)symbol("Od101dll.dll","?GetVertices@CODLinkComp@@QAEXAAPAVCPoint@@AAH@Z");
            typedef void (__cdecl* Free)(void*);
            Free release=(Free)symbol("MSVCR71.dll","??_V@YAXPAX@Z");
            DiagramPoint* old=nullptr; int count=0;
            if (!vertices || !release) return false;
            vertices(wire,&old,&count);
            if (!old || count<2 || count>65536) { if (old) release(old); return false; }
            points.assign(old,old+count);
            if (count>=3) {
                points.front()=a; points.back()=b;
                if (old[0].x==old[1].x) points[1].x=a.x;
                else points[1].y=a.y;
                if (old[count-1].x==old[count-2].x) points[count-2].x=b.x;
                else points[count-2].y=b.y;
            } else {
                long dx=old[1].x-old[0].x,dy=old[1].y-old[0].y;
                if (std::abs(dx)>std::abs(dy)) {
                    if (a.y==b.y && (long long)(b.x-a.x)*dx>0) points={a,b};
                    else {
                        long step=dx>0 ? 9 : -9; long middle=(a.y+b.y)/2;
                        if (middle==a.y || middle==b.y) middle=a.y-18;
                        points={a,{a.x+step,a.y},{a.x+step,middle},{b.x-step,middle},{b.x-step,b.y},b};
                    }
                } else {
                    if (a.x==b.x && (long long)(b.y-a.y)*dy>0) points={a,b};
                    else {
                        long step=dy>0 ? 9 : -9; long middle=(a.x+b.x)/2;
                        if (middle==a.x || middle==b.x) middle=a.x-18;
                        points={a,{a.x,a.y+step},{middle,a.y+step},{middle,b.y-step},{b.x,b.y-step},b};
                    }
                }
            }
            release(old);
        }
        pointsSetter(multiLine,points.data(),(int)points.size());
        void* start = nullptr; void* end = nullptr;
        connectors(wire, &start, &end);
        if (!start || !end) return false;
        translate(graphicGetter(start), (float)(a.x - oldA.x), (float)(a.y - oldA.y));
        translate(graphicGetter(end), (float)(b.x - oldB.x), (float)(b.y - oldB.y));
    }
    return true;
}
static void* currentCircuit(void* sheet) {
    typedef void* (__thiscall* Getter)(void*);
    Getter userData = (Getter)symbol("Od101dll.dll", "?GetUserData@CODComponent@@QAEPAVCObject@@XZ");
    void* root = diagramRoot(currentDiagram(sheet));
    void* rootData = root && userData ? userData(root) : nullptr;
    const char* rootType = runtimeName(rootData);
    if (rootType && !strcmp(rootType, "CiCircuit")) return rootData;
    void* owners[2] = {sheet, currentDiagram(sheet)};
    for (void* owner : owners) {
        void* klass = runtimeClass(owner);
        unsigned bytes = klass ? *(unsigned*)((char*)klass + 4) : 0;
        for (unsigned i = 4; i + 4 <= bytes && i < 0x1000; i += 4) {
            void* item = *(void**)((char*)owner + i);
            const char* name = runtimeName(item);
            if (name && !strcmp(name, "CiCircuit")) return item;
        }
    }
    return nullptr;
}
static int prepareComponent(void* sheet, void* component, bool initialize) {
    // Native Add's property dispatch (SCH_CAP 10.0.1.0) selects the proper
    // Setattr handler, calls InitComponent, and prepares displayed values.
    // Its only call site is SCH_CAP+0x2d0b9, with six cdecl arguments.
    auto code = (unsigned char*)GetModuleHandleA("SCH_CAP.dll") + 0x174290;
    // The handler address at bytes 3..6 is relocated by the PE loader.
    if (!executable(code) || code[0] != 0x6a || code[1] != 0xff || code[2] != 0x68 || code[7] != 0x64 || code[8] != 0xa1) return -1;
    void* diagram = currentDiagram(sheet);
    void* klass = runtimeClass(diagram);
    unsigned bytes = klass ? *(unsigned*)((char*)klass + 4) : 0;
    void* prefs = nullptr;
    for (unsigned offset = 4; offset + 4 <= bytes && offset < 0x1000; offset += 4) {
        void* candidate = *(void**)((char*)diagram + offset);
        const char* name = runtimeName(candidate);
        if (name && !strcmp(name, "CIITCircuitPrefs")) { prefs = candidate; break; }
    }
    typedef void* (__thiscall* Getter)(void*);
    Getter treeGetter = (Getter)symbol("DrwEngine.dll", "?GetTreeInstance@CBaseView@@QBEPAVCTreeInstance@@XZ");
    void* baseView = globalObject("?GetSheetView@@YAPAVCBaseView@@XZ");
    void* tree = baseView && treeGetter ? treeGetter(baseView) : nullptr;
    if (!prefs || !tree) return -1;
    typedef int (__cdecl* Prepare)(void*, void*, bool, void*, void**, int*);
    void* mouseInterface = nullptr;
    int mouseType = 0;
    operationStage = initialize ? "initialize_component_properties" : "refresh_component_properties";
    return ((Prepare)code)(component, prefs, initialize, tree, &mouseInterface, &mouseType);
}
static int synchronizeModel(void* sheet) {
    typedef void* (__thiscall* ExtensionGetter)(void*, void*);
    ExtensionGetter getter = (ExtensionGetter)symbol("DrwEngine.dll", "?GetExtension@CProject@@QAEPAVCProjectExtension@@PAUCRuntimeClass@@@Z");
    void* klass = (void*)symbol("SCH_CAP.dll", "?classCMultisimExtension@CMultisimExtension@@2UCRuntimeClass@@A");
    void* project = currentProject(sheet);
    void* extension = project && getter && klass ? getter(project, klass) : nullptr;
    typedef int (__thiscall* Sync)(void*, bool);
    Sync sync = (Sync)symbol("SCH_CAP.dll", "?SyncRefDesManager@CMultisimExtension@@QAEH_N@Z");
    if (!extension || !sync) return -1;
    int status = sync(extension, true);
    typedef void* (__thiscall* IndexGetter)(void*, unsigned);
    IndexGetter managerGetter = (IndexGetter)symbol("SCH_CAP.dll", "?GetRefDesMgr@CMultisimExtension@@QAEPAVCIITHierRefDesManager@@I@Z");
    typedef unsigned (__thiscall* CountGetter)(void*);
    CountGetter managerCount = (CountGetter)symbol("SCH_CAP.dll", "?GetRefDesManagerCount@CMultisimExtension@@QAEIXZ");
    typedef void* (__thiscall* Getter)(void*);
    Getter treeGetter = (Getter)symbol("DrwEngine.dll", "?GetTreeInstance@CBaseView@@QBEPAVCTreeInstance@@XZ");
    void* baseView = globalObject("?GetSheetView@@YAPAVCBaseView@@XZ");
    void* tree = baseView && treeGetter ? treeGetter(baseView) : nullptr;
    syncTree = tree;
    typedef void* (__thiscall* InfoGetter)(void*, void*, const wchar_t*);
    InfoGetter infoGetter = (InfoGetter)symbol("SCH_CAP.dll", "?GetInstanceRefDesInfo@CIITHierRefDesManager@@QAEPAVCIITHierRefDesInfo@@PAVCTreeInstance@@PB_W@Z");
    typedef void (__thiscall* SectionSetter)(void*, int);
    SectionSetter sectionSetter = (SectionSetter)symbol("SCH_CAP.dll", "?SetIRSectionID@CIITHierRefDesInfo@@QAEXH@Z");
    typedef int (__thiscall* SectionGetter)(void*);
    SectionGetter sectionGetter = (SectionGetter)symbol("SCH_CAP.dll", "?GetIRSectionID@CIITHierRefDesInfo@@QAEHXZ");
    Getter first = (Getter)symbol("Od101dll.dll", "?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    typedef void* (__thiscall* Next)(void*, void**);
    Next next = (Next)symbol("Od101dll.dll", "?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    void* root = diagramRoot(currentDiagram(sheet));
    void* position = root && first ? first(root) : nullptr;
    unsigned count = managerCount ? managerCount(extension) : 0;
    syncManagers = count; syncMatches = 0;
    for (unsigned i = 0; tree && position && next && i < 16384; ++i) {
        void* item = next(root, &position);
        const char* type = runtimeName(item);
        if (!type || (strcmp(type, "CIITSymbolComp") && strcmp(type, "CIITAnimationSymbolComp"))) continue;
        void* data = electricalData(item);
        if (!data) continue;
        std::string name = valueString(data, "?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ", "Circexp.dll");
        int length = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
        std::wstring wide(length, L'\0');
        if (length) MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wide[0], length);
        for (unsigned m = 0; managerGetter && infoGetter && sectionGetter && sectionSetter && m < count && m < 256; ++m) {
            void* manager = managerGetter(extension, m);
            void* info = infoGetter(manager, tree, wide.c_str());
            if (info) ++syncMatches;
        }
    }
    return status;
}
#include "native_nets.h"
#include "native_probes.h"
#include "native_instruments.h"

static bool describeBus(FILE* file,void* bus) {
    typedef void (__thiscall* Vertices)(void*,DiagramPoint**,int*);
    Vertices get=(Vertices)symbol("Od101dll.dll","?GetVertices@CODVerticesComp@@UAEXAAPAVCPoint@@AAH@Z");
    DiagramPoint* points=nullptr;
    int count=0;
    if (get) get(bus,&points,&count);
    void* label=*(void**)((char*)bus+0x38);
    fprintf(file,",\"name\":");
    jsonString(file,label ? valueString(label,"?GetText@CODTextComp@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ") : "");
    fprintf(file,",\"vertices\":[");
    bool valid=count>=2 && count<=16384 && readable(points,count*sizeof(DiagramPoint));
    if (valid) {
        for (int i=0;i<count;++i)
            fprintf(file,"%s[%ld,%ld]",i ? "," : "",points[i].x,points[i].y);
    }
    fprintf(file,"]");
    return valid && label;
}

static bool placeBus(FILE* file,void* diagram,void* view,const wchar_t* name,std::vector<DiagramPoint>& points) {
    typedef void* (__cdecl* Create)();
    typedef void* (__cdecl* Allocate)(unsigned);
    typedef void* (__thiscall* Line)(void*,DiagramPoint*,int,void*);
    typedef void (__thiscall* Graphic)(void*,void*);
    typedef void (__thiscall* InitializeEnds)(void*);
    typedef void* (__thiscall* Label)(void*,const wchar_t*);
    typedef void (__thiscall* Add)(void*,void*,DiagramPoint,void*);
    auto base=(unsigned char*)GetModuleHandleA("SCH_CAP.dll");
    if (!base || !executable(base+0xb76c0) || !executable(base+0xb0740) || !executable(base+0xa10b0) ||
        base[0xb76c0]!=0x6a || base[0xb0740]!=0x64 || !readable(base+0x3269dc,4)) return false;
    Allocate allocate=(Allocate)symbol("MSVCR71.dll","??2@YAPAXI@Z");
    Line line=(Line)symbol("Od101dll.dll","??0CODMultiLine@@QAE@PAVCPoint@@HPAVCODGraphic@@@Z");
    Graphic setGraphic=(Graphic)symbol("Od101dll.dll","?SetGraphic@CODComponent@@UAEXPAVCODGraphic@@@Z");
    Graphic setTransformer=(Graphic)symbol("Od101dll.dll","?SetTransformer@CODGraphic@@QAEXPAVCODTransformer@@@Z");
    Add add=(Add)dynamicMethod(diagram,"??_7CODDiagram@@6B@","?AddComponent@CODDiagram@@UAEXPAVCODComponent@@VCPoint@@PAVCODView@@@Z");
    if (!allocate || !line || !setGraphic || !setTransformer || !add) return false;
    operationStage="create_native_bus";
    void* bus=((Create)(base+0xb76c0))();
    const char* type=runtimeName(bus);
    if (!type || strcmp(type,"CIITBusCompV8")) return false;
    void* storage=allocate(0x30);
    if (!storage) return false;
    void* graphic=line(storage,points.data(),(int)points.size(),nullptr);
    if (!graphic) return false;
    // Native creation switches CODMultiLine to its bus graphic vtable.
    *(void**)graphic=base+0x3269dc;
    setGraphic(bus,graphic);
    setTransformer(graphic,nullptr);
    ((InitializeEnds)(base+0xb0740))(bus);
    operationStage="create_native_bus_label";
    void* labelStorage=allocate(0x2c);
    if (!labelStorage) return false;
    void* label=((Label)(base+0xa10b0))(labelStorage,name);
    if (!label) return false;
    *(void**)((char*)bus+0x38)=label;
    *(void**)((char*)label+0x28)=bus;
    DiagramPoint origin=points.front();
    for (const auto& point : points) {
        origin.x=(std::min)(origin.x,point.x);
        origin.y=(std::min)(origin.y,point.y);
    }
    operationStage="adopt_native_bus";
    add(diagram,bus,origin,view);
    DiagramPoint caption={points.front().x+18,points.front().y+6};
    add(diagram,label,caption,view);
    return describeBus(file,bus);
}
static void inventoryRoot(FILE* file, void* root) {
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*, void**);
    Getter first = (Getter)symbol("Od101dll.dll", "?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    Next next = (Next)symbol("Od101dll.dll", "?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    Getter firstPin = (Getter)symbol("Od101dll.dll", "?GetFirstPinPosition@CIITSymbolComp@@QAEPAU__POSITION@@XZ");
    Next nextPin = (Next)symbol("Od101dll.dll", "?GetNextPin@CIITSymbolComp@@QAEPAVCIITPinSymbolComp@@AAPAU__POSITION@@@Z");
    if (!root || !first || !next) return;
    void* position = first(root);
    unsigned index = 0;
    while (position && index++ < 16384) {
        void* object = next(root, &position);
        const char* name = runtimeName(object);
        fprintf(file, ",{\"label\":\"model_object\",\"class\":\"%s\",\"address\":\"%p\"", name ? name : "unknown", object);
        if (name && (!strcmp(name, "CIITSymbolComp") || !strcmp(name, "CIITAnimationSymbolComp"))) {
            describeInstrument(file,object);
            fprintf(file, ",\"diagnostic_fields\":[");
            for (int offset = 0x40; offset <= 0xe0; offset += 4)
                fprintf(file, "%s%u", offset == 0x40 ? "" : ",", *(unsigned*)((char*)object + offset));
            fprintf(file, "]");
            fprintf(file, ",\"refdes\":");
            // This exported override expects the IRefDesable subobject (+0x40),
            // as confirmed against this build's native implementation.
            jsonString(file, referenceString((char*)object + 0x40, "?GetMultisectionInstanceRefDes@CIITSymbolComp@@UAEAAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ"));
            fprintf(file, ",\"basic_variants\":");
            jsonString(file, valueString(object, "?GetBasicVariantsString@CIITSymbolComp@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ"));
            typedef void* (__thiscall* ActiveVariant)(void*, void*, void*);
            ActiveVariant activeVariant = (ActiveVariant)symbol("SCH_CAP.dll", "?GetActiveVariant@CSCH_CAPDoc@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@PAVCTreeInstance@@@Z");
            Getter treeGetter = (Getter)symbol("DrwEngine.dll", "?GetTreeInstance@CBaseView@@QBEPAVCTreeInstance@@XZ");
            void* baseView = globalObject("?GetSheetView@@YAPAVCBaseView@@XZ");
            void* tree = baseView && treeGetter ? treeGetter(baseView) : nullptr;
            void* sheet = globalObject("?GetDrwSubSheet@@YAPAVCDrwSubSheet@@XZ");
            NativeString active;
            if (activeVariant && tree) activeVariant(currentDocument(sheet), &active, tree);
            fprintf(file, ",\"active_variant\":"); jsonString(file, utf8((const wchar_t*)active.data));
            typedef bool (__thiscall* Exists)(void*, void*, void*);
            Exists exists = (Exists)symbol("Od101dll.dll", "?IsComponentExistInVariant@CIITSymbolComp@@QAE_NV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@PAVCVariantData@@@Z");
            if (exists && active.data) {
                typedef void* (__thiscall* Constructor)(void*, const wchar_t*);
                Constructor ctor = (Constructor)GetProcAddress(GetModuleHandleA("MFC71U.DLL"), MAKEINTRESOURCEA(283));
                void* arg = nullptr; ctor(&arg, (const wchar_t*)active.data);
                fprintf(file, ",\"exists_in_variant\":%s", exists(object, arg, nullptr) ? "true" : "false");
            }
            fprintf(file, ",\"local_name\":");
            jsonString(file, referenceString(object, "?GetOrigLocalName@CIITSymbolComp@@UBEABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ"));
            typedef void* (__cdecl* DataGetter)(void*);
            DataGetter dataGetter = (DataGetter)symbol("SCH_CAP.dll", "?GetSymbolComponentData@CComponentBridge@@SAPAVCiComponent@@PAVCODComponent@@@Z");
            void* data = dataGetter ? dataGetter(object) : nullptr;
            Getter graphicBackref = (Getter)symbol("Circexp.dll", "?GetODSymbolComponent@CiComponent@@QBEPAVCObject@@XZ");
            fprintf(file, ",\"electrical_data\":\"%p\",\"graphic_backref\":\"%p\",\"electrical_name\":", data, data && graphicBackref ? graphicBackref(data) : nullptr);
            jsonString(file, data ? valueString(data, "?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ", "Circexp.dll") : "");
            Getter modelGetter = (Getter)symbol("Circexp.dll", "?GetModel@CiElement@@QBEPAVCiModel@@XZ");
            void* model = data && modelGetter ? modelGetter(data) : nullptr;
            fprintf(file, ",\"electrical_model\":\"%p\",\"spice_template\":", model);
            typedef void* (__cdecl* TemplateGetter)(void*, void*);
            TemplateGetter templateGetter = (TemplateGetter)symbol("SCH_CAP.dll", "?GetSpiceTemplate@CComponentBridge@@SA?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@PAVCODSymbolComp@@@Z");
            NativeString spiceTemplate;
            if (templateGetter) templateGetter(&spiceTemplate, object);
            jsonString(file, utf8((const wchar_t*)spiceTemplate.data));
            typedef int (__thiscall* CountGetter)(void*);
            CountGetter attributeCount = (CountGetter)symbol("Circexp.dll", "?GetAttributeCount@CiElement@@QBEHXZ");
            fprintf(file, ",\"attribute_count\":%d", data && attributeCount ? attributeCount(data) : -1);
            typedef void* (__thiscall* AttributeGetter)(void*, int);
            AttributeGetter attributeGetter = (AttributeGetter)symbol("Circexp.dll", "?GetAttribute@CiElement@@QBEPAVCiAttribute@@H@Z");
            void* parameterList = data && attributeGetter ? attributeGetter(data, 2) : nullptr;
            CountGetter parameterCount = (CountGetter)symbol("Circexp.dll", "?GetParamCount@CiaParamList@@QBEHXZ");
            typedef double (__thiscall* DoubleGetter)(void*, int);
            DoubleGetter doubleGetter = (DoubleGetter)symbol("Circexp.dll", "?GetParam@CiaParamList@@QBENH@Z");
            int parameters = parameterList && parameterCount ? parameterCount(parameterList) : 0;
            fprintf(file, ",\"double_parameters\":[");
            for (int p = 0; p < parameters && p < 256; ++p) fprintf(file, "%s%.17g", p ? "," : "", doubleGetter ? doubleGetter(parameterList, p) : 0);
            fprintf(file, "]");
            fprintf(file, ",\"attributes\":[");
            unsigned attributesListed = 0;
            for (int p = 0; data && attributeGetter && attributeCount && p < attributeCount(data) && p < 256; ++p) {
                void* attribute = attributeGetter(data, p);
                const char* attrClass = runtimeName(attribute);
                if (!attribute) continue;
                fprintf(file, "%s{\"index\":%d,\"class\":\"%s\"", attributesListed++ ? "," : "", p, attrClass ? attrClass : "unknown");
                if (attrClass && !strcmp(attrClass, "CiaDOUBLE")) {
                    typedef double (__thiscall* NumberGetter)(void*);
                    NumberGetter number = (NumberGetter)symbol("Circexp.dll", "?GetDOUBLE@CiaDOUBLE@@UBENXZ");
                    fprintf(file, ",\"number\":%.17g", number ? number(attribute) : 0);
                } else if (attrClass && (!strcmp(attrClass, "CiaCString") || !strcmp(attrClass, "CiaSpiceTmpltExprt"))) {
                    fprintf(file, ",\"string\":");
                    jsonString(file, referenceString(attribute, "?GetString@CiaCString@@UBEABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ", "Circexp.dll"));
                } else if (attrClass && !strcmp(attrClass, "CiaCollString")) {
                    CountGetter count = (CountGetter)symbol("Circexp.dll", "?GetParamCount@CiaCollString@@QBEHXZ");
                    typedef void* (__thiscall* StringGetter)(void*, void*, int);
                    StringGetter getString = (StringGetter)symbol("Circexp.dll", "?GetParam@CiaCollString@@QBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@H@Z");
                    fprintf(file, ",\"strings\":[");
                    int n = count ? count(attribute) : 0;
                    for (int s = 0; getString && s < n && s < 256; ++s) {
                        NativeString text;
                        getString(attribute, &text, s);
                        if (s) fputc(',', file);
                        jsonString(file, utf8((const wchar_t*)text.data));
                    }
                    fprintf(file, "]");
                }
                fprintf(file, "}");
            }
            fprintf(file, "]");
            typedef void* (__thiscall* BoxGetter)(void*, RECT*, void*);
            BoxGetter boxGetter = (BoxGetter)symbol("Od101dll.dll", "?GetBox@CIITSymbolComp@@UAE?AVCRect@@AAUODInstContext@@@Z");
            RECT box = {};
            void* context = (void*)symbol("Od101dll.dll", "?DefIC@@3UODInstContext@@A");
            if (boxGetter && context) boxGetter(object, &box, context);
            typedef unsigned char (__thiscall* RotationGetter)(void*);
            RotationGetter rotationGetter = (RotationGetter)symbol("Od101dll.dll", "?GetRotation@CIITSymbolComp@@QBEEXZ");
            RotationGetter flipGetter = (RotationGetter)symbol("Od101dll.dll", "?GetFlip@CIITSymbolComp@@QBEEXZ");
            fprintf(file, ",\"bounds\":[%ld,%ld,%ld,%ld],\"rotation_native\":%u", box.left, box.top, box.right, box.bottom, rotationGetter ? rotationGetter(object) : 0);
            if (flipGetter) fprintf(file, ",\"flip_native\":%u", flipGetter(object));
            fprintf(file, ",\"pins\":[");
            bool instrument=isInstrumentSymbol(object);
            void* pinPosition = firstPin ? firstPin(object) : nullptr;
            unsigned pinIndex = 0;
            while (pinPosition && nextPin && pinIndex < 256) {
                void* pin = nextPin(object, &pinPosition);
                std::string number=valueString(pin, "?GetPinNumber@CIITPinSymbolComp@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ");
                std::string pinName=valueString(pin, "?GetPinName@CIITPinSymbolComp@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ");
                fprintf(file, "%s{\"number\":", pinIndex++ ? "," : "");
                jsonString(file, number);
                fprintf(file, ",\"name\":");
                jsonString(file, pinName);
                fprintf(file, ",\"terminal\":");
                jsonString(file, instrument ? pinName : number);
                typedef void* (__thiscall* PinPoint)(void*, DiagramPoint*, int);
                PinPoint pointGetter = (PinPoint)symbol("Od101dll.dll", "?GetConnectorPoint@CIITPinSymbolComp@@QAEPAVCIITPinConnectorComp@@AAVCPoint@@H@Z");
                DiagramPoint point = {};
                void* connector = pointGetter ? pointGetter(pin, &point, 0) : nullptr;
                void* node = pinNode(pin);
                fprintf(file, ",\"node\":\"%p\",\"node_name\":", node);
                jsonString(file, node ? valueString(node, "?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ", "Circexp.dll") : "");
                fprintf(file, ",\"connector\":\"%p\",\"point\":[%ld,%ld]}", connector, point.x, point.y);
            }
            fprintf(file, "]");
        } else if (isProbe(object)) {
            describeProbe(file,object);
        } else if (name && !strcmp(name,"CODNodeTextComp")) {
            fprintf(file,",\"text\":");
            std::string displayed=valueString(object,"?GetText@CODTextComp@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ");
            jsonString(file,displayed);
            typedef void* (__thiscall* Box)(void*,RECT*,void*);
            Box box=(Box)dynamicMethod(object,"??_7CODComponent@@6B@","?GetBox@CODComponent@@UAE?AVCRect@@AAUODInstContext@@@Z");
            void* context=(void*)symbol("Od101dll.dll","?DefIC@@3UODInstContext@@A");
            RECT bounds={}; if (box && context) box(object,&bounds,context);
            typedef bool (__thiscall* Visible)(void*,void*);
            Visible visible=(Visible)dynamicMethod(object,"??_7CODComponent@@6B@","?IsVisible@CODComponent@@UAE_NPAVCODView@@@Z");
            fprintf(file,",\"bounds\":[%ld,%ld,%ld,%ld],\"visible\":%s",bounds.left,bounds.top,bounds.right,bounds.bottom,!displayed.empty() && visible && visible(object,nativeView(".?AVCODView@@")) ? "true" : "false");
        } else if (isJunction(object)) {
            DiagramPoint point = {};
            terminalConnector(object,&point);
            void* node = terminalNode(object);
            fprintf(file, ",\"ref\":"); jsonString(file,junctionReference(object));
            fprintf(file, ",\"text\":"); jsonString(file,junctionName(object));
            fprintf(file, ",\"point\":[%ld,%ld],\"connector\":\"%p\",\"node\":\"%p\",\"node_name\":", point.x,point.y,object,node);
            jsonString(file,node ? valueString(node,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll") : "");
            fprintf(file, ",\"vtable_rva\":%lu",(unsigned long)((char*)*(void**)object-(char*)GetModuleHandleA("Od101dll.dll")));
        } else if (name && !strcmp(name, "CIITBusCompV8")) {
            describeBus(file,object);
        } else if (name && !strcmp(name, "CIITLinkComp")) {
            Getter nodeGetter = (Getter)symbol("SCH_CAP.dll", "?GetNode@CIITLinkComp@@QAEPAVCiNode@@XZ");
            void* node=nodeGetter ? nodeGetter(object) : nullptr;
            fprintf(file, ",\"node\":\"%p\",\"node_name\":",node);
            jsonString(file,node ? valueString(node,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll") : "");
            fprintf(file,",\"caption\":"); jsonString(file,netCaption(node));
            Getter nodeText=(Getter)symbol("SCH_CAP.dll","?GetNodeText@CIITLinkComp@@UAEPAVCODNodeTextComp@@XZ");
            fprintf(file,",\"node_text\":\"%p\"",nodeText ? nodeText(object) : nullptr);
            Getter sourceGetter = (Getter)symbol("Od101dll.dll", "?GetSourceConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
            Getter targetGetter = (Getter)symbol("Od101dll.dll", "?GetTargetConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
            fprintf(file, ",\"source_connector\":\"%p\",\"target_connector\":\"%p\"", sourceGetter ? sourceGetter(object) : nullptr, targetGetter ? targetGetter(object) : nullptr);
            typedef void (__thiscall* Endpoints)(void*, DiagramPoint*, DiagramPoint*);
            Endpoints endpoints = (Endpoints)symbol("Od101dll.dll", "?GetEndpoints@CODLinkComp@@QAEXAAVCPoint@@0@Z");
            DiagramPoint a = {}, b = {};
            if (endpoints) endpoints(object, &a, &b);
            fprintf(file, ",\"endpoints\":[[%ld,%ld],[%ld,%ld]]", a.x, a.y, b.x, b.y);
            typedef void (__thiscall* Vertices)(void*, DiagramPoint**, int*);
            Vertices vertices = (Vertices)symbol("Od101dll.dll", "?GetVertices@CODLinkComp@@QAEXAAPAVCPoint@@AAH@Z");
            typedef void (__cdecl* Free)(void*);
            Free release = (Free)symbol("MSVCR71.dll", "??_V@YAXPAX@Z");
            DiagramPoint* points = nullptr; int count = 0;
            if (vertices && release) vertices(object, &points, &count);
            fprintf(file, ",\"vertices\":[");
            for (int p=0; points && p<count && p<65536; ++p)
                fprintf(file, "%s[%ld,%ld]", p ? "," : "", points[p].x, points[p].y);
            fprintf(file, "]");
            if (points && release) release(points);
        }
        fprintf(file, "}");
    }
}
static void describe(FILE* file, const char* label, void* object, bool comma) {
    const char* name = runtimeName(object);
    HMODULE classModule = nullptr;
    void* metadata = runtimeClass(object);
    if (metadata) GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)metadata, &classModule);
    wchar_t modulePath[MAX_PATH] = {};
    if (classModule) GetModuleFileNameW(classModule, modulePath, MAX_PATH);
    wchar_t* moduleName = wcsrchr(modulePath, L'\\');
    fprintf(file, "%s{\"label\":\"%s\",\"address\":\"%p\",\"class\":\"%s\",\"class_module\":", comma ? "," : "", label, object, name ? name : "unknown");
    jsonString(file, utf8(moduleName ? moduleName + 1 : modulePath));
    fprintf(file, ",\"fields\":[");
    bool first = true;
    void* klass = runtimeClass(object);
    unsigned objectBytes = klass ? *(unsigned*)((char*)klass + 4) : 0;
    if (objectBytes > 0x400) objectBytes = 0x400;
    if (name) for (unsigned offset = 4; offset + 4 <= objectBytes; offset += 4) {
        if (!readable((char*)object + offset, 4)) break;
        void* candidate = *(void**)((char*)object + offset);
        const char* candidateName = runtimeName(candidate);
        if (candidateName) {
            fprintf(file, "%s{\"offset\":%u,\"address\":\"%p\",\"class\":\"%s\"}", first ? "" : ",", offset, candidate, candidateName);
            first = false;
        }
    }
    fprintf(file, "]}");
}
static void describeDiagram(FILE* file, void* candidate) {
    typedef void* (__thiscall* Getter)(void*);
    Getter diagramGetter = (Getter)symbol("SCH_CAP.dll", "?GetDiagram@CSCH_CAPDoc@@QBEPAVCODDiagram@@XZ");
    if (!candidate || !diagramGetter || !runtimeName(candidate)) return;
    void* diagram = diagramGetter(candidate);
    describe(file, "diagram", diagram, true);
    Getter rootGetter = (Getter)symbol("Od101dll.dll", "?GetComponent@CODDiagram@@QBEPAVCODComponent@@XZ");
    if (diagram && rootGetter) {
        void* root = rootGetter(diagram);
        describe(file, "diagram_root", root, true);
        Getter dataGetter = (Getter)symbol("Od101dll.dll", "?GetUserData@CODComponent@@QAEPAVCObject@@XZ");
        void* circuit = dataGetter ? dataGetter(root) : nullptr;
        describe(file, "root_user_data", circuit, true);
        const char* dataClass = runtimeName(circuit);
        if (dataClass && !strcmp(dataClass, "CiCircuit")) {
            typedef void* (__thiscall* Constructor)(void*, void*);
            Constructor construct = (Constructor)symbol("Circexp.dll", "??0CiIterCompsInCirc@@QAE@PBVCiCircuit@@@Z");
            Getter first = (Getter)symbol("Circexp.dll", "?Reset@CiIterCompsInCirc@@QAEPAVCiComponent@@XZ");
            Getter next = (Getter)symbol("Circexp.dll", "?GetNext@CiIterCompsInCirc@@QAEPAVCiComponent@@XZ");
            __declspec(align(8)) unsigned char iterator[128] = {};
            if (construct && first && next) {
                construct(iterator, circuit);
                void* data = first(iterator);
                for (unsigned i = 0; data && i < 16384; ++i) {
                    fprintf(file, ",{\"label\":\"electrical_component\",\"address\":\"%p\",\"name\":", data);
                    jsonString(file, valueString(data, "?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ", "Circexp.dll"));
                    fprintf(file, "}");
                    data = next(iterator);
                }
            }
        }
        inventoryRoot(file, root);
    }
}
static void snapshot() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(selfModule, path, MAX_PATH);
    wchar_t* separator = wcsrchr(path, L'\\');
    if (!separator) return;
    wcscpy_s(separator + 1, MAX_PATH - (separator + 1 - path), L"native-snapshot.json");
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") || !file) return;
    currentOutput = file;
    void** baseTable = (void**)symbol("DrwEngine.dll", "??_7CDrwSubSheet@@6B@");
    void* getClass = (void*)symbol("DrwEngine.dll", "?GetRuntimeClass@CDrwSubSheet@@UBEPAUCRuntimeClass@@XZ");
    if (baseTable && getClass) for (int slot = 0; slot < 12; ++slot) if (baseTable[slot] == getClass) runtimeSlot = slot;
    typedef void* (__thiscall* SelectionGetter)(void*);
    SelectionGetter getSelection = (SelectionGetter)symbol("Od101dll.dll", "?GetSelection@CODView@@QAEPAVCODSelection@@XZ");
    void* editorView = nativeView(".?AVCODView@@");
    void* selection = editorView && getSelection ? getSelection(editorView) : nullptr;
    fprintf(file, "{\"pid\":%lu,\"thread\":%lu,\"runtime_slot\":%d,\"editor_ready\":%s,\"objects\":[", GetCurrentProcessId(), GetCurrentThreadId(), runtimeSlot, selection ? "true" : "false");
    void* sheet = globalObject("?GetDrwSubSheet@@YAPAVCDrwSubSheet@@XZ");
    void* view = globalObject("?GetSheetView@@YAPAVCBaseView@@XZ");
    void* workspace = globalObject("?GetWorkSpaceContainer@@YAPAVCWorkspaceContainer@@XZ");
    describe(file, "sheet", sheet, false);
    describe(file, "view", view, true);
    describe(file, "workspace", workspace, true);
    if (sheet && runtimeClass(sheet)) {
        unsigned bytes = *(unsigned*)((char*)runtimeClass(sheet) + 4);
        for (unsigned offset = 4; offset + 4 <= bytes && offset < 0x400; offset += 4) {
            void* candidate = *(void**)((char*)sheet + offset);
            const char* name = runtimeName(candidate);
            if (name && !strcmp(name, "CIITHierDoc")) {
                describe(file, "native_document", candidate, true);
                describeDiagram(file, candidate);
                break;
            }
        }
    }
    typedef void* (__thiscall* Getter)(void*);
    Getter contentsGetter = (Getter)symbol("DrwEngine.dll", "?GetContentsInfo@CBaseView@@UAEPAVCBaseContentsInfo@@XZ");
    if (view && contentsGetter) describe(file, "view_contents", contentsGetter(view), true);
    typedef void (__thiscall* AllProjects)(void*, void*);
    AllProjects allProjects = (AllProjects)symbol("DrwEngine.dll", "?GetAllProjects@CWorkspaceContainer@@QBEXAAV?$vector@PAVCProject@@V?$allocator@PAVCProject@@@std@@@std@@@Z");
    void* vectorData[8] = {};
    if (workspace && allProjects) allProjects(workspace, vectorData);
    for (int pair = 0; pair < 4; ++pair) {
        void** begin = (void**)vectorData[pair];
        void** end = (void**)vectorData[pair + 1];
        if (!begin || end <= begin || end - begin > 16 || !readable(begin, (end - begin) * 4)) continue;
        for (void** current = begin; current < end; ++current) {
            const char* name = runtimeName(*current);
            if (!name) continue;
            describe(file, "project_candidate", *current, true);
            Getter subSheet = (Getter)symbol("DrwEngine.dll", "?GetSubSheet@CProject@@QAEPAVCDrwSubSheet@@XZ");
            if (!strcmp(name, "CProject") && subSheet) describe(file, "project_sheet", subSheet(*current), true);
            AllProjects allTrees = (AllProjects)symbol("DrwEngine.dll", "?GetAllTreeInstances@CProject@@QAEXAAV?$vector@PAVCTreeInstance@@V?$allocator@PAVCTreeInstance@@@std@@@std@@@Z");
            void* trees[8] = {};
            if (allTrees) allTrees(*current, trees);
            typedef void* (__thiscall* TreeSheet)(void*, bool);
            TreeSheet treeSheet = (TreeSheet)symbol("DrwEngine.dll", "?GetSubSheet@CTreeInstance@@QAEPAVCDrwSubSheet@@_N@Z");
            for (int treePair = 0; treePair < 4; ++treePair) {
                void** start = (void**)trees[treePair];
                void** finish = (void**)trees[treePair + 1];
                if (!start || finish <= start || finish - start > 64 || !readable(start, (finish - start) * 4)) continue;
                for (void** tree = start; tree < finish; ++tree) {
                    describe(file, "tree", *tree, true);
                    if (treeSheet) describe(file, "tree_sheet", treeSheet(*tree, false), true);
                }
                break;
            }
        }
        break;
    }
    fprintf(file, "],\"project_vector\":[");
    for (int i = 0; i < 8; ++i) fprintf(file, "%s\"%p\"", i ? "," : "", vectorData[i]);
    fprintf(file, "]}\n");
    fclose(file);
    currentOutput = nullptr;
}
static void factoryOperation(unsigned action) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, siblingPath(L"native-operation.json").c_str(), L"wb") || !file) return;
    currentOutput = file;
    fprintf(file, "{\"pid\":%lu,\"action\":%u,\"attempts\":[", GetCurrentProcessId(), action);
    typedef void* (__cdecl* HandlerGetter)();
    HandlerGetter getHandler = (HandlerGetter)symbol("SCH_CAP.dll", "?GetComponentHandler@@YAPAVCComponentHandler@@XZ");
    typedef long (__thiscall* Generate)(void*, void**, NativeString*, const NativeString*, const NativeString*, const NativeString*, const NativeString*, int, int, int, int);
    Generate generate = (Generate)symbol("SCH_CAP.dll", "?GenerateNewComponent@CComponentHandler@@QAEJAAPAVCIITSymbolComp@@AAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@ABV34@222HHHH@Z");
    const wchar_t* specs[][4] = {
        {L"Basic", L"RESISTOR", L"1k", L""},
        {L"Basic", L"RESISTOR", L"RESISTOR", L""},
        {L"Basic", L"BASIC_VIRTUAL", L"RESISTOR", L""},
        {L"", L"RESISTOR", L"1k", L""},
        {L"", L"Basic", L"RESISTOR", L"1k"},
        {L"Basic", L"RLC", L"RESISTOR", L""},
        {L"BASIC_VIRTUAL", L"RESISTOR", L"RESISTOR", L""}
    };
    void* component = nullptr;
    bool generated = false;
    for (unsigned i = 0; (action < 3 || action == 4) && i < sizeof(specs) / sizeof(specs[0]); ++i) {
        NativeString modelName(L""), first(specs[i][0]), second(specs[i][1]), third(specs[i][2]), fourth(specs[i][3]);
        component = nullptr;
        long status = generate && getHandler ? generate(getHandler(), &component, &modelName, &first, &second, &third, &fourth, 0, 1, 0, 0) : -1;
        const char* name = runtimeName(component);
        fprintf(file, "%s{\"spec\":[", i ? "," : "");
        for (int s = 0; s < 4; ++s) { if (s) fputc(',', file); jsonString(file, utf8(specs[i][s])); }
        fprintf(file, "],\"status\":%ld,\"class\":\"%s\",\"model_characters\":%u", status, name ? name : "null", (unsigned)utf8((const wchar_t*)modelName.data).size());
        pointerDiagnostic(file, component);
        fprintf(file, "}");
        fflush(file);
        if (status == 0 && name && (!strcmp(name, "CIITSymbolComp") || !strcmp(name, "CIITAnimationSymbolComp"))) { generated = true; break; }
    }
    void* view = nativeView(".?AVCODView@@");
    fprintf(file, "],\"generated\":%s,\"native_view\":\"%p\"", generated ? "true" : "false", view);
    fprintf(file, ",\"source_type\":\"%p\",\"target_type\":\"%p\",\"cast_function\":\"%p\"", findTypeDescriptor(".?AVCBaseView@@"), findTypeDescriptor(".?AVCIITView@@"), symbol("MSVCR71.dll", "__RTDynamicCast"));
    if ((generated && (action == 2 || action == 4)) || action == 3 || action == 5 || action == 6) {
        operationStage = "resolve_add";
        void* sheet = globalObject("?GetDrwSubSheet@@YAPAVCDrwSubSheet@@XZ");
        void* diagram = currentDiagram(sheet);
        struct Point { LONG x; LONG y; } point = {1200, 300};
        typedef void (__thiscall* Add)(void*, void*, Point, void*);
        Add add = (Add)dynamicMethod(diagram, "??_7CODDiagram@@6B@", "?AddComponent@CODDiagram@@UAEXPAVCODComponent@@VCPoint@@PAVCODView@@@Z");
        fprintf(file, ",\"add_slot\":%d,\"add_function\":\"%p\",\"diagram_address\":\"%p\"", methodSlot, add, diagram);
        fflush(file);
        operationStage = "add_component";
        if (generated && action != 3 && add && view) add(diagram, component, point, view);
        fprintf(file, ",\"add_called\":%s", generated && action != 3 && add && view ? "true" : "false");
        if (action == 4) {
            operationStage = "attach_electrical_component";
            void* target = findSymbol(diagram, "R1");
            typedef void* (__thiscall* Getter)(void*);
            Getter circuitGetter = (Getter)symbol("Circexp.dll", "?GetCircuit@CiComponent@@QBEPAVCiCircuit@@XZ");
            void* circuit = circuitGetter(electricalData(target));
            void* data = electricalData(component);
            typedef int (__thiscall* NameSetter)(void*, const wchar_t*);
            NameSetter nameSetter = (NameSetter)symbol("Circexp.dll", "?SetLocalName@CiElement@@QAEHPB_W@Z");
            typedef void (__thiscall* RefSetter)(void*, const wchar_t*);
            RefSetter refSetter = (RefSetter)symbol("Od101dll.dll", "?SetMultisectionInstanceRefDes@CIITSymbolComp@@UAEXPB_W@Z");
            nameSetter(data, L"R2");
            refSetter((char*)component + 0x40, L"R2");
            typedef int (__thiscall* Attach)(void*, void*, int);
            Attach attach = (Attach)symbol("Circexp.dll", "?AddComponent@CiCircuit@@QAEHPAVCiComponent@@H@Z");
            int attached = attach(circuit, data, 0);
            fprintf(file, ",\"electrical_circuit\":\"%p\",\"electrical_attached\":%d", circuit, attached);
            fflush(file);
            operationStage = "native_wire";
            unsigned wires = 0;
            if (target) {
                if (makeWire(diagram, pinByNumber(component, "1"), pinByNumber(target, "1"))) ++wires;
                if (makeWire(diagram, pinByNumber(component, "2"), pinByNumber(target, "2"))) ++wires;
            }
            fprintf(file, ",\"wires_created\":%u", wires);
            operationStage = "synchronize_refdes";
            fprintf(file, ",\"synchronize_status\":%d", synchronizeModel(sheet));
        }
        typedef void* (__thiscall* ProjectGetter)(void*, void*);
        operationStage = "resolve_project";
        ProjectGetter getProject = (ProjectGetter)symbol("DrwEngine.dll", "?GetProject@CWorkspaceContainer@@QAEPAVCProject@@PBVCDrwSubSheet@@@Z");
        void* workspace = globalObject("?GetWorkSpaceContainer@@YAPAVCWorkspaceContainer@@XZ");
        void* project = getProject ? getProject(workspace, sheet) : nullptr;
        typedef bool (__thiscall* Allowed)(void*);
        Allowed allowed = (Allowed)symbol("DrwEngine.dll", "?IsAllowedToSave@CProject@@QBE_NXZ");
        fprintf(file, ",\"project\":\"%p\",\"project_class\":\"%s\",\"allowed_to_save\":%s", project, runtimeName(project) ? runtimeName(project) : "unknown", project && allowed && allowed(project) ? "true" : "false");
        if (action == 5 && project && allowed && allowed(project)) {
            operationStage = "set_project_path";
            fflush(file);
            typedef void (__thiscall* PathSetter)(void*, const NativeString*);
            PathSetter pathSetter = (PathSetter)symbol("DrwEngine.dll", "?SetProjectFilePath@CProject@@QAEXABVCPathString@@@Z");
            NativeString outputPath(siblingPath(L"results\\direct-saveas.ms10").c_str());
            if (pathSetter) pathSetter(project, &outputPath);
            typedef void (__thiscall* ModifiedSetter)(void*, bool);
            ModifiedSetter modified = (ModifiedSetter)symbol("DrwEngine.dll", "?SetModified@CProject@@QAEX_N@Z");
            if (modified) modified(project, true);
            fprintf(file, ",\"save_as_path_set\":%s", pathSetter ? "true" : "false");
        }
        typedef void* (__cdecl* SettingsGetter)();
        SettingsGetter settingsGetter = (SettingsGetter)symbol("library.dll", "?GetSettings@@YAPAVCSet@@XZ");
        void* settings = settingsGetter ? settingsGetter() : nullptr;
        void* matrix = settings && readable((char*)settings + 4, 4) ? *(void**)((char*)settings + 4) : nullptr;
        typedef unsigned (__thiscall* DllIndex)(void*, const NativeString*);
        typedef unsigned __int64 (__thiscall* CommandId)(void*, unsigned, unsigned, unsigned);
        DllIndex dllIndex = (DllIndex)symbol("TierMatrix.dll", "?GetDllIndex@CMatrix@@QAEIABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
        CommandId commandId = (CommandId)symbol("TierMatrix.dll", "?GetID@CMatrix@@QAE_KIIW4COMMANDTYPES@1@@Z");
        NativeString moduleName(L"DrwEngine");
        unsigned __int64 id = matrix && dllIndex && commandId ? commandId(matrix, dllIndex(matrix, &moduleName), 3, 4) : 0xFFFFFFFF;
        bool commandAllowed = (id & 0xF818FFFF) < 0xFFFF;
        typedef unsigned (__thiscall* CommandIndex)(void*, unsigned __int64);
        CommandIndex commandIndex = (CommandIndex)symbol("TierMatrix.dll", "?GetCommandIndex@CMatrix@@QAEI_K@Z");
        unsigned mapped = commandAllowed && commandIndex ? commandIndex(matrix, id) : 0;
        fprintf(file, ",\"save_command_allowed\":%s,\"feature_command_index\":%u,\"save_command_raw\":\"%I64x\"", commandAllowed ? "true" : "false", mapped, id);
        typedef unsigned (__thiscall* MatrixSize)(void*);
        typedef unsigned __int64 (__thiscall* MapEntry)(void*, unsigned, unsigned*, void*);
        MatrixSize size = (MatrixSize)symbol("TierMatrix.dll", "?GetSize@CMatrix@@QAEIXZ");
        MapEntry entry = (MapEntry)symbol("TierMatrix.dll", "?GetIDMap@CMatrix@@QAE_KIAAIAAUCALLBACKSTRUCT@@@Z");
        fprintf(file, ",\"same_module_commands\":[");
        unsigned listed = 0;
        unsigned count = matrix && size ? size(matrix) : 0;
        for (unsigned index = 0; entry && index < count && index < 10000; ++index) {
            unsigned menuId = 0; unsigned callback[8] = {};
            unsigned __int64 resource = entry(matrix, index, &menuId, callback);
            if (resource == ~0ui64 || ((resource >> 16) & 0xFFFF) != ((id >> 16) & 0xFFFF)) continue;
            fprintf(file, "%s{\"index\":%u,\"resource\":\"%I64x\",\"mapped_value\":%u,\"callback\":[%u,%u,%u,%u]}", listed++ ? "," : "", index, resource, menuId, callback[0], callback[1], callback[2], callback[3]);
        }
        fprintf(file, "]");
        typedef void* (__cdecl* UiGetter)();
        UiGetter uiGetter = (UiGetter)symbol("PlugInManagement.dll", "?GetUserInterfaceHandler@CUserInterfaceHandler@@SAPAV1@XZ");
        void* ui = uiGetter ? uiGetter() : nullptr;
        typedef void* (__thiscall* Getter)(void*);
        Getter commandsGetter = (Getter)symbol("PlugInManagement.dll", "?GetAvailableCommands@CUserInterfaceHandler@@QAEAAV?$vector@UNEWCOMMANDINFOSTRUCT@@V?$allocator@UNEWCOMMANDINFOSTRUCT@@@std@@@std@@XZ");
        Getter frameGetter = (Getter)symbol("PlugInManagement.dll", "?GetFrameWnd@CUserInterfaceHandler@@QAEPAVCFrameWnd@@XZ");
        void* frame = ui && frameGetter ? frameGetter(ui) : nullptr;
        fprintf(file, ",\"frame_address\":\"%p\",\"frame_hwnd\":\"%p\",\"save_ui_commands\":[", frame, frame && readable((char*)frame + 0x20, 4) ? *(void**)((char*)frame + 0x20) : nullptr);
        typedef unsigned short (__thiscall* UiCommand)(void*, unsigned short, const wchar_t*);
        UiCommand uiCommand = (UiCommand)symbol("PlugInManagement.dll", "?GetCommandID@CUserInterfaceHandler@@QAEGGPB_W@Z");
        void** commands = ui && commandsGetter ? (void**)commandsGetter(ui) : nullptr;
        unsigned uiListed = 0;
        unsigned saveUiCommand = 0;
        if (commands && readable(commands, 16)) {
            char* begin = (char*)commands[1]; char* end = (char*)commands[2];
            if (begin && end >= begin && end - begin < 1000000 && readable(begin, end - begin)) {
                for (char* record = begin; record + 100 <= end; record += 100) {
                    const wchar_t* name = *(const wchar_t**)record;
                    if (!readable(name, 2)) continue;
                    std::string commandName = utf8(name);
                    if (commandName.find("Save") == std::string::npos && commandName.find("save") == std::string::npos && commandName.find("SAVE") == std::string::npos && commandName.find("New") == std::string::npos) continue;
                    unsigned short module = *(unsigned short*)(record + 0x2A);
                    unsigned command = uiCommand ? uiCommand(ui, module, name) : 0xFFFF;
                    if (action != 6 && module == 0 && commandName == "FileSave" && commandAllowed) saveUiCommand = command;
                    if (action == 6 && module == 21 && commandName == "FileNew") saveUiCommand = command;
                    fprintf(file, "%s{\"name\":", uiListed++ ? "," : "");
                    jsonString(file, commandName);
                    fprintf(file, ",\"module\":%u,\"command\":%u}", module, command);
                }
            }
        }
        fprintf(file, "],\"save_command_id\":%u", saveUiCommand);
        operationStage = "command_resolved";
    }
    fprintf(file, "}\n");
    fclose(file);
    currentOutput = nullptr;
}
static std::vector<std::wstring> splitFields(const std::wstring& line) {
    std::vector<std::wstring> result;
    size_t start = 0;
    for (;;) {
        size_t tab = line.find(L'\t', start);
        result.push_back(line.substr(start, tab == std::wstring::npos ? tab : tab - start));
        if (tab == std::wstring::npos) break;
        start = tab + 1;
    }
    return result;
}
static void* createDatabaseComponent(void* sheet, const std::wstring& group, const std::wstring& family, const std::wstring& part) {
    operationStage = "database_component_search";
    typedef void* (__cdecl* HandlerGetter)();
    HandlerGetter handlerGetter = (HandlerGetter)symbol("SCH_CAP.dll", "?GetComponentHandler@@YAPAVCComponentHandler@@XZ");
    void* handler = handlerGetter ? handlerGetter() : nullptr;
    typedef void* (__thiscall* Getter)(void*);
    Getter groupGetter = (Getter)symbol("SCH_CAP.dll", "?GetDatabasePtrGroup@CComponentHandler@@QAEPAVCDatabasePtrGroup@@XZ");
    typedef void* (__thiscall* DatabaseGetter)(void*, int);
    DatabaseGetter databaseGetter = (DatabaseGetter)symbol("database.dll", "?GetDbPtr@CDatabasePtrGroup@@QBEPAVCDBComponent@@H@Z");
    if (!handler || !groupGetter || !databaseGetter) return nullptr;
    void* database = databaseGetter(groupGetter(handler), 0);
    typedef void* (__thiscall* UtilityConstructor)(void*, void*, void*);
    UtilityConstructor utilityConstructor = (UtilityConstructor)symbol("CompEdit.dll", "??0CCEGeneralUtil@@QAE@PAVCDBComponent@@0@Z");
    typedef void* (__thiscall* Constructor)(void*);
    Constructor recordConstructor = (Constructor)symbol("CompEdit.dll", "??0CDataRecordComponent@@QAE@XZ");
    typedef void (__thiscall* Destructor)(void*);
    Destructor recordDestructor = (Destructor)symbol("CompEdit.dll", "??1CDataRecordComponent@@UAE@XZ");
    Destructor utilityDestructor = (Destructor)symbol("CompEdit.dll", "??1CCEGeneralUtil@@UAE@XZ");
    Destructor clearRecord = (Destructor)symbol("CompEdit.dll", "?Clearup_character_FL@CDataRecordComponent@@QAEXXZ");
    typedef int (__thiscall* Search)(void*, const NativeString*, const NativeString*, const NativeString*, const NativeString*, void*, bool, bool, bool);
    Search search = (Search)symbol("CompEdit.dll", "?Search_a_component@CCEGeneralUtil@@QAEHABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@000AAVCDataRecordComponent@@_N22@Z");
    typedef void* (__thiscall* Create)(void*, void*, void*, int, int, void*);
    Create create = (Create)symbol("SCH_CAP.dll", "?CreateComponentFromDataRecord@CComponentHandler@@QAEPAVCIITSymbolComp@@PAVCSCH_CAPSubSheet@@AAVCDataRecordComponent@@HHV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
    if (!database || !utilityConstructor || !recordConstructor || !recordDestructor || !utilityDestructor || !clearRecord || !search || !create) return nullptr;
    __declspec(align(8)) unsigned char utility[32] = {};
    __declspec(align(8)) unsigned char record[4096] = {};
    utilityConstructor(utility, database, database);
    recordConstructor(record);
    NativeString g(group.c_str()), f(family.c_str()), p(part.c_str()), empty(L"");
    int found = search(utility, &f, &g, &p, &empty, record, false, true, true);
    fprintf(currentOutput, ",\"search_sql\":");
    jsonString(currentOutput, valueString(database, "?GetSQLString_SQLDirectSet@CDBComponent@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ", "database.dll"));
    void* component = nullptr;
    if (found) {
        clearRecord(record);
        operationStage = "create_component_in_sheet";
        typedef void* (__thiscall* ArgumentString)(void*, const wchar_t*);
        ArgumentString argumentString = (ArgumentString)GetProcAddress(GetModuleHandleA("MFC71U.DLL"), MAKEINTRESOURCEA(283));
        void* argument = nullptr;
        if (argumentString) {
            argumentString(&argument, L"");
            component = create(handler, sheet, record, 1, 0, argument);
        }
    }
    recordDestructor(record);
    utilityDestructor(utility);
    return component;
}
static bool batchItem(const std::vector<std::wstring>& fields, FILE* file) {
    if (fields.empty()) return false;
    void* sheet = globalObject("?GetDrwSubSheet@@YAPAVCDrwSubSheet@@XZ");
    void* diagram = currentDiagram(sheet);
    void* view = nativeView(".?AVCODView@@");
    if (!sheet || !diagram || !view) return false;
    const std::wstring& op = fields[0];
    if (op==L"place_bus" && fields.size()>=6 && fields.size()<=514 && fields.size()%2==0) {
        if (fields[1].empty() || fields[1].size()>64) return false;
        std::vector<DiagramPoint> points;
        for (size_t i=2;i<fields.size();i+=2) {
            wchar_t *endX=nullptr,*endY=nullptr;
            DiagramPoint point={wcstol(fields[i].c_str(),&endX,10),wcstol(fields[i+1].c_str(),&endY,10)};
            if (fields[i].empty() || fields[i+1].empty() || *endX || *endY ||
                point.x<-1000000 || point.x>1000000 || point.y<-1000000 || point.y>1000000) return false;
            if (!points.empty() && ((points.back().x==point.x)==(points.back().y==point.y))) return false;
            points.push_back(point);
        }
        return placeBus(file,diagram,view,fields[1].c_str(),points);
    }
    if (op==L"scope_read" && fields.size()==3) {
        return readScope(file,findSymbol(diagram,utf8(fields[1].c_str()).c_str()),wcstol(fields[2].c_str(),nullptr,10));
    }
    if (op==L"configure_scope" && fields.size()==9) {
        return configureScope(findSymbol(diagram,utf8(fields[1].c_str()).c_str()),fields);
    }
    if (op==L"interactive_simulation" && fields.size()==2) {
        typedef void* (__cdecl* Settings)();
        typedef bool (__thiscall* Running)(void*);
        Settings settings=(Settings)symbol("SCH_CAP.dll","?GetSCH_CAPDocTypeSettings@@YAPAVCSCH_CAPDocTypeSettings@@XZ");
        Running running=(Running)symbol("SCH_CAP.dll","?IsSimulationRunning@CSCH_CAPDocTypeSettings@@QAE_NXZ");
        void* options=settings ? settings() : nullptr;
        if (!options || !running) return false;
        bool active=running(options);
        fprintf(file,",\"running\":%s",active ? "true" : "false");
        if (fields[1]==L"status") return true;
        bool start=fields[1]==L"start";
        if (!start && fields[1]!=L"stop") return false;
        if (active==start) return true;
        typedef void* (__cdecl* UI)();
        typedef void* (__thiscall* Getter)(void*);
        typedef unsigned short (__thiscall* Command)(void*,unsigned short,const wchar_t*);
        UI uiGetter=(UI)symbol("PlugInManagement.dll","?GetUserInterfaceHandler@CUserInterfaceHandler@@SAPAV1@XZ");
        Getter frameGetter=(Getter)symbol("PlugInManagement.dll","?GetFrameWnd@CUserInterfaceHandler@@QAEPAVCFrameWnd@@XZ");
        Command command=(Command)symbol("PlugInManagement.dll","?GetCommandID@CUserInterfaceHandler@@QAEGGPB_W@Z");
        void* ui=uiGetter ? uiGetter() : nullptr;
        void* frame=ui && frameGetter ? frameGetter(ui) : nullptr;
        HWND window=readable(frame,0x24) ? *(HWND*)((char*)frame+0x20) : nullptr;
        DWORD owner=0;
        GetWindowThreadProcessId(window,&owner);
        if (owner!=GetCurrentProcessId() || !command || !ui) return false;
        unsigned short id=command(ui,25,start ? L"UnifiedSimulationPlay" : L"UnifiedSimulationStop");
        return id && id!=0xffff && PostMessageW(window,WM_COMMAND,MAKEWPARAM(id,0),0)!=FALSE;
    }
    if (op==L"configure_multimeter" && fields.size()==3) {
        return configureMeter(findSymbol(diagram,utf8(fields[1].c_str()).c_str()),wcstol(fields[2].c_str(),nullptr,10));
    }
    if (op==L"configure_generator" && fields.size()==7) {
        return configureGenerator(findSymbol(diagram,utf8(fields[1].c_str()).c_str()),wcstol(fields[2].c_str(),nullptr,10),
                                  wcstod(fields[3].c_str(),nullptr),wcstod(fields[4].c_str(),nullptr),
                                  wcstod(fields[5].c_str(),nullptr),wcstod(fields[6].c_str(),nullptr));
    }
    if ((op==L"place_instrument" || op==L"instrument_debug_create") && fields.size()==8) {
        return makeInstrument(sheet,diagram,fields[1].c_str(),fields[2].c_str(),fields[3].c_str(),wcstoul(fields[4].c_str(),nullptr,10),
                              wcstoul(fields[5].c_str(),nullptr,0),{wcstol(fields[6].c_str(),nullptr,10),wcstol(fields[7].c_str(),nullptr,10)})!=nullptr;
    }
    if (op==L"probe_debug_create" && fields.size()==5) {
        return makeProbe(sheet,diagram,fields[1].c_str(),wcstoul(fields[2].c_str(),nullptr,10),
                         {wcstol(fields[3].c_str(),nullptr,10),wcstol(fields[4].c_str(),nullptr,10)})!=nullptr;
    }
    if (op==L"insert_probe" && fields.size()==9) {
        int kind=(int)wcstol(fields[8].c_str(),nullptr,10);
        if (kind<3 || kind>5) return false;
        return splitWire(diagram,resolveTerminal(diagram,fields[1],fields[2]),resolveTerminal(diagram,fields[3],fields[4]),fields[5].c_str(),
                         {wcstol(fields[6].c_str(),nullptr,10),wcstol(fields[7].c_str(),nullptr,10)},kind);
    }
    if (op==L"net_label" && (fields.size()==8 || fields.size()==9)) {
        DiagramPoint point={wcstol(fields[6].c_str(),nullptr,10),wcstol(fields[7].c_str(),nullptr,10)};
        return setNetLabel(diagram,resolveTerminal(diagram,fields[1],fields[2]),fields[3],fields.size()==8 || fields[8]==L"1" ? &fields[4] : nullptr,fields[5]==L"1",fields[6].empty() ? nullptr : &point);
    }
    if (op == L"junction" && fields.size()==4) {
        std::string name = utf8(fields[1].c_str());
        if (name.empty() || name.rfind("@junction:",0)==0 || findEditable(diagram,name.c_str())) return false;
        DiagramPoint point = {wcstol(fields[2].c_str(),nullptr,10),wcstol(fields[3].c_str(),nullptr,10)};
        return makeJunction(diagram,fields[1].c_str(),point)!=nullptr;
    }
    if (op == L"split_wire" && fields.size()==8) {
        return splitWire(diagram,resolveTerminal(diagram,fields[1],fields[2]),resolveTerminal(diagram,fields[3],fields[4]),fields[5].c_str(),
                         {wcstol(fields[6].c_str(),nullptr,10),wcstol(fields[7].c_str(),nullptr,10)});
    }
    if (op == L"export_netlist" && fields.size() == 3) {
        typedef void* (__thiscall* TreeGetter)(void*);
        TreeGetter treeGetter = (TreeGetter)symbol("DrwEngine.dll", "?GetTreeInstance@CBaseView@@QBEPAVCTreeInstance@@XZ");
        void* baseView = globalObject("?GetSheetView@@YAPAVCBaseView@@XZ");
        void* tree = baseView && treeGetter ? treeGetter(baseView) : nullptr;
        typedef void (__thiscall* Export)(void*, void*, NativeString*, int);
        Export exportNetlist = (Export)symbol("SCH_CAP.dll", "?ExportNetlist@CSCH_CAPDoc@@QAEXPAVCTreeInstance@@AAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@H@Z");
        void* document = currentDocument(sheet);
        if (!tree || !document || !exportNetlist) return false;
        NativeString path(fields[1].c_str());
        operationStage = "export_native_netlist";
        exportNetlist(document, tree, &path, (int)wcstol(fields[2].c_str(), nullptr, 10));
        return GetFileAttributesW(fields[1].c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    if (op == L"catalog" && fields.size() == 3) {
        typedef void* (__cdecl* HandlerGetter)();
        HandlerGetter handlerGetter = (HandlerGetter)symbol("SCH_CAP.dll", "?GetComponentHandler@@YAPAVCComponentHandler@@XZ");
        typedef void* (__thiscall* Getter)(void*);
        Getter groupGetter = (Getter)symbol("SCH_CAP.dll", "?GetDatabasePtrGroup@CComponentHandler@@QAEPAVCDatabasePtrGroup@@XZ");
        typedef void* (__thiscall* DatabaseGetter)(void*, int);
        DatabaseGetter databaseGetter = (DatabaseGetter)symbol("database.dll", "?GetDbPtr@CDatabasePtrGroup@@QBEPAVCDBComponent@@H@Z");
        void* handler = handlerGetter ? handlerGetter() : nullptr;
        void* database = handler && groupGetter && databaseGetter ? databaseGetter(groupGetter(handler), 0) : nullptr;
        typedef void (__thiscall* Open)(void*, void*);
        Open open = (Open)symbol("database.dll", "?OpenSQLDirectSet@CDBComponent@@QAEXV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
        typedef void (__thiscall* Step)(void*);
        Step next = (Step)symbol("database.dll", "?MoveNextRecord_SQLDirectSet@CDBComponent@@QAEXXZ");
        Step close = (Step)symbol("database.dll", "?CloseSQLDirectSet@CDBComponent@@QAEXXZ");
        typedef bool (__thiscall* Eof)(void*);
        Eof eof = (Eof)symbol("database.dll", "?IsEOFRecord_SQLDirectSet@CDBComponent@@QAE_NXZ");
        typedef bool (__thiscall* FieldGetter)(void*, const NativeString*, NativeString*);
        FieldGetter field = (FieldGetter)symbol("database.dll", "?GetFieldValue_SQLDirectSet@CDBComponent@@QAE_NABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@AAV23@@Z");
        typedef bool (__thiscall* IdGetter)(void*, const NativeString*, long*);
        IdGetter idGetter = (IdGetter)symbol("database.dll", "?GetFieldValue_SQLDirectSet@CDBComponent@@QAE_NABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@AAJ@Z");
        if (!database || !open || !next || !close || !eof || !field) return false;
        unsigned limit = wcstoul(fields[2].c_str(), nullptr, 10);
        if (limit < 1 || limit > 1000) return false;
        std::wstring keyword;
        for (wchar_t c : fields[1]) { keyword += c; if (c == L'\'') keyword += c; }
        std::wstring sql = L"SELECT TOP " + std::to_wstring(limit) + L" C.Component_ID AS ID,C.Component_Name AS Part,F.Family_Name AS Family FROM SYS_COMPONENT AS C INNER JOIN SYS_CONFIGURATION AS F ON C.Family_ID=F.Family_ID WHERE C.Component_Name LIKE '*" + keyword + L"*' ORDER BY C.Component_Name";
        typedef void* (__thiscall* StringConstructor)(void*, const wchar_t*);
        StringConstructor stringConstructor = (StringConstructor)GetProcAddress(GetModuleHandleA("MFC71U.DLL"), MAKEINTRESOURCEA(283));
        void* argument = nullptr;
        stringConstructor(&argument, sql.c_str());
        operationStage = "catalog_query";
        open(database, argument);
        NativeString idColumn(L"ID"), nameColumn(L"Part"), familyColumn(L"Family");
        fprintf(file, ",\"matches\":[");
        unsigned count = 0;
        while (count < limit && !eof(database)) {
            NativeString name(L""), family(L"");
            long id = 0;
            bool readId = idGetter && idGetter(database, &idColumn, &id);
            bool readName = field(database, &nameColumn, &name);
            bool readFamily = field(database, &familyColumn, &family);
            if (!readId || !readName || !readFamily) {
                fprintf(file, "],\"read_fields\":[%d,%d,%d]", readId, readName, readFamily);
                close(database); return false;
            }
            fprintf(file, "%s{\"id\":%ld", count++ ? "," : "", id);
            fprintf(file, ",\"part\":"); jsonString(file, utf8((const wchar_t*)name.data));
            fprintf(file, ",\"family\":"); jsonString(file, utf8((const wchar_t*)family.data));
            fprintf(file, "}");
            next(database);
        }
        fprintf(file, "]");
        close(database);
        return true;
    }
    if (op == L"place" && fields.size() == 7) {
        if (fields[1].empty() || findEditable(diagram, utf8(fields[1].c_str()).c_str())) return false;
        typedef void* (__thiscall* RootDataGetter)(void*);
        RootDataGetter rootDataGetter = (RootDataGetter)symbol("Od101dll.dll", "?GetUserData@CODComponent@@QAEPAVCObject@@XZ");
        typedef void (__thiscall* RootDataSetter)(void*, void*);
        RootDataSetter rootDataSetter = (RootDataSetter)symbol("Od101dll.dll", "?SetUserData@CODComponent@@QAEXPAVCObject@@@Z");
        void* root = diagramRoot(diagram);
        void* initialCircuit = currentCircuit(sheet);
        if (root && initialCircuit && rootDataGetter && rootDataSetter && !rootDataGetter(root)) rootDataSetter(root, initialCircuit);
        typedef void* (__cdecl* HandlerGetter)();
        HandlerGetter getHandler = (HandlerGetter)symbol("SCH_CAP.dll", "?GetComponentHandler@@YAPAVCComponentHandler@@XZ");
        typedef long (__thiscall* Generate)(void*, void**, NativeString*, const NativeString*, const NativeString*, const NativeString*, const NativeString*, int, int, int, int);
        Generate generate = (Generate)symbol("SCH_CAP.dll", "?GenerateNewComponent@CComponentHandler@@QAEJAAPAVCIITSymbolComp@@AAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@ABV34@222HHHH@Z");
        NativeString model(L""), group(fields[2].c_str()), family(fields[3].c_str()), part(fields[4].c_str()), empty(L"");
        void* component = createDatabaseComponent(sheet, fields[2], fields[3], fields[4]);
        long status = component ? 0 : 1;
        fprintf(file, ",\"factory_status\":%ld", status);
        const char* klass = runtimeName(component);
        fprintf(file, ",\"factory_class\":\"%s\"", klass ? klass : "unknown");
        pointerDiagnostic(file, component);
        if (status != 0 || !klass || (strcmp(klass, "CIITSymbolComp") && strcmp(klass, "CIITAnimationSymbolComp"))) return false;
        typedef void* (__cdecl* InfoGetter)(void*);
        InfoGetter infoGetter = (InfoGetter)symbol("SCH_CAP.dll", "?GetDatabaseInfo@CComponentBridge@@SAPAVCiaCollString@@PAVCODSymbolComp@@@Z");
        typedef void* (__thiscall* StringGetter)(void*, void*, int);
        StringGetter getString = (StringGetter)symbol("Circexp.dll", "?GetParam@CiaCollString@@QBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@H@Z");
        NativeString actualPart, actualGroup, actualFamily;
        void* info = infoGetter ? infoGetter(component) : nullptr;
        if (!info || !getString) return false;
        getString(info, &actualPart, 1); getString(info, &actualGroup, 14); getString(info, &actualFamily, 0);
        fprintf(file, ",\"actual_part\":"); jsonString(file, utf8((const wchar_t*)actualPart.data));
        fprintf(file, ",\"actual_family\":"); jsonString(file, utf8((const wchar_t*)actualFamily.data));
        if (_wcsicmp((const wchar_t*)actualPart.data, fields[4].c_str()) || _wcsicmp((const wchar_t*)actualGroup.data, fields[2].c_str()) || _wcsicmp((const wchar_t*)actualFamily.data, fields[3].c_str())) return false;
        typedef void (__thiscall* Add)(void*, void*, DiagramPoint, void*);
        Add add = (Add)dynamicMethod(diagram, "??_7CODDiagram@@6B@", "?AddComponent@CODDiagram@@UAEXPAVCODComponent@@VCPoint@@PAVCODView@@@Z");
        if (!add) return false;
        DiagramPoint point = {wcstol(fields[5].c_str(), nullptr, 10), wcstol(fields[6].c_str(), nullptr, 10)};
        add(diagram, component, point, view);
        void* data = electricalData(component);
        void* circuit = currentCircuit(sheet);
        fprintf(file, ",\"electrical_data\":\"%p\",\"circuit\":\"%p\"", data, circuit);
        if (!data || !circuit) return false;
        typedef void (__thiscall* BackrefSetter)(void*, void*);
        BackrefSetter backrefSetter = (BackrefSetter)symbol("Circexp.dll", "?SetODSymbolComponent@CiComponent@@QAEXPAVCObject@@@Z");
        if (backrefSetter) backrefSetter(data, component);
        typedef void* (__cdecl* ExporterFactory)();
        ExporterFactory exporterFactory = (ExporterFactory)symbol("Circexp.dll", "?CreateObject@CiaSpiceTmpltExprt@@SGPAVCObject@@XZ");
        typedef void* (__thiscall* AttributeGetter)(void*, int);
        AttributeGetter attributeGetter = (AttributeGetter)symbol("Circexp.dll", "?GetAttribute@CiElement@@QBEPAVCiAttribute@@H@Z");
        typedef int (__thiscall* AttributeAdder)(void*, void*, int);
        AttributeAdder attributeAdder = (AttributeAdder)symbol("Circexp.dll", "?AddAttribute@CiElement@@QAEHPAVCiAttribute@@H@Z");
        if (!exporterFactory || !attributeGetter || !attributeAdder) return false;
        if (!attributeGetter(data, 1)) {
            void* exporter = exporterFactory();
            typedef void* (__cdecl* TemplateGetter)(void*, void*);
            TemplateGetter templateGetter = (TemplateGetter)symbol("SCH_CAP.dll", "?GetSpiceTemplate@CComponentBridge@@SA?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@PAVCODSymbolComp@@@Z");
            typedef int (__thiscall* StringSetter)(void*, const wchar_t*);
            StringSetter stringSetter = (StringSetter)symbol("Circexp.dll", "?SetString@CiaCString@@UAEHPB_W@Z");
            if (!exporter || !templateGetter || !stringSetter) return false;
            NativeString spice;
            templateGetter(&spice, component);
            stringSetter(exporter, (const wchar_t*)spice.data);
            attributeAdder(data, exporter, 1);
        }
        // ExportAll's normal fault-enabled path skips a component with no
        // CiaFault attribute, even when the intended state is "no fault".
        ExporterFactory faultFactory = (ExporterFactory)symbol("Circexp.dll", "?CreateObject@CiaFault@@SGPAVCObject@@XZ");
        if (!faultFactory) return false;
        if (!attributeGetter(data, 8)) attributeAdder(data, faultFactory(), 8);
        typedef int (__thiscall* NameSetter)(void*, const wchar_t*);
        NameSetter nameSetter = (NameSetter)symbol("Circexp.dll", "?SetLocalName@CiElement@@QAEHPB_W@Z");
        typedef void (__thiscall* RefSetter)(void*, const wchar_t*);
        RefSetter refSetter = (RefSetter)symbol("Od101dll.dll", "?SetMultisectionInstanceRefDes@CIITSymbolComp@@UAEXPB_W@Z");
        typedef int (__thiscall* Attach)(void*, void*, int);
        Attach attach = (Attach)symbol("Circexp.dll", "?AddComponent@CiCircuit@@QAEHPAVCiComponent@@H@Z");
        bool ground = !_wcsicmp(fields[4].c_str(), L"GROUND");
        nameSetter(data, ground ? L"0" : fields[1].c_str());
        refSetter((char*)component + 0x40, fields[1].c_str());
        typedef void* (__thiscall* CircuitGetter)(void*);
        CircuitGetter circuitGetter = (CircuitGetter)symbol("Circexp.dll", "?GetCircuit@CiComponent@@QBEPAVCiCircuit@@XZ");
        int attached = circuitGetter && circuitGetter(data) == circuit ? 1 : attach(circuit, data, 0);
        fprintf(file, ",\"attach_status\":%d", attached);
        int prepared = prepareComponent(sheet, component, true);
        fprintf(file, ",\"prepare_status\":%d", prepared);
        if (ground) {
            // Native ground initialization creates a type-4 node named after
            // the reserved electrical component name "0" and connects port 0.
            auto groundCode = (unsigned char*)GetModuleHandleA("SCH_CAP.dll") + 0x1731e0;
            if (!executable(groundCode) || groundCode[0] != 0x6a || groundCode[1] != 0xff) return false;
            typedef void (__cdecl* GroundInit)(void*);
            operationStage = "initialize_ground_node";
            ((GroundInit)groundCode)(data);
        }
        return attached != 0 && prepared >= 0;
    }
    if (op == L"route" && fields.size() >= 5 && fields.size() <= 2053 && (fields.size()-5)%2==0) {
        void* a = resolveTerminal(diagram,fields[1],fields[2]);
        void* b = resolveTerminal(diagram,fields[3],fields[4]);
        void* wire = findWire(diagram,a,b);
        if (!wire) return false;
        typedef void* (__thiscall* Getter)(void*);
        Getter multiline = (Getter)symbol("Od101dll.dll", "?GetMultiLine@CODLinkComp@@QAEPAVCODMultiLine@@XZ");
        Getter source = (Getter)symbol("Od101dll.dll", "?GetSourceConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
        typedef void (__thiscall* PointsSetter)(void*,DiagramPoint*,int);
        PointsSetter setter = (PointsSetter)symbol("Od101dll.dll", "?SetNewPointsArray@CODVertices@@QAEXPAVCPoint@@H@Z");
        if (!multiline || !source || !setter) return false;
        DiagramPoint start={},end={};
        void* ac = terminalConnector(a,&start); terminalConnector(b,&end);
        std::vector<DiagramPoint> points = {start};
        for (unsigned i=5;i+1<fields.size();i+=2) points.push_back({wcstol(fields[i].c_str(),nullptr,10),wcstol(fields[i+1].c_str(),nullptr,10)});
        points.push_back(end);
        if (source(wire)!=ac) std::reverse(points.begin(),points.end());
        operationStage = "set_wire_route";
        setter(multiline(wire),points.data(),(int)points.size());
        fprintf(file, ",\"vertex_count\":%u",(unsigned)points.size());
        return true;
    }
    if ((op == L"wire" || op == L"disconnect") && fields.size() == 5) {
        void* a = resolveTerminal(diagram,fields[1],fields[2]);
        void* b = resolveTerminal(diagram,fields[3],fields[4]);
        if (!a || !b || a == b) return false;
        void* existing = findWire(diagram, a, b);
        if (op == L"disconnect") return existing && deleteNative(diagram, {existing}) && !findWire(diagram, a, b);
        if (existing) return false;
        void* link = makeWire(diagram, a, b);
        void* node = terminalNode(a);
        return link && node && node == terminalNode(b);
    }
    if ((op == L"value" && fields.size() == 3) || (op == L"parameter" && fields.size() == 4)) {
        void* component = findSymbol(diagram, utf8(fields[1].c_str()).c_str());
        void* data = electricalData(component);
        if (!data) return false;
        int index = op == L"value" ? 1 : (int)wcstol(fields[2].c_str(), nullptr, 10);
        double value = wcstod(fields.back().c_str(), nullptr);
        typedef void (__thiscall* Setter)(void*, double, int);
        Setter setter = (Setter)symbol("Circexp.dll", "?SetDoubleParam@CiElement@@QAEXNH@Z");
        typedef double (__thiscall* Getter)(void*, double, int);
        Getter getter = (Getter)symbol("Circexp.dll", "?GetDoubleParam@CiElement@@QAENNH@Z");
        if (!setter || !getter || index < 0 || index > 256 || !std::isfinite(value)) return false;
        setter(data, value, index);
        double actual = getter(data, -987654321.0, index);
        fprintf(file, ",\"parameter_index\":%d,\"actual\":%.17g", index, actual);
        return actual == value && prepareComponent(sheet, component, false) >= 0;
    }
    if (op == L"move" && fields.size() == 4) {
        void* component = findEditable(diagram, utf8(fields[1].c_str()).c_str());
        typedef void* (__thiscall* BoxGetter)(void*, RECT*, void*);
        BoxGetter boxGetter = (BoxGetter)symbol("Od101dll.dll", "?GetBox@CIITSymbolComp@@UAE?AVCRect@@AAUODInstContext@@@Z");
        typedef void* (__thiscall* Getter)(void*);
        Getter graphic = (Getter)symbol("Od101dll.dll", "?GetGraphic@CODComponent@@QAEPAVCODGraphic@@XZ");
        typedef void (__thiscall* Translate)(void*, float, float);
        Translate translate = (Translate)symbol("Od101dll.dll", "?Translate@CODGraphic@@UAEXMM@Z");
        void* context = (void*)symbol("Od101dll.dll", "?DefIC@@3UODInstContext@@A");
        if (!component || !boxGetter || !graphic || !translate || !context) return false;
        DiagramPoint point = {wcstol(fields[2].c_str(), nullptr, 10), wcstol(fields[3].c_str(), nullptr, 10)};
        RECT box = {};
        if (isScalarConnector(component)) {
            DiagramPoint old = {}; terminalConnector(component,&old);
            typedef void (__cdecl* Snap)(DiagramPoint*);
            Snap snap = (Snap)symbol("Od101dll.dll", "?MovePointOnGrid@CODDiagram@@SAXAAVCPoint@@@Z");
            if (!snap) return false;
            snap(&point);
            box.left=old.x; box.top=old.y;
        } else boxGetter(component, &box, context);
        translate(graphic(component), (float)(point.x - box.left), (float)(point.y - box.top));
        return rerouteIncident(diagram, component);
    }
    if (op == L"duplicate" && fields.size() == 5) {
        void* source=findSymbol(diagram,utf8(fields[1].c_str()).c_str());
        const char* klass=runtimeName(source);
        void* sourceData=electricalData(source);
        void* circuit=currentCircuit(sheet);
        if (!klass || strcmp(klass,"CIITSymbolComp") || isInstrumentSymbol(source) ||
            !sourceData || !circuit || fields[2].empty() || findEditable(diagram,utf8(fields[2].c_str()).c_str()))
            return false;
        if (!referenceString((char*)source+0x40,
            "?GetInstanceRefDesSection@CIITSymbolComp@@UAEAAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ").empty())
            return false;
        typedef void* (__thiscall* Copy)(void*);
        typedef void (__thiscall* Add)(void*,void*,DiagramPoint,void*);
        typedef int (__thiscall* Name)(void*,const wchar_t*);
        typedef void (__thiscall* Ref)(void*,const wchar_t*);
        typedef void (__thiscall* Backref)(void*,void*);
        typedef int (__thiscall* Attach)(void*,void*,int);
        typedef void* (__thiscall* Find)(void*,const wchar_t*);
        typedef void* (__thiscall* Port)(void*,int);
        typedef void* (__thiscall* Node)(void*,int,bool);
        typedef int (__thiscall* Count)(void*);
        typedef unsigned char (__thiscall* Orientation)(void*);
        typedef void (__thiscall* SetOrientation)(void*,unsigned char);
        typedef void (__thiscall* Destroy)(void*);
        typedef void (__cdecl* Free)(void*);
        Copy copy=(Copy)symbol("Od101dll.dll","?Copy@CIITSymbolComp@@UAEPAVCODComponent@@XZ");
        Add add=(Add)dynamicMethod(diagram,"??_7CODDiagram@@6B@","?AddComponent@CODDiagram@@UAEXPAVCODComponent@@VCPoint@@PAVCODView@@@Z");
        Name name=(Name)symbol("Circexp.dll","?SetLocalName@CiElement@@QAEHPB_W@Z");
        Ref ref=(Ref)symbol("Od101dll.dll","?SetMultisectionInstanceRefDes@CIITSymbolComp@@UAEXPB_W@Z");
        Backref backref=(Backref)symbol("Circexp.dll","?SetODSymbolComponent@CiComponent@@QAEXPAVCObject@@@Z");
        Attach attach=(Attach)symbol("Circexp.dll","?AddComponent@CiCircuit@@QAEHPAVCiComponent@@H@Z");
        Find find=(Find)symbol("Circexp.dll","?FindComponent@CiCircuit@@QBEPAVCiComponent@@PB_W@Z");
        Port port=(Port)symbol("Circexp.dll","?GetPort@CiComponent@@QBEPAVCiPort@@H@Z");
        Node node=(Node)symbol("Circexp.dll","?GetNode@CiPort@@QBEPAVCiNode@@H_N@Z");
        Count count=(Count)symbol("Circexp.dll","?GetPortCount@CiComponent@@QBEHXZ");
        Count width=(Count)symbol("Circexp.dll","?GetWidth@CiPort@@QBEHXZ");
        Orientation rotation=(Orientation)symbol("Od101dll.dll","?GetRotation@CIITSymbolComp@@QBEEXZ");
        Orientation flip=(Orientation)symbol("Od101dll.dll","?GetFlip@CIITSymbolComp@@QBEEXZ");
        SetOrientation setFlip=(SetOrientation)symbol("Od101dll.dll","?SetFlip@CIITSymbolComp@@QAEXE@Z");
        SetOrientation setRotation=(SetOrientation)symbol("Od101dll.dll","?SetRotation@CIITSymbolComp@@QAEXE@Z");
        Destroy destroy=(Destroy)symbol("Od101dll.dll","??1CIITSymbolComp@@UAE@XZ");
        Free freeCopy=(Free)symbol("MSVCR71.dll","??3@YAXPAX@Z");
        if (!copy || !add || !name || !ref || !backref || !attach || !find ||
            !port || !node || !count || !width || !destroy || !freeCopy ||
            !rotation || !flip || !setFlip || !setRotation ||
            find(circuit,fields[2].c_str()) || find(circuit,L"0")==sourceData || fields[2]==L"0") return false;
        operationStage="copy_native_symbol";
        void* component=copy(source);
        if (!component) return false;
        void* data=electricalData(component);
        bool detached=data && data!=sourceData && count(data)==count(sourceData);
        for (int i=0;detached && i<count(data);++i) {
            void* terminal=port(data,i);
            detached=terminal && terminal!=port(sourceData,i) && width(terminal)==1 && !node(terminal,0,false);
        }
        fprintf(file,",\"detached_clone\":%s",detached ? "true" : "false");
        if (!detached) {
            destroy(component);
            freeCopy(component);
            return false;
        }
        operationStage="place_native_copy";
        name(data,fields[2].c_str());
        ref((char*)component+0x40,fields[2].c_str());
        // Copy preserves transformed graphics and pins but clears the orientation flags.
        setFlip(component,flip(source));
        setRotation(component,rotation(source));
        DiagramPoint point={wcstol(fields[3].c_str(),nullptr,10),wcstol(fields[4].c_str(),nullptr,10)};
        add(diagram,component,point,view);
        backref(data,component);
        if (find(circuit,fields[2].c_str())!=data && !attach(circuit,data,0)) return false;
        return find(circuit,fields[2].c_str())==data &&
               findSymbol(diagram,utf8(fields[2].c_str()).c_str())==component &&
               rotation(component)==rotation(source) && flip(component)==flip(source) &&
               prepareComponent(sheet,component,false)>=0;
    }
    if (op == L"mirror" && fields.size() == 3) {
        void* component=findSymbol(diagram,utf8(fields[1].c_str()).c_str());
        const bool horizontal=fields[2]==L"horizontal";
        if (!component || (!horizontal && fields[2]!=L"vertical")) return false;
        typedef void* (__thiscall* Selection)(void*);
        typedef void (__thiscall* Clear)(void*);
        typedef void* (__thiscall* Append)(void*,void*);
        typedef void (__thiscall* Transform)(void*,int,int,int);
        typedef unsigned char (__thiscall* GetFlip)(void*);
        Selection getSelection=(Selection)symbol("Od101dll.dll","?GetSelection@CODView@@QAEPAVCODSelection@@XZ");
        Clear clear=(Clear)symbol("Od101dll.dll","?Clear@CODSelection@@QAEXXZ");
        Append append=(Append)GetProcAddress(GetModuleHandleA("MFC71U.dll"),MAKEINTRESOURCEA(998));
        Transform transform=(Transform)symbol("SCH_CAP.dll","?Advanced_Rotate_Flip@CIITView@@QAEXHHH@Z");
        GetFlip getFlip=(GetFlip)symbol("Od101dll.dll","?GetFlip@CIITSymbolComp@@QBEEXZ");
        void* selection=getSelection ? getSelection(view) : nullptr;
        void* iitView=nativeView();
        if (!selection || !iitView || !clear || !append || !transform || !getFlip) return false;
        const unsigned before=getFlip(component);
        clear(selection);
        append(selection,component);
        operationStage="mirror_native_selection";
        // These arguments match CIITView.EditFlipX and EditFlipY respectively.
        transform(iitView,0,horizontal ? -1 : 1,horizontal ? 1 : -1);
        clear(selection);
        const unsigned after=getFlip(component);
        fprintf(file,",\"previous_flip\":%u,\"flip_native\":%u",before,after);
        return after!=before && rerouteIncident(diagram,component);
    }
    if (op == L"rotate" && fields.size() == 3) {
        void* component = findSymbol(diagram, utf8(fields[1].c_str()).c_str());
        if (!component) return false;
        typedef void* (__cdecl* Allocate)(unsigned);
        Allocate allocate = (Allocate)symbol("MSVCR71.dll", "??2@YAPAXI@Z");
        typedef void* (__thiscall* ListConstructor)(void*, void*);
        ListConstructor listConstructor = (ListConstructor)symbol("Od101dll.dll", "??0CODCompList@@QAE@PAVCODComponent@@@Z");
        typedef void* (__thiscall* ActionConstructor)(void*, void*, float);
        ActionConstructor constructor = (ActionConstructor)symbol("Od101dll.dll", "??0CODRotateAct@@QAE@PAVCODView@@M@Z");
        typedef void (__thiscall* ListSetter)(void*, void*);
        ListSetter setter = (ListSetter)symbol("Od101dll.dll", "?SetCompList@CODAction@@UAEXPAVCODCompList@@@Z");
        typedef long (__thiscall* Execute)(void*, void*);
        Execute execute = (Execute)dynamicMethod(diagram, "??_7CODDiagram@@6B@", "?ExecuteAct@CODDiagram@@UAEJPAVCODAction@@@Z");
        if (!allocate || !listConstructor || !constructor || !setter || !execute) return false;
        void* list = listConstructor(allocate(256), component);
        void* action = constructor(allocate(256), view, (float)wcstod(fields[2].c_str(), nullptr));
        setter(action, list);
        long status = execute(diagram, action);
        fprintf(file, ",\"native_action_status\":%ld", status);
        return status == 0 && rerouteIncident(diagram, component);
    }
    if (op == L"delete" && fields.size() == 2) {
        void* component = findEditable(diagram, utf8(fields[1].c_str()).c_str());
        if (!component) return false;
        std::vector<void*> objects = incidentWires(diagram, component);
        objects.push_back(component);
        if (!deleteNative(diagram, objects)) return false;
        return findEditable(diagram, utf8(fields[1].c_str()).c_str()) == nullptr;
    }
    if (op == L"save_as" && fields.size() == 2) {
        void* project = currentProject(sheet);
        typedef bool (__thiscall* Allowed)(void*);
        Allowed allowed = (Allowed)symbol("DrwEngine.dll", "?IsAllowedToSave@CProject@@QBE_NXZ");
        typedef void (__thiscall* Setter)(void*, const NativeString*);
        Setter setter = (Setter)symbol("DrwEngine.dll", "?SetProjectFilePath@CProject@@QAEXABVCPathString@@@Z");
        if (!project || !setter || !allowed || !allowed(project)) return false;
        NativeString path(fields[1].c_str());
        setter(project, &path);
        return true;
    }
    return false;
}
static void batchOperation() {
    FILE* request = nullptr;
    if (_wfopen_s(&request, siblingPath(L"native-request.tsv").c_str(), L"rb") || !request) return;
    fseek(request, 0, SEEK_END);
    long bytes = ftell(request);
    rewind(request);
    if (bytes < 2 || bytes > 1024 * 1024 || bytes % 2) { fclose(request); return; }
    std::wstring input(bytes / 2, L'\0');
    fread(&input[0], 1, bytes, request);
    fclose(request);
    if (input[0] == 0xFEFF) input.erase(0, 1);
    FILE* file = nullptr;
    if (_wfopen_s(&file, siblingPath(L"native-operation.json").c_str(), L"wb") || !file) return;
    currentOutput = file;
    fprintf(file, "{\"pid\":%lu,\"action\":7,\"results\":[", GetCurrentProcessId());
    size_t start = 0;
    unsigned index = 0;
    bool ok = true;
    bool dirty = false;
    while (start < input.size() && index < 4096) {
        size_t end = input.find(L'\n', start);
        std::wstring line = input.substr(start, end == std::wstring::npos ? end : end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        start = end == std::wstring::npos ? input.size() : end + 1;
        if (line.empty()) continue;
        auto fields = splitFields(line);
        fprintf(file, "%s{\"index\":%u,\"op\":", index ? "," : "", index);
        jsonString(file, utf8(fields[0].c_str()));
        operationStage = "batch_operation";
        fflush(file);
        bool itemOk = batchItem(fields, file);
        if (!itemOk) { fprintf(file,",\"stage\":"); jsonString(file,operationStage); }
        if (fields[0] != L"catalog" && fields[0] != L"export_netlist" && fields[0] != L"interactive_simulation" && fields[0]!=L"scope_read") dirty = true;
        fprintf(file, ",\"ok\":%s}", itemOk ? "true" : "false");
        ++index;
        if (!itemOk) { ok = false; break; }
    }
    void* sheet = globalObject("?GetDrwSubSheet@@YAPAVCDrwSubSheet@@XZ");
    operationStage = "batch_synchronize";
    int sync = 0;
    if (dirty) {
        operationStage = "bind_serialized_circuit";
        void* root = diagramRoot(currentDiagram(sheet));
        typedef void* (__thiscall* Getter)(void*);
        Getter userData = (Getter)symbol("Od101dll.dll", "?GetUserData@CODComponent@@QAEPAVCObject@@XZ");
        typedef void (__thiscall* Setter)(void*, void*);
        Setter setData = (Setter)symbol("Od101dll.dll", "?SetUserData@CODComponent@@QAEXPAVCObject@@@Z");
        void* circuit = currentCircuit(sheet);
        if (root && userData && setData && !userData(root) && circuit) setData(root, circuit);
    }
    bool labelsOk=true;
    if (dirty && ok) labelsOk=refreshNetLabels(currentDiagram(sheet));
    if (!labelsOk) ok=false;
    bool probesOk=true;
    if (dirty && ok) probesOk=refreshProbeBindings(currentDiagram(sheet));
    if (!probesOk) ok=false;
    if (dirty) sync = synchronizeModel(sheet);
    typedef void (__thiscall* Modified)(void*, bool);
    Modified modified = (Modified)symbol("DrwEngine.dll", "?SetModified@CProject@@QAEX_N@Z");
    if (modified && dirty) modified(currentProject(sheet), true);
    Modified sheetModified = (Modified)dynamicMethod(sheet, "??_7CDrwSubSheet@@6B@", "?SetModified@CDrwSubSheet@@UAEX_N@Z", "DrwEngine.dll");
    if (sheetModified && dirty) sheetModified(sheet, true);
    fprintf(file, "],\"ok\":%s,\"net_labels_ok\":%s,\"probe_bindings_ok\":%s,\"synchronize_status\":%d,\"sync_managers\":%u,\"sync_matches\":%u,\"sync_tree\":\"%p\"}\n", ok ? "true" : "false",labelsOk ? "true" : "false",probesOk ? "true" : "false", sync, syncManagers, syncMatches, syncTree);
    fclose(file);
    currentOutput = nullptr;
}
static void writeFault(DWORD exception) {
    if (currentOutput) { fclose(currentOutput); currentOutput = nullptr; }
    FILE* fault = nullptr;
    if (!_wfopen_s(&fault, siblingPath(L"native-fault.json").c_str(), L"wb") && fault) {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)faultAddress, &module);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        wchar_t* name = wcsrchr(path, L'\\');
        fprintf(fault, "{\"pid\":%lu,\"stage\":\"%s\",\"exception\":\"0x%08lx\",\"address\":\"%p\",\"module_rva\":\"0x%lx\",\"access_address\":\"%p\",\"ecx\":\"%p\",\"module\":", GetCurrentProcessId(), operationStage, exception, faultAddress, (DWORD)((char*)faultAddress - (char*)module), (void*)accessAddress, (void*)faultEcx);
        jsonString(fault, utf8(name ? name + 1 : path));
        fprintf(fault, "}\n");
        fclose(fault);
    }
}
extern "C" __declspec(dllexport) LRESULT CALLBACK MultisimHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const CWPSTRUCT* call = (const CWPSTRUCT*)lparam;
        if (call && call->message == RegisterWindowMessageW(messageName)) {
            __try {
                if (call->wParam == 0) snapshot();
                else if (call->wParam == 7) batchOperation();
                else factoryOperation((unsigned)call->wParam);
            } __except(captureFault(GetExceptionInformation())) {
                writeFault(GetExceptionCode());
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        selfModule = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
