import sys
import os
import time
import glob
import mr_arguments
import mr_commons
import mr_raxml
import mr_bootstraps
import mr_modeltest
import mr_scheduler



def main_raxml_runner(op):
  output_dir = op.output_dir
  if (os.path.exists(output_dir) and not op.do_continue):
    print("[Error] The output directory " + output_dir + " already exists. Please use another output directory.")
    sys.exit(1)
  mr_commons.makedirs(output_dir)
  print("Results in " + output_dir)
  msas = mr_commons.init_msas(op)
  scriptdir = os.path.dirname(os.path.realpath(__file__))
  raxml_library = os.path.join(scriptdir, "..", "raxml-ng", "bin", "raxml-ng-mpi.so")
  modeltest_library = os.path.join(scriptdir, "..", "modeltest", "build", "src", "modeltest-ng-mpi.so")
  parse_commands_file = mr_raxml.build_parse_command(msas, output_dir, op.cores)
  mr_scheduler.run_mpi_scheduler(raxml_library, parse_commands_file, os.path.join(output_dir, "parse_run"), op.cores)
  print("### end of parsing mpi-scheduler run")
  mr_raxml.analyse_parsed_msas(msas, output_dir)
  if (op.use_modeltest):
    modeltest_commands_file = mr_modeltest.build_modeltest_command(msas, output_dir, op.cores)
    mr_scheduler.run_mpi_scheduler(modeltest_library, modeltest_commands_file, os.path.join(output_dir, "modeltest_run"), op.cores)
    print("### end of modeltest mpi-scheduler run")
    mr_modeltest.parse_modeltest_results(op.modeltest_criteria, msas, output_dir)
  mlsearch_commands_file = mr_raxml.build_mlsearch_command(msas, output_dir, op.random_starting_trees, op.parsimony_starting_trees, op.bootstraps, op.cores)
  mr_scheduler.run_mpi_scheduler(raxml_library, mlsearch_commands_file, os.path.join(output_dir, "mlsearch_run"), op.cores)
  if (op.starting_trees > 1):
    mr_raxml.select_best_ml_tree(msas, op)
  print("### end of mlsearch mpi-scheduler run")
  if (op.bootstraps != 0):
    mr_bootstraps.concatenate_bootstraps(output_dir)
    print("### end of bootstraps concatenation")
    supports_commands_file = mr_bootstraps.build_supports_commands(output_dir)
    mr_scheduler.run_mpi_scheduler(raxml_library, supports_commands_file, os.path.join(output_dir, "supports_run"), op.cores)
    print("### end of supports mpi-scheduler run")

print("#################")
print("#  MULTI RAXML  #")
print("#################")
print("Multi-raxml was called as follow:")
print(" ".join(sys.argv))
start = time.time()
main_raxml_runner(mr_arguments.parse_arguments())
end = time.time()
print("TOTAL ELAPSED TIME SPENT IN " + os.path.basename(__file__) + " " + str(end-start) + "s")

