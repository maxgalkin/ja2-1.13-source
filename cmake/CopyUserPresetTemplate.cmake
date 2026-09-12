function(CopyUserPresetTemplate)
	if(
		NOT DEFINED Languages AND
		NOT DEFINED Applications AND
		NOT EXISTS "${CMAKE_SOURCE_DIR}/CMakePresets.json" AND
		NOT EXISTS "${CMAKE_SOURCE_DIR}/CMakeUserPresets.json"
	)
		file(COPY "${CMAKE_SOURCE_DIR}/cmake/presets/CMakePresets.json" DESTINATION "${CMAKE_SOURCE_DIR}")
		# Upstream raises FATAL_ERROR here, which makes the very first configure of a
		# fresh checkout fail. The fork commits a CMakePresets.json, so this branch is
		# only reached when someone deleted it; a warning is enough.
		message(WARNING "No existing preset was found, copied a preset template to ${CMAKE_SOURCE_DIR}/CMakePresets.json.")
	endif()
endfunction()
