/* Test-only serial shim for parser/oracle checks when libomp is unavailable.
 * Compile WITHOUT -fopenmp. This does not validate OpenMP scheduling or races.
 * Never place this directory on a measured guest build's include path.
 */
#ifndef MOESI_D_SERIAL_OMP_TEST_H
#define MOESI_D_SERIAL_OMP_TEST_H
#include <assert.h>
static inline void omp_set_dynamic(int enabled) { assert(!enabled); }
static inline void omp_set_num_threads(int count) { assert(count == 1); }
static inline int omp_get_thread_num(void) { return 0; }
#endif
