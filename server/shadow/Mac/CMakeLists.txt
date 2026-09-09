include(WarnUnmaintained)
warn_unmaintained("mac shadow server subsystem" "-DWITH_SHADOW_SUBSYSTEM=OFF")

find_library(IOKIT IOKit REQUIRED)
find_library(IOSURFACE IOSurface REQUIRED)
find_library(CARBON Carbon REQUIRED)
find_library(COREAUDIO CoreAudio REQUIRED)
find_library(FOUNDATION Foundation REQUIRED)
find_library(APPKIT AppKit REQUIRED)
find_package(PAM)

set(LIBS ${IOKIT} ${IOSURFACE} ${CARBON} ${COREAUDIO} ${FOUNDATION} ${APPKIT} cliprdr-server)

if(PAM_FOUND)
  add_compile_definitions(WITH_PAM)
  include_directories(SYSTEM ${PAM_INCLUDE_DIR})
  list(APPEND LIBS ${PAM_LIBRARY})
else()
  message("building without PAM authentication support")
endif()

add_compile_definitions(WITH_SHADOW_MAC)
set_source_files_properties(mac_shadow_audio.m mac_shadow_clipboard.m PROPERTIES LANGUAGE C COMPILE_OPTIONS "-fobjc-arc")
add_library(
  freerdp-shadow-subsystem-impl STATIC mac_shadow.h mac_shadow.c mac_shadow_audio.h mac_shadow_audio.m mac_shadow_clipboard.h mac_shadow_clipboard.m
)
target_link_libraries(freerdp-shadow-subsystem-impl PRIVATE ${LIBS})

if(BUILD_TESTING)
  set_source_files_properties(../test/TestMacShadowClipboard.m PROPERTIES LANGUAGE C COMPILE_OPTIONS "-fobjc-arc")
  add_executable(TestMacShadowClipboard ../test/TestMacShadowClipboard.m)
  target_link_libraries(TestMacShadowClipboard PRIVATE freerdp-shadow freerdp winpr ${LIBS})
  add_test(NAME TestMacShadowClipboard COMMAND TestMacShadowClipboard)
  set_tests_properties(TestMacShadowClipboard PROPERTIES TIMEOUT 15)
  add_executable(TestMacShadowPublication ../test/TestMacShadowPublication.c ../shadow_mcevent.c)
  target_link_libraries(TestMacShadowPublication PRIVATE freerdp-shadow-subsystem-impl freerdp-shadow freerdp winpr ${LIBS})
  add_test(NAME TestMacShadowPublication COMMAND TestMacShadowPublication)
  set_tests_properties(TestMacShadowPublication PROPERTIES TIMEOUT 15)
endif()
