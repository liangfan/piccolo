#!/bin/bash

BUILD_TYPE=release
NUM_CORES=24
RESULTS_DIR=results/
PARALLELISM="6 12 18 23"

#strace -c -f -o results/trace.$n.\$BASHPID 

function run_command() {
  echo "Writing output to: $RESULTS_DIR"
  mkdir -p $RESULTS_DIR
  runner=$1
  
  for n in $PARALLELISM; do 
      pdsh -g muppets -f 100 -l root 'echo 3 > /proc/sys/vm/drop_caches'
      echo > $RESULTS_DIR/$runner.n_$n
      echo "$runner :: $n"
      IFS=$(echo -e '\n')

      if [[ $n != $NUM_CORES ]]; then 
        AFFINITY=1
      else
        AFFINITY=0
      fi

      /usr/bin/time ~/share/bin/mpirun \
         -mca mpi_paffinity_alone $AFFINITY \
         -hostfile mpi_hostfile\
         -byslot \
         -n $((n + 1)) \
	        bash -c "\
	              LD_LIBRARY_PATH=/home/power/share/lib \
	              bin/$BUILD_TYPE/examples/example-dsm \
	        			--runner=$runner \
	        			$2 $3 $4 $5 $6 $7 "
	  
  done
}
