cmake_minimum_required(VERSION 3.5)

project(camview)

# Default to C++14
if(NOT CMAKE_CXX_STANDARD)
	set(CMAKE_CXX_STANDARD 14)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(PRINT_FPS)
	add_definitions(-DPRINT_FPS)
endif(PRINT_FPS)

find_package(glfw3 REQUIRED)

include_directories(include)

file(GLOB SOURCES
    include/*.h
    src/*.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME} v4l2 glfw)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
