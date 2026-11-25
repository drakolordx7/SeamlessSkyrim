cmake_minimum_required(VERSION 3.25)

project(
	SeamlessSkyrim
	VERSION 0.6.0
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Targets SE 1.5.97 and AE 1.7.104: addresses are SE/AE ID pairs, and CommonLib resolves struct
# layouts at runtime (GetRuntimeData); AE itself has two (before/after 1.6.629). VR is off: its
# openvr submodule is not checked out and we never target it.
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Render and cell hooks will need hand-written trampolines.
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)

add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)

set(SKYRIM_PLUGINS_DIR
	"E:/SteamLibrary/steamapps/common/Skyrim Special Edition/Data/SKSE/Plugins"
	CACHE PATH "Where the post-build step copies the DLL and PDB. Empty disables the copy.")

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)

# Plain add_library instead of CommonLib's add_commonlibsse_plugin(): its generated declaration
# can't express "uses post-1.6.629 structs", so SKSE disabled the plugin on 1.7.104. The
# declaration lives in src/Skyrim/Plugin/main.cpp instead.
add_library(${PROJECT_NAME} SHARED ${SOURCES})
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(
	${PROJECT_NAME} PRIVATE
	CommonLibSSE::CommonLibSSE
	nlohmann_json::nlohmann_json
	ws2_32
	windowscodecs
)
target_compile_definitions(
	${PROJECT_NAME} PRIVATE
	PLUGIN_NAME="${PROJECT_NAME}"
	PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
	PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
	PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
	# Dependencies in the PCH pull in <windows.h>; without LEAN_AND_MEAN that drags in the old
	# winsock.h, which then clashes with WinSock2.h in the bridge.
	WIN32_LEAN_AND_MEAN
	NOMINMAX
)

target_include_directories(${PROJECT_NAME} PRIVATE src)
# SKSE Menu Framework's consumer header (MIT, vendored from its example plugin): resolves the
# framework's exports at run time, so the plugin works with or without it installed.
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE extern/SKSEMenuFramework)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

if(MSVC)
	# The DLL that ships names no folder of the machine it was built on: source files (asserts,
	# log lines) relative to the repository, the PDB by its file name.
	file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/" SOURCE_ROOT_NATIVE)
	target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8 /Zi "/d1trimfile:${SOURCE_ROOT_NATIVE}")
	# A PDB next to the DLL turns Crash Logger stack traces into function names and line numbers.
	# Named outright: the Visual Studio generator escaped "%_PDB%" and the DLL asked for a PDB called
	# "%%%SeamlessSkyrim.pdb%%%", so neither trainwreck nor llvm-symbolizer found it.
	target_link_options(${PROJECT_NAME} PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF /PDBALTPATH:${PROJECT_NAME}.pdb)
endif()

if(SKYRIM_PLUGINS_DIR)
	add_custom_command(
		TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${SKYRIM_PLUGINS_DIR}"
		# Not `cmake -E copy`: it replaces the file, which cuts the hard link when the DLL is
		# deployed by Vortex (tools/deploy-vortex-mod.ps1).
		COMMAND powershell -NoProfile -ExecutionPolicy Bypass
			-File "${CMAKE_SOURCE_DIR}/tools/copy-in-place.ps1"
			-Destination "${SKYRIM_PLUGINS_DIR}"
			"$<TARGET_FILE:${PROJECT_NAME}>"
			"$<TARGET_PDB_FILE:${PROJECT_NAME}>"
		COMMENT "Deploying ${PROJECT_NAME} to ${SKYRIM_PLUGINS_DIR}"
		VERBATIM
	)
endif()
