static std::string instrumentString(void* object,const char* name) {
    typedef void* (__cdecl* Getter)(void*,void*);
    Getter getter=(Getter)symbol("SCH_CAP.dll",name);
    NativeString result;
    if (getter && object) getter(&result,object);
    return utf8((const wchar_t*)result.data);
}
struct InstrumentContext {
    void* handler=nullptr;
    typedef void (__stdcall* End)();
    End end=nullptr;
    InstrumentContext() {
        typedef void* (__stdcall* Use)();
        Use use=(Use)symbol("SCH_CAP.dll","?UseHandler@CSnapinHandler@@KAPAV1@XZ");
        end=(End)symbol("SCH_CAP.dll","?EndUseHandler@CSnapinHandler@@KAXXZ");
        if (use && end) handler=use();
    }
    ~InstrumentContext() { if (handler && end) end(); }
    void* instance(void* object,const char* module,const char* vtable) {
        typedef void* (__thiscall* Reference)(void*,NativeString*,void*);
        typedef void* (__thiscall* Sink)(void*,const NativeString*);
        Reference reference=(Reference)symbol("SCH_CAP.dll","?GetInstRefID@CSnapinHandler@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@PAVCODSymbolComp@@@Z");
        Sink sink=(Sink)symbol("SCH_CAP.dll","?GetInstEventSink@CSnapinHandler@@QAEPAVCEventSink@@ABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
        void* table=(void*)symbol(module,vtable);
        if (!object || !isInstrumentSymbol(object) || !handler || !reference || !sink || !table) return nullptr;
        NativeString id;
        reference(handler,&id,object);
        void* instrument=sink(handler,&id);
        return readable(instrument,4) && *(void**)instrument==table ? instrument : nullptr;
    }
    bool getString(void* object,int property,NativeString* result) {
        typedef bool (__thiscall* Get)(void*,void*,const int*,NativeString*);
        Get get=(Get)symbol("SCH_CAP.dll","?GetMapVariantDataValue@CSnapinHandler@@QAE_NPAVCODSymbolComp@@ABW4VARIANT_DATA_VALUE@1@AAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
        return handler && get && get(handler,object,&property,result);
    }
};
static bool configureGenerator(void* object,int waveform,double frequency,double amplitude,double offset,double duty) {
    if (!object || !isInstrumentSymbol(object) || !std::isfinite(frequency) ||
        !std::isfinite(amplitude) || !std::isfinite(offset) || !std::isfinite(duty) ||
        waveform<0 || waveform>2 || frequency<=0 || amplitude<0 || duty<=0 || duty>=100) return false;
    InstrumentContext context;
    typedef void (__thiscall* Update)(void*);
    Update update=(Update)symbol("FGEN.DLL","?UpdateFromDatabase@CFunctionGeneratorInstrument@@QAEXXZ");
    operationStage="resolve_generator_event_sink";
    void* generator=context.instance(object,"FGEN.DLL","??_7CFunctionGeneratorInstrument@@6B@");
    if (!generator || !update) return false;
    wchar_t buffer[256];
    const double period=1/frequency;
    const double edge=(std::min)({0.001,duty/200,(100-duty)/200})*period;
    if (!std::isfinite(period) || !std::isfinite(offset+amplitude) ||
        !std::isfinite(offset-amplitude) || edge<=0 ||
        swprintf_s(buffer,L"V1.0\n%d\n%.17g\n%.17g\n%.17g\n%.17g\n%.17g\n",
                   waveform,frequency,duty,amplitude,offset,edge)<0) return false;
    NativeString state(buffer);
    typedef bool (__thiscall* Set)(void*,void*,const int*,const NativeString*);
    Set set=(Set)symbol("SCH_CAP.dll","?SetMapVariantDataValue@CSnapinHandler@@QAE_NPAVCODSymbolComp@@ABW4VARIANT_DATA_VALUE@1@ABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
    int property=0;
    operationStage="set_generator_state";
    if (!set || !set(context.handler,object,&property,&state)) return false;
    operationStage="reload_generator_state";
    update(generator);
    typedef int (__thiscall* Netlist)(void*,NativeString*);
    Netlist netlist=(Netlist)symbol("FGEN.DLL","?GetNetlist@CFunctionGeneratorInstrument@@UAEHAAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
    NativeString model(L"");
    if (!netlist || !netlist(generator,&model)) return false;
    fprintf(currentOutput,",\"generator_netlist\":");
    jsonString(currentOutput,utf8((const wchar_t*)model.data));
    return true;
}

static bool configureMeter(void* object,int mode) {
    if (mode<0 || mode>3) return false;
    InstrumentContext context;
    operationStage="resolve_meter_event_sink";
    void* meter=context.instance(object,"mulmeter.dll","??_7CMultimeterInstrument@@6B@");
    typedef bool (__thiscall* Set)(void*,int);
    typedef int (__thiscall* Get)(void*);
    Set set=(Set)symbol("mulmeter.dll","?SetMeasurementMode@CMultimeterInstrument@@UAE_NH@Z");
    Get get=(Get)symbol("mulmeter.dll","?GetMeasurementMode@CMultimeterInstrument@@UAEHXZ");
    if (!meter || !set || !get) return false;
    operationStage="set_meter_mode";
    int before=get(meter);
    if (!set(meter,mode) || get(meter)!=mode) return false;
    fprintf(currentOutput,",\"previous_mode\":%d,\"mode\":%d",before,mode);
    return true;
}
// Offsets used by the pinned SCOPE.DLL settings loader and archive serializer.
static constexpr int scopeScaleOffsets[]={0xd20,0xd50,0xd70};
static const char* scopeScaleNames[]={"seconds_per_div","a_volts_per_div","b_volts_per_div"};

static void describeScopeSettings(FILE* file,void* instance) {
    const char* data=(const char*)instance;
    const char* modes[]={"none","single","normal","auto"};
    const char* sources[]={"a","b","external"};
    const char* edges[]={"rising","falling"};
    int mode=*(const int*)(data+0xd30);
    int source=*(const int*)(data+0xd34);
    int edge=*(const int*)(data+0xd38);
    double level=*(const double*)(data+0xd40);
    fprintf(file,",\"scope_settings\":{");
    for (int i=0;i<3;++i) {
        double value=*(const double*)(data+scopeScaleOffsets[i]);
        fprintf(file,"%s\"%s\":",i ? "," : "",scopeScaleNames[i]);
        if (std::isfinite(value)) fprintf(file,"%.17g",value);
        else fprintf(file,"null");
    }
    fprintf(file,",\"trigger_mode\":"); jsonString(file,mode>=0 && mode<4 ? modes[mode] : "");
    fprintf(file,",\"trigger_source\":"); jsonString(file,source>=0 && source<3 ? sources[source] : "");
    fprintf(file,",\"trigger_edge\":"); jsonString(file,edge>=0 && edge<2 ? edges[edge] : "");
    fprintf(file,",\"trigger_level\":");
    if (std::isfinite(level)) fprintf(file,"%.17g",level);
    else fprintf(file,"null");
    fprintf(file,"}");
}

static bool configureScope(void* object,const std::vector<std::wstring>& fields) {
    InstrumentContext context;
    void* scope=context.instance(object,"SCOPE.DLL","??_7CScopeInstrument@@6B@");
    typedef void (__thiscall* Reset)(void*);
    Reset timeConstants=(Reset)symbol("SCOPE.DLL","?reset_time_constants@CScopeInstrument@@QAEXXZ");
    Reset displayOffsets=(Reset)symbol("SCOPE.DLL","?reset_display_offsets@CScopeInstrument@@AAEXXZ");
    typedef void (__thiscall* ResetTrigger)(void*,bool);
    ResetTrigger resetTrigger=(ResetTrigger)symbol("SCOPE.DLL","?initial_sampling_trigger_condition@CScopeInstrument@@QAEX_N@Z");
    if (!readable(scope,0xd78) || !timeConstants || !displayOffsets || !resetTrigger) return false;
    double values[3]={};
    bool changed=false;
    for (int i=0;i<3;++i) {
        if (fields[i+2].empty()) continue;
        wchar_t* end=nullptr;
        values[i]=wcstod(fields[i+2].c_str(),&end);
        if (*end || !std::isfinite(values[i]) || values[i]<1e-12 || values[i]>1e6) return false;
        changed=true;
    }
    int choices[]={-1,-1,-1};
    const int limits[]={4,3,2};
    const int offsets[]={0xd30,0xd34,0xd38};
    bool triggerChanged=false;
    for (int i=0;i<3;++i) {
        if (fields[i+5].empty()) continue;
        wchar_t* end=nullptr;
        long choice=wcstol(fields[i+5].c_str(),&end,10);
        if (*end || choice<0 || choice>=limits[i]) return false;
        choices[i]=(int)choice;
        triggerChanged=true;
    }
    double level=0;
    if (!fields[8].empty()) {
        wchar_t* end=nullptr;
        level=wcstod(fields[8].c_str(),&end);
        if (*end || !std::isfinite(level)) return false;
        triggerChanged=true;
    }
    if (!changed && !triggerChanged) return false;
    operationStage="set_scope_settings";
    for (int i=0;i<3;++i)
        if (values[i]) *(double*)((char*)scope+scopeScaleOffsets[i])=values[i];
    if (changed) {
        timeConstants(scope);
        displayOffsets(scope);
    }
    for (int i=0;i<3;++i)
        if (choices[i]>=0) *(int*)((char*)scope+offsets[i])=choices[i];
    if (!fields[8].empty()) *(double*)((char*)scope+0xd40)=level;
    if (triggerChanged) resetTrigger(scope,false);
    describeScopeSettings(currentOutput,scope);
    return true;
}

static bool readScope(FILE* file,void* object,int limit) {
    if (limit<2 || limit>20000) return false;
    InstrumentContext context;
    void* scope=context.instance(object,"SCOPE.DLL","??_7CScopeInstrument@@6B@");
    typedef void* (__thiscall* Getter)(void*);
    typedef long (__thiscall* Count)(void*);
    typedef long (__thiscall* Points)(void*,int,int*);
    typedef long (__thiscall* Point)(void*,int,int,double*,double*);
    typedef long (__thiscall* Label)(void*,unsigned,NativeString*);
    Getter page=(Getter)symbol("SCOPE.DLL","?GetInstrumentGrapherPage@CScopeInstrument@@QAEAAVCStdGrapherPage@@XZ");
    Getter graph=(Getter)symbol("Sharutil.dll","?GetStdGrapherGraph@CStdGrapherPage@@QAEAAVCStdGrapherGraph@@XZ");
    Count count=(Count)symbol("Sharutil.dll","?GetNumberOfTraces@CStdGrapherGraph@@UAEJXZ");
    Points points=(Points)symbol("Sharutil.dll","?GetDataNumberOfPoint@CGrapherGraph@@UAEJHAAH@Z");
    Point point=(Point)symbol("Sharutil.dll","?GetDataPoint@CGrapherGraph@@UAEJHHAAN0@Z");
    Label label=(Label)symbol("Sharutil.dll","?GetTraceLabel@CStdGrapherGraph@@UAEJIAAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
    if (!scope || !page || !graph || !count || !points || !point) return false;
    void* g=graph(page(scope));
    if (!g) return false;
    long traces=count(g);
    fprintf(file,",\"channels\":[");
    // The CStdGrapherGraph count thunk adjusts to CGrapherGraph at +8.
    void* base=(char*)g+8;
    bool comma=false, valid=true;
    for (int i=0;i<traces && i<2;++i) {
        int n=0;
        operationStage="scope_trace_count";
        if (points(base,i,&n)!=0 || n<0) { valid=false; break; }
        if (!n) continue;
        fprintf(file,"%s{\"channel\":\"%c\",\"native_count\":%d",comma?",":"","AB"[i],n);
        comma=true;
        NativeString name(L"");
        if (label && label(g,i,&name)==0) {
            fprintf(file,",\"label\":");
            jsonString(file,utf8((const wchar_t*)name.data));
        }
        fprintf(file,",\"points\":[");
        const int samples=(std::min)(n,limit);
        for (int j=0;j<samples;++j) {
            const int index=samples==1 ? 0 : (int)((long long)j*(n-1)/(samples-1));
            double time=0, voltage=0;
            operationStage="scope_read_point";
            if (point(base,i,index,&time,&voltage)!=0 || !std::isfinite(time) || !std::isfinite(voltage)) {
                valid=false; break;
            }
            fprintf(file,"%s[%.17g,%.17g]",j?",":"",time,voltage);
        }
        fprintf(file,"]}");
        if (!valid) break;
    }
    fprintf(file,"]");
    return valid;
}

static void describeInstrument(FILE* file,void* object) {
    std::string type=instrumentString(object,"?GetInstrumentType@CComponentBridge@@SA?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@PAVCODSymbolComp@@@Z");
    if (type.empty()) return;
    fprintf(file,",\"instrument\":{\"type\":"); jsonString(file,type);
    fprintf(file,",\"manager\":");
    jsonString(file,instrumentString(object,"?GetInstrumentMgr@CComponentBridge@@SA?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@PAVCODSymbolComp@@@Z"));
    typedef void (__cdecl* IDs)(void*,void*,void*);
    IDs ids=(IDs)symbol("SCH_CAP.dll","?GetInstrumentManagerID@CComponentBridge@@SAXPAVCODSymbolComp@@AAV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@1@Z");
    NativeString manager(L""),id(L"");
    if (ids) ids(object,&manager,&id);
    fprintf(file,",\"manager_id\":"); jsonString(file,utf8((const wchar_t*)manager.data));
    fprintf(file,",\"instance_id\":"); jsonString(file,utf8((const wchar_t*)id.data));
    InstrumentContext context;
    typedef void* (__thiscall* Reference)(void*,NativeString*,void*);
    Reference reference=(Reference)symbol("SCH_CAP.dll","?GetInstRefID@CSnapinHandler@@QAE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@PAVCODSymbolComp@@@Z");
    NativeString nativeReference;
    if (context.handler && reference) reference(context.handler,&nativeReference,object);
    fprintf(file,",\"native_ref_id\":"); jsonString(file,utf8((const wchar_t*)nativeReference.data));
    if (type=="SCOPE") {
        void* scope=context.instance(object,"SCOPE.DLL","??_7CScopeInstrument@@6B@");
        if (readable(scope,0xd78)) describeScopeSettings(file,scope);
    }
    fprintf(file,",\"native_strings\":[");
    bool comma=false;
    for (int property=0;property<16;++property) {
        NativeString result(L"");
        if (context.getString(object,property,&result)) {
            fprintf(file,"%s{\"property\":%d,\"value\":",comma ? "," : "",property);
            jsonString(file,utf8((const wchar_t*)result.data)); fprintf(file,"}"); comma=true;
        }
    }
    fprintf(file,"]");
    if (type=="MULMETER") {
        char* meter=(char*)context.instance(object,"mulmeter.dll","??_7CMultimeterInstrument@@6B@");
        if (readable(meter,0xa48)) {
            typedef int (__thiscall* Mode)(void*);
            Mode mode=(Mode)symbol("mulmeter.dll","?GetMeasurementMode@CMultimeterInstrument@@UAEHXZ");
            std::string display=utf8(*(const wchar_t**)(meter+0xa1c));
            bool ready=!display.empty() && display!="-----";
            fprintf(file,",\"meter\":{\"mode_native\":%d,\"ready\":%s,\"display\":",mode ? mode(meter) : -1,ready ? "true" : "false");
            jsonString(file,display);
            if (ready) {
                double dc=*(double*)(meter+0xa38), ac=*(double*)(meter+0xa40);
                if (std::isfinite(dc)) fprintf(file,",\"terminal_voltage_dc\":%.17g",dc);
                if (std::isfinite(ac)) fprintf(file,",\"terminal_voltage_ac_rms\":%.17g",ac);
            }
            fprintf(file,"}");
        }
    }
    fprintf(file,"}");
}
static void* makeInstrument(void* sheet,void* diagram,const wchar_t* reference,const wchar_t* moduleName,const wchar_t* matrixName,unsigned resource,unsigned feature,DiagramPoint point) {
    if (!reference || !*reference || findEditable(diagram,utf8(reference).c_str()) || !moduleName || !*moduleName) return nullptr;
    for (const wchar_t* p=moduleName;*p;++p) if (!iswalnum(*p) && *p!=L'_') return nullptr;
    if (!resource || resource>65535) return nullptr;
    std::wstring filename=std::wstring(moduleName)+L".dll";
    struct ResourceModule {
        HMODULE value=nullptr; bool owned=false;
        ~ResourceModule() { if (owned && value) FreeLibrary(value); }
    } resourceModule;
    operationStage="resolve_instrument_resource";
    resourceModule.value=GetModuleHandleW(filename.c_str());
    if (!resourceModule.value) {
        wchar_t executable[MAX_PATH]={}; GetModuleFileNameW(nullptr,executable,MAX_PATH);
        wchar_t* slash=wcsrchr(executable,L'\\');
        if (!slash) return nullptr;
        std::wstring path(executable,slash+1); path+=filename;
        resourceModule.value=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE);
        resourceModule.owned=true;
    }
    HMODULE module=resourceModule.value;
    if (!module || !FindResourceW(module,MAKEINTRESOURCEW(resource),L"INST_SYM")) return nullptr;
    typedef void* (__cdecl* Settings)();
    Settings settings=(Settings)symbol("library.dll","?GetSettings@@YAPAVCSet@@XZ");
    void* configuration=settings ? settings() : nullptr;
    void* matrix=readable(configuration,8) ? *(void**)((char*)configuration+4) : nullptr;
    typedef unsigned (__thiscall* Index)(void*,const NativeString*);
    Index index=(Index)symbol("TierMatrix.dll","?GetDllIndex@CMatrix@@QAEIABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
    NativeString name(moduleName),matrixKey(matrixName),empty(L"");
    unsigned moduleIndex=matrix && index ? index(matrix,&matrixKey) : 0xffff;
    fprintf(currentOutput,",\"instrument_module_index\":%u",moduleIndex);
    if (moduleIndex>=0xffff) return nullptr;
    // The native caller supplies a fixed feature identifier. GetDllIndex is a
    // runtime module slot and is unrelated to this serialized feature value.
    typedef void* (__cdecl* Create)(HMODULE,const NativeString*,unsigned,unsigned,const NativeString*);
    Create create=(Create)symbol("SCH_CAP.dll","?CreateInstrument@CComponentHandler@@SAPAVCIITSymbolComp@@PAUHINSTANCE__@@ABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@II1@Z");
    if (!create) return nullptr;
    ProbeHiddenScope hidden;
    if (!hidden.settings) return nullptr;
    operationStage="create_native_instrument";
    void* object=create(module,&name,resource,feature,&empty);
    const char* type=runtimeName(object);
    fprintf(currentOutput,",\"instrument_feature\":%u,\"factory_class\":\"%s\"",feature,type ? type : "unknown");
    if (!type || strcmp(type,"CIITSymbolComp")) return nullptr;
    typedef void (__thiscall* Add)(void*,void*,DiagramPoint,void*);
    Add add=(Add)dynamicMethod(diagram,"??_7CODDiagram@@6B@","?AddComponent@CODDiagram@@UAEXPAVCODComponent@@VCPoint@@PAVCODView@@@Z");
    if (!add) return nullptr;
    operationStage="adopt_native_instrument";
    add(diagram,object,point,nativeView(".?AVCODView@@"));
    // CIITView's native paste/add handler performs these two steps before
    // adopting the electrical component. ValidateInternalData builds CiPorts
    // from the instrument's named terminals; the second helper registers the
    // real instrument instance and its native event sink.
    typedef void (__cdecl* Validate)(void*,const NativeString*);
    Validate validate=(Validate)symbol("SCH_CAP.dll","?ValidateInternalData@CComponentBridge@@SAXPAVCIITSymbolComp@@ABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
    auto initialize=(unsigned char*)GetModuleHandleA("SCH_CAP.dll")+0x16d7d0;
    const unsigned char initPrefix[]={0x64,0xa1,0,0,0,0,0x6a,0xff,0x68};
    if (!validate || !executable(initialize) || memcmp(initialize,initPrefix,sizeof(initPrefix))) return nullptr;
    operationStage="validate_instrument_ports";
    validate(object,&empty);
    operationStage="register_instrument_instance";
    typedef bool (__cdecl* Initialize)(void*);
    if (!((Initialize)initialize)(object)) return nullptr;
    InstrumentContext instrumentContext;
    NativeString prefix(L"");
    typedef void* (__cdecl* Info)(void*);
    Info info=(Info)symbol("SCH_CAP.dll","?GetInstanceInfo@CComponentBridge@@SAPAVCiaCollString@@PAVCODSymbolComp@@@Z");
    typedef bool (__thiscall* SetParam)(void*,int,const NativeString*);
    SetParam setParam=(SetParam)symbol("Circexp.dll","?SetParam@CiaCollString@@QAE_NHABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
    void* instanceInfo=info ? info(object) : nullptr;
    if (!instanceInfo || !setParam || !instrumentContext.getString(object,5,&prefix) ||
        utf8((const wchar_t*)prefix.data).empty() || !setParam(instanceInfo,4,&prefix)) return nullptr;
    fprintf(currentOutput,",\"instrument_prefix\":"); jsonString(currentOutput,utf8((const wchar_t*)prefix.data));
    void* data=electricalData(object);
    void* circuit=currentCircuit(sheet);
    typedef void (__thiscall* Backref)(void*,void*);
    Backref backref=(Backref)symbol("Circexp.dll","?SetODSymbolComponent@CiComponent@@QAEXPAVCObject@@@Z");
    typedef int (__thiscall* Name)(void*,const wchar_t*);
    Name rename=(Name)symbol("Circexp.dll","?SetLocalName@CiElement@@QAEHPB_W@Z");
    typedef void (__thiscall* Ref)(void*,const wchar_t*);
    Ref ref=(Ref)symbol("Od101dll.dll","?SetMultisectionInstanceRefDes@CIITSymbolComp@@UAEXPB_W@Z");
    typedef int (__thiscall* Adopt)(void*,void*,int);
    Adopt adopt=(Adopt)symbol("Circexp.dll","?AddComponent@CiCircuit@@QAEHPAVCiComponent@@H@Z");
    if (!data || !circuit || !backref || !rename || !ref || !adopt) return nullptr;
    backref(data,object);
    if (!rename(data,reference)) return nullptr;
    ref((char*)object+0x40,reference);
    if (!adopt(circuit,data,0)) return nullptr;
    typedef bool (__thiscall* SetVariant)(void*,void*,const int*,const NativeString*);
    SetVariant setVariant=(SetVariant)symbol("SCH_CAP.dll","?SetMapVariantDataValue@CSnapinHandler@@QAE_NPAVCODSymbolComp@@ABW4VARIANT_DATA_VALUE@1@ABV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@@Z");
    int nameProperty=2;
    NativeString referenceValue(reference);
    if (!instrumentContext.handler || !setVariant || !setVariant(instrumentContext.handler,object,&nameProperty,&referenceValue)) return nullptr;
    typedef void* (__cdecl* AttributeFactory)();
    typedef int (__thiscall* AttributeAdder)(void*,void*,int);
    AttributeFactory fault=(AttributeFactory)symbol("Circexp.dll","?CreateObject@CiaFault@@SGPAVCObject@@XZ");
    AttributeAdder attribute=(AttributeAdder)symbol("Circexp.dll","?AddAttribute@CiElement@@QAEHPAVCiAttribute@@H@Z");
    if (!fault || !attribute || !attribute(data,fault(),8)) return nullptr;
    NativeString spiceTemplate(L"");
    bool hasTemplate=instrumentContext.getString(object,6,&spiceTemplate) && !utf8((const wchar_t*)spiceTemplate.data).empty();
    if (hasTemplate) {
        AttributeFactory exporter=(AttributeFactory)symbol("Circexp.dll","?CreateObject@CiaSpiceTmpltExprt@@SGPAVCObject@@XZ");
        typedef int (__thiscall* SetString)(void*,const wchar_t*);
        SetString setString=(SetString)symbol("Circexp.dll","?SetString@CiaCString@@UAEHPB_W@Z");
        void* value=exporter ? exporter() : nullptr;
        if (!value || !setString) return nullptr;
        setString(value,(const wchar_t*)spiceTemplate.data);
        if (!attribute(data,value,1)) return nullptr;
    }
    operationStage="initialize_native_instrument";
    int initialized=prepareComponent(sheet,object,true);
    fprintf(currentOutput,",\"instrument_prepare_status\":%d",initialized);
    auto modelCreate=(unsigned char*)GetModuleHandleA("SCH_CAP.dll")+0x16f4a0;
    const unsigned char modelPrefix[]={0x6a,0xff,0x68};
    typedef void* (__cdecl* ModelCreate)(void*,void*,void*,void*,void*,void*,void*);
    typedef int (__thiscall* SetModel)(void*,void*);
    SetModel setModel=(SetModel)symbol("Circexp.dll","?SetModel@CiElement@@QAEHPAVCiModel@@@Z");
    operationStage="initialize_instrument_model";
    if (!executable(modelCreate) || memcmp(modelCreate,modelPrefix,sizeof(modelPrefix)) || !setModel) return nullptr;
    void* model=((ModelCreate)modelCreate)(circuit,object,nullptr,nullptr,nullptr,nullptr,nullptr);
    fprintf(currentOutput,",\"instrument_model\":\"%p\"",model);
    if ((hasTemplate && !model) || (model && !setModel(data,model))) return nullptr;
    return initialized>=0 ? object : nullptr;
}
