#ifndef ART_RUNTIME_MARVIN_TRACE_H_
#define ART_RUNTIME_MARVIN_TRACE_H_

#define MARVIN_TRACE_ENABLED true

#if MARVIN_TRACE_ENABLED
#define MARVIN_TRACE_GET_FIELD(obj) marvin::trace::TraceGetField(obj)
#define MARVIN_TRACE_SET_FIELD(obj) marvin::trace::TraceSetField(obj)
#define MARVIN_TRACE_GET_FIELD_OBJECT(obj, ref) marvin::trace::TraceGetFieldObject(obj, ref)
#define MARVIN_TRACE_SET_FIELD_OBJECT(obj, ref) marvin::trace::TraceSetFieldObject(obj, ref)
#else
#define MARVIN_TRACE_GET_FIELD(obj)
#define MARVIN_TRACE_SET_FIELD(obj)
#define MARVIN_TRACE_GET_FIELD_OBJECT(obj, ref)
#define MARVIN_TRACE_SET_FIELD_OBJECT(obj, ref)
#endif

namespace art {

namespace mirror {
    class Object;
}

namespace marvin {

namespace trace {

void InitializeTrace();
void TraceGetField(mirror::Object * obj);
void TraceGetFieldObject(mirror::Object * obj, mirror::Object * ref);
void TraceSetField(mirror::Object * obj);
void TraceSetFieldObject(mirror::Object * obj, mirror::Object * ref);

}
}
}

#endif // ART_RUNTIME_MARVIN_TRACE_H_
