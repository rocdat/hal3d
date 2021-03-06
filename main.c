#include "../comms.h"
#include "../mesh.h"
#include "../params.h"
#include "../umesh.h"
#include "hale_data.h"
#include "hale_interface.h"
#include <math.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>

// Validates the results of the simulation
void validate(const int ncells, const char* params_filename, const int rank,
              double* density, double* energy);

int main(int argc, char** argv) {
  if (argc != 2) {
    TERMINATE("usage: ./hale <parameter_filename>\n");
  }

  // Store some of the generic mesh meta data
  Mesh mesh;

  const char* hale_params = argv[1];
  mesh.niters = get_int_parameter("iterations", hale_params);
  mesh.max_dt = get_double_parameter("max_dt", ARCH_ROOT_PARAMS);
  mesh.sim_end = get_double_parameter("sim_end", ARCH_ROOT_PARAMS);
  mesh.global_nx = get_int_parameter("nx", hale_params);
  mesh.global_ny = get_int_parameter("ny", hale_params);
  mesh.global_nz = get_int_parameter("nz", hale_params);
  mesh.pad = 0;
  mesh.local_nx = mesh.global_nx + 2 * mesh.pad;
  mesh.local_ny = mesh.global_ny + 2 * mesh.pad;
  mesh.local_nz = mesh.global_nz + 2 * mesh.pad;
  mesh.width = get_double_parameter("width", ARCH_ROOT_PARAMS);
  mesh.height = get_double_parameter("height", ARCH_ROOT_PARAMS);
  mesh.depth = get_double_parameter("depth", ARCH_ROOT_PARAMS);
  mesh.dt = get_double_parameter("dt", hale_params);
  mesh.dt_h = mesh.dt;
  mesh.rank = MASTER;
  mesh.nranks = 1;

  double i0 = omp_get_wtime();

  // Perform initialisation routines
  initialise_mpi(argc, argv, &mesh.rank, &mesh.nranks);
  initialise_comms(&mesh);
  initialise_devices(mesh.rank);
  initialise_mesh_3d(&mesh);

  size_t allocated = 0;

  // Fetch the size of the unstructured mesh
  HaleData hale_data;
  UnstructuredMesh umesh;
  SharedData shared_data;
  initialise_shared_data_3d(mesh.local_nx, mesh.local_ny, mesh.local_nz,
                            mesh.pad, mesh.width, mesh.height, mesh.depth,
                            hale_params, mesh.edgex, mesh.edgey, mesh.edgez,
                            &shared_data);

  allocated += convert_mesh_to_umesh_3d(&umesh, &mesh);
  hale_data.density0 = shared_data.density;
  hale_data.energy0 = shared_data.energy;
  hale_data.reduce_array = shared_data.reduce_array0;

  // Initialise the hale-specific data arrays
  hale_data.visc_coeff1 = get_double_parameter("visc_coeff1", hale_params);
  hale_data.visc_coeff2 = get_double_parameter("visc_coeff2", hale_params);
  hale_data.perform_remap = get_int_parameter("perform_remap", hale_params);
  hale_data.visit_dump = get_int_parameter("visit_dump", hale_params);
  allocated += init_hale_data(&hale_data, &umesh);

  printf("Initialisation time %.4lfs\n", omp_get_wtime() - i0);
  printf("Allocated %.3fGB bytes of data\n", allocated / (double)GB);

  int nthreads = 0;
#pragma omp parallel
  { nthreads = omp_get_num_threads(); }

  if (mesh.rank == MASTER) {
    printf("Number of ranks: %d\n", mesh.nranks);
    printf("Number of threads: %d\n", nthreads);
  }

  // Prepare for solve
  double wallclock = 0.0;
  double elapsed_sim_time = 0.0;

  // Main timestep loop
  int tt;
  for (tt = 0; tt < mesh.niters; ++tt) {

    if (mesh.rank == MASTER) {
      printf("\nIteration %d\n", tt + 1);
    }

    double w0 = omp_get_wtime();

    // Solve a single timestep on the given mesh
    solve_unstructured_hydro_3d(&mesh, &hale_data, &umesh, tt);

    wallclock += omp_get_wtime() - w0;
    elapsed_sim_time += mesh.dt;
    if (elapsed_sim_time >= mesh.sim_end) {
      if (mesh.rank == MASTER) {
        printf("reached end of simulation time\n");
      }
      break;
    }

    if (mesh.rank == MASTER) {
      printf("simulation time: %.4lfs\nwallclock: %.4lfs\n", elapsed_sim_time,
             wallclock);
    }
  }

  barrier();

  validate(umesh.ncells, hale_params, mesh.rank, hale_data.density0,
           hale_data.energy0);

  if (mesh.rank == MASTER) {
    PRINT_PROFILING_RESULTS(&compute_profile);
    PRINT_PROFILING_RESULTS(&comms_profile);
    printf("Wallclock %.4fs, Elapsed Simulation Time %.4fs\n", wallclock,
           elapsed_sim_time);
  }

  finalise_mesh(&mesh);

  return 0;
}

// Validates the results of the simulation
void validate(const int ncells, const char* params_filename, const int rank,
              double* density, double* energy) {
  double* h_energy;
  double* h_density;
  allocate_host_data(&h_energy, ncells);
  allocate_host_data(&h_density, ncells);
  copy_buffer(ncells, &energy, &h_energy, RECV);
  copy_buffer(ncells, &density, &h_density, RECV);

  double local_density_total = 0.0;
  double local_energy_total = 0.0;

#pragma omp parallel for reduction(+ : local_density_total, local_energy_total)
  for (int ii = 0; ii < ncells; ++ii) {
    local_density_total += h_density[ii];
    local_energy_total += h_energy[ii];
  }

  double global_density_total = reduce_all_sum(local_density_total);
  double global_energy_total = reduce_all_sum(local_energy_total);

  if (rank != MASTER) {
    return;
  }

  int nresults = 0;
  char* keys = (char*)malloc(sizeof(char) * MAX_KEYS * (MAX_STR_LEN + 1));
  double* values = (double*)malloc(sizeof(double) * MAX_KEYS);
  if (!get_key_value_parameter(params_filename, HALE_TESTS, keys, values,
                               &nresults)) {
    printf("Warning. Test entry was not found, could NOT validate.\n");
    return;
  }

  double expected_energy;
  double expected_density;
  if (strmatch(&(keys[0]), "energy")) {
    expected_energy = values[0];
    expected_density = values[1];
  } else {
    expected_energy = values[1];
    expected_density = values[0];
  }

  printf("\nExpected energy %.12e, result was %.12e.\n", expected_energy,
         global_energy_total);
  printf("Expected density %.12e, result was %.12e.\n", expected_density,
         global_density_total);

  const int pass = within_tolerance(expected_energy, global_energy_total,
                                    VALIDATE_TOLERANCE) &&
                   within_tolerance(expected_density, global_density_total,
                                    VALIDATE_TOLERANCE);

  if (pass) {
    printf("PASSED validation.\n");
  } else {
    printf("FAILED validation.\n");
  }

  free(keys);
  free(values);
  deallocate_host_data(h_energy);
  deallocate_host_data(h_density);
}
