CMAKE_MINIMUM_REQUIRED(VERSION 3.7.0 FATAL_ERROR)

PROJECT(png-download NONE)

INCLUDE(ExternalProject)
ExternalProject_Add(png
	GIT_REPOSITORY https://github.com/glennrp/libpng.git
	GIT_TAG master
	SOURCE_DIR "${DNN_DEPENDENCIES_SOURCE_DIR}/libpng"
	BINARY_DIR "${DNN_DEPENDENCIES_BINARY_DIR}/libpng"
	CONFIGURE_COMMAND ""
	BUILD_COMMAND ""
	INSTALL_COMMAND ""
	TEST_COMMAND ""
)
