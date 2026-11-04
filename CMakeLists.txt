cmake_minimum_required(VERSION 3.22)

project(GGLBot)

set(GGLBOT_PROFILE "kilobyte" CACHE STRING "Package profile: kilobyte or megabyte")
set_property(CACHE GGLBOT_PROFILE PROPERTY STRINGS kilobyte megabyte)
if(NOT GGLBOT_PROFILE MATCHES "^(kilobyte|megabyte)$")
  message(FATAL_ERROR "GGLBOT_PROFILE must be kilobyte or megabyte")
endif()

set(DEVICE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/rlbot/device.txt")
include(cmake/ReadDevice.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DEVICE_FILE}")
if(GGLBOT_DEVICE STREQUAL "gpu")
  if(NOT WIN32)
    message(FATAL_ERROR "GPU builds currently support Windows only. Select cpu for Linux.")
  endif()
  set(GGLBOT_USE_CUDA TRUE)
  set(GGLBOT_CORE GGLBotCoreCUDA)
  set(runtime_variable LIBTORCH_CUDA_ROOT)
  set(runtime_subdir libtorch)
  set(runtime_marker torch_cuda)
else()
  set(GGLBOT_USE_CUDA FALSE)
  set(GGLBOT_CORE GGLBotCoreCPU)
  set(runtime_variable LIBTORCH_CPU_ROOT)
  set(runtime_subdir torch-archive/torch)
  set(runtime_marker torch_cpu)
endif()

if(WIN32)
  # The shared runtimes use the release MSVC ABI, including for builds with debug symbols.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "Use Release or RelWithDebInfo with the shared LibTorch runtimes (see README.md).")
  endif()
  if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "Supported LibTorch configurations" FORCE)
  endif()
  set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
endif()

# Resolve only the selected SDK. Explicit CMake and environment overrides are exclusive.
if(NOT DEFINED ${runtime_variable})
  if(DEFINED ENV{${runtime_variable}} AND NOT "$ENV{${runtime_variable}}" STREQUAL "")
    set(default_runtime "$ENV{${runtime_variable}}")
  else()
    if(WIN32)
      set(data_home "$ENV{LOCALAPPDATA}")
    elseif(DEFINED ENV{XDG_DATA_HOME} AND NOT "$ENV{XDG_DATA_HOME}" STREQUAL "")
      set(data_home "$ENV{XDG_DATA_HOME}")
    else()
      set(data_home "$ENV{HOME}/.local/share")
    endif()
    set(default_runtime "${data_home}/RLBot5/bots/${runtime_subdir}")
    set(directory "${CMAKE_CURRENT_SOURCE_DIR}/rlbot")
    foreach(level RANGE 1 5)
      set(candidate "${directory}/${runtime_subdir}")
      if(EXISTS "${candidate}/lib/${runtime_marker}.lib" OR EXISTS "${candidate}/lib/lib${runtime_marker}.so")
        set(default_runtime "${candidate}")
        break()
      endif()
      get_filename_component(directory "${directory}" DIRECTORY)
    endforeach()
  endif()
  set(${runtime_variable} "${default_runtime}" CACHE PATH "Selected LibTorch SDK root")
endif()
set(GGLBOT_RUNTIME_ROOT "${${runtime_variable}}")
message(STATUS "GGLBot: ${GGLBOT_DEVICE}, ${runtime_variable}=${GGLBOT_RUNTIME_ROOT}")

add_subdirectory(cpp-interface EXCLUDE_FROM_ALL)

file(GLOB_RECURSE GGLBOT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/inc/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/inc/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE GGLBOT_HEADERS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/inc/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# The launcher locates the selected runtime before loading Torch.
add_executable(GGLBot launcher/GGLBotLauncher.cpp src/Package.cpp)
set_target_properties(GGLBot PROPERTIES OUTPUT_NAME GGLBot)
target_compile_features(GGLBot PRIVATE cxx_std_20)
target_include_directories(GGLBot PRIVATE src)
target_compile_definitions(GGLBot PRIVATE GGLBOT_USE_CUDA=$<BOOL:${GGLBOT_USE_CUDA}>)
if(WIN32)
  target_link_libraries(GGLBot PRIVATE shell32)
endif()

set(GGLBOT_DEPLOY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rlbot/${GGLBOT_PROFILE}" CACHE PATH "Bot package deployment directory")
set(GGLBOT_RUNTIME_DIR "${GGLBOT_DEPLOY_DIR}/000-runtime")

function(configure_gglbot_target target runtime_root use_cuda)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/inc" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src")
  target_compile_definitions(${target} PRIVATE GGL_STATIC GGLBOT_USE_CUDA=$<BOOL:${use_cuda}>)
  target_link_libraries(${target} PRIVATE RLBotCPP-static)
  if(WIN32)
    target_link_libraries(${target} PRIVATE comctl32 shell32 user32 gdi32 uxtheme dwmapi)
  endif()

  if(WIN32)
    # Direct linking avoids requiring a Windows CUDA toolkit inside the Linux cross-compiler.
    set(torch_libraries torch torch_cpu c10)
    if(use_cuda)
      list(APPEND torch_libraries torch_cuda c10_cuda)
    endif()
    foreach(library IN LISTS torch_libraries)
      set(import_library "${runtime_root}/lib/${library}.lib")
      if(NOT EXISTS "${import_library}")
        message(FATAL_ERROR "${target}: missing ${import_library}. Install the selected SDK or set ${runtime_variable}.")
      endif()
      target_link_libraries(${target} PRIVATE "${import_library}")
    endforeach()
    target_include_directories(${target} SYSTEM PRIVATE "${runtime_root}/include" "${runtime_root}/include/torch/csrc/api/include")
    target_compile_definitions(${target} PRIVATE USE_DISTRIBUTED USE_C10D_GLOO)
    target_compile_options(${target} PRIVATE /permissive- /EHsc /bigobj)
    if(use_cuda)
      # Keep CUDA registration code even though the public API lives in torch_cpu.
      target_link_options(${target} PRIVATE "/INCLUDE:?warp_size@cuda@at@@YAHXZ")
    endif()
  else()
    set(Torch_DIR "${runtime_root}/share/cmake/Torch")
    find_package(Torch REQUIRED CONFIG)
    if(TARGET Torch::Torch)
      target_link_libraries(${target} PRIVATE Torch::Torch)
    elseif(TARGET torch)
      target_link_libraries(${target} PRIVATE torch)
    else()
      target_include_directories(${target} SYSTEM PRIVATE ${TORCH_INCLUDE_DIRS})
      target_link_libraries(${target} PRIVATE ${TORCH_LIBRARIES})
    endif()
    # RLBotCPP passes std::string across the core boundary and must use Torch's ABI too.
    separate_arguments(torch_compile_options NATIVE_COMMAND "${TORCH_CXX_FLAGS}")
    target_compile_options(${target} PRIVATE ${torch_compile_options})
    target_compile_options(RLBotCPP-static PUBLIC ${torch_compile_options})
  endif()
endfunction()

function(add_gglbot_core target runtime_root use_cuda)
  add_executable(${target} ${GGLBOT_SOURCES} ${GGLBOT_HEADERS})
  configure_gglbot_target(${target} "${runtime_root}" ${use_cuda})
  add_dependencies(GGLBot ${target})
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GGLBOT_RUNTIME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target}>" "${GGLBOT_RUNTIME_DIR}/$<TARGET_FILE_NAME:${target}>"
    VERBATIM
  )
endfunction()

add_gglbot_core(${GGLBOT_CORE} "${GGLBOT_RUNTIME_ROOT}" ${GGLBOT_USE_CUDA})

add_custom_command(TARGET GGLBot POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${GGLBOT_DEPLOY_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:GGLBot>" "${GGLBOT_DEPLOY_DIR}/$<TARGET_FILE_NAME:GGLBot>"
  VERBATIM
)


# The controller has no Torch dependency. It remains independent
# of the inference process so Apply & respawn can keep the same window alive.
function(configure_skill_gui target)
  if(WIN32)
    target_link_libraries(${target} PRIVATE comctl32 shell32 user32 gdi32 uxtheme dwmapi)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.22)
    target_sources(${target} PRIVATE gui/SkillWindowGtk.cpp)
    target_compile_definitions(${target} PRIVATE GGLBOT_LINUX_GUI=1)
    target_link_libraries(${target} PRIVATE PkgConfig::GTK3)
  endif()
endfunction()
add_executable(ByteSkillUI gui/SkillGuiMain.cpp src/Package.cpp src/SkillControl.cpp
  src/SkillSession.cpp src/SkillSettings.cpp src/SkillPersistence.cpp)
target_compile_features(ByteSkillUI PRIVATE cxx_std_20)
target_include_directories(ByteSkillUI PRIVATE inc src)
target_link_libraries(ByteSkillUI PRIVATE RLBotCPP-static)
configure_skill_gui(ByteSkillUI)
add_dependencies(GGLBot ByteSkillUI)
add_custom_command(TARGET ByteSkillUI POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${GGLBOT_RUNTIME_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ByteSkillUI>" "${GGLBOT_RUNTIME_DIR}/$<TARGET_FILE_NAME:ByteSkillUI>"
  VERBATIM)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(ByteSkillUI PRIVATE Threads::Threads)
endif()

# The same model files and directory layout are used locally and by bob.
add_custom_target(GGLBotPackage ALL
  COMMAND ${CMAKE_COMMAND}
    "-DSOURCE_PACKAGE=${CMAKE_CURRENT_SOURCE_DIR}/rlbot"
    "-DDEST_PACKAGE=${GGLBOT_DEPLOY_DIR}"
    "-DBOT_PROFILE=${GGLBOT_PROFILE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StagePackage.cmake"
  DEPENDS GGLBot
  VERBATIM)

option(GGLBOT_BUILD_TESTS "Build profile and package regression tests" OFF)
if(GGLBOT_BUILD_TESTS)
  enable_testing()
  foreach(profile kilobyte megabyte)
    add_executable(compiled_profile_${profile} tests/compiled_profile.cpp src/Package.cpp)
    target_compile_features(compiled_profile_${profile} PRIVATE cxx_std_20)
    target_include_directories(compiled_profile_${profile} PRIVATE src)
    target_compile_definitions(compiled_profile_${profile} PRIVATE
      GGLBOT_PROFILE_MEGABYTE=$<STREQUAL:${profile},megabyte>)
    add_test(NAME compiled_profile_${profile} COMMAND compiled_profile_${profile} ${profile})
    set_tests_properties(compiled_profile_${profile} PROPERTIES TIMEOUT 30)
  endforeach()
  add_executable(package_paths tests/package_paths.cpp src/Package.cpp)
  target_compile_features(package_paths PRIVATE cxx_std_20)
  target_include_directories(package_paths PRIVATE src)
  add_executable(skill_profiles tests/skill_profiles.cpp src/Package.cpp src/SkillControl.cpp
    src/SkillSession.cpp src/SkillSettings.cpp src/SkillPersistence.cpp)
  target_compile_features(skill_profiles PRIVATE cxx_std_20)
  target_include_directories(skill_profiles PRIVATE inc src)
  target_link_libraries(skill_profiles PRIVATE RLBotCPP-static)
  configure_skill_gui(skill_profiles)
  add_test(NAME skill_profiles COMMAND skill_profiles "${CMAKE_BINARY_DIR}/test-output")
  set_tests_properties(skill_profiles PROPERTIES TIMEOUT 60)
  if(NOT WIN32)
    add_executable(controller_linux tests/controller_linux.cpp src/SkillControl.cpp
      src/SkillSession.cpp src/SkillSettings.cpp src/SkillPersistence.cpp)
    target_compile_features(controller_linux PRIVATE cxx_std_20)
    target_include_directories(controller_linux PRIVATE inc src)
    target_link_libraries(controller_linux PRIVATE RLBotCPP-static Threads::Threads)
    add_test(NAME controller_linux COMMAND controller_linux "${CMAKE_BINARY_DIR}/controller-output")
    set_tests_properties(controller_linux PROPERTIES TIMEOUT 30)
  endif()
endif()

# Every executable containing Package.cpp uses the same build-time identity.
foreach(target GGLBot ${GGLBOT_CORE} ByteSkillUI package_paths skill_profiles)
  if(TARGET ${target})
    target_compile_definitions(${target} PRIVATE
      GGLBOT_PROFILE_MEGABYTE=$<STREQUAL:${GGLBOT_PROFILE},megabyte>)
  endif()
endforeach()
