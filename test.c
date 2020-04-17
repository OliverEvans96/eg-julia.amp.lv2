#include "lv2/core/lv2.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h>

struct arg_struct {
  void (*run)(LV2_Handle instance, uint32_t n_samples);
  LV2_Handle instance;
  uint32_t n_samples;
};

typedef struct {
	// Port buffers
	float* gain;
  float* input;
	float*       output;
} Amp;

void *call_run(void *arguments) {
  struct arg_struct *args = (struct  arg_struct*)arguments;
  int i;
  uint32_t j;
	Amp* amp = (Amp*)args->instance;
  float gain = 3.2f;
  amp->input = (float*) malloc(args->n_samples * sizeof(float));
  amp->output = (float*) malloc(args->n_samples * sizeof(float));
  amp->gain = &gain;

  printf("start call run\n");
  for(i=0; i<10; i++) {
    printf("i=%d. creating input.\n", i);
    for(j=0; j<args->n_samples; j++) {
      printf("i=%d, j=%d. setting.\n", i, j);
      amp->input[j] = (float)(i+1) * (float)(j+1);
      printf("i=%d, j=%d. in=%.2f\n", i, j, amp->input[j]);
    }
    printf("i=%d. calling run.\n", i);
    args->run(args->instance, args->n_samples);
    printf("i=%d. run finished.\n", i);
    for(j=0; j<args->n_samples; j++) {
      printf("out[%d, %d] = %.2f\n", i, j, amp->input[j]);
    }
  }
  printf("finish call run\n");

  free(amp->input);
  free(amp->output);
  return NULL;
}

int main() {
  void *amplib;

  LV2_Descriptor* (*lv2_descriptor)(uint32_t index);
  LV2_Handle (*instantiate)(const LV2_Descriptor*     descriptor,
                            double                    rate,
                            const char*               bundle_path,
                            const LV2_Feature* const* features);
  void (*activate)(LV2_Handle instance);
  void (*connect_port)(LV2_Handle instance,
                       uint32_t   port,
                       void*      data);
  void (*run)(LV2_Handle instance, uint32_t n_samples);
  void (*deactivate)(LV2_Handle instance);
  void (*cleanup)(LV2_Handle instance);

  LV2_Descriptor* descriptor;

  pthread_t thread_id;
  const double rate = 48000;
  const LV2_Feature ** features = NULL;
  const char* bundle_path = "path";

  const char* uri;
  LV2_Handle instance;
  const int n_samples = 20;
  struct arg_struct args;

  printf("DL OPEN\n");
  amplib = dlopen("./julia-amp.so", RTLD_NOW);
  if (amplib == NULL) {
    printf("Failed to open amplib: %s\n", dlerror());
    return 1;
  }
  printf("DL SYM\n");
  lv2_descriptor = dlsym(amplib, "lv2_descriptor");

  printf("Load Descriptor\n");
  descriptor = lv2_descriptor(0);
  printf("Load URI\n");
  uri = descriptor->URI;
  printf("Load Instantiate\n");
  instantiate = descriptor->instantiate;
  printf("Load Activate\n");
  activate = descriptor->activate;
  printf("Load connect_port\n");
  connect_port = descriptor->connect_port;
  printf("Load run\n");
  run = descriptor->run;
  printf("Load deactivate\n");
  deactivate = descriptor->deactivate;
  printf("Load cleanup\n");
  cleanup = descriptor->cleanup;

  printf("The plugin URI is '%s'\n", uri);

  printf("Instantiating.\n");
  instance = instantiate(descriptor, rate, bundle_path, features);

  printf("Activating.\n");
  activate(instance);

  printf("Creating run thread\n");

  args.run = run;
  args.instance = instance;
  args.n_samples = n_samples;
  pthread_create(&thread_id, NULL, &call_run, &args);
  pthread_join(thread_id, NULL);
  printf("Run thread finished\n");



  printf("Deactivating.\n");
  deactivate(instance);

  printf("Cleaning up.\n");
  cleanup(instance);


  dlclose(amplib);
}
