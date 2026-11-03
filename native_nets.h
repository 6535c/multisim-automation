// Native net name/label attributes and their CODNodeTextComp presentation.
static void* circuitPreferences(void* diagram) {
    void* klass=runtimeClass(diagram);
    unsigned bytes=klass ? *(unsigned*)((char*)klass+4) : 0;
    for (unsigned i=4;i+4<=bytes && i<0x1000;i+=4) {
        void* candidate=*(void**)((char*)diagram+i);
        const char* type=runtimeName(candidate);
        if (type && !strcmp(type,"CIITCircuitPrefs")) return candidate;
    }
    return nullptr;
}
static void* nodeAttribute(void* node,int index) {
    typedef void* (__thiscall* Getter)(void*,int);
    Getter get=(Getter)symbol("Circexp.dll","?GetAttribute@CiElement@@QBEPAVCiAttribute@@H@Z");
    return node && get ? get(node,index) : nullptr;
}
static void* ensureNodeAttribute(void* node,int index,const char* factory) {
    void* attribute=nodeAttribute(node,index);
    if (attribute) return attribute;
    typedef void* (__stdcall* Create)();
    Create create=(Create)symbol("Circexp.dll",factory);
    typedef int (__thiscall* Add)(void*,void*,int);
    Add add=(Add)symbol("Circexp.dll","?AddAttribute@CiElement@@QAEHPAVCiAttribute@@H@Z");
    if (!create || !add) return nullptr;
    attribute=create();
    return attribute && add(node,attribute,index) ? attribute : nullptr;
}
static std::string netCaption(void* node) {
    void* label=nodeAttribute(node,12);
    return label ? valueString(label,"?GetLabel@CiaLabel@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll") : "";
}
static bool prepareNetText(void* node,void* prefs,NativeString& result) {
    typedef void* (__thiscall* Constructor)(void*);
    Constructor ctor=(Constructor)symbol("Setattr.dll","??0CUNode@@QAE@XZ");
    typedef void (__thiscall* Destructor)(void*);
    Destructor dtor=(Destructor)symbol("Setattr.dll","??1CUNode@@UAE@XZ");
    typedef void (__thiscall* SetNode)(void*,void*);
    SetNode set=(SetNode)symbol("Setattr.dll","?SetNode@CUNode@@QAEXPAVCiNode@@@Z");
    typedef void* (__thiscall* Prepare)(void*,void*,void*);
    Prepare prepare=(Prepare)symbol("Setattr.dll","?PrepareNameString@CUNode@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@PAVCIITCircuitPrefs@@@Z");
    if (!node || !prefs || !ctor || !dtor || !set || !prepare) return false;
    __declspec(align(8)) unsigned char handler[512]={};
    ctor(handler); set(handler,node); prepare(handler,&result,prefs); dtor(handler);
    return true;
}
static void* createNetText(void* diagram,void* wire,const wchar_t* value,const DiagramPoint* position) {
    typedef void* (__cdecl* Allocate)(unsigned);
    Allocate allocate=(Allocate)symbol("MSVCR71.dll","??2@YAPAXI@Z");
    void* klass=(void*)symbol("Od101dll.dll","?classCODNodeTextComp@CODNodeTextComp@@2UCRuntimeClass@@A");
    unsigned bytes=klass ? *(unsigned*)((char*)klass+4) : 0;
    typedef void* (__thiscall* Constructor)(void*,const wchar_t*);
    Constructor ctor=(Constructor)symbol("Od101dll.dll","??0CODNodeTextComp@@QAE@PB_W@Z");
    typedef void (__thiscall* Add)(void*,void*,DiagramPoint,void*);
    Add add=(Add)dynamicMethod(diagram,"??_7CODDiagram@@6B@","?AddComponent@CODDiagram@@UAEXPAVCODComponent@@VCPoint@@PAVCODView@@@Z");
    typedef void (__thiscall* SetPointer)(void*,void*);
    SetPointer setText=(SetPointer)symbol("SCH_CAP.dll","?SetNodeText@CIITLinkComp@@UAEXPAVCODNodeTextComp@@@Z");
    SetPointer addLink=(SetPointer)symbol("Od101dll.dll","?AddLinkComp@CODNodeTextComp@@QAEXPAVCODLinkComp@@@Z");
    void* view=nativeView(".?AVCODView@@");
    if (!allocate || !ctor || !add || !setText || !addLink || !view || bytes!=0x40) return nullptr;
    operationStage="create_native_net_text";
    void* text=ctor(allocate(bytes),value);
    DiagramPoint at={};
    if (position) at=*position;
    else {
        typedef void (__thiscall* Endpoints)(void*,DiagramPoint*,DiagramPoint*);
        Endpoints endpoints=(Endpoints)symbol("Od101dll.dll","?GetEndpoints@CODLinkComp@@QAEXAAVCPoint@@0@Z");
        DiagramPoint a={},b={}; if (endpoints) endpoints(wire,&a,&b);
        at={(a.x+b.x)/2,(a.y+b.y)/2-20};
    }
    add(diagram,text,at,view);
    setText(wire,text); addLink(text,wire);
    return text;
}
static int netNamePriority(void* node) {
    if (!node) return -1;
    std::string name=valueString(node,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll");
    if (name=="0") return 2;
    void* display=nodeAttribute(node,11);
    typedef int (__thiscall* Flag)(void*);
    Flag show=(Flag)symbol("Circexp.dll","?GetShowReferenceID@CiaDisplay@@QAEHXZ");
    bool numeric=!name.empty() && name.find_first_not_of("0123456789")==std::string::npos;
    return (display && show && show(display)) || (!numeric && name.rfind("N_AUTO_",0)!=0) ? 1 : 0;
}
static bool sameNetName(const std::string& a,const std::string& b) {
    if (a==b) return true;
    int na=MultiByteToWideChar(CP_UTF8,0,a.c_str(),-1,nullptr,0),nb=MultiByteToWideChar(CP_UTF8,0,b.c_str(),-1,nullptr,0);
    if (na<1 || nb<1) return false;
    std::vector<wchar_t> wa(na),wb(nb);
    MultiByteToWideChar(CP_UTF8,0,a.c_str(),-1,wa.data(),na); MultiByteToWideChar(CP_UTF8,0,b.c_str(),-1,wb.data(),nb);
    return CompareStringOrdinal(wa.data(),-1,wb.data(),-1,TRUE)==CSTR_EQUAL;
}
static bool propagateMergedNetNames(void* diagram,const std::string& oldA,const std::string& oldB,void* winner) {
    std::string name=valueString(winner,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll");
    typedef void* (__thiscall* Constructor)(void*,void*);
    Constructor ctor=(Constructor)symbol("Circexp.dll","??0CiIterNodesInCirc@@QAE@PBVCiCircuit@@@Z");
    typedef void* (__thiscall* Getter)(void*);
    Getter reset=(Getter)symbol("Circexp.dll","?Reset@CiIterNodesInCirc@@QAEPAVCiNode@@XZ");
    Getter step=(Getter)symbol("Circexp.dll","?GetNext@CiIterNodesInCirc@@QAEPAVCiNode@@XZ");
    typedef int (__thiscall* Rename)(void*,const wchar_t*);
    Rename rename=(Rename)symbol("Circexp.dll","?SetLocalName@CiElement@@QAEHPB_W@Z");
    void* circuit=currentCircuit(globalObject("?GetDrwSubSheet@@YAPAVCDrwSubSheet@@XZ"));
    if (!circuit || !ctor || !reset || !step || !rename) return false;
    int length=MultiByteToWideChar(CP_UTF8,0,name.c_str(),-1,nullptr,0);
    std::vector<wchar_t> wide(length);
    if (length<1 || !MultiByteToWideChar(CP_UTF8,0,name.c_str(),-1,wide.data(),length)) return false;
    __declspec(align(8)) unsigned char iterator[128]={}; ctor(iterator,circuit);
    void* node=reset(iterator);
    for (unsigned i=0;node && i<16384;++i) {
        std::string prior=valueString(node,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll");
        if (prior!=name && ((!oldA.empty() && sameNetName(prior,oldA)) || (!oldB.empty() && sameNetName(prior,oldB))))
            if (!rename(node,wide.data())) return false;
        node=step(iterator);
    }
    return !node;
}
static bool refreshNetLabels(void* diagram) {
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*,void**);
    Getter first=(Getter)symbol("Od101dll.dll","?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    Next next=(Next)symbol("Od101dll.dll","?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    Getter getNode=(Getter)symbol("SCH_CAP.dll","?GetNode@CIITLinkComp@@QAEPAVCiNode@@XZ");
    Getter getText=(Getter)symbol("SCH_CAP.dll","?GetNodeText@CIITLinkComp@@UAEPAVCODNodeTextComp@@XZ");
    typedef void (__thiscall* Pointer)(void*,void*);
    Pointer set=(Pointer)symbol("SCH_CAP.dll","?SetNodeText@CIITLinkComp@@UAEXPAVCODNodeTextComp@@@Z");
    Pointer remove=(Pointer)symbol("Od101dll.dll","?RemoveLink@CODNodeTextComp@@QAEXPAVCODLinkComp@@@Z");
    typedef void (__thiscall* Text)(void*,const wchar_t*,void*);
    Text update=(Text)symbol("Od101dll.dll","?SetText@CODTextComp@@QAEXPB_WAAUODInstContext@@@Z");
    void* context=(void*)symbol("Od101dll.dll","?DefIC@@3UODInstContext@@A");
    void* prefs=circuitPreferences(diagram);
    if (!first || !next || !getNode || !getText || !set || !remove || !update || !context || !prefs) return false;
    struct Group { void* node; std::vector<void*> wires; std::string text; };
    std::vector<Group> groups;
    std::vector<void*> texts;
    void* root=diagramRoot(diagram); void* position=first(root);
    for (unsigned i=0;position && i<16384;++i) {
        void* wire=next(root,&position);
        const char* type=runtimeName(wire);
        if (!type || strcmp(type,"CIITLinkComp")) continue;
        void* node=getNode(wire); if (!node) continue;
        auto group=std::find_if(groups.begin(),groups.end(),[&](const Group& g){return g.node==node;});
        if (group==groups.end()) {
            NativeString formatted;
            if (!prepareNetText(node,prefs,formatted)) return false;
            groups.push_back({node,{wire},utf8((const wchar_t*)formatted.data)});
        } else group->wires.push_back(wire);
        void* text=getText(wire);
        if (text && std::find(texts.begin(),texts.end(),text)==texts.end()) texts.push_back(text);
    }
    // Shared native text may straddle newly split nodes. Retain it on the
    // group whose electrical name still matches, and detach the other links.
    for (void* text:texts) {
        int owner=-1;
        std::string before=valueString(text,"?GetText@CODTextComp@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ");
        for (unsigned i=0;i<groups.size();++i) for (void* wire:groups[i].wires) if (getText(wire)==text) {
            if (owner<0 || groups[i].text==before) owner=i;
        }
        for (unsigned i=0;i<groups.size();++i) if ((int)i!=owner)
            for (void* wire:groups[i].wires) if (getText(wire)==text) { remove(text,wire); set(wire,nullptr); }
    }
    for (const Group& group:groups) {
        NativeString formatted;
        if (!prepareNetText(group.node,prefs,formatted)) return false;
        bool hasText=false;
        for (void* wire:group.wires) if (void* text=getText(wire)) { update(text,(const wchar_t*)formatted.data,context); hasText=true; }
        if (!hasText && !group.text.empty() && !createNetText(diagram,group.wires[0],(const wchar_t*)formatted.data,nullptr)) return false;
    }
    return true;
}
static bool setNetLabel(void* diagram,void* terminal,const std::wstring& name,const std::wstring* caption,bool visible,const DiagramPoint* position) {
    void* node=terminalNode(terminal);
    if (!node) return false;
    std::string previous=valueString(node,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll");
    if (previous=="0" && !name.empty() && name!=L"0") {
        fprintf(currentOutput,",\"error\":\"Ground net name must remain 0; use its caption instead\""); return false;
    }
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*,void**);
    Getter first=(Getter)symbol("Od101dll.dll","?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    Next next=(Next)symbol("Od101dll.dll","?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    Getter getNode=(Getter)symbol("SCH_CAP.dll","?GetNode@CIITLinkComp@@QAEPAVCiNode@@XZ");
    Getter getText=(Getter)symbol("SCH_CAP.dll","?GetNodeText@CIITLinkComp@@UAEPAVCODNodeTextComp@@XZ");
    typedef void (__thiscall* SetPointer)(void*,void*);
    SetPointer setText=(SetPointer)symbol("SCH_CAP.dll","?SetNodeText@CIITLinkComp@@UAEXPAVCODNodeTextComp@@@Z");
    SetPointer addLink=(SetPointer)symbol("Od101dll.dll","?AddLinkComp@CODNodeTextComp@@QAEXPAVCODLinkComp@@@Z");
    void* prefs=circuitPreferences(diagram);
    if (!first || !next || !getNode || !getText || !setText || !addLink || !prefs) return false;
    std::vector<void*> wires,texts;
    void* root=diagramRoot(diagram); void* cursor=first(root);
    for (unsigned i=0;cursor && i<16384;++i) {
        void* item=next(root,&cursor);
        const char* type=runtimeName(item);
        if (!type || strcmp(type,"CIITLinkComp") || getNode(item)!=node) continue;
        wires.push_back(item);
        void* text=getText(item);
        if (text && std::find(texts.begin(),texts.end(),text)==texts.end()) texts.push_back(text);
    }
    if (wires.empty()) return false;
    operationStage="set_native_net_attributes";
    typedef int (__thiscall* Rename)(void*,const wchar_t*);
    Rename rename=(Rename)symbol("Circexp.dll","?SetLocalName@CiElement@@QAEHPB_W@Z");
    if (!rename || (!name.empty() && !rename(node,name.c_str()))) return false;
    void* display=ensureNodeAttribute(node,11,"?CreateObject@CiaDisplay@@SGPAVCObject@@XZ");
    typedef void (__thiscall* SetFlag)(void*,int);
    SetFlag show=(SetFlag)symbol("Circexp.dll","?SetShowReferenceID@CiaDisplay@@QAEXH@Z");
    if (!display || !show) return false;
    show(display,visible ? 1 : 0);
    typedef int (__thiscall* RemoveAttribute)(void*,int);
    RemoveAttribute remove=(RemoveAttribute)symbol("Circexp.dll","?RemoveAttribute@CiElement@@QAEHH@Z");
    if (caption && caption->empty()) { if (nodeAttribute(node,12) && remove) remove(node,12); }
    else if (caption) {
        void* label=ensureNodeAttribute(node,12,"?CreateObject@CiaLabel@@SGPAVCObject@@XZ");
        typedef void (__thiscall* SetCaption)(void*,void*);
        SetCaption set=(SetCaption)symbol("Circexp.dll","?SetLabel@CiaLabel@@QAEXV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
        if (!label || !set) return false;
        // Native by-value CString is consumed by the callee.
        NativeString value(caption->c_str()); void* raw=value.data; value.data=nullptr;
        set(label,raw);
    }
    typedef bool (__thiscall* Pref)(void*,int,const int*);
    Pref preference=(Pref)symbol("Prefs.dll","?SetPrefs@CPreferences@@QAE_NW4PREFS@CKeys@@ABH@Z");
    int specific=2;
    if (!preference || !preference(prefs,7,&specific)) return false;
    NativeString formatted;
    if (!prepareNetText(node,prefs,formatted)) return false;
    typedef void (__thiscall* Text)(void*,const wchar_t*,void*);
    Text update=(Text)symbol("Od101dll.dll","?SetText@CODTextComp@@QAEXPB_WAAUODInstContext@@@Z");
    void* context=(void*)symbol("Od101dll.dll","?DefIC@@3UODInstContext@@A");
    void* view=nativeView(".?AVCODView@@");
    if (!update || !context || !view) return false;
    DiagramPoint ignored={}; void* connector=terminalConnector(terminal,&ignored);
    Getter source=(Getter)symbol("Od101dll.dll","?GetSourceConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    Getter target=(Getter)symbol("Od101dll.dll","?GetTargetConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
    void* anchor=wires[0];
    for (void* wire:wires) if (source && target && (source(wire)==connector || target(wire)==connector)) { anchor=wire; break; }
    if (texts.empty()) {
        void* text=createNetText(diagram,anchor,(const wchar_t*)formatted.data,position);
        if (!text) return false;
        texts.push_back(text);
    }
    void* active=getText(anchor);
    if (!active) {
        active=texts[0];
        SetPointer removeLink=(SetPointer)symbol("Od101dll.dll","?RemoveLink@CODNodeTextComp@@QAEXPAVCODLinkComp@@@Z");
        if (!removeLink) return false;
        for (void* wire:wires) if (getText(wire)==active) { removeLink(active,wire); setText(wire,nullptr); }
        setText(anchor,active); addLink(active,anchor);
    }
    operationStage="refresh_native_net_text";
    for (void* text:texts) {
        update(text,(const wchar_t*)formatted.data,context);
        if (position && text==active) {
            typedef void* (__thiscall* Box)(void*,RECT*,void*);
            Box box=(Box)dynamicMethod(text,"??_7CODComponent@@6B@","?GetBox@CODComponent@@UAE?AVCRect@@AAUODInstContext@@@Z");
            Getter graphic=(Getter)symbol("Od101dll.dll","?GetGraphic@CODComponent@@QAEPAVCODGraphic@@XZ");
            typedef void (__thiscall* Translate)(void*,float,float);
            Translate translate=(Translate)symbol("Od101dll.dll","?Translate@CODGraphic@@UAEXMM@Z");
            if (!box || !graphic || !translate) return false;
            RECT bounds={}; box(text,&bounds,context);
            translate(graphic(text),(float)(position->x-bounds.left),(float)(position->y-bounds.top));
        }
    }
    fprintf(currentOutput,",\"net_name\":");
    jsonString(currentOutput,valueString(node,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll"));
    fprintf(currentOutput,",\"display_text\":"); jsonString(currentOutput,utf8((const wchar_t*)formatted.data));
    return true;
}
