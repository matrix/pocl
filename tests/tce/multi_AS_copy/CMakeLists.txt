#=============================================================================
#   CMake build system files
#
#   Copyright (c) 2016 pocl developers
#
#   Permission is hereby granted, free of charge, to any person obtaining a copy
#   of this software and associated documentation files (the "Software"), to deal
#   in the Software without restriction, including without limitation the rights
#   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#   copies of the Software, and to permit persons to whom the Software is
#   furnished to do so, subject to the following conditions:
#
#   The above copyright notice and this permission notice shall be included in
#   all copies or substantial portions of the Software.
#
#   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
#   THE SOFTWARE.
#
#=============================================================================

add_executable("multi_AS_copy" host.cpp)

target_link_libraries("multi_AS_copy" ${POCLU_LINK_OPTIONS})

add_compile_options( -Wno-deprecated -Wno-deprecated-declarations)

add_definitions("-DSRCDIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")

add_test_pocl(NAME "tce/multi_AS_copy" COMMAND "multi_AS_copy" WORKITEM_HANDLER "loopvec")

add_test_pocl(NAME "tcele/multi_AS_copy" COMMAND "multi_AS_copy" WORKITEM_HANDLER "loopvec")

add_test_pocl(NAME "almaif/multi_AS_copy" COMMAND "multi_AS_copy" WORKITEM_HANDLER "loopvec")

set_tests_properties( "tce/multi_AS_copy"
  PROPERTIES
    COST 40.0
    PROCESSORS 1
    PASS_REGULAR_EXPRESSION "OK"
    LABELS "tce;tta;ttasim"
    ENVIRONMENT "POCL_DEVICES=pthread ttasim;POCL_TTASIM0_PARAMETERS=${CMAKE_SOURCE_DIR}/tools/data/test_machine.adf"
    DEPENDS "pocl_version_check")

set_tests_properties( "tcele/multi_AS_copy"
  PROPERTIES
    COST 40.0
    PROCESSORS 1
    PASS_REGULAR_EXPRESSION "OK"
    LABELS "tce;tta;ttasim"
    ENVIRONMENT "POCL_DEVICES=pthread ttasim;POCL_TTASIM0_PARAMETERS=${CMAKE_SOURCE_DIR}/tools/data/test_machine_LE.adf"
    DEPENDS "pocl_version_check")

set_tests_properties( "almaif/multi_AS_copy"
  PROPERTIES
    COST 40.0
    PROCESSORS 1
    PASS_REGULAR_EXPRESSION "OK"
    LABELS "almaif"
    ENVIRONMENT "POCL_DEVICES=pthread almaif;POCL_ALMAIF0_PARAMETERS=0xB,${CMAKE_SOURCE_DIR}/tools/data/test_machine_LE"
    DEPENDS "pocl_version_check")

