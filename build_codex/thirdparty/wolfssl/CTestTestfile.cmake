# CMake generated Testfile for 
# Source directory: D:/VS_Code_Data/CSNX/CSNZ_Server/src/thirdparty/wolfssl
# Build directory: D:/VS_Code_Data/CSNX/CSNZ_Server/build_codex/thirdparty/wolfssl
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(wolfcrypttest "D:/VS_Code_Data/CSNX/CSNZ_Server/build_codex/thirdparty/wolfssl/wolfcrypt/test/Debug/testwolfcrypt.exe")
  set_tests_properties(wolfcrypttest PROPERTIES  WORKING_DIRECTORY "D:/VS_Code_Data/CSNX/CSNZ_Server/src/thirdparty/wolfssl" _BACKTRACE_TRIPLES "D:/VS_Code_Data/CSNX/CSNZ_Server/src/thirdparty/wolfssl/CMakeLists.txt;2290;add_test;D:/VS_Code_Data/CSNX/CSNZ_Server/src/thirdparty/wolfssl/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(wolfcrypttest "D:/VS_Code_Data/CSNX/CSNZ_Server/build_codex/thirdparty/wolfssl/wolfcrypt/test/Release/testwolfcrypt.exe")
  set_tests_properties(wolfcrypttest PROPERTIES  WORKING_DIRECTORY "D:/VS_Code_Data/CSNX/CSNZ_Server/src/thirdparty/wolfssl" _BACKTRACE_TRIPLES "D:/VS_Code_Data/CSNX/CSNZ_Server/src/thirdparty/wolfssl/CMakeLists.txt;2290;add_test;D:/VS_Code_Data/CSNX/CSNZ_Server/src/thirdparty/wolfssl/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(wolfcrypttest "D:/VS_Code_Data/CSNX/CSNZ_Server/build_codex/thirdparty/wolfssl/wolfcrypt/test/RelWithDebInfo/testwolfcrypt.exe")
  set_tests_properties(wolfcrypttest PROPERTIES  WORKING_DIRECTORY "D:/VS_Code_Data/CSNX/CSNZ_Server/src/thirdparty/wolfssl" _BACKTRACE_TRIPLES "D:/VS_Code_Data/CSNX/CSNZ_Server/src/thirdparty/wolfssl/CMakeLists.txt;2290;add_test;D:/VS_Code_Data/CSNX/CSNZ_Server/src/thirdparty/wolfssl/CMakeLists.txt;0;")
else()
  add_test(wolfcrypttest NOT_AVAILABLE)
endif()
