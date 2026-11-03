// Scalar schematic terminals: symbol pins and native CODPinComp junctions.
static bool isProbe(void* object) {
    const char* type=runtimeName(object);
    return type && !strcmp(type,"CIITProbeComponent");
}
static void* findProbe(void* diagram,const char* name);
static bool isJunction(void* object) {
    const char* type = runtimeName(object);
    return type && !strcmp(type, "CODPinComp");
}
static bool isScalarConnector(void* object) { return isJunction(object) || isProbe(object); }
static std::string junctionName(void* object) {
    return valueString(object, "?GetText@CODConnector@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ");
}
static std::string junctionReference(void* object);
static void* findJunction(void* diagram, const char* name) {
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*, void**);
    Getter first = (Getter)symbol("Od101dll.dll", "?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    Next next = (Next)symbol("Od101dll.dll", "?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    void* root = diagramRoot(diagram);
    void* position = root && first ? first(root) : nullptr;
    for (unsigned i=0; position && next && i<16384; ++i) {
        void* object = next(root, &position);
        if (isJunction(object) && junctionReference(object)==name) return object;
    }
    return nullptr;
}
static void* terminalConnector(void* object, DiagramPoint* point) {
    if (!object || !point) return nullptr;
    if (isScalarConnector(object)) {
        if (isProbe(object)) {
            typedef void (__thiscall* Center)(void*,float*,float*);
            Center center=(Center)symbol("Od101dll.dll","?GetCenter@CODConnector@@UAEXAAM0@Z");
            float x=0,y=0;
            if (!center) return nullptr;
            center(object,&x,&y); point->x=(LONG)std::lround(x); point->y=(LONG)std::lround(y);
            return object;
        }
        typedef void (__thiscall* Center)(void*, LONG*, LONG*);
        Center center = (Center)symbol("Od101dll.dll", "?GetCenter@CODConnector@@UAEXAAH0@Z");
        if (!center) return nullptr;
        center(object, &point->x, &point->y);
        return object;
    }
    const char* type = runtimeName(object);
    if (!type || strcmp(type,"CIITPinSymbolComp")) return nullptr;
    typedef void* (__thiscall* Point)(void*, DiagramPoint*, int);
    Point get = (Point)symbol("Od101dll.dll", "?GetConnectorPoint@CIITPinSymbolComp@@QAEPAVCIITPinConnectorComp@@AAVCPoint@@H@Z");
    return get ? get(object, point, 0) : nullptr;
}
static std::string junctionReference(void* object) {
    std::string name=junctionName(object);
    if (!name.empty()) return name;
    DiagramPoint point={};
    if (!terminalConnector(object,&point)) return "";
    return "@junction:"+std::to_string(point.x)+","+std::to_string(point.y);
}
static void* terminalNode(void* object) {
    if (!object) return nullptr;
    if (!isScalarConnector(object)) return pinNode(object);
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*,void**);
    Getter first = (Getter)symbol("Od101dll.dll","?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    Next next = (Next)symbol("Od101dll.dll","?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    Getter source = (Getter)symbol("Od101dll.dll","?GetSourceConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    Getter target = (Getter)symbol("Od101dll.dll","?GetTargetConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    Getter getNode = (Getter)symbol("SCH_CAP.dll","?GetNode@CIITLinkComp@@QAEPAVCiNode@@XZ");
    // Native junctions do not own electrical data. Their incident scalar links
    // own the node, including after the normal Multisim file loader runs.
    void* root = diagramRoot(currentDiagram(globalObject("?GetDrwSubSheet@@YAPAVCDrwSubSheet@@XZ")));
    void* position = root && first ? first(root) : nullptr;
    void* result = nullptr;
    if (!next || !source || !target || !getNode) return nullptr;
    for (unsigned i=0; position && i<16384; ++i) {
        void* item = next(root,&position);
        const char* type = runtimeName(item);
        if (!type || strcmp(type,"CIITLinkComp")) continue;
        if (source(item)!=object && target(item)!=object) continue;
        void* node = getNode(item);
        if (result && node && result!=node) return nullptr;
        if (node) result=node;
    }
    return result;
}
static void* terminalPort(void* object) {
    if (!object || isScalarConnector(object)) return nullptr;
    typedef void* (__thiscall* Getter)(void*);
    Getter get = (Getter)symbol("Od101dll.dll", "?GetCiPort@CIITPinSymbolComp@@QAEPAVCiPort@@XZ");
    return get ? get(object) : nullptr;
}
static void* resolveTerminal(void* diagram, const std::wstring& ref, const std::wstring& pin) {
    std::string name = utf8(ref.c_str());
    void* object = findSymbol(diagram, name.c_str());
    if (object) return pinByNumber(object, utf8(pin.c_str()).c_str());
    if (pin!=L"0" && !pin.empty()) return nullptr;
    object=findJunction(diagram,name.c_str());
    return object ? object : findProbe(diagram,name.c_str());
}
static void* findEditable(void* diagram, const char* ref) {
    void* object = findSymbol(diagram, ref);
    if (object) return object;
    object=findJunction(diagram,ref);
    return object ? object : findProbe(diagram,ref);
}
static void* makeJunction(void* diagram, const wchar_t* name, DiagramPoint point) {
    typedef void* (__cdecl* Allocate)(unsigned);
    Allocate allocate = (Allocate)symbol("MSVCR71.dll", "??2@YAPAXI@Z");
    void* klass = (void*)symbol("Od101dll.dll", "?classCODPinComp@CODPinComp@@2UCRuntimeClass@@A");
    unsigned bytes = klass && readable(klass,8) ? *(unsigned*)((char*)klass+4) : 0;
    typedef void* (__thiscall* Constructor)(void*, DiagramPoint, void*);
    Constructor ctor = (Constructor)symbol("Od101dll.dll", "??0CODPinComp@@QAE@VCPoint@@PAVCPen@@@Z");
    void** catalog = (void**)symbol("Od101dll.dll", "?m_pCatalog@CODParams@@1PAVCODCatalog@@A");
    typedef void* (__thiscall* FindPen)(void*, int, int, DWORD*);
    FindPen findPen = (FindPen)symbol("Od101dll.dll", "?FindPen@CODCatalog@@QAEPAVCPen@@HHPAK@Z");
    typedef void (__thiscall* SetText)(void*, const wchar_t*, void*);
    SetText text = (SetText)symbol("Od101dll.dll", "?SetText@CODConnector@@QAEXPB_WAAUODInstContext@@@Z");
    void* context = (void*)symbol("Od101dll.dll", "?DefIC@@3UODInstContext@@A");
    typedef void* (__thiscall* Box)(void*, RECT*, void*);
    typedef void (__thiscall* Add)(void*, void*, DiagramPoint, void*);
    Add add = (Add)dynamicMethod(diagram,"??_7CODDiagram@@6B@","?AddComponent@CODDiagram@@UAEXPAVCODComponent@@VCPoint@@PAVCODView@@@Z");
    void* view = nativeView(".?AVCODView@@");
    if (!allocate || bytes!=0x30 || !ctor || !catalog || !*catalog || !findPen || !text || !context || !add || !view) return nullptr;
    DWORD color = RGB(0,0,0);
    void* pen = findPen(*catalog,0,1,&color);
    if (!pen) return nullptr;
    // CODConnector's integer center uses Multisim's native 9-unit grid.
    typedef void (__cdecl* Snap)(DiagramPoint*);
    Snap snap = (Snap)symbol("Od101dll.dll", "?MovePointOnGrid@CODDiagram@@SAXAAVCPoint@@@Z");
    if (!snap) return nullptr;
    snap(&point);
    operationStage = "junction_construct";
    void* object = ctor(allocate(bytes),point,pen);
    text(object,name,context);
    Box box = (Box)dynamicMethod(object,"??_7CODComponent@@6B@","?GetBox@CODComponent@@UAE?AVCRect@@AAUODInstContext@@@Z");
    if (!box) return nullptr;
    RECT bounds = {}; box(object,&bounds,context);
    operationStage = "junction_append";
    add(diagram,object,{bounds.left,bounds.top},view);
    return object;
}
