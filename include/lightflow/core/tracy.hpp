#pragma once

#if defined(LF_ENABLE_TRACY) && LF_ENABLE_TRACY
    #include <tracy/Tracy.hpp>

    #define LF_ZONE_SCOPED ZoneScoped
    #define LF_ZONE_SCOPED_N(name) ZoneScopedN(name)
    #define LF_ZONE_SCOPED_COLOR(color) ZoneScopedC(color)
    #define LF_ZONE_NAMED(var, name) ZoneNamedN(var, name, true)
    #define LF_ZONE_NAMED_COLOR(var, name, color) ZoneNamedNC(var, name, color, true)
    #define LF_FRAME_MARK FrameMark
    #define LF_FRAME_MARK_NAMED(name) FrameMarkNamed(name)
    #define LF_SET_THREAD_NAME(name) tracy::SetThreadName(name)
#else
    #define LF_ZONE_SCOPED do {} while (0)
    #define LF_ZONE_SCOPED_N(name) do { (void)(name); } while (0)
    #define LF_ZONE_SCOPED_COLOR(color) do { (void)(color); } while (0)
    #define LF_ZONE_NAMED(var, name) do { (void)(name); } while (0)
    #define LF_ZONE_NAMED_COLOR(var, name, color) do { (void)(name); (void)(color); } while (0)
    #define LF_FRAME_MARK do {} while (0)
    #define LF_FRAME_MARK_NAMED(name) do { (void)(name); } while (0)
    #define LF_SET_THREAD_NAME(name) do { (void)(name); } while (0)
#endif
