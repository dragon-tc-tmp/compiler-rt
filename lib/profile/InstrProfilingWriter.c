/*===- InstrProfilingWriter.c - Write instrumentation to a file or buffer -===*\
|*
|*                     The LLVM Compiler Infrastructure
|*
|* This file is distributed under the University of Illinois Open Source
|* License. See LICENSE.TXT for details.
|*
\*===----------------------------------------------------------------------===*/

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"
#include <string.h>

#define INSTR_PROF_VALUE_PROF_DATA
#include "InstrProfData.inc"

COMPILER_RT_VISIBILITY void (*FreeHook)(void *) = NULL;
static ProfBufferIO TheBufferIO;
#define VP_BUFFER_SIZE 8 * 1024
static uint8_t BufferIOBuffer[VP_BUFFER_SIZE];
COMPILER_RT_VISIBILITY uint8_t *DynamicBufferIOBuffer = 0;
COMPILER_RT_VISIBILITY uint32_t VPBufferSize = 0;

/* The buffer writer is reponsponsible in keeping writer state
 * across the call.
 */
COMPILER_RT_VISIBILITY uint32_t lprofBufferWriter(ProfDataIOVec *IOVecs,
                                                  uint32_t NumIOVecs,
                                                  void **WriterCtx) {
  uint32_t I;
  char **Buffer = (char **)WriterCtx;
  for (I = 0; I < NumIOVecs; I++) {
    size_t Length = IOVecs[I].ElmSize * IOVecs[I].NumElm;
    memcpy(*Buffer, IOVecs[I].Data, Length);
    *Buffer += Length;
  }
  return 0;
}

static void llvmInitBufferIO(ProfBufferIO *BufferIO, WriterCallback FileWriter,
                             void *File, uint8_t *Buffer, uint32_t BufferSz) {
  BufferIO->File = File;
  BufferIO->FileWriter = FileWriter;
  BufferIO->BufferStart = Buffer;
  BufferIO->BufferSz = BufferSz;
  BufferIO->CurOffset = 0;
}

COMPILER_RT_VISIBILITY ProfBufferIO *
lprofCreateBufferIO(WriterCallback FileWriter, void *File) {
  uint8_t *Buffer = DynamicBufferIOBuffer;
  uint32_t BufferSize = VPBufferSize;
  if (!Buffer) {
    Buffer = &BufferIOBuffer[0];
    BufferSize = sizeof(BufferIOBuffer);
  }
  llvmInitBufferIO(&TheBufferIO, FileWriter, File, Buffer, BufferSize);
  return &TheBufferIO;
}

COMPILER_RT_VISIBILITY void lprofDeleteBufferIO(ProfBufferIO *BufferIO) {
  if (DynamicBufferIOBuffer)
    FreeHook(DynamicBufferIOBuffer);
}

COMPILER_RT_VISIBILITY int
lprofBufferIOWrite(ProfBufferIO *BufferIO, const uint8_t *Data, uint32_t Size) {
  /* Buffer is not large enough, it is time to flush.  */
  if (Size + BufferIO->CurOffset > BufferIO->BufferSz) {
    if (lprofBufferIOFlush(BufferIO) != 0)
      return -1;
  }
  /* Special case, bypass the buffer completely. */
  ProfDataIOVec IO[] = {{Data, sizeof(uint8_t), Size}};
  if (Size > BufferIO->BufferSz) {
    if (BufferIO->FileWriter(IO, 1, &BufferIO->File))
      return -1;
  } else {
    /* Write the data to buffer */
    uint8_t *Buffer = BufferIO->BufferStart + BufferIO->CurOffset;
    lprofBufferWriter(IO, 1, (void **)&Buffer);
    BufferIO->CurOffset = Buffer - BufferIO->BufferStart;
  }
  return 0;
}

COMPILER_RT_VISIBILITY int lprofBufferIOFlush(ProfBufferIO *BufferIO) {
  if (BufferIO->CurOffset) {
    ProfDataIOVec IO[] = {
        {BufferIO->BufferStart, sizeof(uint8_t), BufferIO->CurOffset}};
    if (BufferIO->FileWriter(IO, 1, &BufferIO->File))
      return -1;
    BufferIO->CurOffset = 0;
  }
  return 0;
}

static int writeOneValueProfData(ProfBufferIO *BufferIO,
                                 VPGatherHookType VPDataGatherer,
                                 const __llvm_profile_data *Data) {
  ValueProfData *CurVData = VPDataGatherer(Data);
  if (!CurVData)
    return 0;
  if (lprofBufferIOWrite(BufferIO, (const uint8_t *)CurVData,
                         CurVData->TotalSize) != 0)
    return -1;
  FreeHook(CurVData);
  return 0;
}

static int writeValueProfData(WriterCallback Writer, void *WriterCtx,
                              VPGatherHookType VPDataGatherer,
                              const __llvm_profile_data *DataBegin,
                              const __llvm_profile_data *DataEnd) {
  ProfBufferIO *BufferIO;
  const __llvm_profile_data *DI = 0;

  if (!VPDataGatherer)
    return 0;

  BufferIO = lprofCreateBufferIO(Writer, WriterCtx);

  for (DI = DataBegin; DI < DataEnd; DI++) {
    if (writeOneValueProfData(BufferIO, VPDataGatherer, DI))
      return -1;
  }

  if (lprofBufferIOFlush(BufferIO) != 0)
    return -1;
  lprofDeleteBufferIO(BufferIO);

  return 0;
}

COMPILER_RT_VISIBILITY int lprofWriteData(WriterCallback Writer,
                                          void *WriterCtx,
                                          VPGatherHookType VPDataGatherer) {
  /* Match logic in __llvm_profile_write_buffer(). */
  const __llvm_profile_data *DataBegin = __llvm_profile_begin_data();
  const __llvm_profile_data *DataEnd = __llvm_profile_end_data();
  const uint64_t *CountersBegin = __llvm_profile_begin_counters();
  const uint64_t *CountersEnd = __llvm_profile_end_counters();
  const char *NamesBegin = __llvm_profile_begin_names();
  const char *NamesEnd = __llvm_profile_end_names();
  return lprofWriteDataImpl(Writer, WriterCtx, DataBegin, DataEnd,
                            CountersBegin, CountersEnd, VPDataGatherer,
                            NamesBegin, NamesEnd);
}

COMPILER_RT_VISIBILITY int
lprofWriteDataImpl(WriterCallback Writer, void *WriterCtx,
                   const __llvm_profile_data *DataBegin,
                   const __llvm_profile_data *DataEnd,
                   const uint64_t *CountersBegin, const uint64_t *CountersEnd,
                   VPGatherHookType VPDataGatherer, const char *NamesBegin,
                   const char *NamesEnd) {

  /* Calculate size of sections. */
  const uint64_t DataSize = __llvm_profile_get_data_size(DataBegin, DataEnd);
  const uint64_t CountersSize = CountersEnd - CountersBegin;
  const uint64_t NamesSize = NamesEnd - NamesBegin;
  const uint64_t Padding = __llvm_profile_get_num_padding_bytes(NamesSize);

  /* Enough zeroes for padding. */
  const char Zeroes[sizeof(uint64_t)] = {0};

  /* Create the header. */
  __llvm_profile_header Header;

  if (!DataSize)
    return 0;

/* Initialize header structure.  */
#define INSTR_PROF_RAW_HEADER(Type, Name, Init) Header.Name = Init;
#include "InstrProfData.inc"

  /* Write the data. */
  ProfDataIOVec IOVec[] = {{&Header, sizeof(__llvm_profile_header), 1},
                           {DataBegin, sizeof(__llvm_profile_data), DataSize},
                           {CountersBegin, sizeof(uint64_t), CountersSize},
                           {NamesBegin, sizeof(uint8_t), NamesSize},
                           {Zeroes, sizeof(uint8_t), Padding}};
  if (Writer(IOVec, sizeof(IOVec) / sizeof(*IOVec), &WriterCtx))
    return -1;

  return writeValueProfData(Writer, WriterCtx, VPDataGatherer, DataBegin,
                            DataEnd);
}
