# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

cmake_minimum_required(VERSION 3.25...3.31)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(cmake/validate_special_target.cmake)
include(cmake/version.cmake)
desktop_app_parse_version(Telegram/build/version)

if (NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(configuration_types_init 1)
endif()

project(Telegram
    LANGUAGES C CXX
    VERSION ${desktop_app_version_cmake}
    DESCRIPTION "NovaGram desktop messenger"
    HOMEPAGE_URL "https://desktop.telegram.org"
)

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

if (configuration_types_init
    AND CMAKE_CONFIGURATION_TYPES
    AND NOT MinSizeRel IN_LIST CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES};MinSizeRel" CACHE STRING "" FORCE)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Telegram)

get_filename_component(third_party_loc "Telegram/ThirdParty" REALPATH)
get_filename_component(submodules_loc "Telegram" REALPATH)
get_filename_component(cmake_helpers_loc "cmake" REALPATH)

if (NOT DESKTOP_APP_USE_PACKAGED AND WIN32)
    set(Python3_EXECUTABLE ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/python/Scripts/python)
endif()

# NovaGram: spell checking is done by the bundled Hunspell, never by the
# operating system. On Windows the platform backend is ISpellChecker, so every
# character typed into the compose field is handed to a Windows COM service
# together with the words added to the personal dictionary. Hunspell runs
# inside this process against dictionaries under tdata/dictionaries, so the
# typed text never leaves the application.
#
# The option is upstream's own and already supported end to end: it swaps
# spellcheck/platform/win/spellcheck_win.cpp for
# spellcheck/third_party/spellcheck_hunspell.cpp, and Hunspell is compiled and
# linked into the Windows build regardless of this flag. Everything above it -
# "Manage dictionaries", auto-download, the per-language list - is the branch
# tdesktop already takes when IsSystemSpellchecker() is false.
#
# FORCE, and not a plain set(), on purpose: out/CMakeCache.txt from an earlier
# configure holds OFF, and a promise that depends on a stale cache entry is not
# a promise. Set it to OFF here to compare against the system spellchecker.
set(DESKTOP_APP_USE_HUNSPELL_ONLY ON CACHE BOOL
    "NovaGram: never hand typed text to the system spellchecker." FORCE)

include(cmake/variables.cmake)
include(cmake/nice_target_sources.cmake)
include(cmake/target_compile_options_if_exists.cmake)
include(cmake/target_link_frameworks.cmake)
include(cmake/target_link_options_if_exists.cmake)
include(cmake/init_target.cmake)
include(cmake/generate_target.cmake)
include(cmake/nuget.cmake)
include(cmake/validate_d3d_compiler.cmake)
include(cmake/target_prepare_qrc.cmake)

include(cmake/options.cmake)
include(cmake/external/qt/package.cmake)

set(desktop_app_skip_libs
    glibmm
    variant
)

add_subdirectory(cmake)
add_subdirectory(Telegram)
