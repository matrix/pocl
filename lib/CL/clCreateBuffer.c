/* OpenCL runtime library: clCreateBuffer()

   Copyright (c) 2011-2013 Universidad Rey Juan Carlos
                           Pekka Jääskeläinen / Tampere University of Tech.
                 2024 Pekka Jääskeläinen / Intel Finland Oy

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

#include "pocl_cl.h"
#include "common.h"
#include "devices.h"
#include "pocl_shared.h"
#include "pocl_tensor_util.h"
#include "pocl_util.h"

cl_mem
pocl_create_memobject (cl_context context,
                       cl_mem_flags flags,
                       size_t size,
                       cl_mem_object_type type,
                       int **device_image_support,
                       void *host_ptr,
                       int host_ptr_is_svm,
                       cl_int *errcode_ret)
{
  cl_mem mem = NULL;
  int errcode = CL_SUCCESS;
  unsigned i;

  POCL_GOTO_ERROR_COND((size == 0), CL_INVALID_BUFFER_SIZE);

  POCL_GOTO_ERROR_COND ((!IS_CL_OBJECT_VALID (context)), CL_INVALID_CONTEXT);

  if (flags == 0)
    flags = CL_MEM_READ_WRITE;

  /* validate presence of extension on all devices */
  if (flags & CL_MEM_DEVICE_PRIVATE_ADDRESS_EXT)
    {
      POCL_GOTO_ERROR_ON (
        context->no_devices_support_bda, CL_INVALID_DEVICE,
        "Requested buffer_device_address allocation, but none of devices "
        "in the context supports the 'cl_ext_buffer_device_address' "
        "extension.");
    }
  cl_mem_flags other_flags = flags & ~CL_MEM_DEVICE_PRIVATE_ADDRESS_EXT;
  POCL_GOTO_ERROR_ON (
    (other_flags > (1 << 10) - 1), CL_INVALID_VALUE,
    "Unknown flag - PoCL only recognizes 10 non-SVM flags\n");

  POCL_GOTO_ERROR_ON (
      ((flags & CL_MEM_READ_WRITE)
       && (flags & CL_MEM_WRITE_ONLY || flags & CL_MEM_READ_ONLY)),
      CL_INVALID_VALUE,
      "Invalid flags: CL_MEM_READ_WRITE cannot be used "
      "together with CL_MEM_WRITE_ONLY or CL_MEM_READ_ONLY\n");

  POCL_GOTO_ERROR_ON (
      ((flags & CL_MEM_READ_ONLY) && (flags & CL_MEM_WRITE_ONLY)),
      CL_INVALID_VALUE,
      "Invalid flags: "
      "can't have both CL_MEM_WRITE_ONLY and CL_MEM_READ_ONLY\n");

  POCL_GOTO_ERROR_ON (
      ((flags & CL_MEM_USE_HOST_PTR)
       && (flags & CL_MEM_ALLOC_HOST_PTR || flags & CL_MEM_COPY_HOST_PTR)),
      CL_INVALID_VALUE,
      "Invalid flags: CL_MEM_USE_HOST_PTR cannot be used "
      "together with CL_MEM_ALLOC_HOST_PTR or CL_MEM_COPY_HOST_PTR\n");

  POCL_GOTO_ERROR_ON (
      ((flags & CL_MEM_HOST_WRITE_ONLY) && (flags & CL_MEM_HOST_READ_ONLY)),
      CL_INVALID_VALUE,
      "Invalid flags: "
      "can't have both CL_MEM_HOST_READ_ONLY and CL_MEM_HOST_WRITE_ONLY\n");

  POCL_GOTO_ERROR_ON (
      ((flags & CL_MEM_HOST_NO_ACCESS)
       && ((flags & CL_MEM_HOST_READ_ONLY)
           || (flags & CL_MEM_HOST_WRITE_ONLY))),
      CL_INVALID_VALUE,
      "Invalid flags: CL_MEM_HOST_NO_ACCESS cannot be used "
      "together with CL_MEM_HOST_READ_ONLY or CL_MEM_HOST_WRITE_ONLY\n");

  if (host_ptr == NULL)
    {
      POCL_GOTO_ERROR_ON (
          ((flags & CL_MEM_USE_HOST_PTR) || (flags & CL_MEM_COPY_HOST_PTR)),
          CL_INVALID_HOST_PTR,
          "host_ptr is NULL, but flags specify {COPY|USE}_HOST_PTR\n");
    }
  else
    {
      POCL_GOTO_ERROR_ON (
          ((~flags & CL_MEM_USE_HOST_PTR) && (~flags & CL_MEM_COPY_HOST_PTR)),
          CL_INVALID_HOST_PTR,
          "host_ptr is not NULL, but flags don't specify "
          "{COPY|USE}_HOST_PTR\n");
    }

  POCL_GOTO_ERROR_ON ((size > context->max_mem_alloc_size),
                      CL_INVALID_BUFFER_SIZE,
                      "Size (%zu) is bigger than max mem alloc size (%zu) "
                      "of all devices in context\n",
                      size, (size_t)context->max_mem_alloc_size);

  mem = (cl_mem)calloc (1, sizeof (struct _cl_mem));
  POCL_GOTO_ERROR_COND ((mem == NULL), CL_OUT_OF_HOST_MEMORY);

  POCL_INIT_OBJECT (mem, context);
  mem->type = type;
  mem->flags = flags;
  if (device_image_support)
    {
      mem->device_supports_this_image = *device_image_support;
      *device_image_support = NULL;
    }

  mem->device_ptrs = (pocl_mem_identifier *)calloc (
      POCL_ATOMIC_LOAD (pocl_num_devices), sizeof (pocl_mem_identifier));
  POCL_GOTO_ERROR_COND ((mem->device_ptrs == NULL), CL_OUT_OF_HOST_MEMORY);

  mem->size = size;
  mem->context = context;
  mem->is_image = (type != CL_MEM_OBJECT_PIPE && type != CL_MEM_OBJECT_BUFFER);
  mem->is_pipe = (type == CL_MEM_OBJECT_PIPE);
  mem->mem_host_ptr_version = 0;
  mem->latest_version = 0;

  if (flags & CL_MEM_DEVICE_PRIVATE_ADDRESS_EXT)
    {
      mem->has_device_address = 1;
    }

  if (flags & CL_MEM_USE_HOST_PTR)
    {
      POCL_MSG_PRINT_MEMORY ("CL_MEM_USE_HOST_PTR %p \n", host_ptr);
      assert (host_ptr);
      mem->mem_host_ptr = host_ptr;
      mem->mem_host_ptr_version = 1;
      mem->mem_host_ptr_refcount = 1;
      mem->mem_host_ptr_is_svm = host_ptr_is_svm;
      mem->latest_version = 1;
    }

  /* If ALLOC flag is present, try to pre-allocate host-visible
   * backing store memory from a driver.
   * First driver to allocate for a physical memory wins; if none of
   * the drivers do it, we allocate the backing store via malloc */
  if (flags & CL_MEM_ALLOC_HOST_PTR)
    {
      POCL_MSG_PRINT_MEMORY (
          "Trying driver allocation for CL_MEM_ALLOC_HOST_PTR\n");
      unsigned i;
      for (i = 0; i < context->num_devices; ++i)
        {
          cl_device_id dev = context->devices[i];
          assert (dev->ops->alloc_mem_obj != NULL);
          /* skip already allocated */
          if (mem->device_ptrs[dev->global_mem_id].mem_ptr != NULL)
            continue;
          int err = dev->ops->alloc_mem_obj (dev, mem, host_ptr);

          if ((err == CL_SUCCESS) && (mem->mem_host_ptr))
            break;
        }

      POCL_GOTO_ERROR_ON ((pocl_alloc_or_retain_mem_host_ptr (mem) != 0),
                          CL_OUT_OF_HOST_MEMORY,
                          "Cannot allocate backing memory!\n");
      mem->mem_host_ptr_version = 0;
      mem->latest_version = 0;
    }

  /* With cl_ext_buffer_device_address we must proactively allocate the device
     memory so it gets the fixed address range assigned, even if the buffer was
     never used. The address can be queried via clGetMemobjInfo() and used
     inside data structures. */
  if (flags & CL_MEM_DEVICE_PRIVATE_ADDRESS_EXT)
    {
      POCL_MSG_PRINT_MEMORY (
        "Trying driver allocation for cl_ext_buffer_device_address\n");
      unsigned i;
      void *ptr = NULL;
      for (i = 0; i < context->num_devices; ++i)
        {
          cl_device_id dev = context->devices[i];
          assert (dev->ops->alloc_mem_obj != NULL);
          int err = 0;

          if (mem->device_ptrs[dev->global_mem_id].mem_ptr == NULL)
            {
              err = dev->ops->alloc_mem_obj (dev, mem, host_ptr);
              ptr = mem->device_ptrs[dev->global_mem_id].mem_ptr;
              POCL_GOTO_ERROR_ON (err != CL_SUCCESS, CL_OUT_OF_RESOURCES,
                                  "Out of device memory?");

              pocl_raw_ptr *item = calloc (1, sizeof (pocl_raw_ptr));
              POCL_RETURN_ERROR_ON ((item == NULL), NULL,
                                    "out of host memory\n");

	      size_t mem_size = size;
	      if (strcmp(dev->ops->device_name, "remote") == 0)
		{
		  /* A work-around for test_svm case for remote
		   * devices which currently doesn't implement BDA
		   * extension properly. It uses mem->id as .mem_ptr
		   * address which does not work out for BDA extension
		   * well. Buffers created back-to-back will likely
		   * have their addresses overlapping (which is caught
		   * by pocl_raw_ptr_set_insert() ahead) which means
		   * when we can't tell which buffer a BDA pointer is
		   * derived from in clSetKernelArgDevicePointerEXT().
		   *
		   * The following work-around gets past the
		   * pocl_raw_ptr_set_insert() call
		   * clSetKernelArgDevicePointerEXT() should work on
		   * BDA pointers pointing to the beginning of the
		   * buffers but offseted ones not.
		   */
		  mem_size = 1;
		}

              POCL_LOCK_OBJ (context);
              item->vm_ptr = NULL;
              item->dev_ptr = ptr;
              item->device = dev;
              item->size = mem_size;
              item->shadow_cl_mem = mem;
              int inserted = pocl_raw_ptr_set_insert (context->raw_ptrs, item);
              assert (inserted);
              (void)inserted;

              POCL_UNLOCK_OBJ (context);

              POCL_MSG_PRINT_MEMORY (
                "Registered a cl_ext_buffer_device_address"
                " allocation with address '%p'.\n",
                ptr);
              mem->device_ptrs[dev->global_mem_id].device_addr = ptr;
            }
        }
    }

  /* If COPY_HOST_PTR is present but no copying happened,
     do the copy here. */
  if ((flags & CL_MEM_COPY_HOST_PTR) && (mem->mem_host_ptr_version == 0))
    {
      POCL_GOTO_ERROR_ON ((pocl_alloc_or_retain_mem_host_ptr (mem) != 0),
                          CL_OUT_OF_HOST_MEMORY,
                          "Cannot allocate backing memory!\n");
      memcpy (mem->mem_host_ptr, host_ptr, size);
      mem->mem_host_ptr_version = 1;
      mem->latest_version = 1;
    }

  goto SUCCESS;

ERROR:
  if (mem)
  {
    if (mem->device_ptrs)
    {
      for (i = 0; i < context->num_devices; ++i)
        {
          cl_device_id dev = context->devices[i];
          pocl_mem_identifier *p = &mem->device_ptrs[dev->global_mem_id];
          if (p->mem_ptr)
            dev->ops->free (dev, mem);
        }
      POCL_MEM_FREE (mem->device_ptrs);
    }

    if (((flags & CL_MEM_USE_HOST_PTR) == 0) && mem->mem_host_ptr)
      POCL_MEM_FREE (mem->mem_host_ptr);

    POCL_MEM_FREE (mem);
  }

SUCCESS:
  if (errcode_ret)
    {
      *errcode_ret = errcode;
    }

  return mem;
}



CL_API_ENTRY cl_mem CL_API_CALL POname (clCreateBuffer) (
    cl_context context, cl_mem_flags flags, size_t size, void *host_ptr,
    cl_int *errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
  cl_mem mem = NULL;
  int errcode = CL_SUCCESS;
  int host_ptr_is_svm = CL_FALSE;

  if ((flags & CL_MEM_USE_HOST_PTR) && host_ptr != NULL)
    {
      pocl_raw_ptr *item = pocl_find_raw_ptr_with_vm_ptr (context, host_ptr);
      if (item)
        {
          POCL_GOTO_ERROR_ON ((item->size < size), CL_INVALID_BUFFER_SIZE,
                              "The provided host_ptr is SVM pointer, "
                              "but the allocated SVM size (%zu) is smaller "
                              "then requested size (%zu)",
                              item->size, size);
          host_ptr_is_svm = CL_TRUE;
        }
    }

  mem = pocl_create_memobject (context, flags, size, CL_MEM_OBJECT_BUFFER,
                               NULL, host_ptr, host_ptr_is_svm, &errcode);
  if (mem == NULL)
    goto ERROR;

  TP_CREATE_BUFFER (context->id, mem->id);

  POname(clRetainContext)(context);

  POCL_MSG_PRINT_MEMORY ("Created Buffer %" PRIu64 " (%p), MEM_HOST_PTR: %p, "
                         "device_ptrs[0]: %p, SIZE %zu, FLAGS %" PRIu64 " \n",
                         mem->id, mem, mem->mem_host_ptr,
                         mem->device_ptrs[0].mem_ptr, size, flags);

  POCL_ATOMIC_INC (buffer_c);

ERROR:
  if (errcode_ret)
    *errcode_ret = errcode;

  return mem;
}
POsym (clCreateBuffer);

static cl_int
pocl_parse_cl_mem_properties (const cl_mem_properties *prop_ptr,
                              const cl_tensor_desc_exp **tdesc,
                              cl_mem_properties *device_address_ext)
{
  *device_address_ext = 0;

  if (!prop_ptr)
    {
      return CL_SUCCESS;
    }

  if (*prop_ptr == 0)
    {
      return CL_SUCCESS;
    }

  while (*prop_ptr)
    {
      switch (*prop_ptr)
        {
        case CL_MEM_DEVICE_PRIVATE_ADDRESS_EXT:
          if (prop_ptr[1])
            {
              *device_address_ext = CL_MEM_DEVICE_PRIVATE_ADDRESS_EXT;
            }
          prop_ptr += 2;
          break;
        case CL_MEM_TENSOR_EXP:
          {
            *tdesc = (const cl_tensor_desc_exp *)prop_ptr[1];
            prop_ptr += 2; /* = CL_MEM_TENSOR and its value. */

            POCL_RETURN_ERROR_ON ((pocl_check_tensor_desc (*tdesc)),
                                  CL_INVALID_PROPERTY,
                                  "invalid tensor description.");
            break;
          }
        default:
          POCL_RETURN_ERROR (CL_INVALID_PROPERTY,
                             "Unknown cl_mem property %zu", *prop_ptr);
        }
    }

  return CL_SUCCESS;
}

CL_API_ENTRY cl_mem CL_API_CALL POname (clCreateBufferWithProperties)(
                               cl_context                context,
                               const cl_mem_properties * properties,
                               cl_mem_flags              flags,
                               size_t                    size,
                               void *                    host_ptr,
                               cl_int *                  errcode_ret)
CL_API_SUFFIX__VERSION_3_0
{
  int errcode;
  const cl_tensor_desc_exp *tdesc = NULL;
  cl_mem_properties bda_props = 0;
  errcode = pocl_parse_cl_mem_properties (properties, &tdesc, &bda_props);
  if (errcode != CL_SUCCESS)
    {
      goto ERROR;
    }

  cl_mem mem = POname (clCreateBuffer) (
    context, flags | (cl_mem_flags)bda_props, size, host_ptr, errcode_ret);
  if (mem == NULL)
    return NULL;

  /* this is checked by CTS tests */
  if (properties && properties[0] == 0)
  {
    mem->num_properties = 1;
    mem->properties[0] = 0;
  }
  if (tdesc)
    {
      mem->num_properties = 1;
      mem->properties[0] = CL_MEM_TENSOR_EXP;
      POCL_GOTO_ERROR_ON ((pocl_copy_tensor_desc2mem (mem, tdesc)),
                          CL_OUT_OF_HOST_MEMORY,
                          "Couldn't allocate space for tensor description.");
    }

  return mem;

ERROR:
  if (errcode_ret)
    {
      *errcode_ret = errcode;
    }

  return NULL;
}
POsym (clCreateBufferWithProperties)
