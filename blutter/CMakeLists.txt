cmake_minimum_required (VERSION 3.20)

project(blutter)
if (NOT DEFINED DARTLIB)
	message(FATAL_ERROR "Please define DARTLIB to build the Blutter project")
endif()
if (NOT ${DARTLIB} MATCHES "^dartvm")
	message(FATAL_ERROR "DARTLIB name should start with 'dartvm'")
endif()

set(BINNAME "${PROJECT_NAME}_${DARTLIB}${NAME_SUFFIX}")

set(CMAKE_INSTALL_PREFIX "${PROJECT_SOURCE_DIR}/../bin" CACHE PATH "" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	# Linux: clang 默认使用 libstdc++（无 std::format），改用 libc++ 提供 <format>
	add_compile_options(-stdlib=libc++ -fexperimental-library)
	add_link_options(-stdlib=libc++ -fexperimental-library)
endif()

find_package(${DARTLIB} REQUIRED PATHS "${PROJECT_SOURCE_DIR}/../packages")

# Dart version compatibility macros are normally supplied by scripts/build.py,
# which probes the dartvm headers and passes -DHAS_*=1. A hand-written cmake
# invocation that omits them compiles out the matching branches in
# src/DartTypes.cpp and makes the parser abort on "Invalid abstract type"
# (e.g. RecordType cid=50 on Dart 3.x when HAS_RECORD_TYPE is missing).
# Probe the same header here as a fallback so the macros cannot be silently lost.
get_target_property(_dartvm_inc_dirs ${DARTLIB} INTERFACE_INCLUDE_DIRECTORIES)
if (_dartvm_inc_dirs)
	list(GET _dartvm_inc_dirs 0 _dartvm_inc_dir)
	if (EXISTS "${_dartvm_inc_dir}/vm/class_id.h")
		file(READ "${_dartvm_inc_dir}/vm/class_id.h" _dartvm_class_id_h)
		if (NOT DEFINED HAS_TYPE_REF)
			string(FIND "${_dartvm_class_id_h}" "V(TypeRef)" _dartvm_pos_ref)
			if (NOT _dartvm_pos_ref EQUAL -1)
				set(HAS_TYPE_REF 1)
			endif()
		endif()
		if (NOT DEFINED HAS_RECORD_TYPE)
			string(FIND "${_dartvm_class_id_h}" "V(RecordType)" _dartvm_pos_rec)
			if (NOT _dartvm_pos_rec EQUAL -1)
				set(HAS_RECORD_TYPE 1)
			endif()
		endif()
		message(STATUS "Auto-detected Dart type macros: HAS_TYPE_REF=${HAS_TYPE_REF} HAS_RECORD_TYPE=${HAS_RECORD_TYPE}")
	endif()
endif()

# On Windows always use the bundled Capstone from init_env_win.py (MSVC may not be
# set yet depending on generator/compiler detection order).
if (WIN32 OR MSVC)
	add_library(capstone SHARED IMPORTED)
	set_target_properties(capstone PROPERTIES IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/../external/capstone/capstone.dll")
	set_target_properties(capstone PROPERTIES IMPORTED_IMPLIB "${PROJECT_SOURCE_DIR}/../external/capstone/capstone_dll.lib")
	target_include_directories(capstone INTERFACE "${PROJECT_SOURCE_DIR}/../external/capstone/include/capstone")
else()
	include(FindPkgConfig)
	pkg_search_module(CAPSTONE REQUIRED capstone)
	link_directories(${CAPSTONE_LIBDIR})
	include_directories(AFTER ${CAPSTONE_INCLUDEDIR})
endif()

# Add source to this project's executable.
set(SRCDIR "src")
include(sourcelist.cmake)
# Arch-specific sources: arm64 analyzer is upstream; x64 stubs for Windows desktop.
if (NOT DEFINED BLUTTER_ARCH)
	set(BLUTTER_ARCH "arm64")
endif()
if (${BLUTTER_ARCH} STREQUAL "x64")
	list(FILTER SRCS EXCLUDE REGEX ".*_arm64\\.(cpp|h)$")
	list(APPEND SRCS Disassembler_x64.cpp Disassembler_x64.h CodeAnalyzer_x64.cpp)
elseif (${BLUTTER_ARCH} STREQUAL "arm64")
	list(FILTER SRCS EXCLUDE REGEX ".*_x64\\.(cpp|h)$")
else()
	message(FATAL_ERROR "Unsupported BLUTTER_ARCH=${BLUTTER_ARCH}")
endif()
message(STATUS "BLUTTER_ARCH=${BLUTTER_ARCH}")
list(TRANSFORM SRCS PREPEND "${SRCDIR}/")
add_executable(${BINNAME} ${SRCS})

target_link_libraries(${BINNAME} PRIVATE ${DARTLIB} capstone)

target_precompile_headers(${BINNAME} PRIVATE "${SRCDIR}/pch.h")

if (MSVC)
	# for dynamic function (exception) table
	target_link_libraries(${BINNAME} PRIVATE ntdll)
	# remove compiler exception handler options
	#string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
	# remove compiler RTTI option
	string(REPLACE "/GR" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
	#set(cc_opts /Oy /GR- /EHs-c-)
	if (CMAKE_GENERATOR MATCHES "Visual Studio")
		# use SEMIDBG for enabling ASSERT macro while linking against release build of Dart VM
		set(cc_opts /GR- /MD /D SEMIDBG)
		set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${BINNAME})
		set_target_properties(${BINNAME} PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "${DBG_CMD}")
	else()
		# assume Ninja
		set(cc_opts /Oy /GR- /sdl- /Oi /GL /Gy /Zc:wchar_t /Zc:inline)
		target_link_options(${BINNAME} PRIVATE /LTCG /OPT:REF /OPT:ICF)
	endif()
else()
	# TODO: gcc options to remove dead code in static dartvm library
	set(cc_opts
		-O3 -fno-rtti
		-fvisibility=hidden -fvisibility-inlines-hidden -fno-omit-frame-pointer
		#-Wl,--gc-sections
		#-fno-exceptions -Wall
	)
endif()

# 运行时按可执行文件所在目录定位模板，编译期不固化绝对路径（否则产物在手机等其它路径无法使用）
set(defines NDEBUG FRIDA_TEMPLATE_DIR="scripts")
if (OLD_MAP_SET_NAME)
	set(defines ${defines} OLD_MAP_SET_NAME)
	if (OLD_MAP_NO_IMMUTABLE)
		set(defines ${defines} OLD_MAP_NO_IMMUTABLE)
	endif()
endif()
if (NO_LAST_INTERNAL_ONLY_CID)
	set(defines ${defines} NO_LAST_INTERNAL_ONLY_CID)
endif()
if (HAS_SHARED_CLASS_TABLE)
	set(defines ${defines} HAS_SHARED_CLASS_TABLE)
endif()
if (HAS_TYPE_REF)
	set(defines ${defines} HAS_TYPE_REF)
endif()
if (HAS_RECORD_TYPE)
	set(defines ${defines} HAS_RECORD_TYPE)
endif()
if (NO_INIT_LATE_STATIC_FIELD)
	set(defines ${defines} NO_INIT_LATE_STATIC_FIELD)
endif()
if (NO_CODE_ANALYSIS)
	set(defines ${defines} NO_CODE_ANALYSIS)
endif()
if (NO_METHOD_EXTRACTOR_STUB)
	set(defines ${defines} NO_METHOD_EXTRACTOR_STUB)
endif()
if (UNIFORM_INTEGER_ACCESS)
	set(defines ${defines} UNIFORM_INTEGER_ACCESS)
endif()
# 解析架构宏（TARGET_ARCH_*）只应由 BLUTTER_ARCH 决定：dartvm 库按产物架构编译，
# 其 PUBLIC 宏（如 arm64 产物传出的 TARGET_ARCH_ARM64）会经 find_package 传导到本
# target，与解析架构冲突（Disassembler.h / DartDumper.cpp / CodeAnalyzer.h 均按
# TARGET_ARCH_* 选择 x64/arm64 分支）。因此 BLUTTER_ARCH=x64 时屏蔽 arm64 宏并声明
# x64 宏；MSVC 下 TARGET_ARCH_X64 已由 x64 编译的 dartvm 库提供，无需重复定义。
if (${BLUTTER_ARCH} STREQUAL "x64")
	if (MSVC)
		target_compile_options(${BINNAME} PRIVATE "/UTARGET_ARCH_ARM64")
	else()
		target_compile_options(${BINNAME} PRIVATE -UTARGET_ARCH_ARM64)
		target_compile_definitions(${BINNAME} PRIVATE TARGET_ARCH_X64)
	endif()
endif()

target_compile_definitions(${BINNAME} PRIVATE ${defines})

# Default semantic-blacklist file, overridable at runtime via --blacklist or
# $BLUTTER_BLACKLIST (see the loader in DartDumper.cpp).
target_compile_definitions(${BINNAME} PRIVATE
	BLUTTER_DEFAULT_BLACKLIST_FILE="${CMAKE_CURRENT_SOURCE_DIR}/src/semantic_blacklist.txt")

target_compile_options(${BINNAME} PRIVATE ${cc_opts})

cmake_path(SET DST_DIR NORMALIZE "${PROJECT_SOURCE_DIR}/../bin")
install (TARGETS ${BINNAME} RUNTIME DESTINATION ${DST_DIR})
