#=============================================================================
#   CMake build system files
#
#   Copyright (c) 2014-2024 pocl developers
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

include_directories(AFTER "devices" ".")

add_subdirectory("devices")

###############################################################################

set(POCL_LIB_SOURCES  "clCreateContextFromType.c"
                   "clReleaseContext.c"
                   "clRetainContext.c"
                   "clGetContextInfo.c"
                   "clCreateCommandQueue.c"
                   "clCreateCommandQueueWithProperties.c"
                   "clReleaseCommandQueue.c"
                   "clRetainCommandQueue.c"
                   "clGetCommandQueueInfo.c"
                   "clCreateBuffer.c"
                   "clCreateSubBuffer.c"
                   "clEnqueueFillImage.c"
                   "clEnqueueFillBuffer.c"
                   "clEnqueueReadBuffer.c"
                   "clEnqueueReadBufferRect.c"
                   "clEnqueueMapBuffer.c"
                   "clEnqueueUnmapMemObject.c"
                   "clEnqueueMarkerWithWaitList.c"
                   "clEnqueueBarrierWithWaitList.c"
                   "clReleaseMemObject.c"
                   "clRetainMemObject.c"
                   "clGetMemObjectInfo.c"
                   "clSetMemObjectDestructorCallback.c"
                   "clCreateProgramWithSource.c"
                   "clCreateProgramWithBinary.c"
                   "clCreateProgramWithIL.c"
                   "clCreateProgramWithBuiltInKernels.c"
                   "clCreateProgramWithDefinedBuiltInKernels.c"
                   "clReleaseProgram.c"
                   "clRetainProgram.c"
                   "clBuildProgram.c"
                   "pocl_build.c"
                   "clCompileProgram.c"
                   "clLinkProgram.c"
                   "clCreateKernel.c"
                   "clReleaseKernel.c"
                   "clRetainKernel.c"
                   "clSetKernelArg.c"
                   "clEnqueueNDRangeKernel.c"
                   "clEnqueueNativeKernel.c"
                   "clCreateKernelsInProgram.c"
                   "clGetKernelInfo.c"
                   "clGetKernelArgInfo.c"
                   "clGetPlatformIDs.c"
                   "clGetPlatformInfo.c"
                   "clGetDeviceAndHostTimer.c"
                   "clGetDeviceIDs.c"
                   "clGetDeviceInfo.c"
                   "clCreateContext.c"
                   "clGetProgramBuildInfo.c"
                   "clEnqueueWriteBuffer.c"
                   "clEnqueueWriteBufferRect.c"
                   "clEnqueueCopyBuffer.c"
                   "clEnqueueCopyBufferRect.c"
                   "clEnqueueCopyBufferToImage.c"
                   "clEnqueueCopyImageToBuffer.c"
                   "clEnqueueMigrateMemObjects.c"
                   "clFinish.c"
                   "clFlush.c"
                   "clEnqueueBarrier.c"
                   "clEnqueueMarker.c"
                   "clGetKernelWorkGroupInfo.c"
                   "clGetKernelSubGroupInfo.c"
                   "clGetProgramInfo.c"
                   "clGetEventProfilingInfo.c"
                   "clWaitForEvents.c"
                   "clEnqueueWaitForEvents.c"
                   "clReleaseEvent.c"
                   "clRetainEvent.c"
                   "clSetEventCallback.c"
                   "clGetEventInfo.c"
                   "clCreateUserEvent.c"
                   "clSetUserEventStatus.c"
                   "clEnqueueTask.c"
                   "clCreateImage.c"
                   "clCreateImage2D.c"
                   "clCreateImage3D.c"
                   "clEnqueueReadImage.c"
                   "clEnqueueWriteImage.c"
                   "clCreateSampler.c"
                   "clReleaseSampler.c"
                   "clRetainSampler.c"
                   "clGetSamplerInfo.c"
                   "clGetMutableCommandInfoKHR.c"
                   "clEnqueueCopyImage.c"
                   "clEnqueueMapImage.c"
                   "clGetHostTimer.c"
                   "clGetImageInfo.c"
                   "clCreateFromGLTexture.c"
                   "clCreateFromGLTexture2D.c"
                   "clCreateFromGLTexture3D.c"
                   "clEnqueueAcquireGLObjects.c"
                   "clEnqueueReleaseGLObjects.c"
                   "clGetGLContextInfoKHR.c"
                   "clUnloadCompiler.c"
                   "clGetSupportedImageFormats.c"
                   "clGetExtensionFunctionAddress.c"
                   "clGetExtensionFunctionAddressForPlatform.c"
                   "clIcdGetPlatformIDsKHR.c"
                   "clIcdGetFunctionAddressForPlatformKHR.c"
                   "clIcdSetPlatformDispatchDataKHR.c"
                   "clReleaseDevice.c"
                   "clRetainDevice.c"
                   "clCreateSubDevices.c"
                   "clUnloadPlatformCompiler.c"
                   "clSetContentSizeBufferPoCL.c"
                   "clCreatePipe.c"
                   "clGetPipeInfo.c"
                   "clSetProgramReleaseCallback.c"
                   "clSetContextDestructorCallback.c"
                   "clSetProgramSpecializationConstant.c"
                   "clCloneKernel.c"
                   "clEnqueueSVMMigrateMem.c"
                   "clUpdateMutableCommandsKHR.c"
                   "clSVMAlloc.c" "clSVMFree.c" "clEnqueueSVMFree.c"
                   "clEnqueueSVMMap.c" "clEnqueueSVMUnmap.c"
                   "clEnqueueSVMMemcpy.c" "clEnqueueSVMMemFill.c"
                   "clSetKernelArgSVMPointer.c" "clSetKernelExecInfo.c"
                   "clSetDefaultDeviceCommandQueue.c"
                   "clCommandBarrierWithWaitListKHR.c"
                   "clCommandCopyBufferKHR.c"
                   "clCommandCopyBufferRectKHR.c"
                   "clCommandCopyBufferToImageKHR.c"
                   "clCommandCopyImageKHR.c"
                   "clCommandCopyImageToBufferKHR.c"
                   "clCommandFillBufferKHR.c"
                   "clCommandFillImageKHR.c"
                   "clCommandSVMMemcpyKHR.c"
                   "clCommandSVMMemFillKHR.c"
                   "clCommandNDRangeKernelKHR.c"
                   "clCreateCommandBufferKHR.c"
                   "clEnqueueCommandBufferKHR.c"
                   "clFinalizeCommandBufferKHR.c"
                   "clGetCommandBufferInfoKHR.c"
                   "clReleaseCommandBufferKHR.c"
                   "clRetainCommandBufferKHR.c"
                   "clRemapCommandBufferKHR.c"
                   "clMemAllocINTEL.c"
                   "clMemFreeINTEL.c"
                   "clGetMemAllocInfoINTEL.c"
                   "clSetKernelArgMemPointerINTEL.c"
                   "clEnqueueMemFillINTEL.c"
                   "clEnqueueMemcpyINTEL.c"
                   "clEnqueueMemAdviseINTEL.c"
                   "clEnqueueMigrateMemINTEL.c"
                   "clCommandReadBufferPOCL.c"
                   "clCommandReadBufferRectPOCL.c"
                   "clCommandReadImagePOCL.c"
                   "clCommandSVMMemcpyPOCL.c"
                   "clCommandSVMMemcpyRectPOCL.c"
                   "clCommandSVMMemfillPOCL.c"
                   "clCommandSVMMemfillRectPOCL.c"
                   "clCommandWriteBufferPOCL.c"
                   "clCommandWriteBufferRectPOCL.c"
                   "clCommandWriteImagePOCL.c"
                   "clEnqueueSVMMemcpyRectPOCL.c"
                   "clEnqueueSVMMemfillRectPOCL.c"
                   "clSetKernelArgDevicePointer.c"

                   "pocl_builtin_kernels.c" "pocl_builtin_kernels.h"
                   "pocl_cl_half_util.c" "pocl_cl_half_util.h"
                   "pocl_cl.h" "pocl_icd.h" "pocl_llvm.h"
                   "pocl_util.h" "pocl_util.c"
                   "pocl_run_command.c" "pocl_run_command.h"
                   "pocl_image_util.c" "pocl_image_util.h"
                   "pocl_img_buf_cpy.c"
                   "pocl_fill_memobj.c"
                   "pocl_ndrange_kernel.c"
                   "pocl_tracing.h" "pocl_tracing.c"
                   "pocl_runtime_config.c" "pocl_runtime_config.h"
                   "pocl_mem_management.c"  "pocl_mem_management.h"
                   "pocl_hash.c"
                   "pocl_debug.h" "pocl_debug.c"
                   "pocl_tensor_util.h" "pocl_tensor_util.c"
                   "pocl_binary.c" "pocl_opengl.c" "pocl_cq_profiling.c"
                   # platform dep
                   "pocl_timing.h" "pocl_threads.h" "pocl_file_util.h"
                   # dbks
                   "dbk/pocl_dbk_khr_jpeg_shared.c"
                   "dbk/pocl_dbk_khr_jpeg_shared.h"
                   "dbk/pocl_dbk_khr_onnxrt_shared.c"
                   "dbk/pocl_dbk_khr_onnxrt_shared.h"
                   "dbk/pocl_dbk_khr_img_shared.c"
                   "dbk/pocl_dbk_khr_img_shared.h"
                   "dbk/pocl_dbk_khr_dnn_utils_shared.c"
                   "dbk/pocl_dbk_khr_dnn_utils_shared.h"
                   "pocl_raw_ptr_set.c"
)

unset(EXTRA_LLVM_SOURCES)

if(ENABLE_LLVM_PLATFORM_SUPPORT)
  list(APPEND POCL_LIB_SOURCES "pocl_timing.cc" "pocl_threads.cc")
  list(APPEND EXTRA_LLVM_SOURCES "pocl_llvm_file_utils.cc" "pocl_llvm_dynlib.cc")
else()
  list(APPEND POCL_LIB_SOURCES "pocl_timing.c" "pocl_threads.c" "pocl_file_util.c" "pocl_dynlib.c")
endif()

if(ANDROID)
  list(APPEND POCL_LIB_SOURCES "pocl_mkstemp.c")
endif()
if(APPLE)
  list(APPEND POCL_LIB_SOURCES "pthread_barrier.c")
endif()

if(WIN32)
  list(APPEND POCL_LIB_SOURCES "pocl_ipc_mutex_win.cc")
endif()

set(LIBPOCL_OBJS "$<TARGET_OBJECTS:libpocl_unlinked_objs>"
                 "$<TARGET_OBJECTS:pocl_cache>")

if(ENABLE_HOST_CPU_DEVICES)
  # always link into libpocl if any CPU device is enabled
  list(APPEND LIBPOCL_OBJS "$<TARGET_OBJECTS:pocl-devices-topology>")
endif()

if(ENABLE_LOADABLE_DRIVERS)
  list(APPEND LIBPOCL_OBJS "$<TARGET_OBJECTS:pocl-devices>")
else()
  # link drivers into libpocl
  list(APPEND LIBPOCL_OBJS ${POCL_DEVICES_OBJS})
endif()

add_library("pocl_cache" OBJECT "pocl_cache.c")
if(BUILD_SHARED_LIBS AND MSVC)
  # Inject __declspec(dllexport).
  target_compile_definitions("pocl_cache" PRIVATE EXPORT_POCL_LIB)
  target_compile_definitions("pocl_cache" INTERFACE IMPORT_POCL_LIB)
endif()
harden("pocl_cache")

if(OCL_ICD_INCLUDE_DIRS)
  target_include_directories("pocl_cache" SYSTEM PUBLIC ${OCL_ICD_INCLUDE_DIRS})
endif()

if (ENABLE_LLVM)
  # this is required, because we're compiling the C++ files of this directory
  # with LLVM-provided flags, which usually contain their own setting of
  # C++ standard, which may be in conflict with pocl's default C++ standard.
  unset(CMAKE_CXX_STANDARD)
  unset(CMAKE_CXX_STANDARD_REQUIRED)

  set(LLVM_API_SOURCES "pocl_llvm_build.cc" "pocl_llvm_metadata.cc" "pocl_llvm_utils.cc" "pocl_llvm_wg.cc" "pocl_llvm_spirv.cc" ${EXTRA_LLVM_SOURCES})
  set_source_files_properties(${LLVM_API_SOURCES} PROPERTIES COMPILE_FLAGS "${LLVM_CXXFLAGS} -I\"${CMAKE_CURRENT_SOURCE_DIR}/../llvmopencl\"")

  add_library("lib_cl_llvm" OBJECT ${LLVM_API_SOURCES})
  target_include_directories(lib_cl_llvm PRIVATE ${LLVM_INCLUDE_DIRS})
  harden("lib_cl_llvm")
  if(BUILD_SHARED_LIBS AND MSVC)
    # Inject __declspec(dllexport).
    target_compile_definitions(lib_cl_llvm PRIVATE EXPORT_POCL_LIB)
    target_compile_definitions(lib_cl_llvm INTERFACE IMPORT_POCL_LIB)
  endif()

  if(OCL_ICD_INCLUDE_DIRS)
    target_include_directories("lib_cl_llvm" SYSTEM PUBLIC ${OCL_ICD_INCLUDE_DIRS})
  endif()

  if (HAVE_LLVM_SPIRV_LIB)
    target_include_directories(lib_cl_llvm PRIVATE ${LLVM_SPIRV_INCLUDEDIR})
  endif()

  if(ENABLE_HEADER_BUNDLING)
    add_dependencies(lib_cl_llvm regenerate_bundled_headers)
  endif()


  list(APPEND LIBPOCL_OBJS "$<TARGET_OBJECTS:llvmpasses>")
  list(APPEND LIBPOCL_OBJS "$<TARGET_OBJECTS:lib_cl_llvm>")

  # pocl_cache.c depends on a SHA1 hash of all built kernel-<target>.bc
  add_dependencies("pocl_cache" "kernellib_hash")

  set(CMAKE_CXX_STANDARD ${POCL_CMAKE_CXX_STANDARD})
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(ENABLE_ICD)
  if(OCL_ICD_CFLAGS)
    add_compile_options(${OCL_ICD_CFLAGS})
  endif()
endif()

if(HAVE_LTTNG_UST)
  add_compile_options(${LTTNG_UST_CFLAGS})
  list(APPEND POCL_LIB_SOURCES "pocl_lttng.c" "pocl_lttng.h")
endif()

# this is so that we don't compile twice when building both libpocl and libOpenCL
add_library("libpocl_unlinked_objs" OBJECT ${POCL_LIB_SOURCES})

target_include_directories(libpocl_unlinked_objs
  PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/STC/include)

if(BUILD_SHARED_LIBS AND MSVC)
  # Inject __declspec(dllexport).
  target_compile_definitions(libpocl_unlinked_objs PRIVATE EXPORT_POCL_LIB)
  target_compile_definitions(libpocl_unlinked_objs INTERFACE IMPORT_POCL_LIB)
endif()

harden("libpocl_unlinked_objs")

#################################################################

set(POCL_PRIVATE_LINK_LIBRARIES)
set(POCL_PRIVATE_LINK_DIRECTORIES)

# this must come 1st
if(SANITIZER_OPTIONS)
  set(POCL_PRIVATE_LINK_LIBRARIES ${SANITIZER_LIBS})
endif()

if(POCL_DEVICES_LINK_LIST)
  list(APPEND POCL_PRIVATE_LINK_LIBRARIES ${POCL_DEVICES_LINK_LIST})
endif()

if(ENABLE_LLVM)
  list(APPEND POCL_PRIVATE_LINK_LIBRARIES ${CLANG_LINK_LIBRARIES} ${LLVM_LINK_LIBRARIES})
  list(APPEND POCL_PRIVATE_LINK_DIRECTORIES ${CLANG_LINK_DIRECTORIES} ${LLVM_LINK_DIRECTORIES})

  list(APPEND POCL_PRIVATE_LINK_LIBRARIES ${LLVM_SYSLIBS})

  if (HAVE_LLVM_SPIRV_LIB)
    list(PREPEND POCL_PRIVATE_LINK_LIBRARIES ${LLVM_SPIRV_LIB})
  endif()
endif()

if(HAVE_LTTNG_UST)
  list(APPEND POCL_PRIVATE_LINK_LIBRARIES ${LTTNG_UST_LDFLAGS})
endif()

# -lrt is required for glibc < 2.17
if(HAVE_CLOCK_GETTIME AND CMAKE_SYSTEM_NAME MATCHES "Linux")
  list(APPEND POCL_PRIVATE_LINK_LIBRARIES "rt")
endif()

if(HAVE_64BIT_ATOMICS_WITH_LIB)
  list(APPEND POCL_PRIVATE_LINK_LIBRARIES "atomic")
endif()

if(ANDROID)
  find_library(ANDROID_LOG_LIB log)
  list(APPEND POCL_PRIVATE_LINK_LIBRARIES ${ANDROID_LOG_LIB})
endif()

list(APPEND POCL_PRIVATE_LINK_LIBRARIES ${LIBMATH} ${CMAKE_DL_LIBS} ${PTHREAD_LIBRARY})

# see lib/CMakeLists.txt
set(POCL_TRANSITIVE_LIBS ${POCL_PRIVATE_LINK_LIBRARIES} PARENT_SCOPE)

#################################################################

add_library("${POCL_LIBRARY_NAME}" ${LIBPOCL_OBJS})
if(BUILD_SHARED_LIBS AND MSVC)
  # Inject __declspec(dllexport).
  target_compile_definitions("${POCL_LIBRARY_NAME}" PRIVATE EXPORT_POCL_LIB)
  target_compile_definitions("${POCL_LIBRARY_NAME}" INTERFACE IMPORT_POCL_LIB)
endif()

harden("${POCL_LIBRARY_NAME}")

if((UNIX OR MINGW) AND (NOT APPLE))
  target_link_options("${POCL_LIBRARY_NAME}" PRIVATE "LINKER:--exclude-libs,ALL")
endif()
if(WIN32)
  target_link_options("${POCL_LIBRARY_NAME}" PRIVATE "LINKER:--enable-auto-import,--enable-runtime-pseudo-reloc")
endif()

set_target_properties("${POCL_LIBRARY_NAME}" PROPERTIES SOVERSION "${LIB_API_VERSION}"
  VERSION "${LIB_BUILD_VERSION}"
  LINK_FLAGS "${LLVM_LDFLAGS}  ${ICD_LD_FLAGS}")

target_link_libraries("${POCL_LIBRARY_NAME}" PRIVATE ${POCL_PRIVATE_LINK_LIBRARIES})
target_link_directories("${POCL_LIBRARY_NAME}" PRIVATE ${POCL_PRIVATE_LINK_DIRECTORIES})
install(TARGETS "${POCL_LIBRARY_NAME}"
        ARCHIVE DESTINATION ${POCL_INSTALL_STATIC_LIBDIR_REL}
        COMPONENT "dev"
        LIBRARY DESTINATION ${POCL_INSTALL_PUBLIC_LIBDIR_REL}
        COMPONENT "lib"
        RUNTIME DESTINATION ${POCL_INSTALL_PUBLIC_LIBDIR_REL}
        COMPONENT "lib"
)


if(ENABLE_IPO)
  if (ENABLE_LLVM)
    set_property(TARGET "lib_cl_llvm" PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
  set_property(TARGET "${POCL_LIBRARY_NAME}" PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(WIN32)
  set_target_properties("${POCL_LIBRARY_NAME}" PROPERTIES PREFIX "")
endif()
