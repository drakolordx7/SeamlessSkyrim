# Fails when a file under CORE_DIR includes anything but the standard library or Core itself.
# Core is the game-independent part of the plugin: no engine (RE/, REL/, SKSE/), no platform
# (Windows, D3D, DXGI), no precompiled header, nothing from src/Skyrim.
file(GLOB_RECURSE files "${CORE_DIR}/*.cpp" "${CORE_DIR}/*.h")
set(bad "")
foreach(file IN LISTS files)
	file(STRINGS "${file}" includes REGEX "^[ \t]*#[ \t]*include")
	foreach(line IN LISTS includes)
		# <vector>, <cmath>: the standard library has no extension and no folder. <windows.h>, <d3d11.h>,
		# <RE/Skyrim.h> and "Skyrim/..." all fail this.
		if(line MATCHES "\"Core/[^\"]+\"" OR line MATCHES "<[a-z_0-9]+>")
			continue()
		endif()
		list(APPEND bad "${file}: ${line}")
	endforeach()
endforeach()
if(bad)
	list(JOIN bad "\n  " text)
	message(FATAL_ERROR "Core may include the standard library and Core headers only:\n  ${text}")
endif()
