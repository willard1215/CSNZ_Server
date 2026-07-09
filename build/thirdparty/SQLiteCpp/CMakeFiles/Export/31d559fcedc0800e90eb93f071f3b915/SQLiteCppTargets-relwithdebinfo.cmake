#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "sqlite3" for configuration "RelWithDebInfo"
set_property(TARGET sqlite3 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(sqlite3 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "C"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/sqlite3.lib"
  )

list(APPEND _cmake_import_check_targets sqlite3 )
list(APPEND _cmake_import_check_files_for_sqlite3 "${_IMPORT_PREFIX}/lib/sqlite3.lib" )

# Import target "SQLiteCpp" for configuration "RelWithDebInfo"
set_property(TARGET SQLiteCpp APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(SQLiteCpp PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "CXX"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/SQLiteCpp.lib"
  )

list(APPEND _cmake_import_check_targets SQLiteCpp )
list(APPEND _cmake_import_check_files_for_SQLiteCpp "${_IMPORT_PREFIX}/lib/SQLiteCpp.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
