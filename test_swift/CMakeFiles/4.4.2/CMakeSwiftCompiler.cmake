# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

set(CMAKE_Swift_COMPILER "/Applications/Xcode-beta.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/swiftc")
set(CMAKE_Swift_COMPILER_ID "Apple")
set(CMAKE_Swift_COMPILER_VERSION "6.4")

set(CMAKE_Swift_COMPILER_LOADED 1)
set(CMAKE_Swift_COMPILER_WORKS "TRUE")

set(CMAKE_Swift_COMPILER_ENV_VAR "SWIFTC")

set(CMAKE_Swift_COMPILER_ID_RUN 1)
set(CMAKE_Swift_SOURCE_FILE_EXTENSIONS swift)

set(CMAKE_Swift_COMPILER_USE_OLD_DRIVER "FALSE")

set(CMAKE_Swift_IMPLICIT_INCLUDE_DIRECTORIES "/Applications/Xcode-beta.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS27.0.sdk/usr/include")

set(CMAKE_Swift_MODULE_TRIPLE "arm64-apple-macos")
set(CMAKE_Swift_SIZEOF_DATA_PTR "8")

if(NOT CMAKE_SIZEOF_VOID_P AND CMAKE_Swift_SIZEOF_DATA_PTR)
  set(CMAKE_SIZEOF_VOID_P "${CMAKE_Swift_SIZEOF_DATA_PTR}")
endif()
