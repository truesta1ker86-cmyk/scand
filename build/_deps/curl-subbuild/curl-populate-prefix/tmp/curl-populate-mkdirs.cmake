# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/vitaliy/scand/build/_deps/curl-src"
  "/home/vitaliy/scand/build/_deps/curl-build"
  "/home/vitaliy/scand/build/_deps/curl-subbuild/curl-populate-prefix"
  "/home/vitaliy/scand/build/_deps/curl-subbuild/curl-populate-prefix/tmp"
  "/home/vitaliy/scand/build/_deps/curl-subbuild/curl-populate-prefix/src/curl-populate-stamp"
  "/home/vitaliy/scand/build/_deps/curl-subbuild/curl-populate-prefix/src"
  "/home/vitaliy/scand/build/_deps/curl-subbuild/curl-populate-prefix/src/curl-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/vitaliy/scand/build/_deps/curl-subbuild/curl-populate-prefix/src/curl-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/vitaliy/scand/build/_deps/curl-subbuild/curl-populate-prefix/src/curl-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
