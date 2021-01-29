CMAKE_MINIMUM_REQUIRED(VERSION 3.5.0 FATAL_ERROR)

PROJECT(nnpack-download NONE)

INCLUDE(ExternalProject)
ExternalProject_Add(nnpack
	GIT_REPOSITORY https://github.com/zulma46/NNPACK.git
	GIT_TAG main
	SOURCE_DIR "${DNN_DEPENDENCIES_SOURCE_DIR}/NNPACK"
	BINARY_DIR "${DNN_DEPENDENCIES_BINARY_DIR}/NNPACK"
	CONFIGURE_COMMAND ""
	BUILD_COMMAND ""
	INSTALL_COMMAND ""
	TEST_COMMAND ""
)