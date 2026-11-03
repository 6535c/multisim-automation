// Reconcile native electrical nodes with surviving scalar pin/junction wires.
// Bus and hidden-port graphs are detected before deleting anything.
struct DeletionTopology {
    struct Pin { void* pin; void* port; void* connector; void* oldNode; bool removed; bool ground; int parent; };
    struct Edge { void* wire; int a; int b; };
    std::vector<Pin> pins;
    std::vector<Edge> edges;
    std::vector<void*> nodes;
    void* diagram = nullptr;
    typedef void* (__thiscall* Getter)(void*);
    Getter ref = (Getter)symbol("Circexp.dll", "?Ref@CiObject@@UAEKXZ");
    Getter deref = (Getter)symbol("Circexp.dll", "?DeRef@CiObject@@UAEKXZ");
    ~DeletionTopology() { if (deref) for (void* node : nodes) deref(node); }
    int root(int i) { while (pins[i].parent != i) { pins[i].parent = pins[pins[i].parent].parent; i = pins[i].parent; } return i; }
    bool refresh() {
        // DoDelete removes orphan junctions and joins two-edge junctions. The
        // surviving wires can therefore be new native objects. Re-read those
        // graphics before assigning electrical nodes; never reuse dead edges.
        Getter first=(Getter)symbol("Od101dll.dll","?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
        typedef void* (__thiscall* Next)(void*,void**);
        Next next=(Next)symbol("Od101dll.dll","?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
        Getter source=(Getter)symbol("Od101dll.dll","?GetSourceConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
        Getter target=(Getter)symbol("Od101dll.dll","?GetTargetConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
        if (!first || !next || !source || !target) return false;
        void* top=diagramRoot(diagram);
        std::vector<void*> objects;
        void* position=first(top);
        for (unsigned i=0;position && i<16384;++i) objects.push_back(next(top,&position));
        for (unsigned i=0;i<pins.size();++i) {
            pins[i].parent=i;
            if (!pins[i].port && std::find(objects.begin(),objects.end(),pins[i].pin)==objects.end()) pins[i].removed=true;
        }
        edges.clear();
        for (void* object:objects) {
            const char* type=runtimeName(object);
            if (!type || strcmp(type,"CIITLinkComp")) continue;
            int a=-1,b=-1;
            for (unsigned i=0;i<pins.size();++i) if (!pins[i].removed) {
                if (pins[i].connector==source(object)) a=i;
                if (pins[i].connector==target(object)) b=i;
            }
            if (a<0 && b<0) continue;
            if (a<0 || b<0) return false;
            edges.push_back({object,a,b});
            pins[root(b)].parent=root(a);
        }
        for (unsigned a=0;a<pins.size();++a) if (pins[a].ground && !pins[a].removed)
            for (unsigned b=0;b<a;++b) if (pins[b].ground && !pins[b].removed) pins[root(b)].parent=root(a);
        return true;
    }
    bool capture(void* d, const std::vector<void*>& removed) {
        diagram = d;
        Getter first = (Getter)symbol("Od101dll.dll", "?GetHeadPosition@CODComposite@@UBEPAU__POSITION@@XZ");
        typedef void* (__thiscall* Next)(void*, void**);
        Next next = (Next)symbol("Od101dll.dll", "?GetNext@CODComposite@@UBEPAVCODComponent@@AAPAU__POSITION@@@Z");
        Getter firstPin = (Getter)symbol("Od101dll.dll", "?GetFirstPinPosition@CIITSymbolComp@@QAEPAU__POSITION@@XZ");
        Next nextPin = (Next)symbol("Od101dll.dll", "?GetNextPin@CIITSymbolComp@@QAEPAVCIITPinSymbolComp@@AAPAU__POSITION@@@Z");
        Getter linkNode = (Getter)symbol("SCH_CAP.dll", "?GetNode@CIITLinkComp@@QAEPAVCiNode@@XZ");
        Getter port = (Getter)symbol("Od101dll.dll", "?GetCiPort@CIITPinSymbolComp@@QAEPAVCiPort@@XZ");
        typedef void* (__thiscall* PointGetter)(void*, DiagramPoint*, int);
        PointGetter connector = (PointGetter)symbol("Od101dll.dll", "?GetConnectorPoint@CIITPinSymbolComp@@QAEPAVCIITPinConnectorComp@@AAVCPoint@@H@Z");
        Getter source = (Getter)symbol("Od101dll.dll", "?GetSourceConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
        Getter target = (Getter)symbol("Od101dll.dll", "?GetTargetConnector@CODLinkComp@@QAEPAVCODConnector@@XZ");
        if (!first || !next || !firstPin || !nextPin || !linkNode || !port || !connector || !source || !target || !ref || !deref) return false;
        for (void* item : removed) {
            const char* type = runtimeName(item);
            if (!type || (strcmp(type, "CIITLinkComp") && !isScalarConnector(item))) continue;
            void* node = isScalarConnector(item) ? terminalNode(item) : linkNode(item);
            if (node && std::find(nodes.begin(),nodes.end(),node)==nodes.end()) { ref(node); nodes.push_back(node); }
        }
        void* top = diagramRoot(diagram);
        void* position = first(top);
        for (unsigned i=0; position && i<16384; ++i) {
            void* item = next(top,&position);
            const char* type = runtimeName(item);
            bool deleted = std::find(removed.begin(),removed.end(),item)!=removed.end();
            if (isScalarConnector(item)) {
                void* node = terminalNode(item);
                if (std::find(nodes.begin(),nodes.end(),node)!=nodes.end())
                    pins.push_back({item,nullptr,item,node,deleted,false,(int)pins.size()});
                continue;
            }
            if (!type || (strcmp(type,"CIITSymbolComp") && strcmp(type,"CIITAnimationSymbolComp"))) continue;
            void* data = electricalData(item);
            bool ground = data && valueString(data,"?GetFullName@CiElement@@UBE?AV?$CStringT@_WV?$StrTraitMFC_DLL@_WV?$ChTraitsCRT@_W@ATL@@@@@ATL@@XZ","Circexp.dll")=="0";
            void* p = firstPin(item);
            for (unsigned k=0; p && k<256; ++k) {
                void* pin = nextPin(item,&p);
                void* node = pinNode(pin);
                if (std::find(nodes.begin(),nodes.end(),node)==nodes.end()) continue;
                DiagramPoint ignored;
                pins.push_back({pin,port(pin),connector(pin,&ignored,0),node,deleted,ground,(int)pins.size()});
            }
        }
        // Reject hidden or non-scalar ports rather than silently dropping them.
        typedef void* (__thiscall* IteratorConstructor)(void*,void*);
        IteratorConstructor ctor = (IteratorConstructor)symbol("Circexp.dll", "??0CiIterPortsOnNode@@QAE@PBVCiNode@@@Z");
        Getter current = (Getter)symbol("Circexp.dll", "?Current@CiIterPortsOnNode@@QBEPAVCiPort@@XZ");
        Getter step = (Getter)symbol("Circexp.dll", "?GetNext@CiIterPortsOnNode@@QAEPAVCiPort@@XZ");
        if (!ctor || !current || !step) return false;
        for (void* node : nodes) {
            __declspec(align(8)) unsigned char iterator[128]={};
            ctor(iterator,node);
            for (unsigned i=0; current(iterator) && i<16384; ++i) {
                void* p = current(iterator);
                if (std::find_if(pins.begin(),pins.end(),[&](const Pin& pin){return pin.port==p;})==pins.end()) return false;
                step(iterator);
            }
        }
        position = first(top);
        for (unsigned i=0; position && i<16384; ++i) {
            void* item = next(top,&position);
            const char* type = runtimeName(item);
            if (!type || strcmp(type,"CIITLinkComp") || std::find(removed.begin(),removed.end(),item)!=removed.end()) continue;
            if (std::find(nodes.begin(),nodes.end(),linkNode(item))==nodes.end()) continue;
            int a=-1,b=-1;
            for (unsigned p=0; p<pins.size(); ++p) {
                if (pins[p].connector==source(item)) a=p;
                if (pins[p].connector==target(item)) b=p;
            }
            if (a<0 || b<0 || pins[a].removed || pins[b].removed) return false;
            edges.push_back({item,a,b});
            pins[root(b)].parent=root(a);
        }
        // Ground symbols remain electrically common across physical islands.
        for (unsigned a=0; a<pins.size(); ++a) if (pins[a].ground && !pins[a].removed)
            for (unsigned b=0; b<a; ++b) if (pins[b].ground && !pins[b].removed) pins[root(b)].parent=root(a);
        return true;
    }
    bool repair() {
        if (!refresh()) return false;
        typedef int (__thiscall* Remove)(void*,void*);
        Remove detach = (Remove)symbol("Circexp.dll", "?RemoveNode@CiPort@@QAEHPAVCiNode@@@Z");
        typedef void* (__thiscall* Attach)(void*,void*,int,void*,bool);
        Attach attach = (Attach)symbol("Circexp.dll", "?AddConnection@CiCircuit@@QAEPAVCiNode@@PAVCiPort@@HPAV2@_N@Z");
        typedef void (__thiscall* SetNode)(void*,void*);
        SetNode setNode = (SetNode)symbol("SCH_CAP.dll", "?SetNode@CIITLinkComp@@QAEXPAVCiNode@@@Z");
        // NI files may not serialize a CiCircuit on the diagram root. A first
        // editing operation must also resolve the circuit from the typed sheet
        // or diagram fields, before the batch's final serialization binding.
        void* circuit = currentCircuit(globalObject("?GetDrwSubSheet@@YAPAVCDrwSubSheet@@XZ"));
        if (!detach || !attach || !setNode || !circuit) return false;
        operationStage = "reconcile_surviving_nets";
        for (auto& p : pins) if (!p.removed && p.port) { void* node=pinNode(p.pin); if (node) detach(p.port,node); }
        std::vector<void*> assigned(pins.size(),nullptr), used;
        std::vector<int> order;
        for (unsigned i=0;i<pins.size();++i) if (!pins[i].removed && pins[i].ground) order.push_back(root(i));
        Getter label=(Getter)symbol("SCH_CAP.dll","?GetNodeText@CIITLinkComp@@UAEPAVCODNodeTextComp@@XZ");
        if (label) for (const auto& edge:edges) if (label(edge.wire)) order.push_back(root(edge.a));
        for (const auto& edge:edges) order.push_back(root(edge.a));
        for (int group:order) {
            if (assigned[group]) continue;
            void* node=nullptr;
            for (unsigned i=0;i<pins.size();++i) if (!pins[i].removed && root(i)==group && std::find(used.begin(),used.end(),pins[i].oldNode)==used.end()) { node=pins[i].oldNode; break; }
            if (!node) {
                typedef void* (__cdecl* Allocate)(unsigned);
                Allocate allocate=(Allocate)symbol("MSVCR71.dll","??2@YAPAXI@Z");
                typedef void* (__thiscall* Constructor)(void*);
                Constructor ctor=(Constructor)symbol("Circexp.dll","??0CiNode@@QAE@XZ");
                void* klass=(void*)symbol("Circexp.dll","?classCiNode@CiNode@@2UCRuntimeClass@@A");
                if (!allocate || !ctor || !klass) return false;
                node=ctor(allocate(*(unsigned*)((char*)klass+4)));
                wchar_t name[80]; swprintf_s(name,L"N_AUTO_%lu_%p",GetTickCount(),node);
                typedef int (__thiscall* NameSetter)(void*,const wchar_t*);
                NameSetter rename=(NameSetter)symbol("Circexp.dll","?SetLocalName@CiElement@@QAEHPB_W@Z");
                Remove add=(Remove)symbol("Circexp.dll","?AddNodeEx@CiCircuit@@QAEHPAVCiNode@@@Z");
                if (!rename || !add) return false;
                rename(node,name); add(circuit,node);
            }
            used.push_back(node);
            for (unsigned i=0;i<pins.size();++i) if (!pins[i].removed && root(i)==group) {
                if (pins[i].port) node=attach(nullptr,pins[i].port,0,node,false);
                if (!node) return false;
            }
            assigned[group]=node;
        }
        for (const auto& edge:edges) setNode(edge.wire,assigned[root(edge.a)]);
        for (const auto& edge:edges) {
            void* node=assigned[root(edge.a)];
            if (!node || terminalNode(pins[edge.a].pin)!=node || terminalNode(pins[edge.b].pin)!=node) return false;
            if (!alignWireEndpoints(edge.wire,pins[edge.a].pin,pins[edge.b].pin)) return false;
        }
        return true;
    }
};
