# ===== CompilerFlags.cmake =====
# GCC/MSVC/Clang flags

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE   "-O3 -march=native -ffast-math -DNDEBUG -fopenmp -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter")
    set(CMAKE_CXX_FLAGS_DEBUG     "-O0 -g -D_DEBUG -fopenmp -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter")

elseif(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE   "/O2 /DNDEBUG /openmp")
    set(CMAKE_CXX_FLAGS_DEBUG     "/Od /D_DEBUG /openmp")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
