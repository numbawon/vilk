include(FetchContent)
FetchContent_Declare(kissfft
    GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
    GIT_TAG        131.1.0
)
set(KISSFFT_TEST      OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS     OFF CACHE BOOL "" FORCE)
set(KISSFFT_STATIC    ON  CACHE BOOL "" FORCE)
set(KISSFFT_PKGCONFIG OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(kissfft)
