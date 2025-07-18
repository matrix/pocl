﻿/// level0-driver.cc - driver for LevelZero Compute API devices.
///
/// Copyright (c) 2022-2023 Michal Babej / Intel Finland Oy
///
/// Permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documentation files (the "Software"), to
/// deal in the Software without restriction, including without limitation the
/// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
/// sell copies of the Software, and to permit persons to whom the Software is
/// furnished to do so, subject to the following conditions:
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
/// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
/// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
/// IN THE SOFTWARE.

#include "level0-driver.hh"

#include "common.h"
#include "common_driver.h"
#include "devices.h"
#include "pocl_cache.h"
#include "pocl_cl.h"
#include "pocl_debug.h"
#include "pocl_llvm.h"
#include "pocl_spir.h"
#include "pocl_timing.h"
#include "pocl_util.h"
#include "spirv_queries.h"

#include "imagefill.h"
#include "memfill.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

#if defined(ENABLE_CONFORMANCE) && defined(ENABLE_LEVEL0_EXTRA_FEATURES)
#error Defined both ENABLE_CONFORMANCE and ENABLE_LEVEL0_EXTRA_FEATURES
#endif

// TODO: do we need to use Barriers, if we're using immediate
// cmdlist in synchronous mode
//#define LEVEL0_IMMEDIATE_CMDLIST

// debugging help. If defined, randomize the execution order by skipping 1-3
// of the commands in the work queue.
//#define LEVEL0_RANDOMIZE_QUEUE

// disable to use zeCommandListAppendMemoryFill API command
// known to crash with CTS "select" test.
//#define ENABLE_L0_MEMFILL

#ifndef ENABLE_CONFORMANCE
// fails some corner cases (with CL_RGBA + CL_FLOAT + 3D image, some CTS
// test fails b/c of GPU rounding a pixel channel value 1e-38 to zero)
// host synchronization when ``CL_MEM_USE_HOST_PTR`` is used works with
// buffers, but doesn't work with Images.
#define ENABLE_IMAGES
// subgroups require device queries which aren't yet available in L0
#define ENABLE_SUBGROUPS
// this is emulated on consumer hardware and fails math corner cases
#define ENABLE_FP64
// this is failing some CTS test cases (math/fract)
#define ENABLE_FP16
// fails a single test (progvar_prog_scope_init) in CTS test "basic"
#define ENABLE_PROGVARS
// fails a c11_atomics subtest with GPU hang (even with increased timeout)
#define ENABLE_64BIT_ATOMICS
// enables large (>32bit) allocations. Fails test_allocations from CTS
#define ENABLE_LARGE_ALLOC
#endif

#define ENABLE_WG_COLLECTIVE
#define ENABLE_GENERIC_AS

using namespace pocl;

static void pocl_level0_abort_on_ze_error(int unused, ze_result_t status,
                                          unsigned line, const char *func,
                                          const char *code) {
  const char *str = code;
  if (status != ZE_RESULT_SUCCESS) {
    // TODO convert level0 errors to strings
    POCL_ABORT("Error %0x from LevelZero API:\n%s\n", (unsigned)status, str);
  }
}

// permits pthread_exit(). to be used only from the PoCL L0 driver thread
#define LEVEL0_CHECK_ABORT(code)                                               \
  pocl_level0_abort_on_ze_error(1, code, __LINE__, __FUNCTION__, #code)

// to be used by ZE API calls made from main (user) thread
#define LEVEL0_CHECK_ABORT_NO_EXIT(code)                                       \
  pocl_level0_abort_on_ze_error(0, code, __LINE__, __FUNCTION__, #code)

void Level0Queue::runThread() {

  bool ShouldExit = false;
  _cl_command_node *Command = nullptr;

#ifdef POCL_DEBUG_MESSAGES
  if (pocl_get_bool_option("POCL_DUMP_TASK_GRAPHS", 0) == 1) {
    pocl_dump_dot_task_graph_wait();
  }
#endif

  do {
    BatchType WorkBatch;
    ShouldExit = WorkHandler->getWorkOrWait(&Command, WorkBatch);
    if (Command != nullptr) {
      // for NPU, execute only the NDRangeKernel using L0 CMD Q
      if (Device->prefersZeQueues() ||
          Command->type == CL_COMMAND_NDRANGE_KERNEL) {
        assert(pocl_command_is_ready(Command->sync.event.event));
        assert(Command->sync.event.event->status == CL_SUBMITTED);
        if (Command->type == CL_COMMAND_COMMAND_BUFFER_KHR)
          execCommandBuffer(Command);
        else
          execCommand(Command);
        reset();
      } else if (Device->prefersHostQueues()) {
        pocl_exec_command(Command);
      } else {
        POCL_ABORT_UNIMPLEMENTED("unknown device type\n");
      }
    }
    if (!WorkBatch.empty()) {
      if (WorkBatch.front()->command_type == CL_COMMAND_COMMAND_BUFFER_KHR) {
        assert(WorkBatch.size() == 1);
        cl_event E = WorkBatch.front();
        POCL_LOCK_OBJ(E);
        pocl_update_event_submitted(E);
        POCL_UNLOCK_OBJ(E);
        execCommandBuffer(E->command);
      } else {
        execCommandBatch(WorkBatch);
      }
      reset();
    }
  } while (!ShouldExit);
}

void Level0Queue::appendEventToList(_cl_command_node *Cmd, const char **Msg,
                                    cl_context Context) {
  cl_device_id dev = Cmd->device;
  assert(dev);
  _cl_command_t *cmd = &Cmd->command;
  assert(cmd);

  cl_mem Mem = Cmd->migr_infos != nullptr ? Cmd->migr_infos->buffer : nullptr;

  switch (Cmd->type) {
  case CL_COMMAND_READ_BUFFER:
    read(cmd->read.dst_host_ptr,
         &cmd->read.src->device_ptrs[dev->global_mem_id], cmd->read.src,
         cmd->read.offset, cmd->read.size);
    *Msg = "Event Read Buffer           ";
    break;

  case CL_COMMAND_WRITE_BUFFER:
    write(cmd->write.src_host_ptr,
          &cmd->write.dst->device_ptrs[dev->global_mem_id], cmd->write.dst,
          cmd->write.offset, cmd->write.size);
    syncUseMemHostPtr(&cmd->write.dst->device_ptrs[dev->global_mem_id],
                      cmd->write.dst, cmd->write.offset, cmd->write.size);
    *Msg = "Event Write Buffer          ";
    break;

  case CL_COMMAND_COPY_BUFFER:
    copy(&cmd->copy.dst->device_ptrs[dev->global_mem_id], cmd->copy.dst,
         &cmd->copy.src->device_ptrs[dev->global_mem_id], cmd->copy.src,
         cmd->copy.dst_offset, cmd->copy.src_offset, cmd->copy.size);
    syncUseMemHostPtr(&cmd->copy.dst->device_ptrs[dev->global_mem_id],
                      cmd->copy.dst, cmd->copy.dst_offset, cmd->copy.size);
    *Msg = "Event Copy Buffer           ";
    break;

  case CL_COMMAND_FILL_BUFFER:
    memFill(&cmd->memfill.dst->device_ptrs[dev->global_mem_id],
            cmd->memfill.dst, cmd->memfill.size, cmd->memfill.offset,
            cmd->memfill.pattern, cmd->memfill.pattern_size);
    syncUseMemHostPtr(&cmd->memfill.dst->device_ptrs[dev->global_mem_id],
                      cmd->memfill.dst, cmd->memfill.offset, cmd->memfill.size);
    *Msg = "Event Fill Buffer           ";
    break;

  case CL_COMMAND_READ_BUFFER_RECT:
    readRect(cmd->read_rect.dst_host_ptr,
             &cmd->read_rect.src->device_ptrs[dev->global_mem_id],
             cmd->read_rect.src, cmd->read_rect.buffer_origin,
             cmd->read_rect.host_origin, cmd->read_rect.region,
             cmd->read_rect.buffer_row_pitch, cmd->read_rect.buffer_slice_pitch,
             cmd->read_rect.host_row_pitch, cmd->read_rect.host_slice_pitch);
    *Msg = "Event Read Buffer Rect      ";
    break;

  case CL_COMMAND_COPY_BUFFER_RECT:
    copyRect(&cmd->copy_rect.dst->device_ptrs[dev->global_mem_id],
             cmd->copy_rect.dst,
             &cmd->copy_rect.src->device_ptrs[dev->global_mem_id],
             cmd->copy_rect.src, cmd->copy_rect.dst_origin,
             cmd->copy_rect.src_origin, cmd->copy_rect.region,
             cmd->copy_rect.dst_row_pitch, cmd->copy_rect.dst_slice_pitch,
             cmd->copy_rect.src_row_pitch, cmd->copy_rect.src_slice_pitch);
    syncUseMemHostPtr(&cmd->copy_rect.dst->device_ptrs[dev->global_mem_id],
                      cmd->copy_rect.dst, cmd->copy_rect.dst_origin,
                      cmd->copy_rect.region, cmd->copy_rect.dst_row_pitch,
                      cmd->copy_rect.dst_slice_pitch);
    *Msg = "Event Copy Buffer Rect      ";
    break;

  case CL_COMMAND_WRITE_BUFFER_RECT:
    writeRect(cmd->write_rect.src_host_ptr,
              &cmd->write_rect.dst->device_ptrs[dev->global_mem_id],
              cmd->write_rect.dst, cmd->write_rect.buffer_origin,
              cmd->write_rect.host_origin, cmd->write_rect.region,
              cmd->write_rect.buffer_row_pitch,
              cmd->write_rect.buffer_slice_pitch,
              cmd->write_rect.host_row_pitch, cmd->write_rect.host_slice_pitch);
    syncUseMemHostPtr(&cmd->write_rect.dst->device_ptrs[dev->global_mem_id],
                      cmd->write_rect.dst, cmd->write_rect.buffer_origin,
                      cmd->write_rect.region, cmd->write_rect.buffer_row_pitch,
                      cmd->write_rect.buffer_slice_pitch);
    *Msg = "Event Write Buffer Rect     ";
    break;

  case CL_COMMAND_MIGRATE_MEM_OBJECTS:
    switch (cmd->migrate.type) {
    case ENQUEUE_MIGRATE_TYPE_D2H: {
      if (Mem->is_image != 0u) {
        size_t region[3] = {Mem->image_width, Mem->image_height,
                            Mem->image_depth};
        if (region[2] == 0) {
          region[2] = 1;
        }
        if (region[1] == 0) {
          region[1] = 1;
        }
        size_t origin[3] = {0, 0, 0};
        readImageRect(Mem,
                      &Cmd->migr_infos->buffer->device_ptrs[dev->global_mem_id],
                      Mem->mem_host_ptr, nullptr, origin, region, 0, 0, 0);
      } else {
        read(Mem->mem_host_ptr,
             &Cmd->migr_infos->buffer->device_ptrs[dev->global_mem_id], Mem, 0,
             Mem->size);
      }
      break;
    }
    case ENQUEUE_MIGRATE_TYPE_H2D: {
      assert(Mem);
      if (Mem->is_image != 0u) {
        size_t region[3] = {Mem->image_width, Mem->image_height,
                            Mem->image_depth};
        if (region[2] == 0) {
          region[2] = 1;
        }
        if (region[1] == 0) {
          region[1] = 1;
        }
        size_t origin[3] = {0, 0, 0};
        writeImageRect(Mem, &Mem->device_ptrs[dev->global_mem_id],
                       Mem->mem_host_ptr, nullptr, origin, region, 0, 0, 0);
      } else {
        write(Mem->mem_host_ptr,
              &Cmd->migr_infos->buffer->device_ptrs[dev->global_mem_id], Mem, 0,
              Mem->size);
      }
      break;
    }
    case ENQUEUE_MIGRATE_TYPE_D2D: {
      assert(dev->ops->can_migrate_d2d);
      assert(dev->ops->migrate_d2d);
      assert(Mem);
      dev->ops->migrate_d2d(
          cmd->migrate.src_device, dev, Mem,
          &Mem->device_ptrs[cmd->migrate.src_device->global_mem_id],
          &Mem->device_ptrs[dev->global_mem_id]);
      break;
    }
    case ENQUEUE_MIGRATE_TYPE_NOP: {
      break;
    }
    }
    // TODO sync USE_HOST_PTR
    *Msg = "Event Migrate Buffer(s)     ";
    break;

  case CL_COMMAND_MAP_BUFFER:
    mapMem(&cmd->map.buffer->device_ptrs[dev->global_mem_id], cmd->map.buffer,
           cmd->map.mapping);
    *Msg = "Event Map Buffer            ";
    break;

  case CL_COMMAND_COPY_IMAGE_TO_BUFFER:
    readImageRect(cmd->read_image.src,
                  &cmd->read_image.src->device_ptrs[dev->global_mem_id], NULL,
                  &cmd->read_image.dst->device_ptrs[dev->global_mem_id],
                  cmd->read_image.origin, cmd->read_image.region,
                  cmd->read_image.dst_row_pitch,
                  cmd->read_image.dst_slice_pitch, cmd->read_image.dst_offset);
    *Msg = "Event CopyImageToBuffer       ";
    break;

  case CL_COMMAND_READ_IMAGE:
    readImageRect(cmd->read_image.src,
                  &cmd->read_image.src->device_ptrs[dev->global_mem_id],
                  cmd->read_image.dst_host_ptr, NULL, cmd->read_image.origin,
                  cmd->read_image.region, cmd->read_image.dst_row_pitch,
                  cmd->read_image.dst_slice_pitch, cmd->read_image.dst_offset);
    *Msg = "Event Read Image            ";
    break;

  case CL_COMMAND_COPY_BUFFER_TO_IMAGE:
    writeImageRect(cmd->write_image.dst,
                   &cmd->write_image.dst->device_ptrs[dev->global_mem_id], NULL,
                   &cmd->write_image.src->device_ptrs[dev->global_mem_id],
                   cmd->write_image.origin, cmd->write_image.region,
                   cmd->write_image.src_row_pitch,
                   cmd->write_image.src_slice_pitch,
                   cmd->write_image.src_offset);
    *Msg = "Event CopyBufferToImage       ";
    break;

  case CL_COMMAND_WRITE_IMAGE:
    writeImageRect(cmd->write_image.dst,
                   &cmd->write_image.dst->device_ptrs[dev->global_mem_id],
                   cmd->write_image.src_host_ptr, NULL, cmd->write_image.origin,
                   cmd->write_image.region, cmd->write_image.src_row_pitch,
                   cmd->write_image.src_slice_pitch,
                   cmd->write_image.src_offset);
    *Msg = "Event Write Image           ";
    break;

  case CL_COMMAND_COPY_IMAGE:
    copyImageRect(cmd->copy_image.src, cmd->copy_image.dst,
                  &cmd->copy_image.src->device_ptrs[dev->global_mem_id],
                  &cmd->copy_image.dst->device_ptrs[dev->global_mem_id],
                  cmd->copy_image.src_origin, cmd->copy_image.dst_origin,
                  cmd->copy_image.region);
    *Msg = "Event Copy Image            ";
    break;

  case CL_COMMAND_FILL_IMAGE:
    fillImage(cmd->fill_image.dst,
              &cmd->fill_image.dst->device_ptrs[dev->global_mem_id],
              cmd->fill_image.origin, cmd->fill_image.region,
              cmd->fill_image.orig_pixel, cmd->fill_image.fill_pixel,
              cmd->fill_image.pixel_size);
    *Msg = "Event Fill Image            ";
    break;

  case CL_COMMAND_MAP_IMAGE:
    mapImage(&cmd->map.buffer->device_ptrs[dev->global_mem_id], cmd->map.buffer,
             cmd->map.mapping);
    *Msg = "Event Map Image             ";
    break;

  case CL_COMMAND_UNMAP_MEM_OBJECT:
    if (cmd->unmap.buffer->is_image == CL_FALSE || IS_IMAGE1D_BUFFER(cmd->unmap.buffer)) {
      unmapMem(&cmd->unmap.buffer->device_ptrs[dev->global_mem_id],
               cmd->unmap.buffer, cmd->unmap.mapping);
      if (cmd->unmap.mapping->map_flags & CL_MAP_WRITE) {
        syncUseMemHostPtr(&cmd->unmap.buffer->device_ptrs[dev->global_mem_id],
                          cmd->unmap.buffer, cmd->unmap.mapping->offset,
                          cmd->unmap.mapping->size);
      }
    } else {
      unmapImage(&cmd->unmap.buffer->device_ptrs[dev->global_mem_id],
                 cmd->unmap.buffer, cmd->unmap.mapping);
    }
    *Msg = "Unmap Mem obj         ";
    break;

  case CL_COMMAND_NDRANGE_KERNEL:
    run(Cmd);
    // synchronize content of writable USE_HOST_PTR buffers with the host
    pocl_buffer_migration_info *MI;
    LL_FOREACH (Cmd->migr_infos, MI) {
      cl_mem MigratedBuf = MI->buffer;
      if ((MigratedBuf->flags & CL_MEM_READ_ONLY) != 0u) {
        continue;
      }
      if ((MigratedBuf->flags & CL_MEM_HOST_NO_ACCESS) != 0u) {
        continue;
      }
      pocl_mem_identifier *MemId =
          &MigratedBuf->device_ptrs[dev->global_mem_id];
      syncUseMemHostPtr(MemId, MigratedBuf, 0, MigratedBuf->size);
    }
    *Msg = "Event Enqueue NDRange       ";
    break;

  case CL_COMMAND_BARRIER:
  case CL_COMMAND_MARKER:
    *Msg = "Event Marker                ";
    break;

  // SVM commands
  case CL_COMMAND_SVM_FREE:
    if (cmd->svm_free.pfn_free_func != nullptr) {
      cmd->svm_free.pfn_free_func(
          cmd->svm_free.queue, cmd->svm_free.num_svm_pointers,
          cmd->svm_free.svm_pointers, cmd->svm_free.data);
    } else {
       for (unsigned i = 0; i < cmd->svm_free.num_svm_pointers; i++) {
	 void *Ptr = cmd->svm_free.svm_pointers[i];
	 /* This updates bookkeeping associated with the 'ptr'
	    done by the PoCL core. */
	 POname (clSVMFree) (Context, Ptr);
       }
    }
    *Msg = "Event SVM Free              ";
    break;

  case CL_COMMAND_SVM_MAP:
    svmMap(cmd->svm_map.svm_ptr);
    *Msg = "Event SVM Map              ";
    break;

  case CL_COMMAND_SVM_UNMAP:
    svmUnmap(cmd->svm_unmap.svm_ptr);
    *Msg = "Event SVM Unmap             ";
    break;

  case CL_COMMAND_SVM_MEMCPY:
  case CL_COMMAND_MEMCPY_INTEL:
    svmCopy(cmd->svm_memcpy.dst, cmd->svm_memcpy.src, cmd->svm_memcpy.size);
    *Msg = "Event SVM Memcpy            ";
    break;

  case CL_COMMAND_SVM_MEMFILL:
  case CL_COMMAND_MEMFILL_INTEL:
    svmFill(cmd->svm_fill.svm_ptr, cmd->svm_fill.size, cmd->svm_fill.pattern,
            cmd->svm_fill.pattern_size);
    *Msg = "Event SVM MemFill           ";
    break;

  case CL_COMMAND_SVM_MIGRATE_MEM:
  case CL_COMMAND_MIGRATEMEM_INTEL:
    svmMigrate(cmd->svm_migrate.num_svm_pointers,
               cmd->svm_migrate.svm_pointers,
               cmd->svm_migrate.sizes);
    *Msg = "Event SVM Migrate_Mem       ";
    break;

  case CL_COMMAND_MEMADVISE_INTEL:
    svmAdvise(cmd->mem_advise.ptr, cmd->mem_advise.size,
              cmd->mem_advise.advice);
    *Msg = "Event SVM Mem_Advise        ";
    break;

  default:
    POCL_ABORT_UNIMPLEMENTED("An unknown command type");
    break;
  }
}

void Level0Queue::allocNextFreeEvent() {
  PreviousEventH = CurrentEventH;

  if (AvailableDeviceEvents.empty())
    CurrentEventH = Device->getNewEvent();
  else {
    CurrentEventH = AvailableDeviceEvents.front();
    AvailableDeviceEvents.pop();
  }
  DeviceEventsToReset.push(CurrentEventH);
}

void Level0Queue::reset() {
  assert(CmdListH);
  if (QueueH) {
    LEVEL0_CHECK_ABORT(zeCommandListReset(CmdListH));
  }
  CurrentEventH = nullptr;
  PreviousEventH = nullptr;
  assert(DeviceEventsToReset.empty());
  UseMemHostPtrsToSync.clear();
  MemPtrsToMakeResident.clear();
}

void Level0Queue::closeCmdList(std::queue<ze_event_handle_t> *EvtList) {
  LEVEL0_CHECK_ABORT(zeCommandListAppendBarrier(CmdListH,
                                   nullptr, // signal event
                                   CurrentEventH ? 1 : 0,
                                   CurrentEventH ? &CurrentEventH : nullptr));

  while (!DeviceEventsToReset.empty()) {
    ze_event_handle_t E = DeviceEventsToReset.front();
    DeviceEventsToReset.pop();
    LEVEL0_CHECK_ABORT(zeCommandListAppendEventReset(CmdListH, E));
    if (EvtList) {
      EvtList->push(E);
    } else {
      AvailableDeviceEvents.push(E);
    }
  }

  if (QueueH) {
    LEVEL0_CHECK_ABORT(zeCommandListClose(CmdListH));
  }
}

void Level0Queue::makeMemResident() {
  for (auto &I : MemPtrsToMakeResident) {
    void *Ptr = I.first;
    size_t Size = I.second;
    assert(Ptr);
    ze_result_t Res = zeContextMakeMemoryResident(
        Device->getContextHandle(), Device->getDeviceHandle(), Ptr, Size);
    LEVEL0_CHECK_ABORT(Res);
  }
  MemPtrsToMakeResident.clear();
}

void Level0Queue::syncMemHostPtrs() {
  for (auto &I : UseMemHostPtrsToSync) {
    char *MemHostPtr = I.first.first;
    char *DevPtr = I.first.second;
    size_t Size = I.second;
    assert(MemHostPtr);
    assert(DevPtr);
    allocNextFreeEvent();
    LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopy(
        CmdListH, MemHostPtr, DevPtr, Size, CurrentEventH,
        PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));
  }
  UseMemHostPtrsToSync.clear();
}

void Level0Queue::execCommand(_cl_command_node *Cmd) {

  cl_event event = Cmd->sync.event.event;

  assert(CurrentEventH == nullptr);
  assert(PreviousEventH == nullptr);

  const char *Msg = nullptr;
  appendEventToList(Cmd, &Msg, event->context);

  makeMemResident();
  syncMemHostPtrs();
  closeCmdList();

  if (QueueH) {
    LEVEL0_CHECK_ABORT(
      zeCommandQueueExecuteCommandLists(QueueH, 1, &CmdListH, nullptr));
  }

  pocl_update_event_running(event);

  if (QueueH) {
    LEVEL0_CHECK_ABORT(
      zeCommandQueueSynchronize(QueueH, std::numeric_limits<uint64_t>::max()));
  } else {
    // immediate cmd list
    LEVEL0_CHECK_ABORT(
      zeCommandListHostSynchronize(CmdListH, std::numeric_limits<uint64_t>::max()));
  }

  POCL_UPDATE_EVENT_COMPLETE_MSG(event, Msg);
}

void Level0Queue::execCommandBatch(BatchType &Batch) {

  ze_result_t res;

  assert(CurrentEventH == nullptr);
  assert(PreviousEventH == nullptr);

  POCL_MEASURE_START(ZeListPrepare);

  const char *Msg = nullptr;
  std::deque<const char *> Msgs;
  for (auto E : Batch) {
    _cl_command_node *Cmd = E->command;
    appendEventToList(Cmd, &Msg, E->context);
    Msgs.push_back(Msg);
  }

  makeMemResident();
  syncMemHostPtrs();
  closeCmdList();

  POCL_MEASURE_FINISH(ZeListPrepare);
  POCL_MEASURE_START(ZeListExec);
  if (QueueH) {
    LEVEL0_CHECK_ABORT(
      zeCommandQueueExecuteCommandLists(QueueH, 1, &CmdListH, nullptr));
  }
  for (auto E : Batch) {
    POCL_LOCK_OBJ(E);
    pocl_update_event_submitted(E);
    pocl_update_event_running_unlocked(E);
    POCL_UNLOCK_OBJ(E);
  }

  if (QueueH) {
    LEVEL0_CHECK_ABORT(
      zeCommandQueueSynchronize(QueueH, std::numeric_limits<uint64_t>::max()));
  } else {
    // immediate cmd list
    LEVEL0_CHECK_ABORT(
      zeCommandListHostSynchronize(CmdListH, std::numeric_limits<uint64_t>::max()));
  }

  POCL_MEASURE_FINISH(ZeListExec);

  for (auto E : Batch) {
    assert(!Msgs.empty());
    const char *Msg = Msgs.front();
    POCL_UPDATE_EVENT_COMPLETE_MSG(E, Msg);
    Msgs.pop_front();
  }
}

void Level0Queue::execCommandBuffer(_cl_command_node *Node) {

  ze_result_t res;

  cl_event Event = Node->sync.event.event;
  cl_command_buffer_khr CmdBuf = Event->command_buffer;
  assert(CmdBuf);

  int dev_id = Device->getClDev()->dev_id;
  // if the CmdList for the CmdBuffer hasn't been created yet, do it now
  if (CmdBuf->data[dev_id] == nullptr) {
    CmdBuf->data[dev_id] = createCommandBuffer(CmdBuf);
  }

  assert(CmdBuf->data[dev_id]);
  Level0CmdBufferData *CmdBufData = (Level0CmdBufferData *)CmdBuf->data[dev_id];
  {
    std::lock_guard<std::mutex> Guard(CmdBufData->Lock);
    ze_command_list_handle_t CBCmdListH = CmdBufData->CmdListH;
    POCL_MSG_PRINT_LEVEL0("Executing CmdList %p for CmbBuf %p\n",
                          (void *)CBCmdListH, (void *)CmdBuf);
    // TODO swap
    assert(MemPtrsToMakeResident.empty());
    CmdBufData->MemPtrsToMakeResident.swap(MemPtrsToMakeResident);
    makeMemResident();
    CmdBufData->MemPtrsToMakeResident.swap(MemPtrsToMakeResident);

    POCL_MEASURE_START(ZeListExec);
    // TODO: does not work with immediate CMD queues
    assert(QueueH);
    LEVEL0_CHECK_ABORT(
        zeCommandQueueExecuteCommandLists(QueueH, 1, &CBCmdListH, nullptr));
    pocl_update_event_running(Event);
    LEVEL0_CHECK_ABORT(zeCommandQueueSynchronize(
        QueueH, std::numeric_limits<uint64_t>::max()));
    POCL_MEASURE_FINISH(ZeListExec);
  }

  POCL_UPDATE_EVENT_COMPLETE_MSG(Event, "Event Command Buffer");
}

void *Level0Queue::createCommandBuffer(cl_command_buffer_khr CmdBuf) {

  POCL_MSG_PRINT_LEVEL0("New CmdList for CmdBuf %p\n", (void *)CmdBuf);
  assert(CmdBuf);

  POCL_MEASURE_START(ZeListPrepare);

  ze_command_list_handle_t SaveCmdListH = CmdListH;
  CmdListH = nullptr;
  ze_command_list_desc_t cmdListDesc = {
      ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC, nullptr, QueueOrdinal,
      ZE_COMMAND_LIST_FLAG_MAXIMIZE_THROUGHPUT};
  LEVEL0_CHECK_RET(nullptr, zeCommandListCreate(Device->getContextHandle(),
                                                Device->getDeviceHandle(),
                                                &cmdListDesc, &CmdListH));
  assert(CmdListH);
  assert(CurrentEventH == nullptr);
  assert(PreviousEventH == nullptr);

  const char *Msg = nullptr;
  std::deque<const char *> Msgs;
  _cl_command_node *Cmd;
  cl_context Ctx = CmdBuf->queues[0]->context;

  LL_FOREACH (CmdBuf->cmds, Cmd) {
    Cmd->device = Device->getClDev();
    appendEventToList(Cmd, &Msg, Ctx);
    Cmd->device = nullptr;
  }

  syncMemHostPtrs();
  std::queue<ze_event_handle_t> CmdBufEvtList;
  closeCmdList(&CmdBufEvtList);

  POCL_MEASURE_FINISH(ZeListPrepare);
  std::swap(CmdListH, SaveCmdListH);
  Level0CmdBufferData *CmdBufData = new Level0CmdBufferData;
  assert(CmdBufData);
  CmdBufData->CmdListH = SaveCmdListH;
  CmdBufData->Events.swap(CmdBufEvtList);
  CmdBufData->MemPtrsToMakeResident.swap(MemPtrsToMakeResident);
  CurrentEventH = nullptr;
  PreviousEventH = nullptr;
  assert(CmdListH != nullptr);
  assert(UseMemHostPtrsToSync.empty());
  assert(MemPtrsToMakeResident.empty());
  assert(DeviceEventsToReset.empty());
  return (void *)CmdBufData;
}

void Level0Queue::freeCommandBuffer(void *CmdBufPtr) {
  assert(CmdBufPtr);
  Level0CmdBufferData *CmdBufData = (Level0CmdBufferData *)CmdBufPtr;
  {
    std::lock_guard<std::mutex> Guard(CmdBufData->Lock);
    while (!CmdBufData->Events.empty()) {
      auto E = CmdBufData->Events.front();
      CmdBufData->Events.pop();
      AvailableDeviceEvents.push(E);
    }
    zeCommandListDestroy((ze_command_list_handle_t)CmdBufData->CmdListH);
  }
  delete CmdBufData;
}

void Level0Queue::syncUseMemHostPtr(pocl_mem_identifier *MemId, cl_mem Mem,
                                    size_t Offset, size_t Size) {
  assert(Mem);

  if ((Mem->flags & CL_MEM_USE_HOST_PTR) == 0) {
    return;
  }

  char *DevPtr = static_cast<char *>(MemId->mem_ptr);
  char *MemHostPtr = static_cast<char *>(Mem->mem_host_ptr);

  // host visible mem = skip
  if (MemHostPtr == DevPtr) {
    return;
  }

//  allocNextFreeEvent();
//  LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopy(
//      CmdListH, MemHostPtr + Offset, DevPtr + Offset, Size, CurrentEventH,
//      PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));
  auto Key = std::make_pair(MemHostPtr + Offset, DevPtr + Offset);
  UseMemHostPtrsToSync.emplace(Key, Size);
}

void Level0Queue::syncUseMemHostPtr(pocl_mem_identifier *MemId, cl_mem Mem,
                                    const size_t Origin[3],
                                    const size_t Region[3],
                                    size_t RowPitch,
                                    size_t SlicePitch) {
  assert(Mem);

  if ((Mem->flags & CL_MEM_USE_HOST_PTR) == 0) {
    return;
  }

  char *DevPtr = static_cast<char *>(MemId->mem_ptr);
  char *MemHostPtr = static_cast<char *>(Mem->mem_host_ptr);

  // host visible mem = skip
  if (DevPtr == MemHostPtr) {
    return;
  }

  ze_copy_region_t ZeRegion;
  ZeRegion.originX = Origin[0];
  ZeRegion.originY = Origin[1];
  ZeRegion.originZ = Origin[2];
  ZeRegion.width = Region[0];
  ZeRegion.height = Region[1];
  ZeRegion.depth = Region[2];

  ze_result_t res = zeCommandListAppendMemoryCopyRegion(
      CmdListH,
      MemHostPtr, &ZeRegion, RowPitch, SlicePitch,
      DevPtr, &ZeRegion, RowPitch, SlicePitch,
      nullptr, 0, nullptr);
  LEVEL0_CHECK_ABORT(res);
}

void Level0Queue::read(void *__restrict__ HostPtr,
                       pocl_mem_identifier *SrcMemId, cl_mem SrcBuf,
                       size_t Offset, size_t Size) {
  char *DevPtr = static_cast<char *>(SrcMemId->mem_ptr);
  if ((DevPtr + Offset) == HostPtr) {
    // this can happen when coming from CL_COMMAND_MIGRATE_MEM_OBJECTS
    POCL_MSG_PRINT_LEVEL0("Read skipped, HostPtr == DevPtr\n");
    return;
  }

  POCL_MSG_PRINT_LEVEL0("READ from: %p to: %p offs: %zu size: %zu \n",
                        DevPtr, HostPtr, Offset, Size);
  allocNextFreeEvent();
  LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopy(
      CmdListH, HostPtr, DevPtr + Offset, Size, CurrentEventH,
      PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));
}

void Level0Queue::write(const void *__restrict__ HostPtr,
                        pocl_mem_identifier *DstMemId, cl_mem DstBuf,
                        size_t Offset, size_t Size) {
  char *DevPtr = static_cast<char *>(DstMemId->mem_ptr);
  if ((DevPtr + Offset) == HostPtr) {
    // this can happen when coming from CL_COMMAND_MIGRATE_MEM_OBJECTS
    POCL_MSG_PRINT_LEVEL0("Write skipped, HostPtr == DevPtr\n");
    return;
  }

  POCL_MSG_PRINT_LEVEL0("WRITE from: %p to: %p offs: %zu size: %zu\n",
                        HostPtr, DevPtr, Offset, Size);
  allocNextFreeEvent();
  LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopy(CmdListH, DevPtr + Offset,
         HostPtr, Size, CurrentEventH, PreviousEventH ? 1 : 0,
         PreviousEventH ? &PreviousEventH : nullptr));
}

void Level0Queue::copy(pocl_mem_identifier *DstMemDd, cl_mem DstBuf,
                       pocl_mem_identifier *SrcMemId, cl_mem SrcBuf,
                       size_t DstOffset, size_t SrcOffset, size_t Size) {
  char *SrcPtr = static_cast<char *>(SrcMemId->mem_ptr);
  char *DstPtr = static_cast<char *>(DstMemDd->mem_ptr);
  POCL_MSG_PRINT_LEVEL0("COPY | SRC %p OFF %zu | DST %p OFF %zu | SIZE %zu\n",
                        SrcPtr, SrcOffset, DstPtr, DstOffset, Size);
  allocNextFreeEvent();
  LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopy(
      CmdListH, DstPtr + DstOffset, SrcPtr + SrcOffset, Size, CurrentEventH,
      PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));
}

void Level0Queue::copyRect(pocl_mem_identifier *DstMemId, cl_mem DstBuf,
                           pocl_mem_identifier *SrcMemId, cl_mem SrcBuf,
                           const size_t *__restrict__ const DstOrigin,
                           const size_t *__restrict__ const SrcOrigin,
                           const size_t *__restrict__ const Region,
                           size_t const DstRowPitch, size_t const DstSlicePitch,
                           size_t const SrcRowPitch,
                           size_t const SrcSlicePitch) {
  char *SrcPtr = static_cast<char *>(SrcMemId->mem_ptr);
  char *DstPtr = static_cast<char *>(DstMemId->mem_ptr);

  POCL_MSG_PRINT_LEVEL0(
      "COPY RECT \n"
      "SRC DEV %p | DST DEV %p | SIZE %zu\n"
      "SRC Origin %u %u %u | DST Origin %u %u %u \n"
      "SRC row_pitch %lu | SRC slice_pitch %lu |"
      "DST row_pitch %lu | DST slice_pitch %lu\n"
      "Reg[0,1,2]  %lu  %lu  %lu\n",
      SrcPtr, DstPtr,
      Region[0] * Region[1] * Region[2],
      (unsigned)SrcOrigin[0], (unsigned)SrcOrigin[1], (unsigned)SrcOrigin[2],
      (unsigned)DstOrigin[0], (unsigned)DstOrigin[1], (unsigned)DstOrigin[2],
      (unsigned long)SrcRowPitch, (unsigned long)SrcSlicePitch,
      (unsigned long)DstRowPitch, (unsigned long)DstSlicePitch,
      (unsigned long)Region[0], (unsigned long)Region[1], (unsigned long)Region[2]);

  ze_copy_region_t DstRegion;
  ze_copy_region_t SrcRegion;
  SrcRegion.originX = SrcOrigin[0];
  SrcRegion.originY = SrcOrigin[1];
  SrcRegion.originZ = SrcOrigin[2];
  SrcRegion.width = Region[0];
  SrcRegion.height = Region[1];
  SrcRegion.depth = Region[2];
  DstRegion.originX = DstOrigin[0];
  DstRegion.originY = DstOrigin[1];
  DstRegion.originZ = DstOrigin[2];
  DstRegion.width = Region[0];
  DstRegion.height = Region[1];
  DstRegion.depth = Region[2];

  allocNextFreeEvent();
  ze_result_t res = zeCommandListAppendMemoryCopyRegion(
      CmdListH, DstPtr, &DstRegion, DstRowPitch, DstSlicePitch, SrcPtr,
      &SrcRegion, SrcRowPitch, SrcSlicePitch, CurrentEventH,
      PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr);
  LEVEL0_CHECK_ABORT(res);
}

void Level0Queue::readRectHelper(char *HostPtr,
                                 const char* DevicePtr,
                                 const size_t *BufferOrigin,
                                 const size_t *HostOrigin,
                                 const size_t *Region,
                                 size_t const BufferRowPitch,
                                 size_t const BufferSlicePitch,
                                 size_t const HostRowPitch,
                                 size_t const HostSlicePitch) {
#if 0
  // Disabled. Should work but is buggy in the Level Zero driver
  ze_copy_region_t HostRegion;
  ze_copy_region_t BufferRegion;
  BufferRegion.originX = BufferOrigin[0];
  BufferRegion.originY = BufferOrigin[1];
  BufferRegion.originZ = BufferOrigin[2];
  BufferRegion.width = Region[0];
  BufferRegion.height = Region[1];
  BufferRegion.depth = Region[2];
  HostRegion.originX = HostOrigin[0];
  HostRegion.originY = HostOrigin[1];
  HostRegion.originZ = HostOrigin[2];
  HostRegion.width = Region[0];
  HostRegion.height = Region[1];
  HostRegion.depth = Region[2];

  allocNextFreeEvent();
  ze_result_t res = zeCommandListAppendMemoryCopyRegion(
      CmdListH, HostPtr, &HostRegion, HostRowPitch, HostSlicePitch, BufferPtr,
      &BufferRegion, BufferRowPitch, BufferSlicePitch, CurrentEventH,
      PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr);
  LEVEL0_CHECK_ABORT(res);
#endif
  const char *AdjustedDevicePtr =
      DevicePtr + BufferOrigin[2] * BufferSlicePitch +
      BufferOrigin[1] * BufferRowPitch + BufferOrigin[0];
  char *AdjustedHostPtr = HostPtr + HostOrigin[2] * HostSlicePitch +
                          HostOrigin[1] * HostRowPitch + HostOrigin[0];

  POCL_MSG_PRINT_LEVEL0("READ RECT \n"
                        "SRC DEV %p | DST HOST %p | SIZE %zu\n"
                        "B Origin %u %u %u | H Origin %u %u %u \n"
                        "buf_row_pitch %lu | buf_slice_pitch %lu |"
                        "host_row_pitch %lu | host_slice_pitch %lu\n"
                        "reg[0] %lu reg[1] %lu reg[2] %lu\n",
                        DevicePtr, HostPtr, Region[0] * Region[1] * Region[2],
                        (unsigned)BufferOrigin[0], (unsigned)BufferOrigin[1],
                        (unsigned)BufferOrigin[2], (unsigned)HostOrigin[0],
                        (unsigned)HostOrigin[1], (unsigned)HostOrigin[2],
                        (unsigned long)BufferRowPitch,
                        (unsigned long)BufferSlicePitch,
                        (unsigned long)HostRowPitch,
                        (unsigned long)HostSlicePitch, (unsigned long)Region[0],
                        (unsigned long)Region[1], (unsigned long)Region[2]);

  if ((BufferRowPitch == HostRowPitch && HostRowPitch == Region[0]) &&
      (BufferSlicePitch == HostSlicePitch &&
       HostSlicePitch == (Region[1] * Region[0]))) {
    allocNextFreeEvent();
    LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopy(
        CmdListH, AdjustedHostPtr, AdjustedDevicePtr,
        (Region[2] * Region[1] * Region[0]), CurrentEventH,
        PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));
  } else {
    for (size_t k = 0; k < Region[2]; ++k)
      for (size_t j = 0; j < Region[1]; ++j) {
        allocNextFreeEvent();
        char *Dst = AdjustedHostPtr + HostRowPitch * j + HostSlicePitch * k;
        const char *Src =
            AdjustedDevicePtr + BufferRowPitch * j + BufferSlicePitch * k;
        LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopy(
            CmdListH, Dst, Src, Region[0], CurrentEventH,
            PreviousEventH ? 1 : 0,
            PreviousEventH ? &PreviousEventH : nullptr));
      }
  }
}

void Level0Queue::readRect(void *__restrict__ HostPtr,
                           pocl_mem_identifier *SrcMemId, cl_mem SrcBuf,
                           const size_t *__restrict__ const BufferOrigin,
                           const size_t *__restrict__ const HostOrigin,
                           const size_t *__restrict__ const Region,
                           size_t const BufferRowPitch,
                           size_t const BufferSlicePitch,
                           size_t const HostRowPitch,
                           size_t const HostSlicePitch) {
  const char *BufferPtr = static_cast<const char *>(SrcMemId->mem_ptr);
  readRectHelper((char *)HostPtr, BufferPtr, BufferOrigin, HostOrigin, Region,
                 BufferRowPitch, BufferSlicePitch, HostRowPitch,
                 HostSlicePitch);
}

void Level0Queue::writeRectHelper(
    const char *HostPtr, char *DevicePtr, const size_t *BufferOrigin,
    const size_t *HostOrigin, const size_t *Region, size_t const BufferRowPitch,
    size_t const BufferSlicePitch, size_t const HostRowPitch,
    size_t const HostSlicePitch) {
#if 0
  // Disabled. Should work but is buggy in the Level Zero driver
  ze_copy_region_t HostRegion;
  ze_copy_region_t BufferRegion;
  BufferRegion.originX = BufferOrigin[0];
  BufferRegion.originY = BufferOrigin[1];
  BufferRegion.originZ = BufferOrigin[2];
  BufferRegion.width = Region[0];
  BufferRegion.height = Region[1];
  BufferRegion.depth = Region[2];
  HostRegion.originX = HostOrigin[0];
  HostRegion.originY = HostOrigin[1];
  HostRegion.originZ = HostOrigin[2];
  HostRegion.width = Region[0];
  HostRegion.height = Region[1];
  HostRegion.depth = Region[2];

  allocNextFreeEvent();
  ze_result_t res = zeCommandListAppendMemoryCopyRegion(
      CmdListH, BufferPtr, &BufferRegion, BufferRowPitch, BufferSlicePitch,
      HostPtr, &HostRegion, HostRowPitch, HostSlicePitch, CurrentEventH,
      PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr);
  LEVEL0_CHECK_ABORT(res);
#endif

  char *AdjustedDevicePtr = (char *)DevicePtr +
                            BufferOrigin[2] * BufferSlicePitch +
                            BufferOrigin[1] * BufferRowPitch + BufferOrigin[0];
  const char *AdjustedHostPtr = (const char *)HostPtr +
                                HostOrigin[2] * HostSlicePitch +
                                HostOrigin[1] * HostRowPitch + HostOrigin[0];

  POCL_MSG_PRINT_LEVEL0("WRITE RECT \n"
                        "SRC HOST %p | DST DEV %p | SIZE %zu\n"
                        "B Origin %u %u %u | H Origin %u %u %u \n"
                        "buf_row_pitch %lu | buf_slice_pitch %lu |"
                        "host_row_pitch %lu | host_slice_pitch %lu\n"
                        "reg[0] %lu reg[1] %lu reg[2] %lu\n",
                        HostPtr, DevicePtr, Region[0] * Region[1] * Region[2],
                        (unsigned)BufferOrigin[0], (unsigned)BufferOrigin[1],
                        (unsigned)BufferOrigin[2], (unsigned)HostOrigin[0],
                        (unsigned)HostOrigin[1], (unsigned)HostOrigin[2],
                        (unsigned long)BufferRowPitch,
                        (unsigned long)BufferSlicePitch,
                        (unsigned long)HostRowPitch,
                        (unsigned long)HostSlicePitch, (unsigned long)Region[0],
                        (unsigned long)Region[1], (unsigned long)Region[2]);

  if ((BufferRowPitch == HostRowPitch && HostRowPitch == Region[0]) &&
      (BufferSlicePitch == HostSlicePitch &&
       HostSlicePitch == (Region[1] * Region[0]))) {
    allocNextFreeEvent();
    LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopy(
        CmdListH, AdjustedDevicePtr, AdjustedHostPtr,
        (Region[2] * Region[1] * Region[0]), CurrentEventH,
        PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));
  } else {
    for (size_t k = 0; k < Region[2]; ++k)
      for (size_t j = 0; j < Region[1]; ++j) {
        allocNextFreeEvent();
        const char *Src =
            AdjustedHostPtr + HostRowPitch * j + HostSlicePitch * k;
        char *Dst =
            AdjustedDevicePtr + BufferRowPitch * j + BufferSlicePitch * k;
        LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopy(
            CmdListH, Dst, Src, Region[0], CurrentEventH,
            PreviousEventH ? 1 : 0,
            PreviousEventH ? &PreviousEventH : nullptr));
      }
  }
}

void Level0Queue::writeRect(const void *__restrict__ HostPtr,
                            pocl_mem_identifier *DstMemId, cl_mem DstBuf,
                            const size_t *__restrict__ const BufferOrigin,
                            const size_t *__restrict__ const HostOrigin,
                            const size_t *__restrict__ const Region,
                            size_t const BufferRowPitch,
                            size_t const BufferSlicePitch,
                            size_t const HostRowPitch,
                            size_t const HostSlicePitch) {
  char *BufferPtr = static_cast<char *>(DstMemId->mem_ptr);

  writeRectHelper((const char *)HostPtr, BufferPtr, BufferOrigin, HostOrigin,
                  Region, BufferRowPitch, BufferSlicePitch, HostRowPitch,
                  HostSlicePitch);
}

void Level0Queue::memfillImpl(Level0Device *Device,
                              ze_command_list_handle_t CmdListH,
                              const void *MemPtr, size_t Size, size_t Offset,
                              const void *__restrict__ Pattern,
                              size_t PatternSize) {

  ze_kernel_handle_t KernelH = nullptr;
  ze_module_handle_t ModuleH = nullptr;
  Level0Kernel *Ker = nullptr;
  bool Res = Device->getMemfillKernel(PatternSize, &Ker, ModuleH, KernelH);
  assert(Res == true);
  assert(KernelH);
  assert(ModuleH);

  // TODO this might be not enough: we might need to hold the lock until after
  // zeQueueSubmit
  std::lock_guard<std::mutex> KernelLockGuard(Ker->getMutex());

  // set kernel arg 0 = mem pointer
  ze_result_t ZeRes =
      zeKernelSetArgumentValue(KernelH, 0, sizeof(void *), &MemPtr);
  LEVEL0_CHECK_ABORT(ZeRes);

  // set kernel arg 1 = pattern (POD type)
  ZeRes = zeKernelSetArgumentValue(KernelH, 1, PatternSize, Pattern);
  LEVEL0_CHECK_ABORT(ZeRes);

  uint32_t TotalWGsX = Size / PatternSize;
  uint32_t OffsetX = Offset / PatternSize;
  uint32_t WGSizeX = 1;

  // TODO fix to have higher utilization
  uint32_t MaxWG = Device->getMaxWGSize() / 2;
  while ((TotalWGsX > 1) && ((TotalWGsX & 1) == 0) && (WGSizeX <= MaxWG)) {
    TotalWGsX /= 2;
    WGSizeX *= 2;
  }

  if (Device->supportsGlobalOffsets()) {
    LEVEL0_CHECK_ABORT(zeKernelSetGlobalOffsetExp(KernelH, OffsetX, 0, 0));
  } else {
    POCL_MSG_ERR("memfill: offset specified but device doesn't "
                 "support Global offsets\n");
  }

  ZeRes = zeKernelSetGroupSize(KernelH, WGSizeX, 1, 1);
  LEVEL0_CHECK_ABORT(ZeRes);
  ze_group_count_t LaunchFuncArgs = {TotalWGsX, 1, 1};
  allocNextFreeEvent();
  ZeRes = zeCommandListAppendLaunchKernel(
      CmdListH, KernelH, &LaunchFuncArgs, CurrentEventH, PreviousEventH ? 1 : 0,
      PreviousEventH ? &PreviousEventH : nullptr);

  LEVEL0_CHECK_ABORT(ZeRes);
}

void Level0Queue::memFill(pocl_mem_identifier *DstMemId, cl_mem DstBuf,
                          size_t Size, size_t Offset,
                          const void *__restrict__ Pattern,
                          size_t PatternSize) {
  char *DstPtr = static_cast<char *>(DstMemId->mem_ptr);
  POCL_MSG_PRINT_LEVEL0(
      "MEMFILL | PTR %p | OFS %zu | SIZE %zu | PAT SIZE %zu\n",
      DstPtr, Offset, Size, PatternSize);
#ifdef ENABLE_L0_MEMFILL
  if (PatternSize <= MaxFillPatternSize) {
    allocNextFreeEvent();
    LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryFill(
        CmdListH, DstPtr + Offset, Pattern, PatternSize, Size, CurrentEventH,
        PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));
  } else
#endif
  {
    POCL_MSG_PRINT_LEVEL0("using PoCL's memoryFill kernels\n");
    memfillImpl(Device, CmdListH, DstPtr, Size, Offset, Pattern, PatternSize);
  }
}


void Level0Queue::mapMem(pocl_mem_identifier *SrcMemId,
                         cl_mem SrcBuf, mem_mapping_t *Map) {
  char *SrcPtr = static_cast<char *>(SrcMemId->mem_ptr);

  POCL_MSG_PRINT_LEVEL0("MAP MEM: %p FLAGS %zu\n", SrcPtr, Map->map_flags);

  if ((Map->map_flags & CL_MAP_WRITE_INVALIDATE_REGION) != 0u) {
    return;
  }

  assert(SrcBuf);
  // host visible mem == skip
  if (SrcBuf->mem_host_ptr == SrcMemId->mem_ptr) {
    assert(Map->host_ptr == (SrcPtr + Map->offset));
    return;
  }

  allocNextFreeEvent();
  ze_result_t res = zeCommandListAppendMemoryCopy(
      CmdListH, Map->host_ptr, SrcPtr + Map->offset, Map->size, CurrentEventH,
      PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr);
  LEVEL0_CHECK_ABORT(res);
}

void Level0Queue::unmapMem(pocl_mem_identifier *DstMemId, cl_mem DstBuf,
                           mem_mapping_t *Map) {
  char *DstPtr = static_cast<char *>(DstMemId->mem_ptr);

  POCL_MSG_PRINT_LEVEL0("UNMAP MEM: %p FLAGS %zu\n", DstPtr, Map->map_flags);

  // for read mappings, don't copy anything
  if (Map->map_flags == CL_MAP_READ) {
    return;
  }

  assert(DstBuf);
  // host visible mem == skip
  if (DstBuf->mem_host_ptr == DstMemId->mem_ptr) {
    assert(Map->host_ptr == (DstPtr + Map->offset));
    return;
  }

  allocNextFreeEvent();
  ze_result_t res = zeCommandListAppendMemoryCopy(
      CmdListH, DstPtr + Map->offset, Map->host_ptr, Map->size, CurrentEventH,
      PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr);
  LEVEL0_CHECK_ABORT(res);
}

void Level0Queue::copyImageRect(cl_mem SrcImage, cl_mem DstImage,
                                pocl_mem_identifier *SrcMemId,
                                pocl_mem_identifier *DstMemId,
                                const size_t *SrcOrigin,
                                const size_t *DstOrigin, const size_t *Region) {

  ze_image_handle_t SrcImg =
      static_cast<ze_image_handle_t>(SrcMemId->extra_ptr);
  ze_image_handle_t DstImg =
      static_cast<ze_image_handle_t>(DstMemId->extra_ptr);
  POCL_MSG_PRINT_LEVEL0("COPY IMAGE RECT | SRC %p | DST %p \n", (void *)SrcImg,
                        (void *)DstImg);

  ze_image_region_t DstRegion;
  ze_image_region_t SrcRegion;
  SrcRegion.originX = SrcOrigin[0];
  SrcRegion.originY = SrcOrigin[1];
  SrcRegion.originZ = SrcOrigin[2];
  SrcRegion.width = Region[0];
  SrcRegion.height = Region[1];
  SrcRegion.depth = Region[2];
  DstRegion.originX = DstOrigin[0];
  DstRegion.originY = DstOrigin[1];
  DstRegion.originZ = DstOrigin[2];
  DstRegion.width = Region[0];
  DstRegion.height = Region[1];
  DstRegion.depth = Region[2];

  allocNextFreeEvent();
  ze_result_t Res = zeCommandListAppendImageCopyRegion(
      CmdListH, DstImg, SrcImg, &DstRegion, &SrcRegion, CurrentEventH,
      PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr);

  LEVEL0_CHECK_ABORT(Res);
}

static bool needsStagingCopy(cl_mem DstImage, size_t &UserRowPitch,
                             size_t &UserSlicePitch, size_t &RowPitch,
                             size_t &SlicePitch) {
  // row/slice pitch with zero padding
  RowPitch = DstImage->image_elem_size * DstImage->image_channels *
             DstImage->image_width;
  SlicePitch = RowPitch * (DstImage->image_height ? DstImage->image_height : 1);

  // if the row/slice pitch are nonzero and not equal to zero-padding values,
  // we need a staging buffer memcopy
  if (UserRowPitch) {
    if (UserRowPitch != RowPitch)
      return true;
  } else {
    UserRowPitch = RowPitch;
  }
  if (UserSlicePitch) {
    if (UserSlicePitch != SlicePitch)
      return true;
  } else {
    UserSlicePitch = SlicePitch;
  }
  return false;
}

void Level0Queue::writeImageRect(cl_mem DstImage, pocl_mem_identifier *DstMemId,
                                 const void *__restrict__ SrcHostPtr,
                                 pocl_mem_identifier *SrcMemId,
                                 const size_t *Origin, const size_t *Region,
                                 size_t SrcRowPitch, size_t SrcSlicePitch,
                                 size_t SrcOffset) {
  const char *SrcPtr = nullptr;
  if (SrcHostPtr != nullptr) {
    SrcPtr = static_cast<const char *>(SrcHostPtr) + SrcOffset;
  } else {
    assert(SrcMemId);
    SrcPtr = static_cast<const char *>(SrcMemId->mem_ptr) + SrcOffset;
  }
  // we're either copying a cl_mem to image, or raw memory to image
  assert(SrcMemId != DstMemId);

  ze_image_handle_t DstImg =
      static_cast<ze_image_handle_t>(DstMemId->extra_ptr);
  char *StagingPtr = (char *)DstMemId->mem_ptr;
  size_t NativeRowPitch, NativeSlicePitch;
  bool NeedsStaging = needsStagingCopy(DstImage, SrcRowPitch, SrcSlicePitch,
                                       NativeRowPitch, NativeSlicePitch);
  POCL_MSG_PRINT_LEVEL0(
      "WRITE IMAGE RECT | DST IMG %p | DST IMG STA %p | SRC PTR %p | "
      " Origin %zu %zu %zu | Region %zu %zu %zu |"
      " SrcRowPitch %zu | SrcSlicePitch %zu | "
      " NativeRowPitch %zu | NativeSlicePitch %zu |"
      " SrcOffset %zu | NeedsStaging: %s \n",
      (void *)DstImg, (void *)StagingPtr, (void *)SrcPtr, Origin[0], Origin[1],
      Origin[2], Region[0], Region[1], Region[2], SrcRowPitch, SrcSlicePitch,
      NativeRowPitch, NativeSlicePitch, SrcOffset,
      (NeedsStaging ? "true" : "false"));

  ze_image_region_t ImgRegion;
  ImgRegion.originX = Origin[0];
  ImgRegion.originY = Origin[1];
  ImgRegion.originZ = Origin[2];
  ImgRegion.width = Region[0];
  ImgRegion.height = Region[1];
  ImgRegion.depth = Region[2];

  size_t ElemBytes = DstImage->image_elem_size * DstImage->image_channels;
  // unfortunately, this returns ZE_RESULT_ERROR_UNSUPPORTED_FEATURE
  //  ze_result_t Res = zeCommandListAppendImageCopyFromMemoryExt(CmdListH,
  //  DstImg, SrcPtr, &DstRegion,
  //                                            SrcRowPitch, SrcSlicePitch,
  //                                            nullptr, 0, nullptr);
  if (NeedsStaging) {
    // if copying from other cl_mem, use the faster & simpler way
    if (SrcHostPtr == nullptr) {
      ze_copy_region_t CopyDstRegion;
      CopyDstRegion.originX = Origin[0] * ElemBytes;
      CopyDstRegion.originY = Origin[1];
      CopyDstRegion.originZ = Origin[2];
      CopyDstRegion.width = Region[0] * ElemBytes;
      CopyDstRegion.height = Region[1];
      CopyDstRegion.depth = Region[2];

      ze_copy_region_t CopySrcRegion;
      CopySrcRegion.originX = 0;
      CopySrcRegion.originY = 0;
      CopySrcRegion.originZ = 0;
      CopySrcRegion.width = Region[0] * ElemBytes;
      CopySrcRegion.height = Region[1];
      CopySrcRegion.depth = Region[2];

      allocNextFreeEvent();
      LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopyRegion(
          CmdListH, StagingPtr, &CopyDstRegion, NativeRowPitch,
          NativeSlicePitch,                                   // DST
          SrcPtr, &CopySrcRegion, SrcRowPitch, SrcSlicePitch, // SRC
          CurrentEventH, PreviousEventH ? 1 : 0,
          PreviousEventH ? &PreviousEventH : nullptr));
    } else {
      // if copying from host memory, use the helper to avoid L0 bug
      size_t HostOrigin[3] = {0, 0, 0};
      size_t DevOrigin[3] = {Origin[0] * ElemBytes, Origin[1], Origin[2]};
      size_t DevRegion[3] = {Region[0] * ElemBytes, Region[1], Region[2]};
      writeRectHelper(SrcPtr, StagingPtr, DevOrigin, HostOrigin, DevRegion,
                      NativeRowPitch, NativeSlicePitch, SrcRowPitch,
                      SrcSlicePitch);
    }

    allocNextFreeEvent();
    LEVEL0_CHECK_ABORT(zeCommandListAppendImageCopyFromMemory(
        CmdListH, DstImg, StagingPtr, &ImgRegion, CurrentEventH,
        PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));

  } else {
    allocNextFreeEvent();
    LEVEL0_CHECK_ABORT(zeCommandListAppendImageCopyFromMemory(
        CmdListH, DstImg, SrcPtr, &ImgRegion, CurrentEventH,
        PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));
  }
}

void Level0Queue::readImageRect(cl_mem SrcImage, pocl_mem_identifier *SrcMemId,
                                void *__restrict__ DstHostPtr,
                                pocl_mem_identifier *DstMemId,
                                const size_t *Origin, const size_t *Region,
                                size_t DstRowPitch, size_t DstSlicePitch,
                                size_t DstOffset) {
  char *DstPtr = nullptr;
  if (DstHostPtr != nullptr) {
    DstPtr = static_cast<char *>(DstHostPtr) + DstOffset;
  } else {
    assert(DstMemId);
    DstPtr = static_cast<char *>(DstMemId->mem_ptr) + DstOffset;
  }
  // we're either copying image to a cl_mem, or image to raw memory
  assert(SrcMemId != DstMemId);

  ze_image_handle_t SrcImg =
      static_cast<ze_image_handle_t>(SrcMemId->extra_ptr);
  char *StagingPtr = (char *)SrcMemId->mem_ptr;
  size_t NativeRowPitch, NativeSlicePitch;
  bool NeedsStaging = needsStagingCopy(SrcImage, DstRowPitch, DstSlicePitch,
                                       NativeRowPitch, NativeSlicePitch);
  POCL_MSG_PRINT_LEVEL0(
      "READ IMAGE RECT | SRC IMG %p | SRC IMG STA %p | DST PTR %p | "
      "DstRowPitch %zu | DstSlicePitch %zu | "
      "NativeRowPitch %zu | NativeSlicePitch %zu | "
      "DstOffset %zu \n | NeedsStaging: %s \n",
      (void *)SrcImg, (void *)StagingPtr, (void *)DstPtr, DstRowPitch,
      DstSlicePitch, NativeRowPitch, NativeSlicePitch, DstOffset,
      (NeedsStaging ? "true" : "false"));

  ze_image_region_t ImgRegion;
  ImgRegion.originX = Origin[0];
  ImgRegion.originY = Origin[1];
  ImgRegion.originZ = Origin[2];
  ImgRegion.width = Region[0];
  ImgRegion.height = Region[1];
  ImgRegion.depth = Region[2];

  size_t ElemBytes = SrcImage->image_elem_size * SrcImage->image_channels;
  // unfortunately, this returns ZE_RESULT_ERROR_UNSUPPORTED_FEATURE
  //  ze_result_t Res = zeCommandListAppendImageCopyToMemoryExt(CmdListH,
  //  DstPtr, SrcImg, &SrcRegion,
  //                                          DstRowPitch, DstSlicePitch,
  //                                          nullptr, 0, nullptr);
  if (NeedsStaging) {
    allocNextFreeEvent();
    LEVEL0_CHECK_ABORT(zeCommandListAppendImageCopyToMemory(
        CmdListH, StagingPtr, SrcImg, &ImgRegion, CurrentEventH,
        PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));

    // if copying to other cl_mem, use the faster & simpler way
    if (DstHostPtr == nullptr) {
      ze_copy_region_t CopySrcRegion;
      CopySrcRegion.originX = Origin[0] * ElemBytes;
      CopySrcRegion.originY = Origin[1];
      CopySrcRegion.originZ = Origin[2];
      CopySrcRegion.width = Region[0] * ElemBytes;
      CopySrcRegion.height = Region[1];
      CopySrcRegion.depth = Region[2];

      ze_copy_region_t CopyDstRegion;
      CopyDstRegion.originX = 0;
      CopyDstRegion.originY = 0;
      CopyDstRegion.originZ = 0;
      CopyDstRegion.width = Region[0] * ElemBytes;
      CopyDstRegion.height = Region[1];
      CopyDstRegion.depth = Region[2];

      allocNextFreeEvent();
      LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopyRegion(
          CmdListH, DstPtr, &CopyDstRegion, DstRowPitch, DstSlicePitch, // DST
          StagingPtr, &CopySrcRegion, NativeRowPitch, NativeSlicePitch, // SRC
          CurrentEventH, PreviousEventH ? 1 : 0,
          PreviousEventH ? &PreviousEventH : nullptr));

    } else {
      // if copying to host memory, use the helper to avoid L0 bug
      size_t HostOrigin[3] = {0, 0, 0};
      size_t DevOrigin[3] = {Origin[0] * ElemBytes, Origin[1], Origin[2]};
      size_t DevRegion[3] = {Region[0] * ElemBytes, Region[1], Region[2]};

      readRectHelper(DstPtr, StagingPtr, DevOrigin, HostOrigin, DevRegion,
                     NativeRowPitch, NativeSlicePitch, DstRowPitch,
                     DstSlicePitch);
    }

  } else {
    allocNextFreeEvent();
    LEVEL0_CHECK_ABORT(zeCommandListAppendImageCopyToMemory(
        CmdListH, DstPtr, SrcImg, &ImgRegion, CurrentEventH,
        PreviousEventH ? 1 : 0, PreviousEventH ? &PreviousEventH : nullptr));
  }
}

void Level0Queue::mapImage(pocl_mem_identifier *MemId,
                           cl_mem SrcImage, mem_mapping_t *Map) {

  if ((Map->map_flags & CL_MAP_WRITE_INVALIDATE_REGION) != 0u) {
    return;
  }

  // mapping is always to mem_host_ptr
  char *DstHostPtr = static_cast<char *>(SrcImage->mem_host_ptr);

  POCL_MSG_PRINT_LEVEL0("MAP IMAGE: %p FLAGS %zu\n", DstHostPtr,
                        Map->map_flags);

  readImageRect(SrcImage, MemId, DstHostPtr, nullptr, Map->origin, Map->region,
                Map->row_pitch, Map->slice_pitch, Map->offset);
}

void Level0Queue::unmapImage(pocl_mem_identifier *MemId,
                             cl_mem DstImage, mem_mapping_t *Map) {

  // for read mappings, don't copy anything
  if (Map->map_flags == CL_MAP_READ) {
    return;
  }

  // mapping is always to mem_host_ptr
  char *SrcHostPtr = static_cast<char *>(DstImage->mem_host_ptr);

  POCL_MSG_PRINT_LEVEL0("UNMAP IMAGE: %p FLAGS %zu\n", SrcHostPtr,
                        Map->map_flags);

  writeImageRect(DstImage, MemId, SrcHostPtr, nullptr, Map->origin, Map->region,
                 Map->row_pitch, Map->slice_pitch, Map->offset);
}

void Level0Queue::fillImage(cl_mem Image, pocl_mem_identifier *MemId,
                            const size_t *Origin, const size_t *Region,
                            cl_uint4 OrigPixel, pixel_t FillPixel,
                            size_t PixelSize) {
  char *MapPtr = static_cast<char *>(MemId->mem_ptr);
  ze_image_handle_t ImageH = (ze_image_handle_t)(MemId->extra_ptr);
  assert(Image);

  POCL_MSG_PRINT_LEVEL0(
      "IMAGEFILL | MEM_PTR %p | IMAGE %p | PIXEL %0x %0x %0x %0x"
      " | P SIZE %zu b | ORIGIN %zu %zu %zu | REGION %zu %zu %zu \n",
      MapPtr, ImageH, OrigPixel.s[0], OrigPixel.s[1], OrigPixel.s[2],
      OrigPixel.s[3], PixelSize, Origin[0], Origin[1], Origin[2], Region[0],
      Region[1], Region[2]);

  ze_kernel_handle_t KernelH = nullptr;
  ze_module_handle_t ModuleH = nullptr;
  Level0Kernel *Ker = nullptr;
  bool Res = Device->getImagefillKernel(Image->image_channel_data_type,
                                        Image->image_channel_order,
                                        Image->type, &Ker, ModuleH, KernelH);
  assert(Res == true);
  assert(KernelH);
  assert(ModuleH);

  // TODO this might be not enough: we might need to hold the lock until after
  // zeQueueSubmit
  std::lock_guard<std::mutex> KernelLockGuard(Ker->getMutex());

  // set kernel arg 0 = image pointer
  ze_result_t ZeRes =
      zeKernelSetArgumentValue(KernelH, 0, sizeof(ze_image_handle_t), &ImageH);
  LEVEL0_CHECK_ABORT(ZeRes);

  // set kernel arg 1 = Pixel pattern (POD type)
  ZeRes = zeKernelSetArgumentValue(KernelH, 1, sizeof(cl_uint4), &OrigPixel);
  LEVEL0_CHECK_ABORT(ZeRes);

  if (Device->supportsGlobalOffsets()) {
    LEVEL0_CHECK_ABORT(zeKernelSetGlobalOffsetExp(KernelH, (uint32_t)Origin[0],
                                                  (uint32_t)Origin[1],
                                                  (uint32_t)Origin[2]));
  } else {
    POCL_MSG_ERR("imagefill: origin specified but device doesn't "
                 "support Global offsets\n");
  }

  // TODO could be better
  LEVEL0_CHECK_ABORT(zeKernelSetGroupSize(KernelH, 1, 1, 1));
  ze_group_count_t LaunchFuncArgs = {(uint32_t)Region[0], (uint32_t)Region[1],
                                     (uint32_t)Region[2]};
  allocNextFreeEvent();
  LEVEL0_CHECK_ABORT(zeCommandListAppendLaunchKernel(
      CmdListH, KernelH, &LaunchFuncArgs, CurrentEventH, PreviousEventH ? 1 : 0,
      PreviousEventH ? &PreviousEventH : nullptr));
}

void Level0Queue::svmMap(void *Ptr) {}

void Level0Queue::svmUnmap(void *Ptr) {}

void Level0Queue::svmCopy(void *DstPtr, const void *SrcPtr, size_t Size) {
  POCL_MSG_PRINT_LEVEL0("SVM COPY | SRC %p | DST %p | SIZE %zu\n", SrcPtr,
                        DstPtr, Size);

  allocNextFreeEvent();
  LEVEL0_CHECK_ABORT(zeCommandListAppendMemoryCopy(
      CmdListH, DstPtr, SrcPtr, Size, CurrentEventH, PreviousEventH ? 1 : 0,
      PreviousEventH ? &PreviousEventH : nullptr));
}

void Level0Queue::svmFill(void *DstPtr, size_t Size, void *Pattern,
                          size_t PatternSize) {
  POCL_MSG_PRINT_LEVEL0("SVM FILL | PTR %p | SIZE %zu | PAT SIZE %zu\n", DstPtr,
                        Size, PatternSize);

  memfillImpl(Device, CmdListH, DstPtr, Size, 0, Pattern, PatternSize);

#if 0
  // this *might* be useful some way (perhaps faster), but:
  // 1) some devices (Arc A750) have insufficient limit on pattern size (16)
  // 2) it seems to have a bug that causes a failure with pattern size 2
  //    ... on test Unit_hipMemset_SetMemoryWithOffset

  ze_result_t Res = zeCommandListAppendMemoryFill(
      CmdListH, DstPtr, Pattern, PatternSize, Size, nullptr, 0, nullptr);
  LEVEL0_CHECK_ABORT(Res);
#endif
}

// The function clEnqueueMigrateMemINTEL explicitly migrates a region of
// a shared Unified Shared Memory allocation to the device associated
// with command_queue. This is a hint that may improve performance and
// is not required for correctness
void Level0Queue::svmMigrate(unsigned num_svm_pointers, void **svm_pointers,
                             size_t *sizes) {
  for (unsigned i = 0; i < num_svm_pointers; ++i) {
    ze_result_t Res =
        zeCommandListAppendMemoryPrefetch(CmdListH, svm_pointers[i], sizes[i]);
    LEVEL0_CHECK_ABORT(Res);
  }
}

void Level0Queue::svmAdvise(const void *ptr, size_t size,
                            cl_mem_advice_intel advice) {
  // TODO convert cl_advice to ZeAdvice. The current API doesn't
  // seem to specify any valid values
  if (advice == 0)
    return;
  else
    POCL_MSG_ERR("svmAdvise: unknown advice value %zu\n", (size_t)advice);
  ze_memory_advice_t ZeAdvice = ZE_MEMORY_ADVICE_BIAS_UNCACHED;
  ze_result_t Res = zeCommandListAppendMemAdvise(
      CmdListH, Device->getDeviceHandle(), ptr, size, ZeAdvice);
  LEVEL0_CHECK_ABORT(Res);
}

bool Level0Queue::setupKernelArgs(ze_module_handle_t ModuleH,
                                  ze_kernel_handle_t KernelH, cl_device_id Dev,
                                  unsigned DeviceI, _cl_command_run *RunCmd) {
  cl_kernel Kernel = RunCmd->kernel;
  struct pocl_argument *PoclArg = RunCmd->arguments;

  // this may be set to non-zero by the LLVM parsing of IR in setup_metadata,
  // however: locals are taken care of in L0 runtime
  //assert(Kernel->meta->num_locals == 0);

  cl_uint i = 0;
  ze_result_t Res = ZE_RESULT_SUCCESS;
  for (i = 0; i < Kernel->meta->num_args; ++i) {
    if (ARG_IS_LOCAL(Kernel->meta->arg_info[i])) {
      assert(PoclArg[i].size > 0);
      Res = zeKernelSetArgumentValue(KernelH, i, PoclArg[i].size, NULL);
      LEVEL0_CHECK_ABORT(Res);

    } else if (Kernel->meta->arg_info[i].type == POCL_ARG_TYPE_POINTER) {
      assert(PoclArg[i].size == sizeof(void *));

      if (PoclArg[i].value == NULL) {
        Res = zeKernelSetArgumentValue(KernelH, i, sizeof(void *), nullptr);
      } else if (PoclArg[i].is_raw_ptr != 0) {
        void *MemPtr = *(void**)PoclArg[i].value;
        if (MemPtr == nullptr)
          Res = zeKernelSetArgumentValue(KernelH, i, sizeof(void *), nullptr);
        else
          Res = zeKernelSetArgumentValue(KernelH, i, sizeof(void *), &MemPtr);
      } else {
        cl_mem arg_buf = (*(cl_mem *)(PoclArg[i].value));
        pocl_mem_identifier *memid = &arg_buf->device_ptrs[Dev->global_mem_id];
        void *MemPtr = memid->mem_ptr;
        Res = zeKernelSetArgumentValue(KernelH, i, sizeof(void *), &MemPtr);
        LEVEL0_CHECK_ABORT(Res);
        // optimization for read-only buffers
        ze_memory_advice_t Adv =
            (PoclArg[i].is_readonly ? ZE_MEMORY_ADVICE_SET_READ_MOSTLY
                                    : ZE_MEMORY_ADVICE_CLEAR_READ_MOSTLY);
        Res = zeCommandListAppendMemAdvise(CmdListH, Device->getDeviceHandle(),
                                           MemPtr, arg_buf->size, Adv);
      }
      LEVEL0_CHECK_ABORT(Res);

    } else if (Kernel->meta->arg_info[i].type == POCL_ARG_TYPE_IMAGE) {
      assert(PoclArg[i].value != NULL);
      assert(PoclArg[i].size == sizeof(void *));

      cl_mem arg_buf = (*(cl_mem *)(PoclArg[i].value));
      pocl_mem_identifier *memid = &arg_buf->device_ptrs[Dev->global_mem_id];
      void *hImage = memid->extra_ptr;
      Res = zeKernelSetArgumentValue(KernelH, i, sizeof(void *), &hImage);
      LEVEL0_CHECK_ABORT(Res);
    } else if (Kernel->meta->arg_info[i].type == POCL_ARG_TYPE_SAMPLER) {
      assert(PoclArg[i].value != NULL);
      assert(PoclArg[i].size == sizeof(void *));

      cl_sampler sam = (*(cl_sampler *)(PoclArg[i].value));
      ze_sampler_handle_t hSampler =
          (ze_sampler_handle_t)sam->device_data[Dev->dev_id];

      Res = zeKernelSetArgumentValue(KernelH, i, sizeof(void *), &hSampler);
      LEVEL0_CHECK_ABORT(Res);
    } else {
      assert(PoclArg[i].value != NULL);
      assert(PoclArg[i].size > 0);
      if (Kernel->meta->arg_info[i].type_size) {
        assert(PoclArg[i].size <= Kernel->meta->arg_info[i].type_size);
      }

      Res = zeKernelSetArgumentValue(KernelH, i, PoclArg[i].size,
                                     PoclArg[i].value);
      LEVEL0_CHECK_ABORT(Res);
    }
  }
  return false;
}

void Level0Queue::run(_cl_command_node *Cmd) {
  cl_event Event = Cmd->sync.event.event;
  _cl_command_run *RunCmd = &Cmd->command.run;
  cl_device_id Dev = Cmd->device;
  assert(Cmd->type == CL_COMMAND_NDRANGE_KERNEL);
  cl_kernel Kernel = Cmd->command.run.kernel;
  cl_program Program = Kernel->program;
  unsigned DeviceI = Cmd->program_device_i;
  if (Program->num_builtin_kernels > 0)
    runBuiltinKernel(RunCmd, Dev, Event, Program, Kernel, DeviceI);
  else
    runNDRangeKernel(RunCmd, Dev, Event, Program, Kernel, DeviceI,
                     Cmd->migr_infos);
}

void Level0Queue::runBuiltinKernel(_cl_command_run *RunCmd, cl_device_id Dev,
                                   cl_event Event, cl_program Program,
                                   cl_kernel Kernel, unsigned DeviceI) {
#ifdef ENABLE_NPU

  assert(Program->data[DeviceI] != nullptr);
  Level0BuiltinProgram *L0Program =
      (Level0BuiltinProgram *)Program->data[DeviceI];
  assert(Kernel->data[DeviceI] != nullptr);
  Level0BuiltinKernel *L0Kernel = (Level0BuiltinKernel *)Kernel->data[DeviceI];
  ze_graph_handle_t GraphH = nullptr;
  bool Res = Device->getBestBuiltinKernel(L0Program, L0Kernel, GraphH);
  assert(Res == true);
  assert(GraphH);

  // TODO this lock should be moved not re-locked
  // necessary to lock the kernel, since we're setting up kernel arguments
  // setting WG sizes and so on; this lock is released after
  // zeCommandListAppendKernel
  // TODO this might be not enough: we might need to hold the lock until after
  // zeQueueSubmit
  std::lock_guard<std::mutex> KernelLockGuard(L0Kernel->getMutex());

  graph_dditable_ext_t *Ext = Device->getDriver()->getGraphExt();
  assert(Ext);
  ze_result_t ZeRes = ZE_RESULT_SUCCESS;

  struct pocl_argument *PoclArg = RunCmd->arguments;

  assert(Kernel->meta->num_locals == 0);
  unsigned i = 0;
  unsigned graphArgIndex = 0;
  for (i = 0; i < Kernel->meta->num_args; ++i) {
    if (ARG_IS_LOCAL(Kernel->meta->arg_info[i]) ||
        Kernel->meta->arg_info[i].type != POCL_ARG_TYPE_POINTER) {
      POCL_MSG_ERR("NPU driver only supports pointer args");
      LEVEL0_CHECK_ABORT(ZE_RESULT_ERROR_INVALID_ARGUMENT);
    }
    // pointer
    assert(PoclArg[i].size == sizeof(void *));
    if (PoclArg[i].value == NULL) {
      POCL_MSG_ERR("NPU driver only supports non-NULL pointer args");
      LEVEL0_CHECK_ABORT(ZE_RESULT_ERROR_INVALID_ARGUMENT);
    }
    // non-null ptr
    void *MemPtr = nullptr;
    assert(PoclArg[i].is_raw_ptr == 0);

    cl_mem arg_buf = (*(cl_mem *)(PoclArg[i].value));
    pocl_mem_identifier *memid = &arg_buf->device_ptrs[Dev->global_mem_id];
    MemPtr = memid->mem_ptr;
    POCL_MSG_PRINT_LEVEL0("NPU: setting argument %u to: %p\n", graphArgIndex,
                          MemPtr);
    LEVEL0_CHECK_ABORT(
        Ext->pfnSetArgumentValue(GraphH, graphArgIndex++, MemPtr));
  }

  POCL_MSG_PRINT_LEVEL0("NPU: append GraphInitialize\n");
  allocNextFreeEvent();
  LEVEL0_CHECK_ABORT(Ext->pfnAppendGraphInitialize(
      CmdListH, GraphH, CurrentEventH, PreviousEventH ? 1 : 0,
      PreviousEventH ? &PreviousEventH : nullptr));

  POCL_MSG_PRINT_LEVEL0("NPU: append GraphExecute\n");
  allocNextFreeEvent();
  LEVEL0_CHECK_ABORT(Ext->pfnAppendGraphExecute(
      CmdListH, GraphH, nullptr, CurrentEventH, PreviousEventH ? 1 : 0,
      PreviousEventH ? &PreviousEventH : nullptr));
#else
  POCL_MSG_ERR("Can't execute builtin kernels without VPU support");
#endif
}

void Level0Queue::runNDRangeKernel(_cl_command_run *RunCmd, cl_device_id Dev,
                                   cl_event Event, cl_program Program,
                                   cl_kernel Kernel, unsigned DeviceI,
                                   pocl_buffer_migration_info *MigInfos) {
  struct pocl_context *PoclCtx = &RunCmd->pc;

  assert(Program->data[DeviceI] != nullptr);
  Level0Program *L0Program = (Level0Program *)Program->data[DeviceI];
  assert(Kernel->data[DeviceI] != nullptr);
  Level0Kernel *L0Kernel = (Level0Kernel *)Kernel->data[DeviceI];

  uint32_t TotalWGsX = PoclCtx->num_groups[0];
  uint32_t TotalWGsY = PoclCtx->num_groups[1];
  uint32_t TotalWGsZ = PoclCtx->num_groups[2];
  // it's valid to enqueue ndrange with zeros
  size_t TotalWGs = TotalWGsX * TotalWGsY * TotalWGsZ;
  if (TotalWGs == 0) {
    return;
  }

  bool Needs64bitPtrs = false;
  pocl_buffer_migration_info *MI = nullptr;
  LL_FOREACH (MigInfos, MI) {
    if (MI->buffer->size > UINT32_MAX) {
      Needs64bitPtrs = true;
      break;
    }
  }

  unsigned TotalLocalWGSize =
      PoclCtx->local_size[0] * PoclCtx->local_size[1] * PoclCtx->local_size[2];
  ze_kernel_handle_t KernelH = nullptr;
  ze_module_handle_t ModuleH = nullptr;
  bool Res = Device->getBestKernel(L0Program, L0Kernel, Needs64bitPtrs,
                                   TotalLocalWGSize, ModuleH, KernelH);
  assert(Res == true);
  assert(KernelH);
  assert(ModuleH);

  // zeKernelSetCacheConfig();

  // TODO this lock should be moved not re-locked
  // necessary to lock the kernel, since we're setting up kernel arguments
  // setting WG sizes and so on; this lock is released after
  // zeCommandListAppendKernel
  // TODO this might be not enough: we might need to hold the lock until after
  // zeQueueSubmit
  std::lock_guard<std::mutex> KernelLockGuard(L0Kernel->getMutex());

  ze_kernel_indirect_access_flags_t Flags = L0Kernel->getIndirectFlags();
  if (Flags != 0) {
    ze_result_t Res = zeKernelSetIndirectAccess(KernelH, Flags);
    LEVEL0_CHECK_ABORT(Res);
  }
  const std::map<void *, size_t> &AccessedPointers =
      L0Kernel->getAccessedPointers();
  for (auto &I : AccessedPointers) {
    void *Ptr = I.first;
    size_t Size = I.second;
    MemPtrsToMakeResident[Ptr] = Size;
    if (Size > UINT32_MAX) {
      Needs64bitPtrs = true;
    }
  }

  if (setupKernelArgs(ModuleH, KernelH, Dev, DeviceI, RunCmd)) {
    POCL_MSG_ERR("Level0: Failed to setup kernel arguments\n");
    return;
  }

  uint32_t WGSizeX = PoclCtx->local_size[0];
  uint32_t WGSizeY = PoclCtx->local_size[1];
  uint32_t WGSizeZ = PoclCtx->local_size[2];
  zeKernelSetGroupSize(KernelH, WGSizeX, WGSizeY, WGSizeZ);

  uint32_t StartOffsetX = PoclCtx->global_offset[0];
  uint32_t StartOffsetY = PoclCtx->global_offset[1];
  uint32_t StartOffsetZ = PoclCtx->global_offset[2];
  bool NonzeroGlobalOffset = (StartOffsetX | StartOffsetY | StartOffsetZ) > 0;

  if (Device->supportsGlobalOffsets()) {
    LEVEL0_CHECK_ABORT(zeKernelSetGlobalOffsetExp(KernelH, StartOffsetX,
                                                  StartOffsetY, StartOffsetZ));
  } else {
    if (NonzeroGlobalOffset)
      POCL_MSG_ERR("command needs global offsets, but device doesn't support "
                   "the zeKernelSetGlobalOffsetExp extension!\n");
  }
  ze_group_count_t LaunchFuncArgs = {TotalWGsX, TotalWGsY, TotalWGsZ};
  allocNextFreeEvent();
  LEVEL0_CHECK_ABORT(zeCommandListAppendLaunchKernel(CmdListH, KernelH,
                     &LaunchFuncArgs, CurrentEventH, PreviousEventH ? 1 : 0,
                     PreviousEventH ? &PreviousEventH : nullptr));
}

Level0Queue::Level0Queue(Level0WorkQueueInterface *WH,
                         ze_command_queue_handle_t Q,
                         ze_command_list_handle_t L, Level0Device *D,
                         size_t MaxPatternSize, unsigned int QO,
                         bool RunThread) {

  WorkHandler = WH;
  QueueH = Q;
  CmdListH = L;
  Device = D;
  QueueOrdinal = QO;
  PreviousEventH = CurrentEventH = nullptr;
  MaxFillPatternSize = MaxPatternSize;

  uint32_t TimeStampBits, KernelTimeStampBits;
  Device->getTimingInfo(TimeStampBits, KernelTimeStampBits, DeviceFrequency,
                        DeviceNsPerCycle);
  DeviceMaxValidTimestamp = (1UL << TimeStampBits) - 1;
  DeviceMaxValidKernelTimestamp = (1UL << KernelTimeStampBits) - 1;
  // since the value will be in NS, and unavoidably there will be some noise,
  // this slightly lowers the wrapping limit.
  uint64_t TimeStampWrapLimit = DeviceMaxValidTimestamp * 15 / 16;
  uint64_t KernelTimeStampWrapLimit = DeviceMaxValidKernelTimestamp * 15 / 16;
  // convert to nanoseconds
  DeviceTimerWrapTimeNs =
      (uint64_t)((double)TimeStampWrapLimit * DeviceNsPerCycle);
  DeviceKernelTimerWrapTimeNs =
      (uint64_t)((double)KernelTimeStampWrapLimit * DeviceNsPerCycle);

  Device->getMaxWGs(&DeviceMaxWGSizes);

  if (RunThread)
    Thread = std::thread(&Level0Queue::runThread, this);
}

Level0Queue::~Level0Queue() {
  if (Thread.joinable()) {
    Thread.join();
  }
  assert(DeviceEventsToReset.empty());
  // events are owned & destroyed by the EventPool
  if (CmdListH != nullptr) {
    zeCommandListDestroy(CmdListH);
  }
  if (QueueH != nullptr) {
    zeCommandQueueDestroy(QueueH);
  }
}

bool Level0QueueGroup::init(unsigned Ordinal, unsigned Count,
                            Level0Device *Device, size_t MaxPatternSize) {

  ThreadExitRequested = false;

  ze_context_handle_t ContextH = Device->getContextHandle();
  ze_device_handle_t DeviceH = Device->getDeviceHandle();

  std::vector<ze_command_queue_handle_t> QHandles;
  std::vector<ze_command_list_handle_t> LHandles;
  assert(Count > 0);
  QHandles.resize(Count);
  LHandles.resize(Count);
  ze_result_t ZeRes = ZE_RESULT_SUCCESS;
  ze_command_queue_handle_t Queue = nullptr;
  ze_command_list_handle_t CmdList = nullptr;

  ze_command_queue_desc_t cmdQueueDesc = {
      ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC,
      nullptr,
      Ordinal,
      0, // index
      0, // flags   // ZE_COMMAND_QUEUE_FLAG_EXPLICIT_ONLY
      ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS,
      ZE_COMMAND_QUEUE_PRIORITY_NORMAL};

  ze_command_list_desc_t cmdListDesc{};

  if (Device->isIntelNPU()) {
    // Works around ZE_RESULT_ERROR_INVALID_ENUMERATION failure for
    // Intel NPU on level-zero 1.20.6 on Meteor Lake by mimicing what
    // OpenVINO/NPU does.
    cmdListDesc = {ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC, nullptr, Ordinal, 0};
  } else {
    cmdListDesc = {ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC, nullptr, Ordinal,
                   ZE_COMMAND_LIST_FLAG_RELAXED_ORDERING |
                       ZE_COMMAND_LIST_FLAG_MAXIMIZE_THROUGHPUT};
  }

#ifdef LEVEL0_IMMEDIATE_CMDLIST
  for (unsigned i = 0; i < Count; ++i) {
    cmdQueueDesc.index = i;
    ZeRes = zeCommandListCreateImmediate(ContextH, DeviceH, &cmdQueueDesc,
                                         &CmdList);
    LEVEL0_CHECK_RET(false, ZeRes);
    QHandles[i] = nullptr;
    LHandles[i] = CmdList;
  }
#else
  for (unsigned i = 0; i < Count; ++i) {
    cmdQueueDesc.index = i;
    ZeRes = zeCommandQueueCreate(ContextH, DeviceH, &cmdQueueDesc, &Queue);
    LEVEL0_CHECK_RET(false, ZeRes);
    ZeRes = zeCommandListCreate(ContextH, DeviceH, &cmdListDesc, &CmdList);
    LEVEL0_CHECK_RET(false, ZeRes);
    QHandles[i] = Queue;
    LHandles[i] = CmdList;
  }
#endif

  for (unsigned i = 0; i < Count; ++i) {
    Queues.emplace_back(new Level0Queue(this, QHandles[i], LHandles[i], Device,
                                        MaxPatternSize, Ordinal));
  }

  // create a special command queue only for converting command buffers to L0
  // cmdlist
  cmdQueueDesc.index = 0;
  ZeRes = zeCommandQueueCreate(ContextH, DeviceH, &cmdQueueDesc, &Queue);
  LEVEL0_CHECK_RET(false, ZeRes);
  cmdListDesc.commandQueueGroupOrdinal = 0;
  ZeRes = zeCommandListCreate(ContextH, DeviceH, &cmdListDesc, &CmdList);
  LEVEL0_CHECK_RET(false, ZeRes);
  CreateQueue.reset(new Level0Queue(this, Queue, CmdList, Device,
                                    MaxPatternSize, Ordinal, false));

  Available = true;
  return true;
}

void Level0QueueGroup::uninit() {
  std::unique_lock<std::mutex> Lock(Mutex);
  ThreadExitRequested = true;
  Cond.notify_all();
  Lock.unlock();
  Queues.clear();
}

Level0QueueGroup::~Level0QueueGroup() {
  if (!ThreadExitRequested)
    uninit();
}

void Level0QueueGroup::pushWork(_cl_command_node *Command) {
  std::lock_guard<std::mutex> Lock(Mutex);
  WorkQueue.push(Command);
  Cond.notify_one();
}

void Level0QueueGroup::pushCommandBatch(BatchType Batch) {
  std::lock_guard<std::mutex> Lock(Mutex);
  BatchWorkQueue.push(std::move(Batch));
  Cond.notify_one();
}

bool Level0QueueGroup::getWorkOrWait(_cl_command_node **Node,
                                     BatchType &Batch) {
  std::unique_lock<std::mutex> Lock(Mutex);
  *Node = nullptr;
  bool ShouldExit;
  do {

    ShouldExit = ThreadExitRequested;
    if (!WorkQueue.empty()) {
#ifdef LEVEL0_RANDOMIZE_QUEUE
      int j = std::rand() % 3 + 1;
      _cl_command_node *Tmp = nullptr;
      // mix up the queue
      for (int i = 0; i < j; ++i) {
        Tmp = WorkQueue.front();
        WorkQueue.pop();
        WorkQueue.push(Tmp);
      }
#endif
      *Node = WorkQueue.front();
      WorkQueue.pop();
      break;
    } else if (!BatchWorkQueue.empty()) {
      Batch = std::move(BatchWorkQueue.front());
      BatchWorkQueue.pop();
      break;
    } else {
      if (!ShouldExit) {
        Cond.wait(Lock);
      }
    }
  } while (!ShouldExit);

  Lock.unlock();
  return ShouldExit;
}

void Level0QueueGroup::freeCmdBuf(void *CmdBufData) {
  CreateQueue->freeCommandBuffer(CmdBufData);
}

void *Level0QueueGroup::createCmdBuf(cl_command_buffer_khr CmdBuf) {
  return CreateQueue->createCommandBuffer(CmdBuf);
}

/// serialize SPIRV of the program since we might need
/// to rebuild it with new Spec Constants
/// also serialize the directory with native binaries
const char *LEVEL0_SERIALIZE_ENTRIES[3] = {"/program.bc", "/program.spv",
                                           "/native"};

static const cl_image_format SupportedImageFormats[] = {
    {CL_R, CL_SIGNED_INT8},        {CL_R, CL_SIGNED_INT16},
    {CL_R, CL_SIGNED_INT32},       {CL_R, CL_SNORM_INT8},
    {CL_R, CL_SNORM_INT16},        {CL_R, CL_UNSIGNED_INT8},
    {CL_R, CL_UNSIGNED_INT16},     {CL_R, CL_UNSIGNED_INT32},
    {CL_R, CL_UNORM_INT8},         {CL_R, CL_UNORM_INT16},
    {CL_R, CL_HALF_FLOAT},         {CL_R, CL_FLOAT},

    {CL_RG, CL_SIGNED_INT8},       {CL_RG, CL_SIGNED_INT16},
    {CL_RG, CL_SIGNED_INT32},      {CL_RG, CL_SNORM_INT8},
    {CL_RG, CL_SNORM_INT16},       {CL_RG, CL_UNSIGNED_INT8},
    {CL_RG, CL_UNSIGNED_INT16},    {CL_RG, CL_UNSIGNED_INT32},
    {CL_RG, CL_UNORM_INT8},        {CL_RG, CL_UNORM_INT16},
    {CL_RG, CL_HALF_FLOAT},        {CL_RG, CL_FLOAT},

    {CL_RGBA, CL_SIGNED_INT8},     {CL_RGBA, CL_SIGNED_INT16},
    {CL_RGBA, CL_SIGNED_INT32},    {CL_RGBA, CL_SNORM_INT8},
    {CL_RGBA, CL_SNORM_INT16},     {CL_RGBA, CL_UNSIGNED_INT8},
    {CL_RGBA, CL_UNSIGNED_INT16},  {CL_RGBA, CL_UNSIGNED_INT32},
    {CL_RGBA, CL_UNORM_INT8},      {CL_RGBA, CL_UNORM_INT16},
    {CL_RGBA, CL_HALF_FLOAT},      {CL_RGBA, CL_FLOAT},

    {CL_BGRA, CL_SIGNED_INT8},     {CL_BGRA, CL_SIGNED_INT16},
    {CL_BGRA, CL_SIGNED_INT32},    {CL_BGRA, CL_SNORM_INT8},
    {CL_BGRA, CL_SNORM_INT16},     {CL_BGRA, CL_UNSIGNED_INT8},
    {CL_BGRA, CL_UNSIGNED_INT16},  {CL_BGRA, CL_UNSIGNED_INT32},
    {CL_BGRA, CL_UNORM_INT8},      {CL_BGRA, CL_UNORM_INT16},
    {CL_BGRA, CL_HALF_FLOAT},      {CL_BGRA, CL_FLOAT},

#ifndef ENABLE_CONFORMANCE
    {CL_RGB, CL_UNORM_INT_101010}, {CL_RGB, CL_UNORM_SHORT_565},
    {CL_RGB, CL_UNORM_SHORT_555},
#endif

};

static constexpr unsigned NumSupportedImageFormats =
    sizeof(SupportedImageFormats) / sizeof(SupportedImageFormats[0]);

static constexpr unsigned MaxPropertyEntries = 32;

static cl_device_unified_shared_memory_capabilities_intel
convertZeAllocCaps(ze_memory_access_cap_flags_t Flags) {
  cl_device_unified_shared_memory_capabilities_intel RetVal = 0;
  if (Flags & ZE_MEMORY_ACCESS_CAP_FLAG_RW)
    RetVal |= CL_UNIFIED_SHARED_MEMORY_ACCESS_INTEL;

  if (Flags & ZE_MEMORY_ACCESS_CAP_FLAG_ATOMIC)
    RetVal |= CL_UNIFIED_SHARED_MEMORY_ATOMIC_ACCESS_INTEL;

  if (Flags & ZE_MEMORY_ACCESS_CAP_FLAG_CONCURRENT)
    RetVal |= CL_UNIFIED_SHARED_MEMORY_CONCURRENT_ACCESS_INTEL;

  if (Flags & ZE_MEMORY_ACCESS_CAP_FLAG_CONCURRENT_ATOMIC)
    RetVal |= CL_UNIFIED_SHARED_MEMORY_CONCURRENT_ATOMIC_ACCESS_INTEL;

  return RetVal;
}

Level0EventPool::Level0EventPool(Level0Device *D, unsigned EvtPoolSize)
    : EvtPoolH(nullptr), Dev(D), LastIdx(0) {
  assert(EvtPoolSize);
  ze_result_t Res = ZE_RESULT_SUCCESS;

  ze_event_pool_flags_t EvtPoolFlags = 0;
  if (D->isIntelNPU()) {
    // Works around ZE_RESULT_ERROR_INVALID_ENUMERATION failure for
    // Intel NPU on level-zero 1.20.6 on Meteor Lake by mimicing what
    // OpenVINO/NPU does.
    EvtPoolFlags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE;
  }

  ze_event_pool_desc_t EvtPoolDesc = {
      ZE_STRUCTURE_TYPE_EVENT_POOL_DESC, nullptr, EvtPoolFlags,
      EvtPoolSize // num events
  };

  ze_device_handle_t DevH = Dev->getDeviceHandle();
  LEVEL0_CHECK_ABORT_NO_EXIT(zeEventPoolCreate(
      Dev->getContextHandle(), &EvtPoolDesc, 1, &DevH, &EvtPoolH));

  ze_event_scope_flags_t EvtWaitFlags =
      ZE_EVENT_SCOPE_FLAG_SUBDEVICE | ZE_EVENT_SCOPE_FLAG_DEVICE;
  if (D->isIntelNPU()) {
    // Works around ZE_RESULT_ERROR_INVALID_ENUMERATION failure for
    // Intel NPU on level-zero 1.20.6 on Meteor Lake by mimicing what
    // OpenVINO/NPU does.
    EvtWaitFlags = 0;
  }

  unsigned Idx = 0;
  AvailableEvents.resize(EvtPoolSize);
  for (Idx = 0; Idx < EvtPoolSize; ++Idx) {

    ze_event_desc_t eventDesc = {
        ZE_STRUCTURE_TYPE_EVENT_DESC,
        nullptr,     // pNext
        Idx,         // index
        0,           // flags on signal
        EvtWaitFlags // flags on wait
    };

    ze_event_handle_t EvH = nullptr;
    LEVEL0_CHECK_ABORT_NO_EXIT(zeEventCreate(EvtPoolH, &eventDesc, &EvH));
    AvailableEvents[Idx] = EvH;
  }
}

Level0EventPool::~Level0EventPool() {
  for (ze_event_handle_t EvH : AvailableEvents) {
    zeEventDestroy(EvH);
  }
  if (EvtPoolH != nullptr) {
    zeEventPoolDestroy(EvtPoolH);
  }
}

ze_event_handle_t Level0EventPool::getEvent() {
  if (LastIdx >= AvailableEvents.size())
    return nullptr;
  return AvailableEvents[LastIdx++];
}

bool Level0Device::setupDeviceProperties(bool HasIPVersionExt) {
  ze_result_t Res = ZE_RESULT_SUCCESS;

  DeviceProperties.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES_1_2;
  DeviceProperties.pNext = nullptr;
  DeviceIPVersion = 0;
  ze_device_ip_version_ext_t DeviceIPVersionExt{};
  if (HasIPVersionExt) {
    DeviceProperties.pNext = &DeviceIPVersionExt;
    DeviceIPVersionExt.stype = ZE_STRUCTURE_TYPE_DEVICE_IP_VERSION_EXT;
    DeviceIPVersionExt.pNext = nullptr;
  }

  Res = zeDeviceGetProperties(DeviceHandle, &DeviceProperties);
  if (Res != ZE_RESULT_SUCCESS) {
    POCL_MSG_ERR("Level Zero: zeDeviceGetProperties() failed\n");
    return false;
  }

  DeviceIPVersion = DeviceIPVersionExt.ipVersion;
  // deviceProperties
  switch (DeviceProperties.type) {
  case ZE_DEVICE_TYPE_CPU:
    ClDev->type = CL_DEVICE_TYPE_CPU;
    break;
  case ZE_DEVICE_TYPE_GPU:
    ClDev->type = CL_DEVICE_TYPE_GPU;
    break;
  case ZE_DEVICE_TYPE_VPU:
    ClDev->type = CL_DEVICE_TYPE_CUSTOM;
    break;
  case ZE_DEVICE_TYPE_FPGA:
  default:
    ClDev->type = CL_DEVICE_TYPE_ACCELERATOR;
    POCL_MSG_ERR("Level Zero: don't know how to handle FPGA devices yet");
    return false;
  }

  // ze_device_property_flags_t
  if (DeviceProperties.flags & ZE_DEVICE_PROPERTY_FLAG_INTEGRATED) {
    Integrated = true;
  }
  if (DeviceProperties.flags & ZE_DEVICE_PROPERTY_FLAG_ECC) {
    ClDev->error_correction_support = CL_TRUE;
  }
  if (DeviceProperties.flags & ZE_DEVICE_PROPERTY_FLAG_ONDEMANDPAGING) {
    OndemandPaging = true;
  }

  // common to all dev types
  ClDev->endian_little = CL_TRUE;
  ClDev->parent_device = NULL;
  ClDev->max_sub_devices = 0;
  ClDev->num_partition_properties = 0;
  ClDev->partition_properties = NULL;
  ClDev->num_partition_types = 0;
  ClDev->partition_type = NULL;
  ClDev->short_name = ClDev->long_name = strdup(DeviceProperties.name);
  memcpy(ClDev->device_uuid, &DeviceProperties.uuid,
         sizeof(DeviceProperties.uuid));
  memcpy(ClDev->driver_uuid, Driver->getUUID(), sizeof(DeviceProperties.uuid));
  ClDev->min_data_type_align_size = MAX_EXTENDED_ALIGNMENT;

  ClDev->mem_base_addr_align = 4096;
  ClDev->host_unified_memory = Integrated ? CL_TRUE : CL_FALSE;
  ClDev->max_clock_frequency = DeviceProperties.coreClockRate;

  // L0 returns 4GB in this property, allocating such buffer works but a kernel
  // working with it then fails (IIRC happens with CTS and constant mem test);
  // therefore limit the max-mem-alloc-size to slighty less.
  ClDev->max_mem_alloc_size = ClDev->max_constant_buffer_size =
    ClDev->global_var_pref_size = DeviceProperties.maxMemAllocSize * 15 / 16;
  Supports64bitBuffers = (DeviceProperties.maxMemAllocSize > UINT32_MAX);

  if (DeviceProperties.type == ZE_DEVICE_TYPE_GPU ||
      DeviceProperties.type == ZE_DEVICE_TYPE_CPU) {
    ClDev->has_own_timer = CL_FALSE;
    ClDev->use_only_clang_opencl_headers = CL_TRUE;

    ClDev->local_as_id = SPIR_ADDRESS_SPACE_LOCAL;
    ClDev->constant_as_id = SPIR_ADDRESS_SPACE_CONSTANT;
    ClDev->global_as_id = SPIR_ADDRESS_SPACE_GLOBAL;

    // TODO the values here are copied from the Intel NEO.
    // we need a way to figure out the suitable values for
    // the real underlying device.
    ClDev->preferred_vector_width_char = 16;
    ClDev->preferred_vector_width_short = 8;
    ClDev->preferred_vector_width_int = 4;
    ClDev->preferred_vector_width_long = 1;
    ClDev->preferred_vector_width_float = 1;
    ClDev->preferred_vector_width_double = 1;
    ClDev->preferred_vector_width_half = 8;
    ClDev->native_vector_width_char = 16;
    ClDev->native_vector_width_short = 8;
    ClDev->native_vector_width_int = 4;
    ClDev->native_vector_width_long = 1;
    ClDev->native_vector_width_float = 1;
    ClDev->native_vector_width_double = 1;
    ClDev->native_vector_width_half = 8;

    ClDev->has_64bit_long = CL_TRUE;

    ClDev->max_constant_args = 8;
    ClDev->global_var_max_size = 64 * 1024;

    ClDev->num_serialize_entries = 2;
    ClDev->serialize_entries = LEVEL0_SERIALIZE_ENTRIES;

#ifdef ENABLE_WG_COLLECTIVE
    ClDev->wg_collective_func_support = CL_TRUE;
#endif

    ClDev->on_host_queue_props
        = CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE;
    ClDev->version_of_latest_passed_cts = "v2025-02-25-01";
  } else {
    // FPGA / VPU custom devices
    ClDev->on_host_queue_props = CL_QUEUE_PROFILING_ENABLE;
  }

  MaxCommandQueuePriority = DeviceProperties.maxCommandQueuePriority;

  ClDev->max_compute_units = DeviceProperties.numSlices *
      DeviceProperties.numSubslicesPerSlice *
      DeviceProperties.numEUsPerSubslice;

  ClDev->preferred_wg_size_multiple = 64; // props.physicalEUSimdWidth;

  /// When stype==::ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES the
  ///< units are in nanoseconds. When
  ///< stype==::ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES_1_2 units are in
  ///< cycles/sec
  TimerFrequency = (double)DeviceProperties.timerResolution;
  TimerNsPerCycle = 1000000000.0 / TimerFrequency;
  ClDev->profiling_timer_resolution = (size_t)TimerNsPerCycle;
  if (ClDev->profiling_timer_resolution == 0)
    ClDev->profiling_timer_resolution = 1;

  TSBits = DeviceProperties.timestampValidBits;
  KernelTSBits = DeviceProperties.kernelTimestampValidBits;

  return true;
}

bool Level0Device::setupComputeProperties() {
  ze_result_t Res = ZE_RESULT_SUCCESS;
  ze_device_compute_properties_t ComputeProperties{};
  ComputeProperties.stype = ZE_STRUCTURE_TYPE_DEVICE_COMPUTE_PROPERTIES;
  ComputeProperties.pNext = nullptr;
  Res = zeDeviceGetComputeProperties(DeviceHandle, &ComputeProperties);
  if (Res != ZE_RESULT_SUCCESS || ComputeProperties.maxTotalGroupSize == 0) {
    POCL_MSG_PRINT_LEVEL0("%s: zeDeviceGetComputeProperties failed\n",
                          ClDev->short_name);
    // some defaults
    ClDev->max_work_group_size =  128;
    ClDev->max_work_item_dimensions = 3;
    ClDev->max_work_item_sizes[0] =
          ClDev->max_work_item_sizes[1] =
          ClDev->max_work_item_sizes[2] = 128;

      ClDev->local_mem_type = CL_GLOBAL;
      ClDev->local_mem_size = 65536;
      ClDev->max_num_sub_groups = 0;
      MaxWGCount[0] = MaxWGCount[1] =  MaxWGCount[2] = 65536;
    return false;
  }

  // computeProperties
  ClDev->max_work_group_size = ComputeProperties.maxTotalGroupSize;
  ClDev->max_work_item_dimensions = 3;
  ClDev->max_work_item_sizes[0] = ComputeProperties.maxGroupSizeX;
  ClDev->max_work_item_sizes[1] = ComputeProperties.maxGroupSizeY;
  ClDev->max_work_item_sizes[2] = ComputeProperties.maxGroupSizeZ;

  // level0 devices typically don't have unlimited number of groups per
  MaxWGCount[0] = ComputeProperties.maxGroupCountX;
  MaxWGCount[1] = ComputeProperties.maxGroupCountY;
  MaxWGCount[2] = ComputeProperties.maxGroupCountZ;

  ClDev->local_mem_type = CL_LOCAL;
  ClDev->local_mem_size = ComputeProperties.maxSharedLocalMemory;

#ifdef ENABLE_SUBGROUPS
  cl_uint Max = 0;
  if (ComputeProperties.numSubGroupSizes > 0) {
    for (unsigned i = 0; i < ComputeProperties.numSubGroupSizes; ++i) {
      if (ComputeProperties.subGroupSizes[i] > Max) {
        Max = ComputeProperties.subGroupSizes[i];
      }
    }
    ClDev->max_num_sub_groups = Max;

    SupportedSubgroupSizes.resize(ComputeProperties.numSubGroupSizes);
    for (unsigned i = 0; i < ComputeProperties.numSubGroupSizes; ++i) {
      SupportedSubgroupSizes[i] = ComputeProperties.subGroupSizes[i];
    }
  }
#else
  ClDev->max_num_sub_groups = 0;
#endif

  POCL_MSG_PRINT_LEVEL0("Device Max WG SIZE %zu ||| WG counts: %u | %u | %u\n",
      ClDev->max_work_group_size, MaxWGCount[0],
      MaxWGCount[1], MaxWGCount[2]);
  return true;
}

static cl_device_fp_config convertZeFPFlags(ze_device_fp_flags_t FPFlags) {
  cl_device_fp_config FPConfig = 0;
  if ((FPFlags & ZE_DEVICE_FP_FLAG_DENORM) != 0u) {
    FPConfig |= CL_FP_DENORM;
  }
  if ((FPFlags & ZE_DEVICE_FP_FLAG_INF_NAN) != 0u) {
    FPConfig |= CL_FP_INF_NAN;
  }
  if ((FPFlags & ZE_DEVICE_FP_FLAG_ROUND_TO_NEAREST) != 0u) {
    FPConfig |= CL_FP_ROUND_TO_NEAREST;
  }
  if ((FPFlags & ZE_DEVICE_FP_FLAG_ROUND_TO_INF) != 0u) {
    FPConfig |= CL_FP_ROUND_TO_INF;
  }
  if ((FPFlags & ZE_DEVICE_FP_FLAG_ROUND_TO_ZERO) != 0u) {
    FPConfig |= CL_FP_ROUND_TO_ZERO;
  }
  if ((FPFlags & ZE_DEVICE_FP_FLAG_FMA) != 0u) {
    FPConfig |= CL_FP_FMA;
  }
  if ((FPFlags & ZE_DEVICE_FP_FLAG_SOFT_FLOAT) != 0u) {
    FPConfig |= CL_FP_SOFT_FLOAT;
  }
  return FPConfig;
}

static cl_device_fp_atomic_capabilities_ext convertZeAtomicFlags(
    ze_device_fp_atomic_ext_flags_t FPFlags, std::string Prefix,
    std::string &OclFeatures) {

  cl_device_fp_atomic_capabilities_ext FPAtomicCaps = 0;
  if (FPFlags& ZE_DEVICE_FP_ATOMIC_EXT_FLAG_GLOBAL_LOAD_STORE) {
    FPAtomicCaps |=CL_DEVICE_GLOBAL_FP_ATOMIC_LOAD_STORE_EXT;
    OclFeatures += " __opencl_c_ext_" + Prefix + "_global_atomic_load_store";
  }
  if (FPFlags& ZE_DEVICE_FP_ATOMIC_EXT_FLAG_GLOBAL_ADD) {
    FPAtomicCaps |=CL_DEVICE_GLOBAL_FP_ATOMIC_ADD_EXT;
    OclFeatures += " __opencl_c_ext_" + Prefix + "_global_atomic_add";
  }
  if (FPFlags& ZE_DEVICE_FP_ATOMIC_EXT_FLAG_GLOBAL_MIN_MAX) {
    FPAtomicCaps |=CL_DEVICE_GLOBAL_FP_ATOMIC_MIN_MAX_EXT;
    OclFeatures += " __opencl_c_ext_" + Prefix + "_global_atomic_min_max";
  }

  if (FPFlags& ZE_DEVICE_FP_ATOMIC_EXT_FLAG_LOCAL_LOAD_STORE) {
    FPAtomicCaps |=CL_DEVICE_LOCAL_FP_ATOMIC_LOAD_STORE_EXT;
    OclFeatures += " __opencl_c_ext_" + Prefix + "_local_atomic_load_store";
  }
  if (FPFlags& ZE_DEVICE_FP_ATOMIC_EXT_FLAG_LOCAL_ADD) {
    FPAtomicCaps |=CL_DEVICE_LOCAL_FP_ATOMIC_ADD_EXT;
    OclFeatures += " __opencl_c_ext_" + Prefix + "_local_atomic_add";
  }
  if (FPFlags& ZE_DEVICE_FP_ATOMIC_EXT_FLAG_LOCAL_MIN_MAX) {
    FPAtomicCaps |=CL_DEVICE_LOCAL_FP_ATOMIC_MIN_MAX_EXT;
    OclFeatures += " __opencl_c_ext_" + Prefix + "_local_atomic_min_max";
  }

  return FPAtomicCaps;
}

bool Level0Device::setupModuleProperties(bool &SupportsInt64Atomics,
                                         bool HasFloatAtomics,
                                         std::string &Features) {

  ze_device_module_properties_t ModuleProperties{};
  ze_float_atomic_ext_properties_t FloatProperties{};
  ModuleProperties.stype = ZE_STRUCTURE_TYPE_DEVICE_MODULE_PROPERTIES;
  ModuleProperties.pNext = HasFloatAtomics ? &FloatProperties : nullptr;
  FloatProperties.stype = ZE_STRUCTURE_TYPE_FLOAT_ATOMIC_EXT_PROPERTIES;
  FloatProperties.pNext = nullptr;
  ze_result_t Res = zeDeviceGetModuleProperties(DeviceHandle, &ModuleProperties);
  if (Res != ZE_RESULT_SUCCESS) {
    POCL_MSG_PRINT_LEVEL0("%s zeDeviceGetModuleProperties() failed\n",
                          ClDev->short_name);
    SupportsInt64Atomics = false;
    ClDev->device_side_printf = 0;
    ClDev->printf_buffer_size = 0;
    ClDev->max_parameter_size = 8; // TODO
    return false;
  }

  ClDev->single_fp_config = convertZeFPFlags(ModuleProperties.fp32flags);
#ifdef ENABLE_FP64
  // TODO we should check & rely on ZE_DEVICE_FP_FLAG_SOFT_FLOAT,
  // but it's not set by the LevelZero driver
  if ((ModuleProperties.flags & ZE_DEVICE_MODULE_FLAG_FP64) != 0u) {
    ClDev->double_fp_config = convertZeFPFlags(ModuleProperties.fp64flags);
  }
#endif
#ifdef ENABLE_FP16
  if ((ModuleProperties.flags & ZE_DEVICE_MODULE_FLAG_FP16) != 0u) {
    ClDev->half_fp_config = convertZeFPFlags(ModuleProperties.fp16flags);
  }
#endif

#ifdef ENABLE_64BIT_ATOMICS
  SupportsInt64Atomics = (ModuleProperties.flags &
                          ZE_DEVICE_MODULE_FLAG_INT64_ATOMICS) != 0u;
#endif
  // clear flags set in setupDeviceProperties
  if (ClDev->double_fp_config == 0) {
    ClDev->preferred_vector_width_double = 0;
    ClDev->native_vector_width_double = 0;
  }
  if (ClDev->half_fp_config == 0) {
    ClDev->preferred_vector_width_half = 0;
    ClDev->native_vector_width_half = 0;
  }

  KernelUUID = ModuleProperties.nativeKernelSupported;
  SupportsDP4A = (ModuleProperties.flags & ZE_DEVICE_MODULE_FLAG_DP4A) > 0;
  // TODO this seems not reported
  // SupportsDPAS = (ModuleProperties.flags & ZE_DEVICE_MODULE_FLAG_DPAS) > 0;
  if (SupportsDP4A || SupportsDPAS) {
    // TODO how to get these properties from L0
    ClDev->dot_product_caps =
        CL_DEVICE_INTEGER_DOT_PRODUCT_INPUT_4x8BIT_KHR |
        CL_DEVICE_INTEGER_DOT_PRODUCT_INPUT_4x8BIT_PACKED_KHR;
    ClDev->dot_product_accel_props_8bit.signed_accelerated = CL_TRUE;
    ClDev->dot_product_accel_props_8bit.unsigned_accelerated = CL_TRUE;
    ClDev->dot_product_accel_props_4x8bit.signed_accelerated = CL_TRUE;
    ClDev->dot_product_accel_props_4x8bit.unsigned_accelerated = CL_TRUE;
  }

  //  POCL_MSG_PRINT_LEVEL0("Using KernelUUID: %s\n", KernelUUID);
  if (HasFloatAtomics) {
    ClDev->single_fp_atomic_caps = convertZeAtomicFlags(FloatProperties.fp32Flags, "fp32", Features);
    if (ClDev->double_fp_config)
      ClDev->double_fp_atomic_caps =
          convertZeAtomicFlags(FloatProperties.fp64Flags, "fp64", Features);
    if (ClDev->half_fp_config)
      ClDev->half_fp_atomic_caps =
          convertZeAtomicFlags(FloatProperties.fp16Flags, "fp16", Features);
  }

  ClDev->device_side_printf = 0;
  ClDev->printf_buffer_size = ModuleProperties.printfBufferSize;
  // leaving the default gives an error with CTS test_api:
  //
  // error: Total size of kernel arguments exceeds limit!
  //        Total arguments size: 2060, limit: 2048
  // in kernel: 'get_kernel_arg_info'
  //
  // this is a bug in the CTS, fixed in our branch,
  // but this workaround is needed for upstream CTS
  ClDev->max_parameter_size = ModuleProperties.maxArgumentsSize;
#ifdef ENABLE_CONFORMANCE
  if (ModuleProperties.maxArgumentsSize > 256)
    ClDev->max_parameter_size = ModuleProperties.maxArgumentsSize - 64;
#endif

  uint32_t SpvVer = ModuleProperties.spirvVersionSupported;
  SupportedSpvVersion =
      pocl_version_t(ZE_MAJOR_VERSION(SpvVer), ZE_MINOR_VERSION(SpvVer));

  if (SpvVer == 0)
    return true;

  ClDev->compiler_available = CL_TRUE;
  ClDev->linker_available = CL_TRUE;
#ifdef ENABLE_GENERIC_AS
  ClDev->generic_as_support = CL_TRUE;
#endif

#ifdef USE_LLVM_SPIRV_TARGET
  LLVMTargetTriple = "spirv64v";
  LLVMTargetTriple.push_back('0' + SupportedSpvVersion.major);
  LLVMTargetTriple.push_back('.');
  LLVMTargetTriple.push_back('0' + SupportedSpvVersion.minor);
  LLVMTargetTriple.append("-unknown-unknown");
#else
  LLVMTargetTriple = "spir64-unknown-unknown";
#endif
  ClDev->llvm_target_triplet = LLVMTargetTriple.c_str();

  for (int minor = SupportedSpvVersion.minor; minor >= 0; --minor) {
      if (!SupportedILVersions.empty())
          SupportedILVersions.push_back(' ');
      SupportedILVersions.append("SPIR-V_1.");
      SupportedILVersions.push_back('0' + minor);
  }
  ClDev->supported_spir_v_versions = SupportedILVersions.c_str();

  return true;
}

bool Level0Device::setupQueueGroupProperties() {
  ze_result_t Res = ZE_RESULT_SUCCESS;
  uint32_t QGroupPropCount = MaxPropertyEntries;
  ze_command_queue_group_properties_t QGroupProps[MaxPropertyEntries];
  for (uint32_t i = 0; i < MaxPropertyEntries; ++i) {
    QGroupProps[i].pNext = nullptr;
    QGroupProps[i].stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_GROUP_PROPERTIES;
  }
  Res = zeDeviceGetCommandQueueGroupProperties(DeviceHandle, &QGroupPropCount,
                                               QGroupProps);
  if (Res != ZE_RESULT_SUCCESS) {
    POCL_MSG_ERR("Level Zero: %s"
                 "zeDeviceGetCommandQueueGroupProperties() failed\n",
                 ClDev->short_name);
    return false;
  }

  // QGroupProps
  uint32_t UniversalQueueOrd = UINT32_MAX;
  uint32_t CopyQueueOrd = UINT32_MAX;
  uint32_t ComputeQueueOrd = UINT32_MAX;
  uint32_t NumUniversalQueues = 0;
  uint32_t NumCopyQueues = 0;
  uint32_t NumComputeQueues = 0;

  for (uint32_t i = 0; i < QGroupPropCount; ++i) {
    bool IsCompute = ((QGroupProps[i].flags &
                       ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE) != 0);
    bool IsCopy = ((QGroupProps[i].flags &
                    ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY) != 0);
    if (IsCompute && IsCopy && UniversalQueueOrd == UINT32_MAX) {
      UniversalQueueOrd = i;
      NumUniversalQueues = QGroupProps[i].numQueues;
    }

    if (IsCompute && !IsCopy && ComputeQueueOrd == UINT32_MAX) {
      ComputeQueueOrd = i;
      NumComputeQueues = QGroupProps[i].numQueues;
    }

    if (!IsCompute && IsCopy && CopyQueueOrd == UINT32_MAX) {
      CopyQueueOrd = i;
      NumCopyQueues = QGroupProps[i].numQueues;
    }
  }

  if (UniversalQueueOrd == UINT32_MAX &&
      (ComputeQueueOrd == UINT32_MAX || CopyQueueOrd == UINT32_MAX)) {
    POCL_MSG_ERR(
          "No universal queue and either of copy/compute queue are missing\n");
    return false;
  }

  // create specialized queues
  if (ComputeQueueOrd != UINT32_MAX) {
    ComputeQueues.init(ComputeQueueOrd, NumComputeQueues, this,
                       QGroupProps[ComputeQueueOrd].maxMemoryFillPatternSize);
  }
  if (CopyQueueOrd != UINT32_MAX) {
    CopyQueues.init(CopyQueueOrd, NumCopyQueues, this,
                    QGroupProps[CopyQueueOrd].maxMemoryFillPatternSize);
  }

  // always create universal queues, if available
  if (UniversalQueueOrd != UINT32_MAX) {
    uint32_t num = std::max(1U, NumUniversalQueues);
    UniversalQueues.init(
        UniversalQueueOrd, num, this,
        QGroupProps[UniversalQueueOrd].maxMemoryFillPatternSize);
  }

  return true;
}

bool Level0Device::setupMemoryProperties(bool &HasUSMCapability) {
  ze_result_t Res1 = ZE_RESULT_SUCCESS;
  ze_result_t Res2 = ZE_RESULT_SUCCESS;

  uint32_t MemPropCount = MaxPropertyEntries;
  ze_device_memory_properties_t MemProps[MaxPropertyEntries];
  for (uint32_t i = 0; i < MaxPropertyEntries; ++i) {
    MemProps[i].pNext = nullptr;
    MemProps[i].stype = ZE_STRUCTURE_TYPE_DEVICE_MEMORY_PROPERTIES;
  }
  Res1 = zeDeviceGetMemoryProperties(DeviceHandle, &MemPropCount, MemProps);

  ze_device_memory_access_properties_t MemAccessProperties{};
  MemAccessProperties.stype = ZE_STRUCTURE_TYPE_DEVICE_MEMORY_ACCESS_PROPERTIES;
  MemAccessProperties.pNext = nullptr;
  Res2 = zeDeviceGetMemoryAccessProperties(DeviceHandle, &MemAccessProperties);

  // ClDev->max_mem_alloc_size was setup in setupDeviceProperties()
  // set a default value to the be maxMemAllocSize
  ClDev->global_mem_size = ClDev->max_mem_alloc_size;
  if (Res1 != ZE_RESULT_SUCCESS || Res2 != ZE_RESULT_SUCCESS) {
    POCL_MSG_PRINT_LEVEL0("%s: zeDeviceGetMemoryProperties() failed\n",
                          ClDev->short_name);
    HasUSMCapability = false;
    return false;
  }

  for (uint32_t i = 0; i < MemPropCount; ++i) {
    if (ClDev->global_mem_size < MemProps[i].totalSize) {
      ClDev->global_mem_size = MemProps[i].totalSize;
      GlobalMemOrd = i;
    }
  }

  if (int MemLimit = pocl_get_int_option("POCL_MEMORY_LIMIT", 0)) {
    uint64_t MemInGBytes =
        std::max<uint64_t>((ClDev->global_mem_size >> 30), 1UL);
    if (MemLimit > 0 && (uint64_t)MemLimit <= MemInGBytes) {
      ClDev->global_mem_size = (size_t)MemLimit << 30;
      // ensure MaxMemAllocSize <= ClDev->global_mem_size
      ClDev->max_mem_alloc_size = ClDev->max_constant_buffer_size =
        ClDev->global_var_pref_size =
          std::min(ClDev->max_mem_alloc_size, ClDev->global_mem_size) * 15 / 16;
    }
  }

  // memAccessProperties
  if (MemAccessProperties.sharedSingleDeviceAllocCapabilities &
      (ZE_MEMORY_ACCESS_CAP_FLAG_RW | ZE_MEMORY_ACCESS_CAP_FLAG_ATOMIC)) {
    ClDev->svm_allocation_priority = 2;
    ClDev->atomic_memory_capabilities =
        CL_DEVICE_ATOMIC_ORDER_RELAXED | CL_DEVICE_ATOMIC_ORDER_ACQ_REL |
        CL_DEVICE_ATOMIC_ORDER_SEQ_CST | CL_DEVICE_ATOMIC_SCOPE_WORK_GROUP |
        CL_DEVICE_ATOMIC_SCOPE_DEVICE;
    ClDev->atomic_fence_capabilities =
        CL_DEVICE_ATOMIC_ORDER_RELAXED | CL_DEVICE_ATOMIC_ORDER_ACQ_REL |
        CL_DEVICE_ATOMIC_ORDER_SEQ_CST | CL_DEVICE_ATOMIC_SCOPE_WORK_ITEM |
        CL_DEVICE_ATOMIC_SCOPE_WORK_GROUP | CL_DEVICE_ATOMIC_SCOPE_DEVICE;
    // OpenCL 2.0 properties
    bool A1 = MemAccessProperties.sharedSingleDeviceAllocCapabilities
        & ZE_MEMORY_ACCESS_CAP_FLAG_ATOMIC;
    bool A2 = MemAccessProperties.hostAllocCapabilities
        & ZE_MEMORY_ACCESS_CAP_FLAG_ATOMIC;
    bool A3 = MemAccessProperties.deviceAllocCapabilities
        & ZE_MEMORY_ACCESS_CAP_FLAG_ATOMIC;
    // the CL_DEVICE_SVM_ATOMICS implies support for fine-grained
    // so it will likely require from the device
    // ZE_MEMORY_ACCESS_CAP_FLAG_CONCURRENT_ATOMIC flag
    if (A1 && A2 && A3)
      ClDev->svm_caps = CL_DEVICE_SVM_COARSE_GRAIN_BUFFER |
                        CL_DEVICE_SVM_FINE_GRAIN_BUFFER;
    else
      ClDev->svm_caps = CL_DEVICE_SVM_COARSE_GRAIN_BUFFER;

  } else {
    POCL_MSG_PRINT_LEVEL0("SVM disabled for device\n");
  }

  ClDev->host_usm_capabs =
      convertZeAllocCaps(MemAccessProperties.hostAllocCapabilities);
  ClDev->device_usm_capabs =
      convertZeAllocCaps(MemAccessProperties.deviceAllocCapabilities);
  ClDev->single_shared_usm_capabs = convertZeAllocCaps(
        MemAccessProperties.sharedSingleDeviceAllocCapabilities);
  ClDev->cross_shared_usm_capabs = convertZeAllocCaps(
        MemAccessProperties.sharedCrossDeviceAllocCapabilities);
  ClDev->system_shared_usm_capabs =
      convertZeAllocCaps(MemAccessProperties.sharedSystemAllocCapabilities);

  POCL_MSG_PRINT_LEVEL0("Device: %u || SingleShared: %u || CrossShared: %u || SystemShared: %u\n",
                        MemAccessProperties.deviceAllocCapabilities,
                        MemAccessProperties.sharedSingleDeviceAllocCapabilities,
                        MemAccessProperties.sharedCrossDeviceAllocCapabilities,
                        MemAccessProperties.sharedSystemAllocCapabilities);

  // the minimum capability required for USM
  HasUSMCapability =
      ClDev->device_usm_capabs & CL_UNIFIED_SHARED_MEMORY_ACCESS_INTEL;

  return true;
}

bool Level0Device::setupCacheProperties() {
  ze_result_t Res = ZE_RESULT_SUCCESS;

  uint32_t CachePropCount = MaxPropertyEntries;
  ze_device_cache_properties_t CacheProperties[MaxPropertyEntries];
  for (uint32_t i = 0; i < MaxPropertyEntries; ++i) {
    CacheProperties[i].pNext = nullptr;
    CacheProperties[i].stype = ZE_STRUCTURE_TYPE_DEVICE_CACHE_PROPERTIES;
  }
  Res = zeDeviceGetCacheProperties(DeviceHandle, &CachePropCount,
                                   CacheProperties);
  if (Res != ZE_RESULT_SUCCESS) {
    POCL_MSG_PRINT_LEVEL0("%s: zeDeviceGetCacheProperties() failed\n",
                          ClDev->short_name);
    ClDev->global_mem_cacheline_size = 0;
    ClDev->global_mem_cache_type = CL_NONE;
    return false;
  }

  // cacheProperties
  for (uint32_t i = 0; i < CachePropCount; ++i) {
    // find largest cache that is not user-controlled
    if ((CacheProperties[i].flags &
         ZE_DEVICE_CACHE_PROPERTY_FLAG_USER_CONTROL) != 0u) {
      continue;
    }
    if (ClDev->global_mem_cache_size < CacheProperties[i].cacheSize) {
      ClDev->global_mem_cache_size = CacheProperties[i].cacheSize;
    }
  }
  ClDev->global_mem_cacheline_size = HOST_CPU_CACHELINE_SIZE;
  ClDev->global_mem_cache_type = CL_READ_WRITE_CACHE;

  return true;
}

bool Level0Device::setupImageProperties() {
  ze_result_t Res = ZE_RESULT_SUCCESS;

  ze_device_image_properties_t ImageProperties{};
  ImageProperties.stype = ZE_STRUCTURE_TYPE_DEVICE_IMAGE_PROPERTIES;
  ImageProperties.pNext = nullptr;
  Res = zeDeviceGetImageProperties(DeviceHandle, &ImageProperties);

  if (Res != ZE_RESULT_SUCCESS) {
    POCL_MSG_PRINT_LEVEL0("%s: zeDeviceGetImageProperties() failed\n",
                          ClDev->short_name);
    ClDev->image_support = CL_FALSE;
    return false;
  }

  // imageProperties
  ClDev->max_read_image_args = ImageProperties.maxReadImageArgs;
  ClDev->max_read_write_image_args = ImageProperties.maxWriteImageArgs;
  ClDev->max_write_image_args = ImageProperties.maxWriteImageArgs;
  ClDev->max_samplers = ImageProperties.maxSamplers;

  ClDev->image_max_array_size = ImageProperties.maxImageArraySlices;
  ClDev->image_max_buffer_size = ImageProperties.maxImageBufferSize;

  ClDev->image2d_max_height = ClDev->image2d_max_width =
      ImageProperties.maxImageDims2D;
  ClDev->image3d_max_depth = ClDev->image3d_max_height =
      ClDev->image3d_max_width = ImageProperties.maxImageDims3D;

  for (unsigned i = 0; i < NUM_OPENCL_IMAGE_TYPES; ++i) {
    ClDev->num_image_formats[i] = NumSupportedImageFormats;
    ClDev->image_formats[i] = SupportedImageFormats;
  }
  ClDev->image_support = CL_TRUE;

  return true;
}

bool Level0Device::setupPCIAddress() {
  ze_pci_ext_properties_t PCIProps;
  PCIProps.stype = ZE_STRUCTURE_TYPE_PCI_EXT_PROPERTIES;
  PCIProps.pNext = nullptr;
  PCIProps.address = {0};
  PCIProps.maxSpeed = {0};

  ze_result_t Res = zeDevicePciGetPropertiesExt(DeviceHandle, &PCIProps);
  if (Res != ZE_RESULT_SUCCESS)
    return false;

  ClDev->pci_bus_info.pci_bus = PCIProps.address.bus;
  ClDev->pci_bus_info.pci_device = PCIProps.address.device;
  ClDev->pci_bus_info.pci_domain = PCIProps.address.domain;
  ClDev->pci_bus_info.pci_function = PCIProps.address.function;
  return true;
}

void Level0Device::setupGlobalMemSize(bool HasRelaxedAllocLimits) {
  if (HasRelaxedAllocLimits && ClDev->global_mem_size > UINT32_MAX) {
    // allow allocating 85% of total memory in a single buffer
    ClDev->max_mem_alloc_size = ClDev->global_mem_size * 85 / 100;
    // TODO: figure out if relaxed limits also apply to these
    // for now, assume it doesn't and leave it at DevProps.maxMemAlloc
//    ClDev->max_constant_buffer_size =
//    ClDev->global_var_pref_size = ClDev->max_mem_alloc_size;
    Supports64bitBuffers = true;
    NeedsRelaxedLimits = true;
  }
#ifndef ENABLE_PROGVARS
  ClDev->global_var_pref_size = 0;
  ClDev->global_var_max_size = 0;
#endif
}

// dev -> Dev
Level0Device::Level0Device(Level0Driver *Drv, ze_device_handle_t DeviceH,
                           cl_device_id dev, const char *Parameters)
    : Driver(Drv), ClDev(dev), DeviceHandle(DeviceH),
      MemfillProgram(nullptr), ImagefillProgram(nullptr) {

  SETUP_DEVICE_CL_VERSION(dev, 3, 0);

  ClDev->execution_capabilities = CL_EXEC_KERNEL;
  ClDev->address_bits = 64;
  ClDev->vendor = "Intel Corporation";
  ClDev->vendor_id = 0x8086;
  ClDev->profile = "FULL_PROFILE";

  ClDev->available = &this->Available;
  ContextHandle = Drv->getContextHandle();
  assert(DeviceHandle);
  assert(ContextHandle);
  HasGOffsets = Drv->hasExtension("ZE_experimental_global_offset");
  HasCompression = Drv->hasExtension("ZE_extension_memory_compression_hints");
  bool HasIPVerExt = Drv->hasExtension("ZE_extension_device_ip_version");

  // both of these are mandatory, the rest are optional
  if (!setupDeviceProperties(HasIPVerExt)) {
    return;
  }
  if (!setupQueueGroupProperties()) {
    return;
  }

  // test support for importing/exporting external memory
  ze_device_external_memory_properties_t ExternalMemProperties{};
  ExternalMemProperties.stype =
      ZE_STRUCTURE_TYPE_DEVICE_EXTERNAL_MEMORY_PROPERTIES;
  ExternalMemProperties.pNext = nullptr;
  ze_result_t Res =
      zeDeviceGetExternalMemoryProperties(DeviceHandle, &ExternalMemProperties);
  if (Res == ZE_RESULT_SUCCESS) {
    HasDMABufImport = ExternalMemProperties.memoryAllocationImportTypes &
                      ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
    HasDMABufExport = ExternalMemProperties.memoryAllocationExportTypes &
                      ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
  }

#if 0
  /// support for subdevices. Currently unimplemented
  deviceProperties.subdeviceId  ZE_DEVICE_PROPERTY_FLAG_SUBDEVICE
  uint32_t subDeviceCount = 0;
  zeDeviceGetSubDevices(device, &subDeviceCount, nullptr);
  ze_device_handle_t subDevices[2] = {};
  zeDeviceGetSubDevices(device, &subDeviceCount, subDevices);
#endif

  setupComputeProperties();

  bool Supports64bitIntAtomics = false;
  std::string FPAtomicFeatures;
  setupModuleProperties(Supports64bitIntAtomics,
                        Drv->hasExtension("ZE_extension_float_atomics"),
                        FPAtomicFeatures);

  bool HasUsmCapability = false;
  setupMemoryProperties(HasUsmCapability);

#ifdef ENABLE_LARGE_ALLOC
  bool HasRelaxedAllocLimits =
      Driver->hasExtension("ZE_experimental_relaxed_allocation_limits");
#else
  bool HasRelaxedAllocLimits = false;
#endif
  setupGlobalMemSize(HasRelaxedAllocLimits);

  setupCacheProperties();
#ifdef ENABLE_IMAGES
  setupImageProperties();
#endif

  Extensions = std::string("cl_khr_byte_addressable_store"
                           " cl_khr_create_command_queue"
                           " cl_khr_global_int32_base_atomics"
                           " cl_khr_global_int32_extended_atomics"
                           " cl_khr_local_int32_base_atomics"
                           " cl_khr_local_int32_extended_atomics"
                           " cl_khr_device_uuid"
                           " cl_khr_il_program"
                           " cl_khr_spirv_queries"
                           " cl_khr_spirv_no_integer_wrap_decoration"
#ifdef ENABLE_LEVEL0_EXTRA_FEATURES
                           " cl_intel_split_work_group_barrier"
#endif
#ifdef ENABLE_ICD
                           " cl_khr_icd"
#endif
  );
  SPVExtensions = std::string("+SPV_KHR_no_integer_wrap_decoration"
                              ",+SPV_KHR_non_semantic_info"
                              ",+SPV_KHR_expect_assume"

                              ",+SPV_INTEL_arbitrary_precision_integers"
                              ",+SPV_INTEL_arithmetic_fence"
                              ",+SPV_INTEL_bfloat16_conversion"
                              ",+SPV_INTEL_cache_controls"
                              ",+SPV_INTEL_fp_fast_math_mode"
                              ",+SPV_INTEL_function_pointers"
                              ",+SPV_INTEL_hw_thread_queries"
                              ",+SPV_INTEL_inline_assembly"
                              ",+SPV_INTEL_kernel_attributes"
#if LLVM_MAJOR < 20
                              ",+SPV_INTEL_long_constant_composite"
#else
                              ",+SPV_INTEL_long_composites"
#endif
                              ",+SPV_INTEL_masked_gather_scatter"
                              ",+SPV_INTEL_optimization_hints"

                              ",+SPV_INTEL_runtime_aligned"


                              ",+SPV_INTEL_split_barrier"
                              ",+SPV_INTEL_tensor_float32_rounding"
                              ",+SPV_INTEL_unstructured_loop_controls"
                              ",+SPV_INTEL_variable_length_array"

                              // ",+SPV_KHR_cooperative_matrix"
                              // ",+SPV_KHR_subgroup_rotate"
                              // ",+SPV_KHR_uniform_group_instructions"
                              // ",+SPV_KHR_bit_instructions"
                              //
                              // "SPV_INTEL_arbitrary_precision_fixed_point"
                              // "SPV_INTEL_arbitrary_precision_floating_point"
                              // "SPV_INTEL_memory_access_aliasing"
                              // "SPV_INTEL_tensor_float32_conversion"
                              // "SPV_EXT_relaxed_printf_string_address_space"
                              // "SPV_INTEL_bindless_images"

                              // SPV_INTEL_float_controls2
                              // SPV_INTEL_vector_compute

                              // TODO:
                              // this breaks scalarwave test:
                              // ",+SPV_INTEL_optnone"
  );

  if (ClDev->generic_as_support)
    OpenCL30Features.append(" __opencl_c_generic_address_space");

  if (ClDev->global_var_pref_size)
    OpenCL30Features.append(" __opencl_c_program_scope_global_variables");

  if (ClDev->wg_collective_func_support)
    OpenCL30Features.append(" __opencl_c_work_group_collective_functions");

  if (ClDev->atomic_memory_capabilities & CL_DEVICE_ATOMIC_ORDER_ACQ_REL)
    OpenCL30Features.append(" __opencl_c_atomic_order_acq_rel");

  if (ClDev->atomic_memory_capabilities & CL_DEVICE_ATOMIC_ORDER_SEQ_CST)
    OpenCL30Features.append(" __opencl_c_atomic_order_seq_cst");

  if (ClDev->atomic_memory_capabilities & CL_DEVICE_ATOMIC_SCOPE_DEVICE)
    OpenCL30Features.append(" __opencl_c_atomic_scope_device");

  if (ClDev->atomic_memory_capabilities & CL_DEVICE_ATOMIC_SCOPE_ALL_DEVICES)
    OpenCL30Features.append(" __opencl_c_atomic_scope_all_devices");

#if !defined(ENABLE_CONFORMANCE) && !defined(LEVEL0_IMMEDIATE_CMDLIST)
  // command buffers only make sense if we're using LevelZero queues for all commands
  if (prefersZeQueues()) {
    Extensions.append(" cl_khr_command_buffer");
    ClDev->cmdbuf_capabilities =
        CL_COMMAND_BUFFER_CAPABILITY_SIMULTANEOUS_USE_KHR |
        CL_COMMAND_BUFFER_CAPABILITY_KERNEL_PRINTF_KHR;
    //| CL_COMMAND_BUFFER_CAPABILITY_MULTIPLE_QUEUE_KHR;
    ClDev->cmdbuf_required_properties = 0;
    ClDev->native_command_buffers = CL_TRUE;
  }
#endif

  if (ClDev->image_support != CL_FALSE) {
    Extensions += " cl_khr_3d_image_writes"
                  " cl_khr_depth_images";
    OpenCL30Features += " __opencl_c_images"
                        " __opencl_c_read_write_images"
                        " __opencl_c_3d_image_writes";
  }

  if (Drv->hasExtension("ZE_extension_linkonce_odr")) {
    Extensions.append(" cl_khr_spirv_linkonce_odr");
    SPVExtensions.append(",+SPV_KHR_linkonce_odr");
  }

  if (Drv->hasExtension("ZE_extension_pci_properties") && setupPCIAddress()) {
    Extensions.append(" cl_khr_pci_bus_info");
  }

  if (HasIPVerExt) {
    Extensions.append(" cl_intel_device_attribute_query");
  }

  if (Supports64bitIntAtomics) {
    Extensions.append(" cl_khr_int64_base_atomics"
                      " cl_khr_int64_extended_atomics");
  }

  if (ClDev->type == CL_DEVICE_TYPE_CUSTOM) {
    Extensions.append(" cl_exp_tensor"
                      " cl_exp_defined_builtin_kernels");
  }

  if (ClDev->half_fp_config != 0u) {
    Extensions.append(" cl_khr_fp16");
    OpenCL30Features.append(" __opencl_c_fp16");
  }

  if (ClDev->double_fp_config != 0u) {
    Extensions.append(" cl_khr_fp64");
    OpenCL30Features.append(" __opencl_c_fp64");
  }

  if (ClDev->max_num_sub_groups > 0) {
    Extensions.append(" cl_khr_subgroups"
                      " cl_intel_spirv_subgroups"
#ifdef ENABLE_LEVEL0_EXTRA_FEATURES
                           " cl_khr_subgroup_shuffle"
                           " cl_khr_subgroup_shuffle_relative"
                           " cl_khr_subgroup_extended_types"
                           " cl_khr_subgroup_non_uniform_arithmetic"
                           " cl_khr_subgroup_non_uniform_vote"
                           " cl_khr_subgroup_ballot"
                           " cl_khr_subgroup_clustered_reduce"

                           " cl_intel_subgroups"
                           " cl_intel_subgroups_char"
                           " cl_intel_subgroups_short"
                           " cl_intel_subgroups_long"
                           " cl_intel_subgroup_local_block_io"
                           " cl_intel_required_subgroup_size"
#endif
                      );
    OpenCL30Features.append(" __opencl_c_subgroups");
    SPVExtensions.append(",+SPV_INTEL_subgroups");
#if LLVM_MAJOR > 18
    SPVExtensions.append(",+SPV_INTEL_subgroup_requirements");
#endif
  }

  if (ClDev->has_64bit_long != 0) {
    OpenCL30Features.append(" __opencl_c_int64");
  }

  if (HasUsmCapability) {
    Extensions.append(" cl_intel_unified_shared_memory");
    SPVExtensions.append(",+SPV_INTEL_usm_storage_classes");
  }

  if (supportsDeviceUSM()) {
    Extensions.append(" cl_ext_buffer_device_address");
  }

  if (Drv->hasExtension("ZE_extension_float_atomics")) {
    Extensions.append(" cl_ext_float_atomics");
    OpenCL30Features.append(FPAtomicFeatures);
    SPVExtensions.append(",+SPV_EXT_shader_atomic_float_add");
    SPVExtensions.append(",+SPV_EXT_shader_atomic_float_min_max");
    if (ClDev->half_fp_atomic_caps)
      SPVExtensions.append(",+SPV_EXT_shader_atomic_float16_add");
  }

#ifdef ENABLE_LEVEL0_EXTRA_FEATURES
  if (SupportsDP4A || SupportsDPAS) {
    Extensions.append(" cl_khr_integer_dot_product");
    OpenCL30Features.append(" __opencl_c_integer_dot_product_input_4x8bit");
    OpenCL30Features.append(
        " __opencl_c_integer_dot_product_input_4x8bit_packed");
    SPVExtensions.append(",+SPV_KHR_integer_dot_product");
    if (SupportsDPAS)
      SPVExtensions.append(",+SPV_INTEL_joint_matrix");
  }
#endif

  if (ClDev->type == CL_DEVICE_TYPE_CPU
      || ClDev->type == CL_DEVICE_TYPE_GPU) {
    ClDev->extensions = Extensions.c_str();
    ClDev->features = OpenCL30Features.c_str();
    ClDev->supported_spirv_extensions = SPVExtensions.c_str();

    pocl_setup_opencl_c_with_version(ClDev, CL_TRUE);
    pocl_setup_features_with_version(ClDev);
    pocl_setup_extensions_with_version(ClDev);
    pocl_setup_ils_with_version(ClDev);
    pocl_setup_spirv_queries(ClDev);
  }

  if (ClDev->type == CL_DEVICE_TYPE_CUSTOM ||
      ClDev->type == CL_DEVICE_TYPE_ACCELERATOR) {
    ClDev->extensions = Extensions.c_str();
    ClDev->features = "";
    pocl_setup_extensions_with_version(ClDev);

#ifdef ENABLE_NPU
    pocl::getNpuGraphModelsList(BuiltinKernels, NumBuiltinKernels);
    POCL_MSG_PRINT_LEVEL0("NPU BiK list:\n %s\n", BuiltinKernels.c_str());
    ClDev->builtin_kernel_list = BuiltinKernels.data();
    ClDev->num_builtin_kernels = NumBuiltinKernels;

    pocl_setup_builtin_kernels_with_version(ClDev);
#endif
  }

  // calculate KernelCacheHash
  //
  // Note!!! there is no need to add Spec Constants or Compiler options
  // into KernelCacheHash, because pocl_cache_create_program_cachedir
  // has already taken care of those
  SHA1_CTX HashCtx;
  uint8_t Digest[SHA1_DIGEST_SIZE];
  pocl_SHA1_Init(&HashCtx);

  // not reliable
  // const ze_driver_uuid_t DrvUUID = Driver->getUUID();
  // pocl_SHA1_Update(&HashCtx, (const uint8_t*)&DrvUUID.id,
  // sizeof(DrvUUID.id));
  uint32_t DrvVersion = Driver->getVersion();
  pocl_SHA1_Update(&HashCtx, (const uint8_t *)&DrvVersion,
                   sizeof(DrvVersion));

  pocl_SHA1_Update(&HashCtx, (const uint8_t *)&ClDev->type,
                   sizeof(ClDev->type));

  pocl_SHA1_Update(&HashCtx, (const uint8_t *)&ClDev->vendor_id,
                   sizeof(ClDev->vendor_id));
  // not reliable
  // pocl_SHA1_Update(&HashCtx,
  //                 (const uint8_t*)&deviceProperties.uuid,
  //                 sizeof(deviceProperties.uuid));
  pocl_SHA1_Update(&HashCtx, (const uint8_t *)ClDev->short_name,
                   strlen(ClDev->short_name));
  pocl_SHA1_Final(&HashCtx, Digest);

  std::stringstream SStream;
  for (unsigned i = 0; i < sizeof(Digest); ++i) {
    SStream << std::setfill('0') << std::setw(2) << std::hex
            << (unsigned)Digest[i];
  }
  SStream.flush();
  KernelCacheHash = SStream.str();

  if (ClDev->compiler_available != CL_FALSE) {
    initHelperKernels();
  }

  for (unsigned i = 0; i < 4; ++i)
    EventPools.emplace_back(this, EventPoolSize);

  Alloc.reset(new Level0DefaultAllocator{Driver, this});

  POCL_MSG_PRINT_LEVEL0("Device %s initialized & available\n", ClDev->short_name);
  this->Available = CL_TRUE;
}

Level0CompilationJobScheduler &Level0Device::getJobSched() {
  return Driver->getJobSched();
}

cl_device_id Level0Device::getClDev() {
  return Driver->getClDevForHandle(DeviceHandle);
}

Level0Device::~Level0Device() {
  UniversalQueues.uninit();
  ComputeQueues.uninit();
  CopyQueues.uninit();
  destroyHelperKernels();
  EventPools.clear();
  Alloc->clear(this);
}

static void calculateHash(uint8_t *BuildHash,
                          const uint8_t *Data,
                          const size_t Len) {
  SHA1_CTX HashCtx;
  uint8_t TempDigest[SHA1_DIGEST_SIZE];
  pocl_SHA1_Init(&HashCtx);
  pocl_SHA1_Update(&HashCtx, Data, Len);
  pocl_SHA1_Final(&HashCtx, TempDigest);

  uint8_t *hashstr = BuildHash;
  for (unsigned i = 0; i < SHA1_DIGEST_SIZE; i++) {
    *hashstr++ = (TempDigest[i] & 0x0F) + 65;
    *hashstr++ = ((TempDigest[i] & 0xF0) >> 4) + 65;
  }
  *hashstr = 0;
  BuildHash[2] = '/';
}

bool Level0Device::initHelperKernels() {
  std::vector<uint8_t> SpvData;
  std::vector<char> ProgramBCData;
  std::string BuildLog;
  SHA1_digest_t BuildHash;
  Level0Kernel *K;
  char ProgramCacheDir[POCL_MAX_PATHNAME_LENGTH];
  assert(Driver);

  // fake program with BuildHash to get a cache path
  struct _cl_program FakeProgram;
  FakeProgram.num_devices = 1;
  FakeProgram.build_hash = &BuildHash;

  calculateHash(BuildHash, MemfillSpv, MemfillSpvLen);
  pocl_cache_program_path(ProgramCacheDir, &FakeProgram, 0);

  SpvData.clear();
  SpvData.insert(SpvData.end(), MemfillSpv, MemfillSpv + MemfillSpvLen);
  MemfillProgram = Driver->getJobSched().createProgram(
      ContextHandle, DeviceHandle,
      false, // JITCompilation,
      BuildLog,
      false, // Optimize,
      Supports64bitBuffers,
      0,       // SpecConstantIDs.size(),
      nullptr, // SpecConstantIDs.data(),
      nullptr, // SpecConstantPtrs.data(),
      nullptr, // SpecConstantSizes.data(),
      SpvData,
      ProgramBCData, // can be empty if JIT = disabled
      ProgramCacheDir, KernelCacheHash);
  if (MemfillProgram == nullptr) {
    POCL_MSG_ERR("Level0 Device: Failed to build memfill kernels");
    return false;
  }

  for (unsigned i = 1; i <= 128; i *= 2) {
    std::string Kernel1D = "memfill_" + std::to_string(i);
    K = Driver->getJobSched().createKernel(MemfillProgram, Kernel1D.c_str());
    assert(K);
    MemfillKernels[Kernel1D] = K;

    std::string Kernel3D = "memfill_rect_" + std::to_string(i);
    K = Driver->getJobSched().createKernel(MemfillProgram, Kernel1D.c_str());
    assert(K);
    MemfillKernels[Kernel3D] = K;
  }

  calculateHash(BuildHash, ImagefillSpv, ImagefillSpvLen);
  pocl_cache_program_path(ProgramCacheDir, &FakeProgram, 0);

  SpvData.clear();
  SpvData.insert(SpvData.end(), ImagefillSpv, ImagefillSpv + ImagefillSpvLen);
  ImagefillProgram = Driver->getJobSched().createProgram(
      ContextHandle, DeviceHandle,
      false, // JITCompilation,
      BuildLog,
      false,   // Optimize,
      false,   // Supports64bitBuffers,
      0,       // SpecConstantIDs.size(),
      nullptr, // SpecConstantIDs.data(),
      nullptr, // SpecConstantPtrs.data(),
      nullptr, // SpecConstantSizes.data(),
      SpvData,
      ProgramBCData, // can be empty if JIT = disabled
      ProgramCacheDir, KernelCacheHash);
  if (ImagefillProgram == nullptr) {
    POCL_MSG_ERR("Level0 Device: Failed to build imagefill kernels");
    return false;
  }

  std::vector<std::string> PixelTypes = { "f", "ui", "i"};
  std::vector<std::string> ImgTypes = { "2d_", "2d_array_",
                                        "1d_", "1d_array_",
                                        "1d_buffer_",
                                        "3d_" };
  for (auto ImgT : ImgTypes) {
    for (auto PixT : PixelTypes) {
      std::string KernelName = "imagefill_" + ImgT + PixT;
      K = Driver->getJobSched().createKernel(ImagefillProgram,
                                             KernelName.c_str());
      assert(K);
      ImagefillKernels[KernelName] = K;
    }
  }

  return true;
}

void Level0Device::destroyHelperKernels() {
  if (MemfillProgram) {
    for (auto &I : MemfillKernels) {
      Driver->getJobSched().releaseKernel(MemfillProgram, I.second);
    }
    Driver->getJobSched().releaseProgram(MemfillProgram);
  }
  if (ImagefillProgram) {
    for (auto &I : ImagefillKernels) {
      Driver->getJobSched().releaseKernel(ImagefillProgram, I.second);
    }
    Driver->getJobSched().releaseProgram(ImagefillProgram);
  }
}

void Level0Device::pushCommand(_cl_command_node *Command) {
  if (Command->type == CL_COMMAND_NDRANGE_KERNEL ||
      Command->type == CL_COMMAND_SVM_MEMFILL ||
      Command->type == CL_COMMAND_MEMFILL_INTEL ||
      Command->type == CL_COMMAND_FILL_BUFFER ||
      Command->type == CL_COMMAND_FILL_IMAGE) {
    if (ComputeQueues.available())
      ComputeQueues.pushWork(Command);
    else
      UniversalQueues.pushWork(Command);
  } else {
    if (CopyQueues.available())
      CopyQueues.pushWork(Command);
    else
      UniversalQueues.pushWork(Command);
  }
}

void Level0Device::pushCommandBatch(BatchType Batch) {
  if (supportsCmdQBatching()) {
    UniversalQueues.pushCommandBatch(Batch);
  } else {
    POCL_ABORT_UNIMPLEMENTED("this code path should not be entered - BUG\n");
  }
}

ze_event_handle_t Level0Device::getNewEvent() {
  std::lock_guard<std::mutex> Guard(EventPoolLock);
  if (EventPools.front().isEmpty())
    EventPools.emplace_front(this, EventPoolSize);
  return EventPools.front().getEvent();
}

void *Level0Device::allocUSMSharedMem(uint64_t Size, bool EnableCompression,
                                      ze_device_mem_alloc_flags_t DevFlags,
                                      ze_host_mem_alloc_flags_t HostFlags) {
  void *Ptr = nullptr;
  ze_device_mem_alloc_desc_t MemAllocDesc = {
      ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC, nullptr, DevFlags, GlobalMemOrd};
  ze_host_mem_alloc_desc_t HostDesc = {ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC,
                                       nullptr, HostFlags};

  ze_memory_compression_hints_ext_desc_t MemCompHints = {
      ZE_STRUCTURE_TYPE_MEMORY_COMPRESSION_HINTS_EXT_DESC, nullptr,
      ZE_MEMORY_COMPRESSION_HINTS_EXT_FLAG_COMPRESSED};
  if (EnableCompression && supportsCompression()) {
    MemAllocDesc.pNext = &MemCompHints;
  }

  ze_relaxed_allocation_limits_exp_desc_t RelaxedLimits = {
    ZE_STRUCTURE_TYPE_RELAXED_ALLOCATION_LIMITS_EXP_DESC, nullptr,
    ZE_RELAXED_ALLOCATION_LIMITS_EXP_FLAG_MAX_SIZE};
  if (NeedsRelaxedLimits && Size > UINT32_MAX) {
    MemAllocDesc.pNext = &RelaxedLimits;
  }

  uint64_t NextPowerOf2 = pocl_size_ceil2_64(Size);
  uint64_t Align = std::min(NextPowerOf2, (uint64_t)MAX_EXTENDED_ALIGNMENT);

  ze_result_t Res = zeMemAllocShared(ContextHandle, &MemAllocDesc, &HostDesc,
                                     Size, Align, DeviceHandle, &Ptr);
  LEVEL0_CHECK_RET(nullptr, Res);
  return Ptr;
}

void *Level0Device::allocUSMDeviceMem(uint64_t Size,
                                      ze_device_mem_alloc_flags_t DevFlags) {
  void *Ptr = nullptr;
  ze_device_mem_alloc_desc_t MemAllocDesc = {
      ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC, nullptr, DevFlags, GlobalMemOrd};

  uint64_t NextPowerOf2 = pocl_size_ceil2_64(Size);
  uint64_t Align = std::min(NextPowerOf2, (uint64_t)MAX_EXTENDED_ALIGNMENT);

  LEVEL0_CHECK_RET(nullptr, zeMemAllocDevice(ContextHandle, &MemAllocDesc, Size,
                                             Align, DeviceHandle, &Ptr));
  return Ptr;
}

void *Level0Device::allocUSMHostMem(uint64_t Size,
                                    ze_device_mem_alloc_flags_t HostFlags,
                                    void *pNext) {
  void *Ptr = nullptr;
  ze_host_mem_alloc_desc_t HostDesc = {ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC,
                                       pNext, HostFlags};

  uint64_t NextPowerOf2 = pocl_size_ceil2_64(Size);
  uint64_t Align = std::min(NextPowerOf2, (uint64_t)MAX_EXTENDED_ALIGNMENT);

  LEVEL0_CHECK_RET(nullptr,
                   zeMemAllocHost(ContextHandle, &HostDesc, Size, Align, &Ptr));
  return Ptr;
}

void Level0Device::freeUSMMem(void *Ptr) {
  if (Ptr == nullptr)
    return;
  LEVEL0_CHECK_ABORT_NO_EXIT(zeMemFree(ContextHandle, Ptr));
}

bool Level0Device::freeUSMMemBlocking(void *Ptr) {
  if (Ptr == nullptr)
    return true;

  if (!Driver->hasExtension("ZE_extension_memory_free_policies"))
    return false;

  ze_memory_free_ext_desc_t FreeExtDesc = {
      ZE_STRUCTURE_TYPE_MEMORY_FREE_EXT_DESC, nullptr,
      ZE_DRIVER_MEMORY_FREE_POLICY_EXT_FLAG_BLOCKING_FREE};
  ze_result_t Res = zeMemFreeExt(ContextHandle, &FreeExtDesc, Ptr);
  LEVEL0_CHECK_ABORT_NO_EXIT(Res);
  return true;
}

void Level0Device::freeCmdBuf(void *CmdBufData) {
  UniversalQueues.freeCmdBuf(CmdBufData);
}

void *Level0Device::createCmdBuf(cl_command_buffer_khr CmdBuf) {
  return UniversalQueues.createCmdBuf(CmdBuf);
}

static void convertOpenclToZeImgFormat(cl_channel_type ChType,
                                       cl_channel_order ChOrder,
                                       ze_image_format_t &ZeFormat) {
  ze_image_format_type_t ZeType = {};
  ze_image_format_layout_t ZeLayout = {};

  switch (ChType) {
  case CL_SNORM_INT8:
  case CL_SNORM_INT16:
    ZeType = ZE_IMAGE_FORMAT_TYPE_SNORM;
    break;
  case CL_UNORM_INT8:
  case CL_UNORM_INT16:
  case CL_UNORM_SHORT_555:
  case CL_UNORM_SHORT_565:
  case CL_UNORM_INT_101010:
    ZeType = ZE_IMAGE_FORMAT_TYPE_UNORM;
    break;
  case CL_SIGNED_INT8:
  case CL_SIGNED_INT16:
  case CL_SIGNED_INT32:
    ZeType = ZE_IMAGE_FORMAT_TYPE_SINT;
    break;
  case CL_UNSIGNED_INT8:
  case CL_UNSIGNED_INT16:
  case CL_UNSIGNED_INT32:
    ZeType = ZE_IMAGE_FORMAT_TYPE_UINT;
    break;
  case CL_HALF_FLOAT:
  case CL_FLOAT:
    ZeType = ZE_IMAGE_FORMAT_TYPE_FLOAT;
    break;
  default:
    ZeType = ZE_IMAGE_FORMAT_TYPE_FORCE_UINT32;
  }

  switch (ChOrder) {
  case CL_R: {
    ZeFormat.x = ZE_IMAGE_FORMAT_SWIZZLE_R;
    ZeFormat.y = ZE_IMAGE_FORMAT_SWIZZLE_0;
    ZeFormat.z = ZE_IMAGE_FORMAT_SWIZZLE_0;
    ZeFormat.w = ZE_IMAGE_FORMAT_SWIZZLE_1;
    switch (ChType) {
    case CL_SNORM_INT8:
    case CL_UNORM_INT8:
    case CL_SIGNED_INT8:
    case CL_UNSIGNED_INT8:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_8;
      break;
    case CL_SNORM_INT16:
    case CL_UNORM_INT16:
    case CL_SIGNED_INT16:
    case CL_UNSIGNED_INT16:
    case CL_HALF_FLOAT:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_16;
      break;
    case CL_SIGNED_INT32:
    case CL_UNSIGNED_INT32:
    case CL_FLOAT:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_32;
      break;
    default:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_FORCE_UINT32;
    }
    break;
  }
  case CL_RG: {
    ZeFormat.x = ZE_IMAGE_FORMAT_SWIZZLE_R;
    ZeFormat.y = ZE_IMAGE_FORMAT_SWIZZLE_G;
    ZeFormat.z = ZE_IMAGE_FORMAT_SWIZZLE_0;
    ZeFormat.w = ZE_IMAGE_FORMAT_SWIZZLE_1;
    switch (ChType) {
    case CL_SNORM_INT8:
    case CL_UNORM_INT8:
    case CL_SIGNED_INT8:
    case CL_UNSIGNED_INT8:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_8_8;
      break;
    case CL_SNORM_INT16:
    case CL_UNORM_INT16:
    case CL_SIGNED_INT16:
    case CL_UNSIGNED_INT16:
    case CL_HALF_FLOAT:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_16_16;
      break;
    case CL_SIGNED_INT32:
    case CL_UNSIGNED_INT32:
    case CL_FLOAT:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_32_32;
      break;
    default:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_FORCE_UINT32;
    }
    break;
  }
  case CL_RGB: {
    ZeFormat.x = ZE_IMAGE_FORMAT_SWIZZLE_R;
    ZeFormat.y = ZE_IMAGE_FORMAT_SWIZZLE_G;
    ZeFormat.z = ZE_IMAGE_FORMAT_SWIZZLE_B;
    ZeFormat.w = ZE_IMAGE_FORMAT_SWIZZLE_1;
    switch (ChType) {
    case CL_UNORM_SHORT_565:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_5_6_5;
      break;
    case CL_UNORM_SHORT_555:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_5_5_5_1;
      break;
    case CL_UNORM_INT_101010:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_10_10_10_2;
      break;
    default:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_FORCE_UINT32;
    }
    break;
  }
  case CL_RGBA: {
    ZeFormat.x = ZE_IMAGE_FORMAT_SWIZZLE_R;
    ZeFormat.y = ZE_IMAGE_FORMAT_SWIZZLE_G;
    ZeFormat.z = ZE_IMAGE_FORMAT_SWIZZLE_B;
    ZeFormat.w = ZE_IMAGE_FORMAT_SWIZZLE_A;
    switch (ChType) {
    case CL_SNORM_INT8:
    case CL_UNORM_INT8:
    case CL_SIGNED_INT8:
    case CL_UNSIGNED_INT8:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_8_8_8_8;
      break;
    case CL_SNORM_INT16:
    case CL_UNORM_INT16:
    case CL_SIGNED_INT16:
    case CL_UNSIGNED_INT16:
    case CL_HALF_FLOAT:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_16_16_16_16;
      break;
    case CL_SIGNED_INT32:
    case CL_UNSIGNED_INT32:
    case CL_FLOAT:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_32_32_32_32;
      break;
    default:
      ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_FORCE_UINT32;
    }
    break;
  }
  case CL_BGRA: {
      ZeFormat.x = ZE_IMAGE_FORMAT_SWIZZLE_B;
      ZeFormat.y = ZE_IMAGE_FORMAT_SWIZZLE_G;
      ZeFormat.z = ZE_IMAGE_FORMAT_SWIZZLE_R;
      ZeFormat.w = ZE_IMAGE_FORMAT_SWIZZLE_A;
      switch (ChType) {
        case CL_SNORM_INT8:
        case CL_UNORM_INT8:
        case CL_SIGNED_INT8:
        case CL_UNSIGNED_INT8:
          ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_8_8_8_8;
          break;
        case CL_SNORM_INT16:
        case CL_UNORM_INT16:
        case CL_SIGNED_INT16:
        case CL_UNSIGNED_INT16:
        case CL_HALF_FLOAT:
          ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_16_16_16_16;
          break;
        case CL_SIGNED_INT32:
        case CL_UNSIGNED_INT32:
        case CL_FLOAT:
          ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_32_32_32_32;
          break;
        default:
          ZeLayout = ZE_IMAGE_FORMAT_LAYOUT_FORCE_UINT32;
      }
      break;
  }
  }
  ZeFormat.layout = ZeLayout;
  ZeFormat.type = ZeType;
}

ze_image_handle_t Level0Device::allocImage(cl_channel_type ChType,
                                           cl_channel_order ChOrder,
                                           cl_mem_object_type ImgType,
                                           cl_mem_flags ImgFlags, size_t Width,
                                           size_t Height, size_t Depth,
                                           size_t ArraySize) {

  // Specify single component FLOAT32 format
  ze_image_format_t ZeFormat{};
  convertOpenclToZeImgFormat(ChType, ChOrder, ZeFormat);
  ze_image_type_t ZeImgType;
  switch (ImgType) {
  case CL_MEM_OBJECT_IMAGE1D:
    ZeImgType = ZE_IMAGE_TYPE_1D;
    break;
  case CL_MEM_OBJECT_IMAGE2D:
    ZeImgType = ZE_IMAGE_TYPE_2D;
    break;
  case CL_MEM_OBJECT_IMAGE3D:
    ZeImgType = ZE_IMAGE_TYPE_3D;
    break;
  case CL_MEM_OBJECT_IMAGE1D_ARRAY:
    ZeImgType = ZE_IMAGE_TYPE_1DARRAY;
    break;
  case CL_MEM_OBJECT_IMAGE2D_ARRAY:
    ZeImgType = ZE_IMAGE_TYPE_2DARRAY;
    break;
  case CL_MEM_OBJECT_IMAGE1D_BUFFER:
    ZeImgType = ZE_IMAGE_TYPE_BUFFER;
    break;
  default:
    ZeImgType = ZE_IMAGE_TYPE_FORCE_UINT32;
  }

  ze_image_flags_t ZeFlags = 0;
  if (((ImgFlags & CL_MEM_READ_WRITE) != 0u) ||
      ((ImgFlags & CL_MEM_WRITE_ONLY) != 0u)) {
    ZeFlags = ZE_IMAGE_FLAG_KERNEL_WRITE;
  }

  ze_image_desc_t imageDesc = {
      ZE_STRUCTURE_TYPE_IMAGE_DESC,
      nullptr,
      ZeFlags,
      ZeImgType,
      ZeFormat,
      (uint32_t)Width,
      (uint32_t)Height,
      (uint32_t)Depth,
      (uint32_t)ArraySize, // array levels
      0  // mip levels
  };
  ze_image_handle_t ImageH = nullptr;
  ze_result_t Res =
      zeImageCreate(ContextHandle, DeviceHandle, &imageDesc, &ImageH);
  LEVEL0_CHECK_RET(nullptr, Res);
  return ImageH;
}

void Level0Device::freeImage(ze_image_handle_t ImageH) {
  ze_result_t Res = zeImageDestroy(ImageH);
  LEVEL0_CHECK_ABORT_NO_EXIT(Res);
}

ze_sampler_handle_t Level0Device::allocSampler(cl_addressing_mode AddrMode,
                                               cl_filter_mode FilterMode,
                                               cl_bool NormalizedCoords) {
  ze_sampler_address_mode_t ZeAddrMode = {};
  switch (AddrMode) {
  case CL_ADDRESS_NONE:
    ZeAddrMode = ZE_SAMPLER_ADDRESS_MODE_NONE;
    break;
  default:
  case CL_ADDRESS_CLAMP:
    ZeAddrMode = ZE_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    break;
  case CL_ADDRESS_CLAMP_TO_EDGE:
    ZeAddrMode = ZE_SAMPLER_ADDRESS_MODE_CLAMP;
    break;
  case CL_ADDRESS_REPEAT:
    ZeAddrMode = ZE_SAMPLER_ADDRESS_MODE_REPEAT;
    break;
  case CL_ADDRESS_MIRRORED_REPEAT:
    ZeAddrMode = ZE_SAMPLER_ADDRESS_MODE_MIRROR;
    break;
  }

  ze_sampler_filter_mode_t ZeFilterMode = {};
  switch (FilterMode) {
  case CL_FILTER_LINEAR:
    ZeFilterMode = ZE_SAMPLER_FILTER_MODE_LINEAR;
    break;
  default:
  case CL_FILTER_NEAREST:
    ZeFilterMode = ZE_SAMPLER_FILTER_MODE_NEAREST;
    break;
  }

  ze_sampler_desc_t SamplerDesc = {
      ZE_STRUCTURE_TYPE_SAMPLER_DESC, nullptr, ZeAddrMode, ZeFilterMode,
      static_cast<ze_bool_t>((char)NormalizedCoords)};
  ze_sampler_handle_t SamplerH = nullptr;
  LEVEL0_CHECK_RET(nullptr, zeSamplerCreate(ContextHandle, DeviceHandle,
                                            &SamplerDesc, &SamplerH));
  return SamplerH;
}

void Level0Device::freeSampler(ze_sampler_handle_t SamplerH) {
  ze_result_t Res = zeSamplerDestroy(SamplerH);
  LEVEL0_CHECK_ABORT_NO_EXIT(Res);
}

int Level0Device::createSpirvProgram(cl_program Program, cl_uint DeviceI) {

  cl_device_id Dev = Program->devices[DeviceI];
  int Res = pocl_bitcode_is_spirv_execmodel_kernel(
      Program->program_il, Program->program_il_size, Dev->address_bits);
  POCL_RETURN_ERROR_ON((Res == 0), CL_BUILD_PROGRAM_FAILURE,
                       "Binary is not a SPIR-V module!\n");

  std::vector<uint8_t> Spirv(Program->program_il,
                             Program->program_il + Program->program_il_size);

  std::vector<char> ProgramBC;
  char *BinaryPtr = (char *)Program->binaries[DeviceI];
  size_t BinarySize = Program->binary_sizes[DeviceI];
  int TestR = pocl_bitcode_is_triple(BinaryPtr, BinarySize, "spir");
  assert(TestR && "Program->binaries[] is not LLVM bitcode!");
  ProgramBC.insert(ProgramBC.end(), BinaryPtr, BinaryPtr + BinarySize);

  assert(Program->data[DeviceI] == nullptr);
  char ProgramCacheDir[POCL_MAX_PATHNAME_LENGTH];
  pocl_cache_program_path(ProgramCacheDir, Program, DeviceI);

  std::vector<uint32_t> SpecConstantIDs;
  std::vector<const void *> SpecConstantPtrs;
  std::vector<size_t> SpecConstantSizes;

  if (Program->num_spec_consts != 0u) {
    for (size_t i = 0; i < Program->num_spec_consts; ++i) {
      if (Program->spec_const_is_set[i] == CL_FALSE) {
        continue;
      }
      SpecConstantIDs.push_back(Program->spec_const_ids[i]);
      SpecConstantPtrs.push_back(&Program->spec_const_values[i]);
      SpecConstantSizes.push_back(sizeof(uint64_t));
    }
  }

  std::string UserJITPref(pocl_get_string_option("POCL_LEVEL0_JIT", "auto"));
  bool JITCompilation = false;
  if (UserJITPref == "0")
    JITCompilation = false;
  else if (UserJITPref == "1")
    JITCompilation = true;
  else {
    // use heuristic
    if (UserJITPref != "auto")
      POCL_MSG_WARN("unknown option given to POCL_LEVEL0_JIT: '%s' \n",
                    UserJITPref.c_str());
    JITCompilation =
        (Program->num_kernels > 256 && Program->program_il_size > 128000);
  }
  POCL_MSG_PRINT_LEVEL0("createProgram | using JIT: %s\n",
                        (JITCompilation ? "YES" : "NO"));

  std::string CompilerOptions(
      Program->compiler_options != nullptr ? Program->compiler_options : "");
  bool Optimize =
      (CompilerOptions.find("-cl-disable-opt") == std::string::npos);

  std::string BuildLog;
  Level0Program *ProgramData = Driver->getJobSched().createProgram(
      ContextHandle, DeviceHandle, JITCompilation, BuildLog, Optimize,
      Supports64bitBuffers, SpecConstantIDs.size(), SpecConstantIDs.data(),
      SpecConstantPtrs.data(), SpecConstantSizes.data(), Spirv, ProgramBC,
      ProgramCacheDir, KernelCacheHash);

  if (ProgramData == nullptr) {
    if (!BuildLog.empty()) {
      pocl_append_to_buildlog(Program, DeviceI, strdup(BuildLog.c_str()),
                              BuildLog.size());
    }
    POCL_RETURN_ERROR_ON(1, CL_BUILD_PROGRAM_FAILURE,
                         "Failed to compile program\n");
  }

  Program->data[DeviceI] = ProgramData;
  return CL_SUCCESS;
}

int Level0Device::createBuiltinProgram(cl_program Program, cl_uint DeviceI) {
#ifdef ENABLE_NPU

  assert(Program->data[DeviceI] == nullptr);
  char ProgramCacheDir[POCL_MAX_PATHNAME_LENGTH];
  char ProgramBcPath[POCL_MAX_PATHNAME_LENGTH];
  // TODO: better input to Hash value calculation.
  std::string Hash{Program->concated_builtin_names};
  int errcode = pocl_cache_create_program_cachedir(
      Program, DeviceI, (char *)Hash.data(), Hash.size(), ProgramBcPath);

  pocl_cache_program_path(ProgramCacheDir, Program, DeviceI);

  std::string BuildLog;
  Level0BuiltinProgram *ProgramData =
      Driver->getJobSched().createBuiltinProgram(
          ContextHandle, DeviceHandle, BuildLog, Program->num_builtin_kernels,
          Program->builtin_kernel_names, Program->builtin_kernel_ids,
          Program->builtin_kernel_attributes, ProgramCacheDir, KernelCacheHash);

  if (ProgramData == nullptr) {
    if (!BuildLog.empty()) {
      pocl_append_to_buildlog(Program, DeviceI, strdup(BuildLog.c_str()),
                              BuildLog.size());
      POCL_MSG_WARN("Build log: \n%s", BuildLog.c_str());
    }
    POCL_RETURN_ERROR_ON(1, CL_BUILD_PROGRAM_FAILURE,
                         "Failed to compile program\n");
  }

  Program->data[DeviceI] = ProgramData;
  return CL_SUCCESS;
#else
  std::string BuildLog("Builtin programs on non-NPU devices are not supported");
  pocl_append_to_buildlog(Program, DeviceI, strdup(BuildLog.c_str()),
                          BuildLog.size());
  return CL_BUILD_PROGRAM_FAILURE;
#endif
}

int Level0Device::freeProgram(cl_program Program, cl_uint DeviceI) {
  // module can be NULL if compilation fails.
  if (Program->data[DeviceI] == nullptr) {
    return CL_SUCCESS;
  }

  if (Program->num_builtin_kernels > 0) {
#ifdef ENABLE_NPU
    Level0BuiltinProgram *ProgramData =
        (Level0BuiltinProgram *)Program->data[DeviceI];
    Driver->getJobSched().releaseBuiltinProgram(ProgramData);
    Program->data[DeviceI] = nullptr;
#else
    return CL_OUT_OF_RESOURCES;
#endif
  } else {
    Level0Program *ProgramData = (Level0Program *)Program->data[DeviceI];
    Driver->getJobSched().releaseProgram(ProgramData);
    Program->data[DeviceI] = nullptr;
  }
  return CL_SUCCESS;
}

int Level0Device::createKernel(cl_program Program, cl_kernel Kernel,
                               unsigned ProgramDeviceI) {
  if (Program->num_builtin_kernels > 0) {
#ifdef ENABLE_NPU
    Level0BuiltinProgram *L0Program =
        (Level0BuiltinProgram *)Program->data[ProgramDeviceI];
    Level0BuiltinKernel *Ker =
        Driver->getJobSched().createBuiltinKernel(L0Program, Kernel->name);
    Kernel->data[ProgramDeviceI] = Ker;
#else
    return CL_OUT_OF_RESOURCES;
#endif
  } else {
    Level0Program *L0Program = (Level0Program *)Program->data[ProgramDeviceI];
    Level0Kernel *Ker =
        Driver->getJobSched().createKernel(L0Program, Kernel->name);
    Kernel->data[ProgramDeviceI] = Ker;
  }

  return Kernel->data[ProgramDeviceI] == nullptr ? CL_OUT_OF_RESOURCES
                                                 : CL_SUCCESS;
}

int Level0Device::freeKernel(cl_program Program, cl_kernel Kernel,
                             unsigned ProgramDeviceI) {
  bool Res;
  if (Program->num_builtin_kernels > 0) {
#ifdef ENABLE_NPU
    Level0BuiltinProgram *L0Program =
        (Level0BuiltinProgram *)Program->data[ProgramDeviceI];
    Level0BuiltinKernel *Ker =
        (Level0BuiltinKernel *)Kernel->data[ProgramDeviceI];
    Res = Driver->getJobSched().releaseBuiltinKernel(L0Program, Ker);
#else
    return CL_OUT_OF_RESOURCES;
#endif
  } else {
    Level0Program *L0Program = (Level0Program *)Program->data[ProgramDeviceI];
    Level0Kernel *Ker = (Level0Kernel *)Kernel->data[ProgramDeviceI];
    Res = Driver->getJobSched().releaseKernel(L0Program, Ker);
  }

  return Res == true ? CL_SUCCESS : CL_INVALID_KERNEL;
}

bool Level0Device::getBestKernel(Level0Program *Program, Level0Kernel *Kernel,
                                 bool LargeOffset, unsigned LocalWGSize,
                                 ze_module_handle_t &Mod,
                                 ze_kernel_handle_t &Ker) {

  return Driver->getJobSched().getBestKernel(Program, Kernel, LargeOffset,
                                             LocalWGSize, Mod, Ker);
}

#ifdef ENABLE_NPU
bool Level0Device::getBestBuiltinKernel(Level0BuiltinProgram *Program,
                                        Level0BuiltinKernel *Kernel,
                                        ze_graph_handle_t &Graph) {
  return Driver->getJobSched().getBestBuiltinKernel(Program, Kernel, Graph);
}
#endif

bool Level0Device::getMemfillKernel(unsigned PatternSize,
                                    Level0Kernel **L0Kernel,
                                    ze_module_handle_t &ModH,
                                    ze_kernel_handle_t &KerH) {

  std::string KernelName = "memfill_" + std::to_string(PatternSize);
  // TODO locking? errcheck!
  Level0Kernel *K = MemfillKernels[KernelName];
  assert(K);
  *L0Kernel = K;
  return Driver->getJobSched().getBestKernel(MemfillProgram, K,
                                             false, // LargeOffset,
                                             1024,  // LocalWGSize,
                                             ModH, KerH);
}

bool Level0Device::getImagefillKernel(cl_channel_type ChType,
                                      cl_channel_order ChOrder,
                                      cl_mem_object_type ImgType,
                                      Level0Kernel **L0Kernel,
                                      ze_module_handle_t &ModH,
                                      ze_kernel_handle_t &KerH) {

  std::string PixelType;
  switch (ChType) {
  case CL_UNSIGNED_INT8:
  case CL_UNSIGNED_INT16:
  case CL_UNSIGNED_INT32:
    PixelType = "ui";
    break;
  case CL_SIGNED_INT8:
  case CL_SIGNED_INT16:
  case CL_SIGNED_INT32:
    PixelType = "i";
    break;
  default:
    PixelType = "f";
  }
  std::string ImageType;
  switch (ImgType) {
  case CL_MEM_OBJECT_IMAGE1D:
    ImageType = "1d_";
    break;
  case CL_MEM_OBJECT_IMAGE1D_ARRAY:
    ImageType = "1d_array_";
    break;
  case CL_MEM_OBJECT_IMAGE1D_BUFFER:
    ImageType = "1d_buffer_";
    break;
  case CL_MEM_OBJECT_IMAGE2D:
    ImageType = "2d_";
    break;
  case CL_MEM_OBJECT_IMAGE2D_ARRAY:
    ImageType = "2d_array_";
    break;
  case CL_MEM_OBJECT_IMAGE3D:
    ImageType = "3d_";
    break;
  default:
    ImageType = "_unknown";
    break;
  }

  std::string KernelName = "imagefill_" + ImageType + PixelType;
  // TODO locking? errcheck!
  Level0Kernel *K = ImagefillKernels[KernelName];
  assert(K);
  *L0Kernel = K;
  return Driver->getJobSched().getBestKernel(ImagefillProgram, K,
                                             false, // LargeOffset,
                                             128,   // LocalWGSize,
                                             ModH, KerH);
}

cl_bitfield Level0Device::getMemCaps(cl_device_info Type) {
  switch (Type) {
  case CL_DEVICE_HOST_MEM_CAPABILITIES_INTEL:
    return ClDev->host_usm_capabs;
  case CL_DEVICE_DEVICE_MEM_CAPABILITIES_INTEL:
    return ClDev->device_usm_capabs;
  case CL_DEVICE_SINGLE_DEVICE_SHARED_MEM_CAPABILITIES_INTEL:
    return ClDev->single_shared_usm_capabs;
  case CL_DEVICE_CROSS_DEVICE_SHARED_MEM_CAPABILITIES_INTEL:
    return ClDev->cross_shared_usm_capabs;
  case CL_DEVICE_SHARED_SYSTEM_MEM_CAPABILITIES_INTEL:
    return ClDev->system_shared_usm_capabs;
  default:
    assert(0 && "unhandled switch value");
  }
  return 0;
}

void *Level0Device::getMemBasePtr(const void *USMPtr) {
  void *Base = nullptr;
  size_t Size = 0;
  ze_result_t Res = zeMemGetAddressRange(ContextHandle, USMPtr, &Base, &Size);
  if (Res != ZE_RESULT_SUCCESS)
    return nullptr;
  return Base;
}

size_t Level0Device::getMemSize(const void *USMPtr) {
  void *Base = nullptr;
  size_t Size = 0;
  ze_result_t Res = zeMemGetAddressRange(ContextHandle, USMPtr, &Base, &Size);
  if (Res != ZE_RESULT_SUCCESS)
    return 0;
  return Size;
}

cl_device_id Level0Device::getMemAssoc(const void *USMPtr) {
  ze_memory_allocation_properties_t Props = {};
  ze_device_handle_t AssocDev = nullptr;
  ze_result_t Res =
      zeMemGetAllocProperties(ContextHandle, USMPtr, &Props, &AssocDev);
  if (Res != ZE_RESULT_SUCCESS || AssocDev == nullptr)
    return nullptr;

  return Driver->getClDevForHandle(AssocDev);
}

cl_unified_shared_memory_type_intel
Level0Device::getMemType(const void *USMPtr) {
  ze_memory_allocation_properties_t Props = {};
  ze_device_handle_t AssocDev = nullptr;
  ze_result_t Res =
      zeMemGetAllocProperties(ContextHandle, USMPtr, &Props, &AssocDev);
  if (Res != ZE_RESULT_SUCCESS)
    return CL_MEM_TYPE_UNKNOWN_INTEL;

  switch (Props.type) {
  case ZE_MEMORY_TYPE_HOST:
    return CL_MEM_TYPE_HOST_INTEL;
  case ZE_MEMORY_TYPE_DEVICE:
    return CL_MEM_TYPE_DEVICE_INTEL;
  case ZE_MEMORY_TYPE_SHARED:
    return CL_MEM_TYPE_SHARED_INTEL;
  case ZE_MEMORY_TYPE_UNKNOWN:
  default:
    return CL_MEM_TYPE_UNKNOWN_INTEL;
  }
}

cl_mem_alloc_flags_intel Level0Device::getMemFlags(const void *USMPtr) {
  // TODO
  return 0;
}

void Level0Device::getTimingInfo(uint32_t &TS, uint32_t &KernelTS,
                                 double &TimerFreq, double &NsPerCycle) {
  TS = TSBits;
  KernelTS = KernelTSBits;
  TimerFreq = TimerFrequency;
  NsPerCycle = TimerNsPerCycle;
}

void Level0Device::getMaxWGs(uint32_t_3 *MaxWGs) {
  std::memcpy(MaxWGs, MaxWGCount, sizeof(uint32_t_3));
}

uint32_t Level0Device::getMaxWGSizeForKernel(Level0Kernel *Kernel) {
#ifdef ZE_STRUCTURE_TYPE_KERNEL_MAX_GROUP_SIZE_EXT_PROPERTIES
  // TODO what default should we return here ?
  if (!Driver->hasExtension("ZE_extension_kernel_max_group_size_properties"))
    return getMaxWGSize();

  // TODO this makes the returned value dependent on random choice;
  ze_kernel_handle_t hKernel = Kernel->getAnyCreated();
  if (hKernel == nullptr)
    return getMaxWGSize();

  ze_kernel_max_group_size_properties_ext_t MaxGroupProps = {
    .stype = ZE_STRUCTURE_TYPE_KERNEL_MAX_GROUP_SIZE_EXT_PROPERTIES,
    .pNext = nullptr,
    .maxGroupSize = 0
  };

  ze_kernel_properties_t KernelProps = {
    .stype = ZE_STRUCTURE_TYPE_KERNEL_PROPERTIES,
    .pNext = &MaxGroupProps,
  };

  ze_result_t Res = zeKernelGetProperties(hKernel, &KernelProps);
  if (Res != ZE_RESULT_SUCCESS)
    return getMaxWGSize();

  return MaxGroupProps.maxGroupSize;
#else
  return getMaxWGSize();
#endif
}

bool Level0Device::isIntelNPU() const {
  // Used OpenVINO as reference - it just check if the driver is an
  // Intel NPU driver.
  return Driver->isIntelNPU();
}

Level0Driver::Level0Driver(ze_driver_handle_t DrvHandle) : DriverH(DrvHandle) {
  ze_driver_properties_t DriverProperties = {};
  DriverProperties.stype = ZE_STRUCTURE_TYPE_DRIVER_PROPERTIES;
  DriverProperties.pNext = nullptr;
  ze_result_t Res = zeDriverGetProperties(DriverH, &DriverProperties);
  if (Res != ZE_RESULT_SUCCESS) {
    POCL_MSG_ERR("zeDriverGetProperties FAILED\n");
    return;
  }
  UUID = DriverProperties.uuid;
  Version = DriverProperties.driverVersion;

  uint32_t ExtCount = 0;
  Res = zeDriverGetExtensionProperties(DriverH, &ExtCount, nullptr);
  if (Res != ZE_RESULT_SUCCESS) {
    POCL_MSG_ERR("zeDriverGetExtensionProperties 1 FAILED\n");
    return;
  }

  std::vector<ze_driver_extension_properties_t> Extensions;
  if (ExtCount > 0) {
    POCL_MSG_PRINT_LEVEL0("%u Level0 extensions found\n", ExtCount);
    Extensions.resize(ExtCount);
    Res = zeDriverGetExtensionProperties(DriverH, &ExtCount, Extensions.data());
    if (Res != ZE_RESULT_SUCCESS) {
      POCL_MSG_ERR("zeDriverGetExtensionProperties 2 FAILED\n");
      return;
    }
    for (auto &E : Extensions) {
      POCL_MSG_PRINT_LEVEL0("Level0 extension: %s\n", E.name);
      ExtensionSet.insert(E.name);
    }
  } else {
    POCL_MSG_PRINT_LEVEL0("No Level0 extensions found\n");
  }

  ze_context_desc_t ContextDescription = {};
  ContextDescription.stype = ZE_STRUCTURE_TYPE_CONTEXT_DESC;
  ContextDescription.pNext = nullptr;
  ContextDescription.flags = 0;

  Res = zeContextCreate(DriverH, &ContextDescription, &ContextH);
  if (Res != ZE_RESULT_SUCCESS) {
    POCL_MSG_ERR("zeContextCreate FAILED\n");
    return;
  }

  uint32_t DeviceCount = 0;
  Res = zeDeviceGet(DriverH, &DeviceCount, nullptr);
  if (Res != ZE_RESULT_SUCCESS || DeviceCount == 0) {
    POCL_MSG_ERR("zeDeviceGet 1 FAILED\n");
    return;
  }

  if (DeviceCount == 0) {
    POCL_MSG_ERR("zeDriver: zero devices available\n");
    return;
  }

  std::vector<ze_device_handle_t> DeviceArray;
  DeviceArray.resize(DeviceCount);
  Devices.resize(DeviceCount);
  DeviceHandles.resize(DeviceCount);

  Res = zeDeviceGet(DriverH, &DeviceCount, DeviceArray.data());
  if (Res != ZE_RESULT_SUCCESS) {
    POCL_MSG_ERR("zeDeviceGet 2 FAILED\n");
    return;
  }

  for (uint32_t i = 0; i < DeviceCount; ++i) {
    DeviceHandles[i] = DeviceArray[i];
  }
  ze_device_properties_t DeviceProperties{};
  Res = zeDeviceGetProperties(DeviceHandles[0], &DeviceProperties);
  if (Res != ZE_RESULT_SUCCESS) {
    POCL_MSG_ERR("zeDeviceGetProperties FAILED\n");
    return;
  }

#ifdef ENABLE_NPU
  Res = zeDriverGetExtensionFunctionAddress(
      DriverH, GRAPH_EXT_NAME, reinterpret_cast<void **>(&GraphDDITableExt));
  if (Res != ZE_RESULT_SUCCESS)
    GraphDDITableExt = nullptr;

  Res = zeDriverGetExtensionFunctionAddress(
      DriverH, ZE_PROFILING_DATA_EXT_NAME,
      reinterpret_cast<void **>(&GraphProfDDITableExt));
  if (Res != ZE_RESULT_SUCCESS)
    GraphProfDDITableExt = nullptr;

  if (!GraphDDITableExt || !GraphProfDDITableExt) {
    POCL_MSG_PRINT_LEVEL0("Failed to initialize LevelZero Graph Ext "
                          "for driver %u\n",
                          DriverProperties.driverVersion);
  }
#endif

  if (!JobSched.init(DriverH, DeviceHandles)) {
    Devices.clear();
    DeviceHandles.clear();
    POCL_MSG_ERR("Failed to initialize compilation job scheduler\n");
    return;
  }
  assert(Devices[0].get() == nullptr);
}

Level0Driver::~Level0Driver() {
  Devices.clear();
  DeviceHandles.clear();
  if (ContextH != nullptr) {
    zeContextDestroy(ContextH);
  }
}

Level0Device *Level0Driver::createDevice(unsigned Index, cl_device_id Dev,
                                         const char *Params) {
  assert(Index < Devices.size());
  assert(Devices[Index].get() == nullptr);
  Devices[Index].reset(
      new Level0Device(this, DeviceHandles[Index], Dev, Params));
  POCL_MSG_PRINT_LEVEL0("createDEVICE | Cl Dev %p | Dri %p | Dev %p \n", Dev,
                        DriverH, Devices[Index].get());
  ++NumDevices;
  HandleToIDMap[DeviceHandles[Index]] = Dev;
  return Devices[Index].get();
}

void Level0Driver::releaseDevice(Level0Device *Dev) {
  if (empty()) {
    return;
  }
  for (auto &Device : Devices) {
    if (Device.get() == Dev) {
      Device.reset(nullptr);
      --NumDevices;
    }
  }
}

Level0Device *Level0Driver::getExportDevice() {
  // first find device which can only export not import
  for (auto &Device : Devices) {
    if (Device->supportsExportByDmaBuf() && !Device->supportsImportByDmaBuf()) {
      return Device.get();
    }
  }

  // then find any dev that can export
  for (auto &Device : Devices) {
    if (Device->supportsExportByDmaBuf()) {
      return Device.get();
    }
  }

  return nullptr;
}

bool Level0Driver::getImportDevices(std::vector<Level0Device *> &ImportDevices,
                                    Level0Device *ExcludeDev) {
  unsigned UnsupportingDevices = 0;
  for (auto &Device : Devices) {
    if (ExcludeDev && Device.get() == ExcludeDev)
      continue;
    if (Device->supportsImportByDmaBuf())
      ImportDevices.push_back(Device.get());
    else
      ++UnsupportingDevices;
  }
  return UnsupportingDevices == 0;
}

/// Return true if the driver is known to be an Intel NPU driver.
bool Level0Driver::isIntelNPU() const {
#ifdef ENABLE_NPU
  constexpr ze_driver_uuid_t IntelNPUUUID = ze_intel_npu_driver_uuid;
  return std::memcmp(&UUID, &IntelNPUUUID, sizeof(UUID)) == 0;
#else
  return false; // Actually don't know.
#endif
}

void *Level0DefaultAllocator::allocBuffer(uintptr_t Key, Level0Device *,
                                          ze_device_mem_alloc_flags_t DevFlags,
                                          ze_host_mem_alloc_flags_t HostFlags,
                                          size_t Size, bool &IsHostAccessible) {
  if (Device->isHostUnifiedMemory()) {
    IsHostAccessible = true;
    if (Device->supportsSingleSharedUSM()) {
      // iGPU
      return Device->allocUSMSharedMem(Size, /* Compress */ false, DevFlags,
                                       HostFlags);
    } else {
      // NPU device uses L0 Host Mem
      return Device->allocUSMHostMem(Size, HostFlags);
    }
  } else {
    IsHostAccessible = false;
    // dGPU
    return Device->allocUSMDeviceMem(Size, DevFlags);
  }
}

bool Level0DefaultAllocator::freeBuffer(uintptr_t Key, Level0Device *,
                                        void *Ptr) {
  Device->freeUSMMem(Ptr);
  return true;
}

void *Level0DMABufAllocator::allocBuffer(uintptr_t Key, Level0Device *D,
                                         ze_device_mem_alloc_flags_t DevFlags,
                                         ze_host_mem_alloc_flags_t HostFlags,
                                         size_t Size, bool &IsHostAccessible) {
  assert(D->isHostUnifiedMemory());
  IsHostAccessible = true;
  auto ImportIt = std::find(ImportDevices.begin(), ImportDevices.end(), D);
  bool DevIsImport = ImportIt != ImportDevices.end();
  bool DevIsExport = ExportDevice == D;
  assert(DevIsExport || DevIsImport);

  // we must have an available filedescriptor -> do an Export allocation first
  void *ExportPtr =
      Allocations[Key].allocExport(ExportDevice, DevFlags, HostFlags, Size);
  if (DevIsExport)
    return ExportPtr;
  if (ExportPtr == nullptr)
    return nullptr;

  assert(Allocations[Key].isValid());
  assert(DevIsImport);
  return Allocations[Key].allocImport(D, DevFlags, HostFlags, Size);
}

bool Level0DMABufAllocator::freeBuffer(uintptr_t Key, Level0Device *D,
                                       void *Ptr) {
  assert(D->isHostUnifiedMemory());
  if (!Allocations[Key].isValid())
    return false;

  auto ImportIt = std::find(ImportDevices.begin(), ImportDevices.end(), D);
  bool DevIsImport = ImportIt != ImportDevices.end();
  bool DevIsExport = ExportDevice == D;
  assert(DevIsExport || DevIsImport);

  return Allocations[Key].free(D);
}

bool Level0DMABufAllocator::clear(Level0Device *D) {
  for (auto &A : Allocations) {
    A.second.free(D);
  }
  return true;
}

void *DMABufAllocation::allocExport(Level0Device *D,
                                    ze_device_mem_alloc_flags_t DevFlags,
                                    ze_host_mem_alloc_flags_t HostFlags,
                                    size_t Size) {
  if (ExportPtr != nullptr)
    return ExportPtr;

  ze_external_memory_export_desc_t descExport = {};
  descExport.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_DESC;
  descExport.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;

  void *Ptr = D->allocUSMHostMem(Size, HostFlags, &descExport);
  POCL_MSG_PRINT_LEVEL0("ALLOCATED: %p SIZE: %zu | FROM ExportDev: %s\n", Ptr,
                        Size, D->getClDev()->short_name);

  // only one export device is supported, all others must be import devices
  assert(FD < 0);
  ze_external_memory_export_fd_t FdExport = {};
  FdExport.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_FD;
  FdExport.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;

  ze_memory_allocation_properties_t propAlloc = {};
  propAlloc.stype = ZE_STRUCTURE_TYPE_MEMORY_ALLOCATION_PROPERTIES;
  propAlloc.pNext = &FdExport;

  ze_result_t Res =
      zeMemGetAllocProperties(D->getContextHandle(), Ptr, &propAlloc, nullptr);
  assert(Res == ZE_RESULT_SUCCESS);
  assert(FdExport.fd != 0);

  if (Ptr && FdExport.fd >= 0) {
    ExportDev = D;
    ExportPtr = Ptr;
    FD = FdExport.fd;
    return Ptr;
  } else {
    return nullptr;
  }
}

void *DMABufAllocation::allocImport(Level0Device *D,
                                    ze_device_mem_alloc_flags_t DevFlags,
                                    ze_host_mem_alloc_flags_t HostFlags,
                                    size_t Size) {
  if (BufferImportMap[D] != nullptr)
    return BufferImportMap[D];

  // export mem must be allocated before import is called
  assert(ExportDev);
  assert(ExportPtr);
  assert(FD >= 0);

  ze_external_memory_import_fd_t FdImport = {};
  FdImport.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMPORT_FD;
  FdImport.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
  FdImport.fd = FD;

  void *Ptr = D->allocUSMHostMem(Size, HostFlags, &FdImport);
  POCL_MSG_PRINT_LEVEL0("ALLOCATED: %p SIZE: %zu | FROM ImportDev: %s\n", Ptr,
                        Size, D->getClDev()->short_name);

  if (Ptr)
    BufferImportMap[D] = Ptr;
  return Ptr;
}

bool DMABufAllocation::free(Level0Device *D) {
  if (D == ExportDev) {
    if (BufferImportMap.empty()) {
      D->freeUSMMem(ExportPtr);
      ExportPtr = nullptr;
      ExportDev = nullptr;
      FD = -1;
    } else {
      POCL_MSG_PRINT_LEVEL0("Not freeing Export alloc "
                            "because Import(s) remain\n");
      return false; // can we release export mem while we have active imports?
    }
  } else {
    auto It = BufferImportMap.find(D);
    if (It == BufferImportMap.end()) {
      // this is OK in general; the allocation
      // could be freed earlier for a particular device
#if 0
      POCL_MSG_PRINT_LEVEL0("Could not find allocation "
                            "for Device %p in the ImportMap\n",
                            D);
#endif
      return false;
    }
    D->freeUSMMem(It->second);
    BufferImportMap.erase(D);
  }
  return true;
}

DMABufAllocation::~DMABufAllocation() {
  for (auto &[Dev, Ptr] : BufferImportMap) {
    Dev->freeUSMMem(Ptr);
  }
  if (ExportDev && ExportPtr)
    ExportDev->freeUSMMem(ExportPtr);
}
