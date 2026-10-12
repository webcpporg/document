// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

/**
 The runtime of a worker that joins the module's memory, an Emscripten JS library (--js-library):
 a factory called with `webcppJoining` makes no memory and runs no constructor, and its
 `webcppJoin(slot)` instantiates the module on the memory of the slot and starts this thread on the
 slot's stack, thread-local storage and pthread, as a Wasm Worker of Emscripten's starts.

 Tip: Emscripten's own Wasm Worker is a Worker it creates and names, which runs only the C++
 functions posted to it. Here the C side prepares its workers as usual
 (`emscripten_malloc_wasm_worker`: a stack, a pthread, the locks of a threaded libc), but
 `_emscripten_create_wasm_worker` records each one's slot instead of creating a Worker, and a worker
 the program creates takes a slot over. The switch to the worker's runtime is made after the code
 that would claim the worker's `onmessage` and Node's `process` events has been passed by, so that
 the worker's globals stay the program's. It reads the runtime's internals (`createWasm`,
 `wasmMemory`, `updateMemoryViews`), which Emscripten 6.0.11 has.
*/

addToLibrary({
    // The slots of the workers prepared so far, in this thread.
    $webcppSlots: [],

    _emscripten_create_wasm_worker__deps: ['$webcppSlots'],
    _emscripten_create_wasm_worker: (id, stack, size, pthread) => {
        webcppSlots.push({ id, stack, size, pthread })
        return 1
    },

    $webcppJoin__deps: ['$webcppSlots', '_emscripten_wasm_worker_initialize', '__set_thread_state',
        '_embind_initialize_bindings'],
    $webcppJoin__force: true,
    $webcppJoin__postset: `
if (Module['webcppJoining']) ENVIRONMENT_IS_WASM_WORKER = true;
Module['webcppJoin'] = webcppJoin;
Module['webcppShared'] = () => ({ module: wasmModule, memory: wasmMemory, slots: webcppSlots.slice() });
`,
    // module and memory, the main thread's, and slot, one of its webcppSlots.
    $webcppJoin: (module, memory, slot) => {
        wasmMemory = memory
        updateMemoryViews()
        wasmModule = module
        // A worker instantiates synchronously, from the module it was given.
        createWasm()
        runtimeInitialized = true
        __emscripten_wasm_worker_initialize(slot.id, slot.stack, slot.size)
        ___set_thread_state(slot.pthread, /* is_main_thread= */ 0, /* is_runtime_thread= */ 0,
            /* supports_wait= */ 1)
#if STACK_OVERFLOW_CHECK
        writeStackCookie()
#endif
        // Embind's classes are JavaScript of each thread's own: their registrations run again.
        __embind_initialize_bindings()
    },
})
