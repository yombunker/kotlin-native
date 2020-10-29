/*
 * Copyright 2010-2017 JetBrains s.r.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Alloc.h"
#include "Atomic.h"
#include "Cleaner.h"
#include "Exceptions.h"
#include "KAssert.h"
#include "Memory.h"
#include "ObjCExportInit.h"
#include "Porting.h"
#include "Runtime.h"
#include "Worker.h"

typedef void (*Initializer)(int initialize, MemoryState* memory);
struct InitNode {
  Initializer init;
  InitNode* next;
};

namespace {

InitNode* initHeadNode = nullptr;
InitNode* initTailNode = nullptr;

enum class RuntimeStatus {
    kUninitialized,
    kRunning,
    kDestroying,
};

struct RuntimeState {
    MemoryState* memoryState;
    Worker* worker;
    RuntimeStatus status = RuntimeStatus::kUninitialized;
};

enum {
  INIT_GLOBALS = 0,
  INIT_THREAD_LOCAL_GLOBALS = 1,
  DEINIT_THREAD_LOCAL_GLOBALS = 2,
  DEINIT_GLOBALS = 3
};

void InitOrDeinitGlobalVariables(int initialize, MemoryState* memory) {
  InitNode* currentNode = initHeadNode;
  while (currentNode != nullptr) {
    currentNode->init(initialize, memory);
    currentNode = currentNode->next;
  }
}

KBoolean g_checkLeaks = KonanNeedDebugInfo;
KBoolean g_checkLeakedCleaners = KonanNeedDebugInfo;

constexpr RuntimeState* kInvalidRuntime = nullptr;

THREAD_LOCAL_VARIABLE RuntimeState* runtimeState = kInvalidRuntime;

inline bool isValidRuntime() {
  return ::runtimeState != kInvalidRuntime;
}

volatile int aliveRuntimesCount = 0;

enum class GlobalRuntimeStatus {
    kUninitialized,
    kRunning,
    kDestroyed,
};

volatile GlobalRuntimeStatus globalRuntimeStatus = GlobalRuntimeStatus::kUninitialized;

RuntimeState* initRuntime() {
  SetKonanTerminateHandler();
  RuntimeState* result = konanConstructInstance<RuntimeState>();
  if (!result) return kInvalidRuntime;
  RuntimeCheck(!isValidRuntime(), "No active runtimes allowed");
  ::runtimeState = result;
  bool firstRuntime = compareAndSet(&globalRuntimeStatus, GlobalRuntimeStatus::kUninitialized, GlobalRuntimeStatus::kRunning);
  RuntimeCheck(atomicGet(&globalRuntimeStatus) == GlobalRuntimeStatus::kRunning, "Must be running");
  atomicAdd(&aliveRuntimesCount, 1);
  result->memoryState = InitMemory(firstRuntime);
  result->worker = WorkerInit(true);
  // Keep global variables in state as well.
  if (firstRuntime) {
    konan::consoleInit();
#if KONAN_OBJC_INTEROP
    Kotlin_ObjCExport_initialize();
#endif
    InitOrDeinitGlobalVariables(INIT_GLOBALS, result->memoryState);
  }
  InitOrDeinitGlobalVariables(INIT_THREAD_LOCAL_GLOBALS, result->memoryState);
  RuntimeAssert(result->status == RuntimeStatus::kUninitialized, "Runtime must still be in the uninitialized state");
  result->status = RuntimeStatus::kRunning;
  return result;
}

void deinitRuntime(RuntimeState* state, bool destroyRuntime) {
  RuntimeAssert(state->status == RuntimeStatus::kRunning, "Runtime must be in the running state");
  state->status = RuntimeStatus::kDestroying;
  // This may be called after TLS is zeroed out, so ::memoryState in Memory cannot be trusted.
  RestoreMemory(state->memoryState);
  atomicAdd(&aliveRuntimesCount, -1);
  InitOrDeinitGlobalVariables(DEINIT_THREAD_LOCAL_GLOBALS, state->memoryState);
  if (destroyRuntime)
    InitOrDeinitGlobalVariables(DEINIT_GLOBALS, state->memoryState);
  auto workerId = GetWorkerId(state->worker);
  WorkerDeinit(state->worker);
  DeinitMemory(state->memoryState, destroyRuntime);
  konanDestructInstance(state);
  WorkerDestroyThreadDataIfNeeded(workerId);
}

void Kotlin_deinitRuntimeCallback(void* argument) {
  auto* state = reinterpret_cast<RuntimeState*>(argument);
  deinitRuntime(state, false);
}

}  // namespace

extern "C" {

void AppendToInitializersTail(InitNode *next) {
  // TODO: use RuntimeState.
  if (initHeadNode == nullptr) {
    initHeadNode = next;
  } else {
    initTailNode->next = next;
  }
  initTailNode = next;
}

void Kotlin_initRuntimeIfNeeded() {
  if (!isValidRuntime()) {
    if (atomicGet(&globalRuntimeStatus) == GlobalRuntimeStatus::kDestroyed) {
      konan::consoleErrorf("Kotlin runtime was previously destroyed. Cannot create new runtime.\n");
      konan::abort();
    }
    initRuntime();
    // Register runtime deinit function at thread cleanup.
    konan::onThreadExit(Kotlin_deinitRuntimeCallback, runtimeState);
  }
}

void Kotlin_deinitRuntimeIfNeeded() {
  if (isValidRuntime()) {
    deinitRuntime(::runtimeState, false);
    ::runtimeState = kInvalidRuntime;
  }
}

void Kotlin_destroyRuntime() {
    RuntimeAssert(atomicGet(&globalRuntimeStatus) == GlobalRuntimeStatus::kRunning, "Kotlin runtime must be running");
    RuntimeAssert(isValidRuntime(), "Current thread must have Kotlin runtime on it.");

    if (Kotlin_cleanersLeakCheckerEnabled()) {
        // Make sure to collect any lingering cleaners.
        PerformFullGC();
        // Execute all the cleaner blocks and stop the Cleaner worker.
        ShutdownCleaners(true);
    } else {
        // Stop the cleaner worker without executing remaining cleaner blocks.
        ShutdownCleaners(false);
    }
    if (Kotlin_memoryLeakCheckerEnabled()) WaitNativeWorkersTermination();

    atomicSet(&globalRuntimeStatus, GlobalRuntimeStatus::kDestroyed);

    auto otherRuntimesCount = atomicGet(&aliveRuntimesCount) - 1;
    RuntimeAssert(otherRuntimesCount >= 0, "Cannot be negative.");
    if (otherRuntimesCount > 0) {
        konan::consoleErrorf("Cannot destroy runtime while there're %d alive threads with Kotlin runtime on them.\n", otherRuntimesCount);
        konan::abort();
    }

    deinitRuntime(::runtimeState, true);
    ::runtimeState = kInvalidRuntime;
}

KInt Konan_Platform_canAccessUnaligned() {
#if KONAN_NO_UNALIGNED_ACCESS
  return 0;
#else
  return 1;
#endif
}

KInt Konan_Platform_isLittleEndian() {
#ifdef __BIG_ENDIAN__
  return 0;
#else
  return 1;
#endif
}

KInt Konan_Platform_getOsFamily() {
#if KONAN_MACOSX
  return 1;
#elif KONAN_IOS
  return 2;
#elif KONAN_LINUX
  return 3;
#elif KONAN_WINDOWS
  return 4;
#elif KONAN_ANDROID
  return 5;
#elif KONAN_WASM
  return 6;
#elif KONAN_TVOS
  return 7;
#elif KONAN_WATCHOS
  return 8;
#else
#warning "Unknown platform"
  return 0;
#endif
}

KInt Konan_Platform_getCpuArchitecture() {
#if KONAN_ARM32
  return 1;
#elif KONAN_ARM64
  return 2;
#elif KONAN_X86
  return 3;
#elif KONAN_X64
  return 4;
#elif KONAN_MIPS32
  return 5;
#elif KONAN_MIPSEL32
  return 6;
#elif KONAN_WASM
  return 7;
#else
#warning "Unknown CPU"
  return 0;
#endif
}

KInt Konan_Platform_getMemoryModel() {
  return IsStrictMemoryModel ? 0 : 1;
}

KBoolean Konan_Platform_isDebugBinary() {
  return KonanNeedDebugInfo ? true : false;
}

void Kotlin_zeroOutTLSGlobals() {
  if (runtimeState != nullptr && runtimeState->memoryState != nullptr)
    InitOrDeinitGlobalVariables(DEINIT_THREAD_LOCAL_GLOBALS, runtimeState->memoryState);
}

bool Kotlin_memoryLeakCheckerEnabled() {
  return g_checkLeaks;
}

KBoolean Konan_Platform_getMemoryLeakChecker() {
  return g_checkLeaks;
}

void Konan_Platform_setMemoryLeakChecker(KBoolean value) {
  g_checkLeaks = value;
}

bool Kotlin_cleanersLeakCheckerEnabled() {
    return g_checkLeakedCleaners;
}

KBoolean Konan_Platform_getCleanersLeakChecker() {
    return g_checkLeakedCleaners;
}

void Konan_Platform_setCleanersLeakChecker(KBoolean value) {
    g_checkLeakedCleaners = value;
}

}  // extern "C"
