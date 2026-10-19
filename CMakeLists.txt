# Shared components for the Equinox Audio plugin family (Equinox, Meridian,
# Zenith, Horizon): design language / LookAndFeel, meters, preset ecosystem,
# snapshot StateManager, loudness metering.
#
# Header-only INTERFACE library. Each plugin vendors JUCE itself and pulls
# this in with:
#
#   add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../jdm_shared" jdm_shared)
#   target_link_libraries(<plugin> PRIVATE jdm_shared)

cmake_minimum_required(VERSION 3.22)

project(jdm_shared LANGUAGES CXX)

add_library(jdm_shared INTERFACE)
target_include_directories(jdm_shared INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
