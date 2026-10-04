add_library(jc2core STATIC
    src/mechanics.cpp
)

target_include_directories(jc2core PUBLIC include)
target_compile_features(jc2core PUBLIC cxx_std_20)

if(MSVC)
    # Precise float semantics so replays line up with JC2 tick by tick.
    target_compile_options(jc2core PUBLIC /fp:precise PRIVATE /W4 /permissive-)
endif()

set_target_properties(jc2core PROPERTIES FOLDER "core")
