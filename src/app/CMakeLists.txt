add_executable(vilk main.cpp)

target_link_libraries(vilk PRIVATE
    vilk_audio
    vilk_render
)
