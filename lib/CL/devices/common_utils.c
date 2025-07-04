/* common_utils.c - common utilities for CPU device drivers

   Copyright (c) 2011-2013 Universidad Rey Juan Carlos and
                 2011-2019 Pekka Jääskeläinen and
                 2021 Tobias Baumann / Zuse Institute Berlin

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.
*/

#include <string.h>

#include "CL/cl.h"
#include "config2.h"

#include "common.h"
#include "common_utils.h"
#include "cpuinfo.h"
#include "pocl_builtin_kernels.h"
#ifdef ENABLE_LLVM
#include "pocl_llvm.h"
#endif
#include "pocl_mem_management.h"
#include "pocl_runtime_config.h"
#include "pocl_tensor_util.h"
#include "spirv_queries.h"
#include "topology/pocl_topology.h"
#include "utlist.h"

#if defined(__i386__) || defined(_M_IX86) || \
    defined(__x86_64__) || defined(_M_X64)
#define POCL_ON_X86
#include <immintrin.h>
#endif

void
pocl_restore_ftz (unsigned ftz)
{
#if defined(POCL_ON_X86)

#ifdef _MM_FLUSH_ZERO_ON
  if (ftz & _MM_FLUSH_ZERO_ON)
    _MM_SET_FLUSH_ZERO_MODE (_MM_FLUSH_ZERO_ON);
  else
    _MM_SET_FLUSH_ZERO_MODE (_MM_FLUSH_ZERO_OFF);
#endif
#ifdef _MM_DENORMALS_ZERO_ON
  if (ftz & _MM_DENORMALS_ZERO_ON)
    _MM_SET_DENORMALS_ZERO_MODE (_MM_DENORMALS_ZERO_ON);
  else
    _MM_SET_DENORMALS_ZERO_MODE (_MM_DENORMALS_ZERO_OFF);
#endif

#endif
}

unsigned
pocl_save_ftz ()
{
#if defined(POCL_ON_X86)

  unsigned s = 0;
#ifdef _MM_FLUSH_ZERO_ON
  if (_MM_GET_FLUSH_ZERO_MODE ())
    s |= _MM_FLUSH_ZERO_ON;
  else
    s &= (~_MM_FLUSH_ZERO_ON);
#endif
#ifdef _MM_DENORMALS_ZERO_ON
  if (_MM_GET_DENORMALS_ZERO_MODE ())
    s |= _MM_DENORMALS_ZERO_ON;
  else
    s &= (~_MM_DENORMALS_ZERO_ON);
#endif
  return s;

#else
  return 0;
#endif
}

void
pocl_set_ftz (unsigned ftz)
{
#if defined(POCL_ON_X86)
  if (ftz)
    {
#ifdef _MM_FLUSH_ZERO_ON
      _MM_SET_FLUSH_ZERO_MODE (_MM_FLUSH_ZERO_ON);
#endif

#ifdef _MM_DENORMALS_ZERO_ON
      _MM_SET_DENORMALS_ZERO_MODE (_MM_DENORMALS_ZERO_ON);
#endif
    }
  else
    {
#ifdef _MM_FLUSH_ZERO_OFF
      _MM_SET_FLUSH_ZERO_MODE (_MM_FLUSH_ZERO_OFF);
#endif

#ifdef _MM_DENORMALS_ZERO_OFF
      _MM_SET_DENORMALS_ZERO_MODE (_MM_DENORMALS_ZERO_OFF);
#endif
    }
#endif
}

void
pocl_set_default_rm ()
{
#if defined(POCL_ON_X86) && defined(_MM_ROUND_NEAREST)
  unsigned rm = _MM_GET_ROUNDING_MODE ();
  if (rm != _MM_ROUND_NEAREST)
    _MM_SET_ROUNDING_MODE (_MM_ROUND_NEAREST);
#endif
}

unsigned
pocl_save_rm ()
{
#if defined(POCL_ON_X86) && defined(_MM_ROUND_NEAREST)
  return _MM_GET_ROUNDING_MODE ();
#else
  return 0;
#endif
}

void
pocl_restore_rm (unsigned rm)
{
#if defined(POCL_ON_X86) && defined(_MM_ROUND_NEAREST)
  _MM_SET_ROUNDING_MODE (rm);
#endif
}

void
pocl_cpu_save_rm_and_ftz (unsigned *rm, unsigned *ftz)
{
  *rm = pocl_save_rm ();
  *ftz = pocl_save_ftz ();
}

void
pocl_cpu_restore_rm_and_ftz (unsigned rm, unsigned ftz)
{
  pocl_restore_rm (rm);
  pocl_restore_ftz (ftz);
}

void
pocl_cpu_setup_rm_and_ftz (cl_device_id dev, cl_program prog)
{
  /* Flush to zero is only set once at start of kernel (because FTZ is
   * a compilation option) */
  cl_device_fp_config supports_any_denorms
    = (dev->half_fp_config | dev->single_fp_config | dev->double_fp_config)
      & CL_FP_DENORM;
  if (supports_any_denorms)
    pocl_set_ftz (prog->flush_denorms);
  else
    pocl_set_ftz (1);
  /* Rounding mode change is deprecated & only supported by OpenCL 1.0 */
  pocl_set_default_rm ();
}

#ifdef HAVE_LIBXSMM
#include <libxsmm.h>
#endif

/* NOTE: k->lock is probably unnecessary for the tbb device */
#ifdef USE_POCL_MEMMANAGER

static kernel_run_command *volatile kernel_pool = 0;
static int kernel_pool_initialized = 0;
static pocl_lock_t kernel_pool_lock;

void
pocl_init_kernel_run_command_manager ()
{
  if (!kernel_pool_initialized)
    {
      kernel_pool_initialized = 1;
      POCL_INIT_LOCK (kernel_pool_lock);
    }
}

void
pocl_init_thread_argument_manager ()
{
  if (!kernel_pool_initialized)
    {
      kernel_pool_initialized = 1;
      POCL_INIT_LOCK (kernel_pool_lock);
    }
}

kernel_run_command* new_kernel_run_command ()
{
  kernel_run_command *volatile k = NULL;
  POCL_LOCK (kernel_pool_lock);
  if ((k = kernel_pool))
    {
      LL_DELETE (kernel_pool, k);
      memset (k, 0, sizeof(kernel_run_command));
      POCL_INIT_LOCK (&k->lock);
      POCL_UNLOCK (kernel_pool_lock);
      return k;
    }

  POCL_UNLOCK (kernel_pool_lock);
  k = (kernel_run_command*)calloc (1, sizeof (kernel_run_command));
  POCL_INIT_LOCK (&k->lock);
  return k;
}

void free_kernel_run_command (kernel_run_command *k)
{
  POCL_LOCK (kernel_pool_lock);
  POCL_DESTROY_LOCK (&k->lock);
  LL_PREPEND (kernel_pool, k);
  POCL_UNLOCK (kernel_pool_lock);
}

#endif

#define ARGS_SIZE (sizeof (void *) * (meta->num_args + meta->num_locals + 1))

static char *
align_ptr (char *p)
{
  uintptr_t r = (uintptr_t)p;
  if (r & (MAX_EXTENDED_ALIGNMENT - 1))
    {
      r = r & (~(MAX_EXTENDED_ALIGNMENT - 1));
      r += MAX_EXTENDED_ALIGNMENT;
    }
  return (char *)r;
}

#define FALLBACK_MAX_THREAD_COUNT 8

static const char *final_ld_flags[] = { HOST_LD_FLAGS_ARRAY, NULL };

/** Initializes device info defaults for CPU (host) devices.
 *
 * pocl_init_default_device_infos() can be called instead
 * for non-CPU (host) devices.
 */
cl_int
pocl_cpu_init_common (cl_device_id device)
{
  int ret = CL_SUCCESS;

#ifdef ENABLE_LLVM
  device->llvm_target_triplet = OCL_KERNEL_TARGET;

#ifdef KERNELLIB_HOST_DISTRO_VARIANTS
  const char* kernellib_variant = pocl_get_distro_kernellib_variant ();
  device->llvm_cpu = pocl_get_distro_cpu_name (kernellib_variant);
#else
  device->llvm_cpu = OCL_KERNEL_TARGET_CPU;
  if (device->llvm_cpu == NULL)
    device->llvm_cpu = pocl_get_llvm_cpu_name ();
#endif

  char kernellib[POCL_MAX_PATHNAME_LENGTH] = "kernel-";
  char kernellib_fallback[POCL_MAX_PATHNAME_LENGTH];
  strcat(kernellib, device->llvm_target_triplet);
  strcat(kernellib, "-");

#ifdef KERNELLIB_HOST_DISTRO_VARIANTS
  strcpy(kernellib_fallback, kernellib);
  strcat(kernellib_fallback, "generic");
  strcat(kernellib, kernellib_variant);
#elif defined(HOST_CPU_FORCED)
  strcat(kernellib, OCL_KERNEL_TARGET_CPU);
#else
  strncpy (kernellib_fallback, kernellib, POCL_MAX_PATHNAME_LENGTH);
  strncat (kernellib_fallback, OCL_KERNEL_TARGET_CPU,
           POCL_MAX_PATHNAME_LENGTH - strlen (kernellib));
  strncat (kernellib, device->llvm_cpu,
           POCL_MAX_PATHNAME_LENGTH - strlen (kernellib)
             - strlen (OCL_KERNEL_TARGET_CPU));
#endif
  device->kernellib_fallback_name = strdup(kernellib_fallback);
  device->kernellib_name = strdup(kernellib);
  if (device->kernellib_subdir == NULL)
    device->kernellib_subdir = "host";
  device->llvm_abi = pocl_get_llvm_cpu_abi ();

#ifndef ENABLE_SIGFPE_HANDLER
  if (strstr (OCL_KERNEL_TARGET, "x86") != NULL)
    device->run_sanitize_divrem_pass = CL_TRUE;
#endif

#endif

  pocl_init_default_device_infos (device, HOST_DEVICE_EXTENSIONS);

#ifdef HOST_CPU_ENABLE_SPIRV
  device->supported_spirv_extensions = "+SPV_KHR_no_integer_wrap_decoration"
                                       ",+SPV_KHR_expect_assume"
                                       ",+SPV_INTEL_fp_fast_math_mode"
                                       ",+SPV_EXT_shader_atomic_float_add"
                                       ",+SPV_INTEL_memory_access_aliasing"
                                       ",+SPV_INTEL_inline_assembly";

#if LLVM_MAJOR >= 20
  device->supported_spir_v_versions
    = "SPIR-V_1.5 SPIR-V_1.4 SPIR-V_1.3 SPIR-V_1.2 SPIR-V_1.1 SPIR-V_1.0";
#elif LLVM_MAJOR >= 18
  device->supported_spir_v_versions
    = "SPIR-V_1.4 SPIR-V_1.3 SPIR-V_1.2 SPIR-V_1.1 SPIR-V_1.0";
#else
  device->supported_spir_v_versions = "SPIR-V_1.2 SPIR-V_1.1 SPIR-V_1.0";
#endif
#endif

  if (strstr (HOST_DEVICE_EXTENSIONS, "cl_khr_subgroup") != NULL)
    {
      /* In reality there is no independent SG progress implemented in this
         version because we can only have one SG in flight at a time, but it's
         a corner case which allows us to advertise it for full CTS compliance.
       */
      device->sub_group_independent_forward_progress = CL_TRUE;

      /* Just an arbitrary number here based on assumption of SG size 32. */
      device->max_num_sub_groups = device->max_work_group_size / 32;
    }

  if (device->builtin_kernel_list
      && strstr (HOST_DEVICE_EXTENSIONS, "cl_exp_defined_builtin_kernels")
           != NULL)
    {
      POCL_MEM_FREE (device->builtin_kernel_list);
      device->builtin_kernel_list
        = strdup ("pocl.add.i8;"
                  "org.khronos.openvx.scale_image.nn.u8;"
                  "org.khronos.openvx.scale_image.bl.u8;"
                  "org.khronos.openvx.tensor_convert_depth.wrap.u8.f32;"
                  "img_color_convert_exp;"
#ifdef HAVE_LIBXSMM
                  "gemm_exp;"
                  "matmul_exp;"
#endif
#ifdef HAVE_LIBJPEG_TURBO
                  "jpeg_encode_exp;"
                  "jpeg_decode_exp;"
#endif
#ifdef HAVE_ONNXRT
                  "onnx_inference_exp;"
#endif
#ifdef HAVE_OPENCV
                  "nms_box_exp;"
#endif
        );
      device->num_builtin_kernels = 5
#ifdef HAVE_LIBXSMM
                                    + 2
#endif
#ifdef HAVE_LIBJPEG_TURBO
                                    + 2
#endif
#ifdef HAVE_ONNXRT
                                    + 1
#endif
#ifdef HAVE_OPENCV
                                    + 1
#endif
        ;
    }

  /* 0 is the host memory shared with all drivers that use it */
  device->global_mem_id = 0;

#ifndef HOST_CPU_ENABLE_DENORMS
  if (device->single_fp_config)
    device->single_fp_config = device->single_fp_config & (~CL_FP_DENORM);
  if (device->half_fp_config)
    device->half_fp_config = device->half_fp_config & (~CL_FP_DENORM);
#ifndef ENABLE_CONFORMANCE
  /* denorm is mandatory for FP64, but when conformance=OFF
   * we can disable it also for FP64 */
  if (device->double_fp_config)
    device->double_fp_config = device->double_fp_config & (~CL_FP_DENORM);
#endif
#endif

  device->version_of_latest_passed_cts = "v2024-08-08-00";
  device->extensions = HOST_DEVICE_EXTENSIONS;

  device->features = HOST_DEVICE_FEATURES_30;
  if (strstr (HOST_DEVICE_FEATURES_30, "__opencl_c_program_scope_global_variables") != NULL)
    device->run_program_scope_variables_pass = CL_TRUE;
  device->generic_as_support = CL_TRUE;
  device->wg_collective_func_support = CL_TRUE;
  device->device_side_printf = CL_TRUE;

  if (strstr (HOST_DEVICE_EXTENSIONS, "cl_ext_float_atomics") != NULL)
    {
      device->single_fp_atomic_caps = device->double_fp_atomic_caps
        = CL_DEVICE_GLOBAL_FP_ATOMIC_ADD_EXT
          | CL_DEVICE_GLOBAL_FP_ATOMIC_MIN_MAX_EXT
          | CL_DEVICE_LOCAL_FP_ATOMIC_ADD_EXT
          | CL_DEVICE_LOCAL_FP_ATOMIC_MIN_MAX_EXT;
      device->features
        = HOST_DEVICE_FEATURES_30 " __opencl_c_ext_fp32_global_atomic_add"
                                  " __opencl_c_ext_fp64_global_atomic_add"
                                  " __opencl_c_ext_fp32_local_atomic_add"
                                  " __opencl_c_ext_fp64_local_atomic_add"
                                  " __opencl_c_ext_fp32_global_atomic_min_max"
                                  " __opencl_c_ext_fp64_global_atomic_min_max"
                                  " __opencl_c_ext_fp32_local_atomic_min_max"
                                  " __opencl_c_ext_fp64_local_atomic_min_max";
    }

  pocl_setup_opencl_c_with_version (device, CL_TRUE);
  pocl_setup_features_with_version (device);

  pocl_setup_extensions_with_version (device);

  pocl_setup_builtin_kernels_with_version (device);

  pocl_setup_ils_with_version (device);
  pocl_setup_spirv_queries (device);

  device->on_host_queue_props
      = CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE;

#if (!defined(ENABLE_CONFORMANCE)                                             \
     || (defined(ENABLE_CONFORMANCE) && (HOST_DEVICE_CL_VERSION_MAJOR >= 3)))
  /* full memory consistency model for atomic memory and fence operations
  https://www.khronos.org/registry/OpenCL/specs/3.0-unified/html/OpenCL_API.html#opencl-3.0-backwards-compatibility*/
  device->atomic_memory_capabilities
    = CL_DEVICE_ATOMIC_ORDER_RELAXED | CL_DEVICE_ATOMIC_ORDER_ACQ_REL
      | CL_DEVICE_ATOMIC_ORDER_SEQ_CST | CL_DEVICE_ATOMIC_SCOPE_WORK_GROUP
      | CL_DEVICE_ATOMIC_SCOPE_DEVICE | CL_DEVICE_ATOMIC_SCOPE_ALL_DEVICES;
  device->atomic_fence_capabilities
    = CL_DEVICE_ATOMIC_ORDER_RELAXED | CL_DEVICE_ATOMIC_ORDER_ACQ_REL
      | CL_DEVICE_ATOMIC_ORDER_SEQ_CST | CL_DEVICE_ATOMIC_SCOPE_WORK_ITEM
      | CL_DEVICE_ATOMIC_SCOPE_WORK_GROUP | CL_DEVICE_ATOMIC_SCOPE_DEVICE;

  device->svm_allocation_priority = 1;

  /* OpenCL 2.0 properties */
  device->svm_caps = CL_DEVICE_SVM_COARSE_GRAIN_BUFFER
                     | CL_DEVICE_SVM_FINE_GRAIN_BUFFER
                     | CL_DEVICE_SVM_FINE_GRAIN_SYSTEM
                     | CL_DEVICE_SVM_ATOMICS;
#endif

  if (strstr (HOST_DEVICE_EXTENSIONS, "cl_intel_unified_shared_memory")
      != NULL)
    {
      device->host_usm_capabs = CL_UNIFIED_SHARED_MEMORY_ACCESS_INTEL
                                | CL_UNIFIED_SHARED_MEMORY_ATOMIC_ACCESS_INTEL;

      device->device_usm_capabs
          = CL_UNIFIED_SHARED_MEMORY_ACCESS_INTEL
            | CL_UNIFIED_SHARED_MEMORY_ATOMIC_ACCESS_INTEL;

      device->single_shared_usm_capabs
          = CL_UNIFIED_SHARED_MEMORY_ACCESS_INTEL
            | CL_UNIFIED_SHARED_MEMORY_ATOMIC_ACCESS_INTEL;
    }

  /* hwloc probes OpenCL device info at its initialization in case
     the OpenCL extension is enabled. This causes to printout
     an unimplemented property error because hwloc is used to
     initialize global_mem_size which it is not yet. Just put
     a nonzero there for now. */
  device->global_mem_size = 1;
  int err = pocl_topology_detect_device_info (device);
  if (err)
    return CL_INVALID_DEVICE;

  /* device->max_compute_units was set up by topology_detect,
   * but if the user requests, lower it */
  /* if hwloc/topology detection failed, use a fixed maximum */
  int fallback = (device->max_compute_units == 0) ? FALLBACK_MAX_THREAD_COUNT
                                                  : device->max_compute_units;

  /* old env variable */
  int max_threads = pocl_get_int_option ("POCL_MAX_PTHREAD_COUNT", 0);
  if (max_threads <= 0)
    max_threads = pocl_get_int_option ("POCL_CPU_MAX_CU_COUNT", 0);
  if (max_threads <= 0)
    max_threads = pocl_get_int_option ("POCL_MAX_COMPUTE_UNITS", fallback);

  device->max_compute_units
      = max ((unsigned)max_threads, (unsigned)1);

  pocl_cpuinfo_detect_device_info (device);
  pocl_set_buffer_image_limits (device);

  device->local_mem_size = pocl_get_int_option ("POCL_CPU_LOCAL_MEM_SIZE",
                                                device->local_mem_size);
  device->final_linkage_flags = final_ld_flags;

#ifndef ENABLE_CONFORMANCE
  device->cmdbuf_capabilities
    = CL_COMMAND_BUFFER_CAPABILITY_SIMULTANEOUS_USE_KHR
      | CL_COMMAND_BUFFER_CAPABILITY_KERNEL_PRINTF_KHR
      | CL_COMMAND_BUFFER_CAPABILITY_MULTIPLE_QUEUE_KHR;
  device->cmdbuf_required_properties = 0;
  device->cmdbuf_supported_properties = device->on_host_queue_props;
  /* TBD: arguments, in particular buffers, require more work
   * because of migration commands */
  device->cmdbuf_mutable_dispatch_capabilities
    = CL_MUTABLE_DISPATCH_GLOBAL_SIZE_KHR | CL_MUTABLE_DISPATCH_LOCAL_SIZE_KHR
      | CL_MUTABLE_DISPATCH_GLOBAL_OFFSET_KHR;
#endif

  return ret;
}

/* called from kernel setup code.
 * Sets up the actual arguments, except the local ones. */
void
pocl_setup_kernel_arg_array (kernel_run_command *k)
{
  struct pocl_argument *al;

  pocl_kernel_metadata_t *meta = k->kernel->meta;
  cl_uint i;
  void **arguments;
  void **arguments2;
  k->arguments = arguments = malloc (ARGS_SIZE);
  k->arguments2 = arguments2 = malloc (ARGS_SIZE);

  for (i = 0; i < meta->num_args; ++i)
    {
      al = &(k->kernel_args[i]);
      if (ARG_IS_LOCAL (meta->arg_info[i]))
        {
          arguments[i] = NULL;
          arguments2[i] = NULL;
        }
      else if (meta->arg_info[i].type == POCL_ARG_TYPE_POINTER)
        {
          /* It's legal to pass a NULL pointer to clSetKernelArguments. In
             that case we must pass the same NULL forward to the kernel.
             Otherwise, the user must have created a buffer with per device
             pointers stored in the cl_mem. */
          arguments[i] = &arguments2[i];
          if (al->value == NULL)
            {
              arguments2[i] = NULL;
            }
          else
            {
              void *ptr = NULL;
              if (al->is_raw_ptr)
                {
                  ptr = *(void **)al->value;
                }
              else
                {
                  cl_mem m = (*(cl_mem *)(al->value));
                  ptr = m->device_ptrs[k->device->global_mem_id].mem_ptr;
                }
              arguments2[i] = (char *)ptr;
            }
        }
      else if (meta->arg_info[i].type == POCL_ARG_TYPE_IMAGE)
        {
          dev_image_t di = { NULL };
          pocl_fill_dev_image_t (&di, al, k->device);
          void *devptr = pocl_aligned_malloc (MAX_EXTENDED_ALIGNMENT,
                                              sizeof (dev_image_t));
          arguments[i] = &arguments2[i];
          arguments2[i] = devptr;
          memcpy (devptr, &di, sizeof (dev_image_t));
        }
      else if (meta->arg_info[i].type == POCL_ARG_TYPE_SAMPLER)
        {
          dev_sampler_t ds;
          pocl_fill_dev_sampler_t (&ds, al);

          arguments[i] = &arguments2[i];
          arguments2[i] = (void *)ds;
        }
      else
        arguments[i] = al->value;
    }
}

/* called from each driver thread.
 * "arguments" and "arguments2" are the output:
 * driver-thread-local copies of kern args.
 *
 * they're set up by 1) memcpy from kernel_run_command, 2) all
 * local args are set to thread-local "local memory" storage. */
int
pocl_setup_kernel_arg_array_with_locals (void **arguments,
                                         void **arguments2,
                                         kernel_run_command *k,
                                         char *local_mem,
                                         size_t local_mem_size)
{
  pocl_kernel_metadata_t *meta = k->kernel->meta;
  cl_uint i;

  memcpy (arguments2, k->arguments2, ARGS_SIZE);
  memcpy (arguments, k->arguments, ARGS_SIZE);

  char *start = local_mem;

  for (i = 0; i < meta->num_args; ++i)
    {
      if (ARG_IS_LOCAL (meta->arg_info[i]))
        {
          size_t size = k->kernel_args[i].size;
          if (!k->device->device_alloca_locals)
            {
              arguments[i] = &arguments2[i];
              arguments2[i] = start;
              start += size;
              start = align_ptr (start);
              assert ((size_t) (start - local_mem) <= local_mem_size);
            }
          else
            {
              /* Local buffers are allocated in the device side work-group
                 launcher. Let's pass only the sizes of the local args in
                 the arg buffer. */
              assert (sizeof (size_t) == sizeof (void *));
              arguments[i] = (void *)size;
            }
        }
    }
  if (k->device->device_alloca_locals)
    {
      /* Local buffers are allocated in the device side work-group
         launcher. Let's pass only the sizes of the local args in
         the arg buffer. */
      for (i = 0; i < meta->num_locals; ++i)
        {
          assert (sizeof (size_t) == sizeof (void *));
          size_t s = meta->local_sizes[i];
          size_t j = meta->num_args + i;
          *(size_t *)(arguments[j]) = s;
        }
    }
  else
    {
      /* Allocate the automatic local buffers which are implemented as implicit
         extra arguments at the end of the kernel argument list. */
      for (i = 0; i < meta->num_locals; ++i)
        {
          cl_uint j = meta->num_args + i;
          size_t size = meta->local_sizes[i];
          arguments[j] = &arguments2[j];
          arguments2[j] = start;
          if ((size_t)(start - local_mem + size) > local_mem_size)
            {
              size_t total_auto_local_size = 0;
              for (i = 0; j < meta->num_locals; ++j)
                {
                  total_auto_local_size += meta->local_sizes[j];
                }
              POCL_MSG_ERR (
                  "PoCL detected an OpenCL program error: "
                  "%d automatic local buffer(s) with total size %zu "
                  "bytes doesn't fit to the local memory of size %zu\n",
                  meta->num_locals, total_auto_local_size, local_mem_size);
              return CL_FAILED;
            }
          start += size;
          start = align_ptr (start);
        }
    }
  return CL_SUCCESS;
}

/* called from kernel teardown code.
 * frees the actual arguments, except the local ones. */
void
pocl_free_kernel_arg_array (kernel_run_command *k)
{
  cl_uint i;
  pocl_kernel_metadata_t *meta = k->kernel->meta;
  void **arguments = k->arguments;
  void **arguments2 = k->arguments2;

  for (i = 0; i < meta->num_args; ++i)
    {
      if (ARG_IS_LOCAL (meta->arg_info[i]))
        {
          if (!k->device->device_alloca_locals)
            {
              assert (arguments[i] == NULL);
              assert (arguments2[i] == NULL);
            }
          else
            {
              /* Device side local space allocation has deallocation via stack
                 unwind. */
            }
        }
      else if (meta->arg_info[i].type == POCL_ARG_TYPE_IMAGE)
        {
          pocl_aligned_free (arguments2[i]);
        }
    }

  POCL_MEM_FREE (k->arguments);
  POCL_MEM_FREE (k->arguments2);
}

/* called from each driver thread.
 * frees the local arguments. */
void
pocl_free_kernel_arg_array_with_locals (void **arguments, void **arguments2,
                                   kernel_run_command *k)
{
  pocl_kernel_metadata_t *meta = k->kernel->meta;
  cl_uint i;

  for (i = 0; i < meta->num_args; ++i)
    {
      if (ARG_IS_LOCAL (meta->arg_info[i]))
        {
          arguments[i] = NULL;
          arguments2[i] = NULL;
        }
    }

  for (i = 0; i < meta->num_locals; ++i)
    {
      arguments[meta->num_args + i] = NULL;
      arguments2[meta->num_args + i] = NULL;
    }
}

/***************************************************************************/


#ifdef HAVE_LIBXSMM

static libxsmm_datatype
pocl_convert_to_libxsmm_type (cl_tensor_datatype_exp T)
{
  switch (T)
    {
    case CL_TENSOR_DTYPE_FP64_EXP:
      return LIBXSMM_DATATYPE_F64;
    case CL_TENSOR_DTYPE_FP32_EXP:
      return LIBXSMM_DATATYPE_F32;
    case CL_TENSOR_DTYPE_FP16_EXP:
      return LIBXSMM_DATATYPE_F16;
    case CL_TENSOR_DTYPE_INT64_EXP:
      return LIBXSMM_DATATYPE_I64;
    case CL_TENSOR_DTYPE_UINT64_EXP:
      return LIBXSMM_DATATYPE_U64;
    case CL_TENSOR_DTYPE_INT32_EXP:
      return LIBXSMM_DATATYPE_I32;
    case CL_TENSOR_DTYPE_UINT32_EXP:
      return LIBXSMM_DATATYPE_U32;
    case CL_TENSOR_DTYPE_INT16_EXP:
      return LIBXSMM_DATATYPE_I16;
    case CL_TENSOR_DTYPE_UINT16_EXP:
      return LIBXSMM_DATATYPE_U16;
    case CL_TENSOR_DTYPE_INT8_EXP:
      return LIBXSMM_DATATYPE_I8;
    case CL_TENSOR_DTYPE_UINT8_EXP:
      return LIBXSMM_DATATYPE_U8;
    case CL_TENSOR_DTYPE_INT4_EXP:
      return LIBXSMM_DATATYPE_IMPLICIT;
    case CL_TENSOR_DTYPE_UINT4_EXP:
      return LIBXSMM_DATATYPE_IMPLICIT;

    default:
      return LIBXSMM_DATATYPE_UNSUPPORTED;
    }
}

int
pocl_cpu_validate_khr_gemm (cl_bool TransA,
                            cl_bool TransB,
                            const cl_tensor_desc_exp *TenA,
                            const cl_tensor_desc_exp *TenB,
                            const cl_tensor_desc_exp *TenCIOpt,
                            const cl_tensor_desc_exp *TenCOut,
                            const cl_tensor_datatype_value_exp *Alpha,
                            const cl_tensor_datatype_value_exp *Beta)
{
  /* TODO: We probably need to have support for mixed input/output
   * precisions to be able to fit results of large, low precision input
   * matrices. precision inputs. E.g.
   *
   *  * i8 x i8   --> i32
   *  * f16 x f16 --> f32
   */

  /* datatype match between A&B and CIopt&COut already checked in
   * initial validation (pocl_validate_khr_gemm) */

  /* currently FP 16-64 and INT 8-64 are supported */
  /* FIXME: This check does not scale well. convert this into
            whitelisted check. */
  POCL_RETURN_ERROR_ON ((TenA->dtype == CL_TENSOR_DTYPE_FP8E4M3_EXP
                         || TenA->dtype == CL_TENSOR_DTYPE_FP8E5M2_EXP
                         || TenA->dtype == CL_TENSOR_DTYPE_INT4_EXP
                         || TenCOut->dtype == CL_TENSOR_DTYPE_INT4_EXP),
                        CL_INVALID_TENSOR_DATATYPE_EXP,
                        "Datatype support not yet implemented. CPU supports "
                        "only FP16/32/64 and INT8/16/32/64 currently\n");

  /* type mixing check */
  POCL_RETURN_ERROR_ON ((pocl_tensor_type_is_int (TenA->dtype)
                         != pocl_tensor_type_is_int (TenCOut->dtype)),
                        CL_INVALID_TENSOR_DATATYPE_EXP,
                        "Datatype mixing (INT/FP) not supported");

  POCL_RETURN_ERROR_ON ((pocl_tensor_type_size (TenA->dtype)
                         > pocl_tensor_type_size (TenCOut->dtype)),
                        CL_INVALID_TENSOR_DATATYPE_EXP,
                        "Datatype of C is smaller than A");

  const cl_tensor_properties_exp P = TenA->properties[0];
  if (P != 0)
    {
      POCL_RETURN_ERROR_ON ((P == CL_TENSOR_PROPERTY_MUTABLE_DTYPE_EXP),
                            CL_INVALID_TENSOR_PROPERTY_EXP,
                            "CPU driver does not "
                            "support CL_TENSOR_PROPERTY_MUTABLE_DTYPE_EXP\n");
      POCL_RETURN_ERROR_ON ((P == CL_TENSOR_PROPERTY_MUTABLE_LAYOUT_EXP),
                            CL_INVALID_TENSOR_PROPERTY_EXP,
                            "CPU driver does not "
                            "support CL_TENSOR_PROPERTY_MUTABLE_LAYOUT_EXP\n");
      // Mutable dims are supported by CPU
      POCL_RETURN_ERROR_ON ((P != CL_TENSOR_PROPERTY_MUTABLE_SHAPE_EXP),
                            CL_INVALID_TENSOR_PROPERTY_EXP,
                            "Unknown Property %" PRIu64 "\n", P);
    }

  /* TODO check the value in respective type */
  if (Alpha)
    {
      cl_bool IsAlphaOne
        = pocl_tensor_dtype_value_equals (TenA->dtype, Alpha, 1.0, 1, 1, 1, 1);

      POCL_RETURN_ERROR_ON (IsAlphaOne == CL_FALSE,
                            CL_DBK_INVALID_ATTRIBUTE_EXP,
                            "CPU supports only Alpha == 1.0\n");
    }
  if (Beta)
    {
      cl_bool IsBetaOne
        = pocl_tensor_dtype_value_equals (TenA->dtype, Beta, 1.0, 1, 1, 1, 1);

      cl_bool IsBetaZero
        = pocl_tensor_dtype_value_equals (TenA->dtype, Beta, 0.0, 0, 0, 0, 0);

      POCL_RETURN_ERROR_ON ((!IsBetaOne && !IsBetaZero),
                            CL_DBK_INVALID_ATTRIBUTE_EXP,
                            "CPU supports only Beta == 0.0 or 1.0\n");
    }

  /* TODO: check validity of data layouts of the tensors. Now assume
   * they are correct and they are using BLAS-like layout. */

  return CL_SUCCESS;
}
#endif

int
pocl_cpu_supports_dbk (cl_device_id device,
                       cl_dbk_id_exp kernel_id,
                       const void *kernel_attributes)
{
  switch (kernel_id)
    {
#ifdef HAVE_LIBXSMM
    case CL_DBK_GEMM_EXP:
    case CL_DBK_MATMUL_EXP:
      {
        /* The following code checks for LIBXSMM specific requirements put
         * on the tensors that are part of the kernel attributes. */
        return pocl_validate_dbk_attributes (kernel_id, kernel_attributes,
                                             pocl_cpu_validate_khr_gemm);
      }
#endif
#ifdef HAVE_LIBJPEG_TURBO
    case CL_DBK_JPEG_DECODE_EXP:
    case CL_DBK_JPEG_ENCODE_EXP:
      return pocl_validate_dbk_attributes (kernel_id, kernel_attributes, NULL);
#endif
#ifdef HAVE_ONNXRT
    case CL_DBK_ONNX_INFERENCE_EXP:
      return pocl_validate_dbk_attributes (kernel_id, kernel_attributes, NULL);
#endif
    case CL_DBK_IMG_COLOR_CONVERT_EXP:
      return CL_SUCCESS;
#ifdef HAVE_OPENCV
    case CL_DBK_NMS_BOX_EXP:
      return pocl_validate_dbk_attributes (kernel_id, kernel_attributes, NULL);
#endif
    default:
      POCL_RETURN_ERROR (
        CL_DBK_UNSUPPORTED_EXP,
        "The CPU driver does not support DBK (kernel id %d).\n", kernel_id);
    }
}

void
pocl_cpu_probe ()
{
#ifdef HAVE_LIBXSMM
  libxsmm_init ();
#endif
}

int
pocl_cpu_build_defined_builtin (cl_program program, cl_uint device_i)
{

#ifdef HAVE_LIBXSMM
  /* TODO perhaps prebuild something here ? */
  return CL_SUCCESS;
#endif
#ifdef HAVE_LIBJPEG_TURBO
  return CL_SUCCESS;
#endif
#ifdef HAVE_ONNXRT
  return CL_SUCCESS;
#endif
#ifdef HAVE_OPENCV
  return CL_SUCCESS;
#endif
  /* TODO: is it necessary to return an error here or can it be caught earlier
     on? */
  POCL_RETURN_ERROR (
    CL_BUILD_PROGRAM_FAILURE,
    "The CPU driver has not been compiled with support for DBKs\n");
}

#ifdef HAVE_LIBXSMM

static cl_bool
tensor_is_blas_row_major (const cl_tensor_desc_exp *A)
{
  assert (A);
  assert (A->layout && "Does not have data layout!");
  assert ((A->layout_type == CL_TENSOR_LAYOUT_BLAS_EXP
           || A->layout_type == CL_TENSOR_LAYOUT_BLAS_PITCHED_EXP)
          && "The method must not be called for tensors with non-BLAS data "
             "layouts");
  const cl_tensor_layout_blas_exp *BL
    = (const cl_tensor_layout_blas_exp *)A->layout;
  assert (A->rank >= 2 && "Not a (batched) matrix!");

  return BL->leading_dims[0] == (A->rank - 1u) ? CL_TRUE : CL_FALSE;
}

static unsigned
tensor_get_trailing_dim (const cl_tensor_desc_exp *A,
                         const cl_tensor_dim_exp *leading_dims)
{
  assert (A);
  assert ((A->rank < (sizeof (unsigned) * 8))
          && "Too many dimensions for the bitset.");

  unsigned DimSet = (1u << A->rank) - 1;
  for (unsigned I = 0; I < A->rank - 1; I++)
    DimSet &= ~(1u << leading_dims[I]);

  assert (__builtin_popcount (DimSet) == 1 && "Invalid data layout?");
  unsigned TrailingDim = __builtin_ctz (DimSet);
  assert (TrailingDim < A->rank);
  return TrailingDim;
}

static cl_tensor_stride_exp
tensor_get_blas_stride_in_elements (const cl_tensor_desc_exp *A, unsigned Dim)
{
  assert (A);
  assert (A->rank >= 2);
  assert (A->layout && "Does not have data layout!");
  assert ((A->layout_type == CL_TENSOR_LAYOUT_BLAS_PITCHED_EXP
           || A->layout_type == CL_TENSOR_LAYOUT_BLAS_EXP)
          && "The method must not be called for tensors with non-BLAS data "
             "layouts");

  if (A->layout_type == CL_TENSOR_LAYOUT_BLAS_EXP)
    {
      const cl_tensor_layout_blas_exp *BL = A->layout;
      cl_tensor_stride_exp stride = 1;
      for (unsigned i = 0; i < Dim; i++)
        {
          assert (A->shape[BL->leading_dims[i]]);
          stride *= A->shape[BL->leading_dims[i]];
        }
      return stride;
    }

  const cl_tensor_layout_blas_pitched_exp *BL = A->layout;
  if (Dim < (A->rank - 1))
    return BL->leading_strides[Dim];
  else
    return BL->leading_strides[A->rank - 1]
           * tensor_get_trailing_dim (A, BL->leading_dims);
}

static int
pocl_cpu_execute_gemm_anytype (char *Aptr,
                               char *Bptr,
                               char *COut,
                               char *CIopt,
                               libxsmm_datatype InElemType,
                               size_t InElemSize,
                               libxsmm_datatype OutElemType,
                               size_t OutElemSize,
                               cl_bool TransposeA,
                               cl_bool TransposeB,
                               const cl_tensor_desc_exp *TenA,
                               const cl_tensor_desc_exp *TenB,
                               const cl_tensor_desc_exp *TenCout,
                               const cl_tensor_desc_exp *TenCIOpt,
                               float Alpha,
                               float Beta)
{
  libxsmm_datatype CompElemType = OutElemType;
  size_t CompElemSize = OutElemSize;

  size_t BatchDims = TenA->rank - 2;
  size_t Am = TenA->shape[BatchDims + 0];
  size_t Ak = TenA->shape[BatchDims + 1];
  if (TransposeA)
    {
      size_t Temp = Am;
      Am = Ak;
      Ak = Temp;
    }

  size_t Bk = TenB->shape[BatchDims + 0];
  size_t Bn = TenB->shape[BatchDims + 1];
  if (TransposeB)
    {
      size_t Temp = Bk;
      Bk = Bn;
      Bn = Temp;
    }

  size_t COm = TenCout->shape[BatchDims + 0];
  size_t COn = TenCout->shape[BatchDims + 1];

  assert (Ak == Bk);
  assert (Am == COm);
  assert (Bn == COn);

  size_t Lda = tensor_get_blas_stride_in_elements (TenA, 0);
  size_t Ldb = tensor_get_blas_stride_in_elements (TenB, 0);
  size_t Ldc = tensor_get_blas_stride_in_elements (TenCout, 0);
  size_t ABatchStrideInElts = tensor_get_blas_stride_in_elements (TenA, 1);
  size_t BBatchStrideInElts = tensor_get_blas_stride_in_elements (TenB, 1);
  size_t CBatchStrideInElts = tensor_get_blas_stride_in_elements (TenCout, 1);

  /* libxsmm expects data in column-major format but we can feed it
   * row-major data by transposing the inputs and and the output. */
  cl_bool LibTransposeA = TransposeA ^ tensor_is_blas_row_major (TenA);
  cl_bool LibTransposeB = TransposeB ^ tensor_is_blas_row_major (TenB);

  int flags_trans = (LibTransposeA ? LIBXSMM_GEMM_FLAG_TRANS_A : 0)
                    | (LibTransposeB ? LIBXSMM_GEMM_FLAG_TRANS_B : 0);
  int flags_ab = (LIBXSMM_NEQ (0.0f, Beta) ? 0 : LIBXSMM_GEMM_FLAG_BETA_0);

  /*    POCL_MSG_WARN( "Trans_A: %u Trans_B: %u Alpha: %f Beta: %f\n",
                      LibTransposeA, LibTransposeB, Alpha, Beta);
  */

  /* determine matrix shape and precision */
  const libxsmm_gemm_shape gemm_shape = libxsmm_create_gemm_shape (
    COm, COn, Ak,
    // m /*lda*/, k /*ldb*/, m /*ldc*/,
    Lda, Ldb, Ldc, InElemType, InElemType, OutElemType, CompElemType);

  /* generate and dispatch a matrix multiplication kernel */
  const libxsmm_gemmfunction kernel = libxsmm_dispatch_gemm (
    gemm_shape, (libxsmm_bitfield)(flags_trans | flags_ab),
    (libxsmm_bitfield)LIBXSMM_GEMM_PREFETCH_NONE);
  assert (NULL != kernel && "LIBXSMM: JIT generation of kernel failed");

  libxsmm_gemm_param gemm_param
    = { 0 }; /* collect call-arguments into single structure */

  size_t BatchSize = TenA->rank > 2 ? TenA->shape[0] : 1;

  for (size_t BatchIndex = 0; BatchIndex < BatchSize; ++BatchIndex)
    {

      char *Src = &CIopt[BatchIndex * CBatchStrideInElts * OutElemSize];
      char *Dst = &COut[BatchIndex * CBatchStrideInElts * OutElemSize];

      if (TenCIOpt && Beta != 0.0f)
        {
          if (tensor_is_blas_row_major (TenCIOpt))
            {
              /* Need to convert C input to column-major. */
              libxsmm_otrans (Dst, Src, OutElemSize, COm, COn, Ldc, COm);
            }
          else
            {
              /* copy CIn to COut */
              libxsmm_matcopy (Dst, Src, OutElemSize, COm, COn, Ldc, COm);
            }
        }
      else
        {
          /* Zero-initialize. */
          libxsmm_matcopy (Dst, NULL, OutElemSize, COm, COn, Ldc, COm);
        }

      gemm_param.a.primary
        = &Aptr[BatchIndex * ABatchStrideInElts * InElemSize];
      gemm_param.b.primary
        = &Bptr[BatchIndex * BBatchStrideInElts * InElemSize];
      gemm_param.c.primary
        = &COut[BatchIndex * CBatchStrideInElts * OutElemSize];
      kernel (&gemm_param);

      if (tensor_is_blas_row_major (TenCout))
        {
          /* Results are always in column-major. */
          libxsmm_itrans (Dst, OutElemSize, COm, COn, COm, Ldc);
        }
    }

  return CL_SUCCESS;
}

static int
pocl_xsmm_execute_dbk (cl_program program,
                       cl_kernel kernel,
                       pocl_kernel_metadata_t *meta,
                       cl_uint dev_i,
                       struct pocl_argument *arguments)
{
  cl_device_id dev = program->devices[dev_i];
  unsigned mem_id = dev->global_mem_id;
  void *A = pocl_cpu_get_ptr (&arguments[0], mem_id);
  void *B = pocl_cpu_get_ptr (&arguments[1], mem_id);
  void *Cin = NULL;
  void *Cout = pocl_cpu_get_ptr (&arguments[2], mem_id);
  float Alpha = 1.0f, Beta = 0.0f;
  cl_tensor_datatype_exp InDtype, OutDtype;
  cl_bool TransposeA, TransposeB;
  const cl_tensor_desc_exp *TenA;
  const cl_tensor_desc_exp *TenB;
  const cl_tensor_desc_exp *TenCout;
  const cl_tensor_desc_exp *TenCIOpt;

  switch (meta->builtin_kernel_id)
    {
    case CL_DBK_GEMM_EXP:
      {
        const cl_dbk_attributes_gemm_exp *Attrs
          = (const cl_dbk_attributes_gemm_exp *)meta->builtin_kernel_attrs;
        void *Cin = pocl_cpu_get_ptr (&arguments[2], mem_id);
        void *Cout = pocl_cpu_get_ptr (&arguments[3], mem_id);
        memcpy (&Alpha, arguments[4].value, sizeof (float));
        memcpy (&Beta, arguments[5].value, sizeof (float));
        InDtype = Attrs->a.dtype;
        OutDtype = Attrs->c_out.dtype;
        TransposeA = Attrs->trans_a;
        TransposeB = Attrs->trans_b;
        TenA = &Attrs->a;
        TenB = &Attrs->b;
        TenCout = &Attrs->c_out;
        TenCIOpt = &Attrs->c_in;
        break;
      }
    case CL_DBK_MATMUL_EXP:
      {
        const cl_dbk_attributes_matmul_exp *Attrs
          = (const cl_dbk_attributes_matmul_exp *)meta->builtin_kernel_attrs;
        InDtype = Attrs->a.dtype;
        OutDtype = Attrs->c.dtype;
        TransposeA = Attrs->trans_a;
        TransposeB = Attrs->trans_b;
        TenA = &Attrs->a;
        TenB = &Attrs->b;
        TenCout = &Attrs->c;
        TenCIOpt = NULL;
        break;
      }
    default:
      POCL_MSG_ERR ("this code path should have "
                    "been eliminated earlier");
      return CL_FAILED;
    }

  libxsmm_datatype InElemType = pocl_convert_to_libxsmm_type (InDtype);
  size_t InElemSize = pocl_tensor_type_size (InDtype);
  libxsmm_datatype OutElemType = pocl_convert_to_libxsmm_type (OutDtype);
  size_t OutElemSize = pocl_tensor_type_size (OutDtype);

  return pocl_cpu_execute_gemm_anytype (
    A, B, Cout, Cin, InElemType, InElemSize, OutElemType, OutElemSize,
    TransposeA, TransposeB, TenA, TenB, TenCout, TenCIOpt, Alpha, Beta);
}

#endif

int
pocl_cpu_execute_dbk (cl_program program,
                      cl_kernel kernel,
                      pocl_kernel_metadata_t *meta,
                      cl_uint dev_i,
                      struct pocl_argument *arguments)
{
  switch (meta->builtin_kernel_id)
    {
#ifdef HAVE_LIBXSMM
    case CL_DBK_GEMM_EXP:
    case CL_DBK_MATMUL_EXP:
      return pocl_xsmm_execute_dbk (program, kernel, meta, dev_i, arguments);
#endif
#ifdef HAVE_LIBJPEG_TURBO
    case CL_DBK_JPEG_ENCODE_EXP:
      return pocl_cpu_execute_dbk_khr_jpeg_encode (program, kernel, meta,
                                                   dev_i, arguments);
    case CL_DBK_JPEG_DECODE_EXP:
      return pocl_cpu_execute_dbk_khr_jpeg_decode (program, kernel, meta,
                                                   dev_i, arguments);
#endif
#ifdef HAVE_ONNXRT
    case CL_DBK_ONNX_INFERENCE_EXP:
      {
        cl_device_id dev = program->devices[dev_i];
        unsigned mem_id = dev->global_mem_id;
        return pocl_perform_ort_inference (
            kernel->data[dev_i], pocl_cpu_get_ptr (&arguments[0], mem_id),
            pocl_cpu_get_ptr (&arguments[1], mem_id),
            pocl_cpu_get_ptr (&arguments[2], mem_id),
            pocl_cpu_get_ptr (&arguments[3], mem_id));
      }
#endif
    case CL_DBK_IMG_COLOR_CONVERT_EXP:
      return pocl_cpu_execute_dbk_exp_img_yuv2rgb (program, kernel, meta,
                                                   dev_i, arguments);
#ifdef HAVE_OPENCV
    case CL_DBK_NMS_BOX_EXP:
      return pocl_cpu_execute_dbk_khr_nms_box (program, kernel, meta, dev_i,
                                               arguments);
#endif
    default:
      {
        POCL_MSG_ERR ("Unhandled DBK id %d.\n", meta->builtin_kernel_id);
        return CL_FAILED;
      }
    }
}
