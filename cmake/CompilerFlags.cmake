# ===== CompilerFlags.cmake =====
# GCC/MSVC/Clang 各自 flags

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(COMMON_WARNINGS -Wall -Wextra -Wno-unused-parameter)
    set(COMMON_FLAGS   -fopenmp -fvisibility=hidden)

    set(CMAKE_CXX_FLAGS_RELEASE   "-O2 -DNDEBUG ${COMMON_FLAGS} ${COMMON_WARNINGS}")
    set(CMAKE_CXX_FLAGS_DEBUG     "-O0 -g -D_DEBUG ${COMMON_FLAGS} ${COMMON_WARNINGS}")

elseif(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE   "/O2 /DNDEBUG /openmp")
    set(CMAKE_CXX_FLAGS_DEBUG     "/Od /D_DEBUG /openmp")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
