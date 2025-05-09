cmake_minimum_required(VERSION 3.5...3.29)

project(CoreFunctionsExtension)

include_directories(include)

add_subdirectory(aggregate)
add_subdirectory(scalar)

set(CORE_FUNCTION_FILES ${CORE_FUNCTION_FILES} core_functions_extension.cpp
                        function_list.cpp lambda_functions.cpp)

build_static_extension(core_functions ${CORE_FUNCTION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(core_functions ${PARAMETERS} ${CORE_FUNCTION_FILES})
target_link_libraries(core_functions_loadable_extension duckdb_skiplistlib)

install(
  TARGETS core_functions_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
