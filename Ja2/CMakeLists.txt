# GameVersion.cpp carries two placeholders, @Version@ and @Build@. Upstream
# substitutes them with sed in the GitHub Actions workflow, so a build that runs
# anywhere else ships an executable whose in-game version reads "@Version@".
# The fork keeps the file as GameVersion.cpp.in and lets CMake fill it in.
# JA2MOD_VERSION must stay within 15 characters (czVersionString is CHAR8[16]).
if(NOT DEFINED JA2MOD_VERSION)
	set(JA2MOD_VERSION "ja2mod-dev")
endif()
if(NOT DEFINED JA2MOD_BUILD)
	set(JA2MOD_BUILD "ja2mod local build")
endif()
string(LENGTH "${JA2MOD_VERSION}" ja2modVersionLength)
if(ja2modVersionLength GREATER 15)
	message(FATAL_ERROR "JA2MOD_VERSION is ${ja2modVersionLength} characters, the engine allows 15")
endif()
set(Version "${JA2MOD_VERSION}")
set(Build "${JA2MOD_BUILD}")
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/GameVersion.cpp.in"
	"${CMAKE_CURRENT_BINARY_DIR}/GameVersion.cpp"
	@ONLY)

set(Ja2Src
"${CMAKE_CURRENT_SOURCE_DIR}/aniviewscreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/Credits.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/Fade Screen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/FeaturesScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/GameInitOptionsScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/gameloop.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/gamescreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/GameSettings.cpp"
"${CMAKE_CURRENT_BINARY_DIR}/GameVersion.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/HelpScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/Init.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/Intro.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/JA2 Splash.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/Ja25Update.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/jascreens.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/Loading Screen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/MainMenuScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/MessageBoxScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/MPChatScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/MPConnectScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/MPHostScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/MPJoinScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/MPScoreScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/MPXmlTeams.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/Options Screen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/profiler.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/SaveLoadGame.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/SaveLoadScreen.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/Screens.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/Sys Globals.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/TimeLogging.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/ub_config.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/XML_DifficultySettings.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/XML_IntroFiles.cpp"
"${CMAKE_CURRENT_SOURCE_DIR}/XML_Layout_MainMenu.cpp"
PARENT_SCOPE)
