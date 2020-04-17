/*
  Copyright 2006-2016 David Robillard <d@drobilla.net>
  Copyright 2006 Steve Harris <steve@plugin.org.uk>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

/**
   LV2 headers are based on the URI of the specification they come from, so a
   consistent convention can be used even for unofficial extensions.  The URI
   of the core LV2 specification is <http://lv2plug.in/ns/lv2core>, by
   replacing `http:/` with `lv2` any header in the specification bundle can be
   included, in this case `lv2.h`.
*/
#include "lv2/core/lv2.h"

/** Include standard C headers */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include <julia.h>


class Worker {
    bool running = true;
    std::thread t;
    std::mutex mtx;
    std::condition_variable cond;
    std::deque<std::function<void()>> tasks;

public:
    Worker() : t{&Worker::threadFunc, this} {}
    ~Worker() {
        {
            std::unique_lock<std::mutex> lock(mtx);
            running = false;
        }
        cond.notify_one();
        t.join();
    }

    template <typename F> auto spawn(const F& f) -> std::packaged_task<decltype(f())()> {
        std::packaged_task<decltype(f())()> task(f);
        {
            std::unique_lock<std::mutex> lock(mtx);
            tasks.push_back([&task] { task(); });
        }
        cond.notify_one();
        return task;
    }

    template <typename F> auto run(const F& f) -> decltype(f()) { return spawn(f).get_future().get(); }

private:
    void threadFunc() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mtx);
                while (tasks.empty() && running) {
                    cond.wait(lock);
                }
                if (!running) {
                    break;
                }
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task();
        }
    }
};



class Julia {
    Worker worker;

private:
    static Julia& instance() {
        static Julia instance;
        return instance;
    }

    Julia() {
        worker.run([] {
            jl_init();
            jl_eval_string("println(\"JULIA  START\")");
        });
    }
    ~Julia() {
        worker.run([] {
            jl_eval_string("println(\"JULIA END\")");
            jl_atexit_hook(0);
        });
    }

public:
    template <typename F> static auto spawn(const F& f) -> std::packaged_task<decltype(f())()> {
        return instance().worker.spawn(f);
    }
    template <typename F> static auto run(const F& f) -> decltype(f()) { return instance().worker.run(f); }
    static void run(const char* s) {
        return instance().worker.run([&] { jl_eval_string(s); });
    }
};

extern "C" {
  LV2_SYMBOL_EXPORT
  const LV2_Descriptor*
  lv2_descriptor(uint32_t index);
}

/**
   The URI is the identifier for a plugin, and how the host associates this
   implementation in code with its description in data.  In this plugin it is
   only used once in the code, but defining the plugin URI at the top of the
   file is a good convention to follow.  If this URI does not match that used
   in the data files, the host will fail to load the plugin.
*/
#define AMP_URI "http://lv2plug.in/plugins/eg-julia-amp"

/**
   In code, ports are referred to by index.  An enumeration of port indices
   should be defined for readability.
*/
typedef enum {
	AMP_GAIN   = 0,
	AMP_INPUT  = 1,
	AMP_OUTPUT = 2
} PortIndex;

/**
   Every plugin defines a private structure for the plugin instance.  All data
   associated with a plugin instance is stored here, and is available to
   every instance method.  In this simple plugin, only port buffers need to be
   stored, since there is no additional instance data.
*/
typedef struct {
	// Port buffers
	const float* gain;
	const float* input;
	float*       output;
  jl_function_t* db_to_coef;
} Amp;

/**
   The `instantiate()` function is called by the host to create a new plugin
   instance.  The host passes the plugin descriptor, sample rate, and bundle
   path for plugins that need to load additional resources (e.g. waveforms).
   The features parameter contains host-provided features defined in LV2
   extensions, but this simple plugin does not use any.

   This function is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features)
{
	Amp* amp = (Amp*)calloc(1, sizeof(Amp));

	return (LV2_Handle)amp;
}

/**
   The `connect_port()` method is called by the host to connect a particular
   port to a buffer.  The plugin must store the data location, but data may not
   be accessed except in run().

   This method is in the ``audio'' threading class, and is called in the same
   context as run().
*/
static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
	Amp* amp = (Amp*)instance;

	switch ((PortIndex)port) {
	case AMP_GAIN:
		amp->gain = (const float*)data;
		break;
	case AMP_INPUT:
		amp->input = (const float*)data;
		break;
	case AMP_OUTPUT:
		amp->output = (float*)data;
		break;
	}
}

/**
   The `activate()` method is called by the host to initialise and prepare the
   plugin instance for running.  The plugin must reset all internal state
   except for buffer locations set by `connect_port()`.  Since this plugin has
   no other internal state, this method does nothing.

   This method is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static void
activate(LV2_Handle instance)
{
  Amp* self = (Amp*)instance;

  // This is necessary for embedding to succeed
  // see https://discourse.julialang.org/t/embedding-julia-without-rtld-global-in-dlopen/37655
  printf("dlopen libjulia.so\n");
  dlopen("libjulia.so", RTLD_NOW | RTLD_GLOBAL);


  printf("Julia init\n");
  Julia::run([] {jl_eval_string("println(\"Hello from Julia!)");});

  jl_function_t* db_to_coef = Julia::run([] {
      printf("Including amp.jl\n");
      jl_module_t* julia_amp = (jl_module_t *)jl_eval_string(
          "include(\"/home/oliver/local/src/lv2/plugins/eg-julia.amp.lv2/amp.jl\")"
      );
      if (jl_exception_occurred())
        printf("E2: %s \n", jl_typeof_str(jl_exception_occurred()));
      printf("Getting julia function\n");
      jl_function_t* db_to_coef = jl_get_function(julia_amp, "db_to_coef");
      if (jl_exception_occurred())
        printf("E3: %s \n", jl_typeof_str(jl_exception_occurred()));
      return db_to_coef;
  });

  printf("Saving julia function\n");
  self->db_to_coef = db_to_coef;

  float coef = Julia::run([&self] {
      printf("Testing julia function.\n");
      float gain = -3.0f;
      jl_value_t* ret = jl_call1(self->db_to_coef, jl_box_float32(gain));
      float unbox32;

      if (jl_exception_occurred())
        printf("E4: %s \n", jl_typeof_str(jl_exception_occurred()));
      if (jl_typeis(ret, jl_float32_type)) {
        unbox32 = jl_unbox_float32(ret);
        printf("Got32 gain=%.2f -> coef=%.2f\n", gain, unbox32);
        return unbox32;
      } else {
        printf("Received wrong type from julia.\n");
        return -1.0f;
      }
  });
  printf("Test coef = %.2f\n", coef);

  printf("activate complete\n");

}

/**
   The `run()` method is the main process function of the plugin.  It processes
   a block of audio in the audio context.  Since this plugin is
   `lv2:hardRTCapable`, `run()` must be real-time safe, so blocking (e.g. with
   a mutex) or memory allocation are not allowed.
*/
static void
run(LV2_Handle instance, uint32_t n_samples)
{
	const Amp* amp = (const Amp*)instance;

	const float        gain   = *(amp->gain);
	const float* const input  = amp->input;
	float* const       output = amp->output;
	float coef;

  coef = Julia::run([amp, gain] {
      float j_coef;
      jl_value_t *ret = jl_call1(amp->db_to_coef, jl_box_float32(gain));
      if (jl_typeis(ret, jl_float32_type)) {
        j_coef = jl_unbox_float32(ret);
      } else {
        j_coef = -1;
      }
      return j_coef;
  });
  printf("coef = %.2f\n", coef);

	for (uint32_t pos = 0; pos < n_samples; pos++) {
		output[pos] = input[pos] * coef;
	}
}

/**
   The `deactivate()` method is the counterpart to `activate()`, and is called by
   the host after running the plugin.  It indicates that the host will not call
   `run()` again until another call to `activate()` and is mainly useful for more
   advanced plugins with ``live'' characteristics such as those with auxiliary
   processing threads.  As with `activate()`, this plugin has no use for this
   information so this method does nothing.

   This method is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static void
deactivate(LV2_Handle instance)
{
}

/**
   Destroy a plugin instance (counterpart to `instantiate()`).

   This method is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static void
cleanup(LV2_Handle instance)
{
	free(instance);
}

/**
   The `extension_data()` function returns any extension data supported by the
   plugin.  Note that this is not an instance method, but a function on the
   plugin descriptor.  It is usually used by plugins to implement additional
   interfaces.  This plugin does not have any extension data, so this function
   returns NULL.

   This method is in the ``discovery'' threading class, so no other functions
   or methods in this plugin library will be called concurrently with it.
*/
static const void*
extension_data(const char* uri)
{
	return NULL;
}

/**
   Every plugin must define an `LV2_Descriptor`.  It is best to define
   descriptors statically to avoid leaking memory and non-portable shared
   library constructors and destructors to clean up properly.
*/
static const LV2_Descriptor descriptor = {
	AMP_URI,
	instantiate,
	connect_port,
	activate,
	run,
	deactivate,
	cleanup,
	extension_data
};

/**
   The `lv2_descriptor()` function is the entry point to the plugin library.  The
   host will load the library and call this function repeatedly with increasing
   indices to find all the plugins defined in the library.  The index is not an
   indentifier, the URI of the returned descriptor is used to determine the
   identify of the plugin.

   This method is in the ``discovery'' threading class, so no other functions
   or methods in this plugin library will be called concurrently with it.
*/
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
	switch (index) {
	case 0:  return &descriptor;
	default: return NULL;
	}
}
