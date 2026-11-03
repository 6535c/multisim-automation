static void* probeData(void* object,bool create=false) {
    if (!isProbe(object) || !readable(object,0x7c)) return nullptr;
    void* data=*(void**)((char*)object+0x6c);
    if (!data && create) {
        unsigned char* code=(unsigned char*)GetModuleHandleA("SCH_CAP.dll")+0x18dc50;
        if (!executable(code) || code[0]!=0x6a || code[1]!=0xff || code[2]!=0x68 || code[7]!=0x64 || code[8]!=0xa1) return nullptr;
        typedef void* (__thiscall* Getter)(void*);
        operationStage="create_native_probe_data";
        data=((Getter)code)(object);
    }
    const char* type=runtimeName(data);
    return type && !strcmp(type,"CiProbeComp") ? data : nullptr;
}
static void* findProbe(void* diagram,const char* name) {
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*,void**);
    Getter first=(Getter)symbol("Od101dll.dll","?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    Next next=(Next)symbol("Od101dll.dll","?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    void* root=diagramRoot(diagram); void* position=root && first ? first(root) : nullptr;
    for (unsigned i=0;position && next && i<16384;++i) {
        void* object=next(root,&position);
        if (!isProbe(object)) continue;
        void* data=probeData(object);
        if (data && valueString(data,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll")==name) return object;
    }
    return nullptr;
}
static void describeProbe(FILE* file,void* object) {
    typedef void* (__thiscall* Getter)(void*);
    void* data=probeData(object);
    fprintf(file,",\"probe_data\":\"%p\",\"ref\":",data);
    jsonString(file,data ? valueString(data,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll") : "");
    typedef void* (__thiscall* Ref)(void*);
    void* refObject=(char*)object+0x48;
    Ref refGetter=(Ref)dynamicMethod(refObject,"??_7CIITSymbolComp@@6BIRefDesable@@@","?GetMultisectionInstanceRefDes@CIITSymbolComp@@UAEAAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ");
    void* reference=refGetter ? refGetter(refObject) : nullptr;
    fprintf(file,",\"instance_refdes\":"); jsonString(file,readable(reference,4) ? utf8(*(const wchar_t**)reference) : "");
    typedef void* (__cdecl* Info)(void*);
    Info getInfo=(Info)symbol("SCH_CAP.dll","?GetInstanceInfo@CComponentBridge@@SAPAVCiaCollString@@PAVCiComponent@@@Z");
    void* info=data && getInfo ? getInfo(data) : nullptr;
    typedef int (__thiscall* Count)(void*);
    Count count=(Count)symbol("Circexp.dll","?GetParamCount@CiaCollString@@QBEHXZ");
    typedef void* (__thiscall* Param)(void*,void*,int);
    Param param=(Param)symbol("Circexp.dll","?GetParam@CiaCollString@@QBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@H@Z");
    fprintf(file,",\"instance_metadata\":[");
    for (int i=0,n=info && count ? count(info) : 0;param && i<n && i<128;++i) {
        NativeString value; param(info,&value,i);
        if (i) fputc(',',file); jsonString(file,utf8((const wchar_t*)value.data));
    }
    fprintf(file,"]");
    fprintf(file,",\"instrument_id\":");
    jsonString(file,valueString((char*)object+0x38,"?GetProbeInstrUniqueID@CProbeInstrumentComp@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","SCH_CAP.dll"));
    typedef void (__thiscall* Center)(void*,float*,float*);
    Center center=(Center)symbol("Od101dll.dll","?GetCenter@CODConnector@@UAEXAAM0@Z");
    float x=0,y=0; if (center) center(object,&x,&y);
    fprintf(file,",\"point\":[%.9g,%.9g]",x,y);
    void* graphic=*(void**)((char*)object+4);
    typedef bool (__thiscall* Attachment)(void*,void**,int*,void**,int*);
    Attachment attachment=(Attachment)((char*)GetModuleHandleA("SCH_CAP.dll")+0x18de70);
    void* positive=nullptr; void* negative=nullptr; int positiveDirection=-1,negativeDirection=-1;
    bool attached=attachment(object,&positive,&positiveDirection,&negative,&negativeDirection);
    fprintf(file,",\"native_attachment\":{\"ok\":%s,\"graphic_direction\":%d,\"positive\":\"%p\",\"negative\":\"%p\",\"directions\":[%d,%d]}",
        attached ? "true" : "false",readable(graphic,0x44) ? *(int*)((char*)graphic+0x40) : -1,positive,negative,positiveDirection,negativeDirection);
    fprintf(file,",\"voltage_target\":"); jsonString(file,readable((char*)object+0x70,4) ? utf8(*(const wchar_t**)((char*)object+0x70)) : "");
    fprintf(file,",\"native_strings\":{");
    const unsigned stringOffsets[]={0x4c,0x50,0x58,0x5c,0x60,0x64,0x70,0x74};
    for (unsigned i=0;i<sizeof(stringOffsets)/sizeof(*stringOffsets);++i) {
        fprintf(file,"%s\"%x\":",i ? "," : "",stringOffsets[i]);
        jsonString(file,utf8(*(const wchar_t**)((char*)object+stringOffsets[i])));
    }
    fprintf(file,"}");
    typedef void* (__thiscall* Tree)(void*);
    Tree getTree=(Tree)symbol("DrwEngine.dll","?GetTreeInstance@CBaseView@@QBEPAVCTreeInstance@@XZ");
    void* baseView=globalObject("?GetSheetView@@YAPAVCBaseView@@XZ");
    void* tree=baseView && getTree ? getTree(baseView) : nullptr;
    typedef void* (__thiscall* ListCtor)(void*,int);
    typedef void (__thiscall* ListDtor)(void*);
    typedef int (__thiscall* Params)(void*,void*,void*);
    ListCtor ctor=(ListCtor)symbol("MFC71U.DLL",MAKEINTRESOURCEA(558));
    ListDtor dtor=(ListDtor)symbol("MFC71U.DLL",MAKEINTRESOURCEA(746));
    Params getParams=(Params)symbol("SCH_CAP.dll","?GetProbeParamSettings@CProbeInstrumentComp@@QAEHPAVCStringList@@PAVCTreeInstance@@@Z");
    fprintf(file,",\"native_parameters\":[");
    if (ctor && dtor && getParams && tree) {
        alignas(4) unsigned char list[128]={}; ctor(list,10);
        operationStage="inspect_probe_parameters";
        int status=getParams((char*)object+0x38,list,tree);
        void* cursor=*(void**)(list+4);
        for (unsigned i=0;status && cursor && readable(cursor,12) && i<512;++i) {
            if (i) fputc(',',file);
            jsonString(file,utf8(*(const wchar_t**)((char*)cursor+8)));
            cursor=*(void**)cursor;
        }
        dtor(list);
    }
    fprintf(file,"]");
    fprintf(file,",\"info_text\":");
    jsonString(file,valueString((char*)object+0x30,"?GetInfoText@CIITInfoExtension@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ"));
    for (const char* side:{"pos","neg"}) {
        Getter node=(Getter)symbol("Circexp.dll",side[0]=='p' ? "?GetPosNode@CiProbeComp@@QAEPAVCiNode@@XZ" : "?GetNegNode@CiProbeComp@@QAEPAVCiNode@@XZ");
        Getter link=(Getter)symbol("Circexp.dll",side[0]=='p' ? "?GetPosLink@CiProbeComp@@QAEPAVCObject@@XZ" : "?GetNegLink@CiProbeComp@@QAEPAVCObject@@XZ");
        void* n=data && node ? node(data) : nullptr;
        fprintf(file,",\"%s_node\":\"%p\",\"%s_link\":\"%p\",\"%s_node_name\":",side,n,side,data && link ? link(data) : nullptr,side);
        jsonString(file,n ? valueString(n,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll") : "");
    }
    Getter connections=(Getter)symbol("Od101dll.dll","?GetConnections@CODConnector@@QBEPAVCODCompList@@XZ");
    Getter parent=(Getter)symbol("Od101dll.dll","?GetParent@CODComponent@@QAEPAV1@XZ");
    void* list=connections ? connections(object) : nullptr;
    void* position=readable(list,16) ? *(void**)((char*)list+4) : nullptr;
    fprintf(file,",\"connections\":[");
    for (unsigned i=0;position && readable(position,12) && i<16384;++i) {
        void* connector=*(void**)((char*)position+8);
        void* owner=connector && parent ? parent(connector) : nullptr;
        const char* type=runtimeName(owner);
        fprintf(file,"%s{\"connector\":\"%p\",\"parent\":\"%p\",\"parent_class\":\"%s\"}",i ? "," : "",connector,owner,type ? type : "unknown");
        position=*(void**)position;
    }
    fprintf(file,"]");
}
struct ProbeHiddenScope {
        void* settings=nullptr; int previous=0;
        typedef void (__thiscall* Set)(void*,int);
        Set set=nullptr;
        ProbeHiddenScope() {
            typedef void* (__cdecl* Settings)();
            Settings get=(Settings)symbol("library.dll","?GetSettings@@YAPAVCSet@@XZ");
            typedef int (__thiscall* State)(void*);
            State state=(State)symbol("library.dll","?GetAppState@CSet@@QBE?AW4EW_APPSTATE@1@XZ");
            set=(Set)symbol("library.dll","?SetAppState@CSet@@QAEXW4EW_APPSTATE@1@@Z");
            settings=get && state && set ? get() : nullptr;
            if (settings) { previous=state(settings); set(settings,2); }
        }
        ~ProbeHiddenScope() { if (settings) set(settings,previous); }
};
static void* makeProbe(void* sheet,void* diagram,const wchar_t* name,unsigned kind,DiagramPoint at) {
    if (!name || !*name || findEditable(diagram,utf8(name).c_str())) return nullptr;
    ProbeHiddenScope hidden;
    if (!hidden.settings) return nullptr;
    typedef void* (__thiscall* Create)(void*,unsigned,const DiagramPoint*);
    Create create=(Create)symbol("SCH_CAP.dll","?CreateStaticProbeComp@CIITView@@QAEPAVCIITProbeComponent@@IPBVCPoint@@@Z");
    void* view=nativeView();
    if (!create || !view) return nullptr;
    operationStage="create_static_probe";
    void* object=create(view,kind,&at);
    if (!isProbe(object)) return nullptr;
    void* data=probeData(object,true);
    typedef int (__thiscall* Rename)(void*,const wchar_t*);
    Rename rename=(Rename)symbol("Circexp.dll","?SetLocalName@CiElement@@QAEHPB_W@Z");
    typedef int (__thiscall* Adopt)(void*,void*);
    Adopt adopt=(Adopt)symbol("Circexp.dll","?AddProbe@CiCircuit@@QAEHPAVCiProbeComp@@@Z");
    if (!data || !rename || !adopt || !rename(data,name) || !adopt(currentCircuit(sheet),data)) return nullptr;
    typedef void* (__thiscall* Box)(void*,RECT*,void*);
    Box box=(Box)dynamicMethod(object,"??_7CODComponent@@6B@","?GetBox@CODComponent@@UAE?AVCRect@@AAUODInstContext@@@Z");
    typedef void (__thiscall* Add)(void*,void*,DiagramPoint,void*);
    Add add=(Add)dynamicMethod(diagram,"??_7CODDiagram@@6B@","?AddComponent@CODDiagram@@UAEXPAVCODComponent@@VCPoint@@PAVCODView@@@Z");
    void* context=(void*)symbol("Od101dll.dll","?DefIC@@3UODInstContext@@A");
    if (!box || !add || !context) return nullptr;
    RECT bounds={}; box(object,&bounds,context);
    operationStage="adopt_static_probe_graphic";
    add(diagram,object,{bounds.left,bounds.top},nativeView(".?AVCODView@@"));
    typedef bool (__thiscall* Finish)(void*,void*);
    Finish finish=(Finish)symbol("SCH_CAP.dll","?EndPlaceStaticProbe@CIITView@@QAE_NPAVCIITProbeComponent@@@Z");
    fprintf(currentOutput,",\"object\":\"%p\",\"data\":\"%p\"",object,data);
    operationStage="finish_static_probe";
    return finish && finish(view,object) ? object : nullptr;
}
static bool setProbeDirection(void* object,DiagramPoint at,DiagramPoint positive) {
    if (!isProbe(object) || (at.x==positive.x && at.y==positive.y)) return false;
    unsigned char* module=(unsigned char*)GetModuleHandleA("SCH_CAP.dll");
    unsigned char* directionCode=module+0x17bbb0;
    unsigned char* setterCode=module+0x18db70;
    if (!executable(directionCode) || memcmp(directionCode,"\x8b\x44\x24\x04\x8b\x48\x04",7) ||
        !executable(setterCode) || memcmp(setterCode,"\x8b\x49\x04\x6a\x00",5)) return false;
    typedef int (__cdecl* Direction)(const DiagramPoint*,const DiagramPoint*);
    typedef void (__thiscall* Set)(void*,int);
    operationStage="orient_static_probe";
    int direction=((Direction)directionCode)(&at,&positive);
    if (direction<2 || direction>5) return false;
    ((Set)setterCode)(object,direction);
    typedef bool (__thiscall* Attachment)(void*,void**,int*,void**,int*);
    void* a=nullptr; void* b=nullptr; int da=0,db=0;
    return ((Attachment)(module+0x18de70))(object,&a,&da,&b,&db);
}
static bool bindProbe(void* object,void* node) {
    if (!isProbe(object) || !node) return false;
    ProbeHiddenScope hidden;
    if (!hidden.settings) return false;
    typedef void* (__thiscall* Getter)(void*);
    Getter treeGetter=(Getter)symbol("DrwEngine.dll","?GetTreeInstance@CBaseView@@QBEPAVCTreeInstance@@XZ");
    void* view=globalObject("?GetSheetView@@YAPAVCBaseView@@XZ");
    void* tree=view && treeGetter ? treeGetter(view) : nullptr;
    typedef void* (__thiscall* Name)(void*,void*);
    Name name=(Name)symbol("Circexp.dll","?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ");
    typedef void (__thiscall* Connect)(void*,const NativeString*,void*,bool);
    void* instrument=(char*)object+0x38;
    Connect connect=(Connect)dynamicMethod(instrument,"??_7CProbeInstrumentComp@@6B@","?ConnectProbeToNode@CProbeInstrumentComp@@UAEXABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@PAVCTreeInstance@@_N@Z","SCH_CAP.dll");
    if (!tree || !name || !connect) return false;
    NativeString value; name(node,&value);
    operationStage="bind_static_probe_to_node";
    connect(instrument,&value,tree,false);
    typedef bool (__thiscall* Finish)(void*,void*);
    Finish finish=(Finish)symbol("SCH_CAP.dll","?EndPlaceStaticProbe@CIITView@@QAE_NPAVCIITProbeComponent@@@Z");
    return finish && finish(nativeView(),object);
}
static bool refreshProbeBindings(void* diagram) {
    typedef void* (__thiscall* Getter)(void*);
    typedef void* (__thiscall* Next)(void*,void**);
    Getter first=(Getter)symbol("Od101dll.dll","?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
    Next next=(Next)symbol("Od101dll.dll","?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
    Getter connections=(Getter)symbol("Od101dll.dll","?GetConnections@CODConnector@@QBEPAVCODCompList@@XZ");
    if (!first || !next || !connections) return false;
    void* root=diagramRoot(diagram); void* position=root ? first(root) : nullptr;
    for (unsigned i=0;position && i<16384;++i) {
        void* object=next(root,&position);
        if (!isProbe(object)) continue;
        void* list=connections(object);
        // Disconnected imported probes remain disconnected. Inline probes need
        // two live link connections, as required by the native attachment code.
        if (!readable(list,16) || *(int*)((char*)list+12)!=2) continue;
        typedef bool (__thiscall* Attachment)(void*,void**,int*,void**,int*);
        Attachment attachment=(Attachment)((char*)GetModuleHandleA("SCH_CAP.dll")+0x18de70);
        void* positive=nullptr; void* negative=nullptr; int da=0,db=0;
        if (!attachment(object,&positive,&da,&negative,&db)) {
            auto wires=incidentWires(diagram,object);
            typedef void (__thiscall* Vertices)(void*,DiagramPoint**,int*);
            Vertices vertices=(Vertices)symbol("Od101dll.dll","?GetVertices@CODLinkComp@@QAEXAAPAVCPoint@@AAH@Z");
            typedef void (__cdecl* Free)(void*);
            Free release=(Free)symbol("MSVCR71.dll","??_V@YAXPAX@Z");
            DiagramPoint at={}; terminalConnector(object,&at);
            bool oriented=false;
            for (void* wire:wires) {
                DiagramPoint* points=nullptr; int count=0;
                if (!vertices || !release) return false;
                vertices(wire,&points,&count);
                if (points && count>=2) {
                    if (points[0].x==at.x && points[0].y==at.y)
                        oriented=setProbeDirection(object,at,points[1]);
                    else if (points[count-1].x==at.x && points[count-1].y==at.y)
                        oriented=setProbeDirection(object,at,points[count-2]);
                }
                if (points) release(points);
                if (oriented) break;
            }
            if (!oriented) return false;
        }
        void* node=terminalNode(object);
        if (!node) return false;
        std::string name=valueString(node,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll");
        if (name!=utf8(*(const wchar_t**)((char*)object+0x70)) && !bindProbe(object,node)) return false;
    }
    return true;
}
