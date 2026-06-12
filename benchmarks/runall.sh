#!/bin/bash
##########################################
# Script for running all experiments in paper except for the htm experiments
##########################################

# print various information about the machine/compiler/date at head of file
procs=`nproc`
today=`date +"%m-%d-%Y"`
machine=`lscpu | grep 'Model name' | rev | cut -c 1-5 | rev`
machine_nowhite=${machine//[[:space:]]/}
filename=../../timings/${procs}-${machine}_${today}

head="numactl -i all"

echo ${filename}
> ${filename}
date >> ${filename}
echo `lscpu | grep 'Model name'` >> ${filename}
echo "Git branch:" `git rev-parse --short HEAD` >> ${filename}
echo "Compiler:" `g++ --version | head -1` >> ${filename}
echo "OS:" `lsb_release -a | grep 'Description' | cut -c 14-` >> ${filename}
echo "Hugepage:" `cat /sys/kernel/mm/transparent_hugepage/enabled` >> ${filename}
echo "NoTurbo:" `cat /sys/devices/system/cpu/intel_pstate/no_turbo` >> ${filename}
echo "MaxPerfPct:" `cat /sys/devices/system/cpu/intel_pstate/max_perf_pct` >> ${filename}
echo "Command Prefix: ${head}"

env_vars="LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2"
export ${env_vars}
echo ${env_vars}

# Experiments start here

llsc_schemes="seqlock wf_indirect wf_direct lf_lowmem"
cas_schemes="simplock std"
all_schemes="${llsc_schemes} ${cas_schemes}"

other_hash="folly_hash folly_sharded libcuckoo seq_hash tbb_hash parallel_hashmap"

all_threads="8,16,32,64,96,192,384,512,768"
all_threads_space="8 16 32 64 96 192 384 512 768"
some_threads="$(nproc) $(( $(nproc) * 4 ))"

default_block_size=4
all_block_sizes="1 2 4 8 16"
some_block_sizes="1 4 16"

default_size=10000000
all_sizes="100,1000,10000,100000,1000000,10000000,100000000"
some_sizes="10000,1000000,100000000"

default_zipfian=.9
all_zipfians="0.0,0.5,0.8,0.9,.95,.99"
some_zipfians="0.0,0.99"

default_update=5
all_updates="0,5,10,20,50,100"
some_updates="5,50"

rounds=1

##########################################
# Figure 1
##########################################
echo -e "\nbigatomic array geomean over schemes and sizes, with and without oversubscription (figure 1)\n" >> ${filename}

for scheme in ${llsc_schemes} ${cas_schemes}
do
    for block_size in ${some_block_sizes}
    do
        for p in ${some_threads}
        do
            command="./bench_hash_array_${block_size}_${scheme} -p ${p} -n ${some_sizes} -z ${some_zipfians} -u ${some_updates} -r ${rounds}"
            echo ${command}
            ${head} ${command} >> ${filename}
            echo "" >> ${filename}
        done
    done
done

##########################################
# Figure 3
##########################################
application= bench_hash_array_${default_block_size}
for p in ${some_threads}
do
    str="\n${application} varying updates (figure 3, second row)\n"
    echo -e ${str}
    echo -e ${str} >> ${filename}
    for scheme in ${all_schemes} 
    do
        ${head} ./${application}_${scheme} -n ${default_size} -u ${all_updates} -z ${default_zipfian} -r ${rounds} -p ${p} >> ${filename}
        echo "" >> ${filename}
    done

    str="\n${application} varying zipfians (figure 3, third row)\n"
    echo -e ${str}
    echo -e ${str} >> ${filename}
    for scheme in ${all_schemes}
    do
        ${head} ./${application}_${scheme} -n ${default_size} -u ${default_update} -z ${all_zipfians} -r ${rounds} -p ${p}>> ${filename}
        echo "" >> ${filename}
    done

    str="\n${application} varying size (figure 3, fourth row)\n" 
    echo -e ${str}
    echo -e ${str} >> ${filename}
    for scheme in ${all_schemes}
    do
        ${head} ./${application}_${scheme} -n ${all_sizes} -u ${default_update} -z ${default_zipfian} -r ${rounds} -p ${p} >> ${filename}
        echo "" >> ${filename}
    done
done

str="\n${application} varying threads (figure 3, top left)\n"
echo -e ${str}
echo -e ${str} >> ${filename}
for scheme in ${all_schemes}
do
    ${head} ./${application}_${scheme} -n ${default_size} -u 5,20 -z ${default_zipfian} -p ${all_threads} -r ${rounds} >> ${filename}
    echo "" >> ${filename}
done

str="\n$hash_array varying k (figure 3, top right)\n"
echo -e ${str}
echo -e ${str} >> ${filename}
for scheme in ${all_schemes} 
do
    for k in ${all_block_sizes}
    do
        ${head} ./bench_hash_array_${k}_${scheme} -n ${default_size} -u ${default_update} -z ${default_zipfian} -r ${rounds} >> ${filename}
        echo "" >> ${filename}
    done
done

##########################################
# Figure 5
##########################################
application=bench_parlay_hash
for p in ${procs}
do
    str="\n${application} varying updates (figure 5, bottom left)\n"
    echo -e ${str}
    echo -e ${str} >> ${filename}
    for scheme in ${llsc_schemes} 
    do
        ${head} ./${application}_${scheme} -n ${default_size} -u ${all_updates} -z ${default_zipfian} -r ${rounds} -p ${p} >> ${filename}
        echo "" >> ${filename}
    done

    str="\n${application} varying zipfians (figure 5, top right)\n"
    echo -e ${str}
    echo -e ${str} >> ${filename}
    for scheme in ${llsc_schemes}
    do
        ${head} ./${application}_${scheme} -n ${default_size} -u ${default_update} -z ${all_zipfians} -r ${rounds} -p ${p}>> ${filename}
        echo "" >> ${filename}
    done

    str="\n${application} varying size (figure 5, bottom left)\n" 
    echo -e ${str}
    echo -e ${str} >> ${filename}
    for scheme in ${llsc_schemes}
    do
        ${head} ./${application}_${scheme} -n ${all_sizes} -u ${default_update} -z ${default_zipfian} -r ${rounds} -p ${p} >> ${filename}
        echo "" >> ${filename}
    done
done

str="\n${application} varying threads (figure 5, top left)\n"
echo -e ${str}
echo -e ${str} >> ${filename}
for scheme in ${llsc_schemes}
do
    ${head} ./${application}_${scheme} -n ${default_size} -u 5,20 -z ${default_zipfian} -p ${all_threads} -r ${rounds} >> ${filename}
    echo "" >> ${filename}
done

##########################################
# Figure 4 (other data also taken from figure 1)
##########################################
application=bench_hash_array_4_wf_ablation
str="\n${application} varying updates (figure 4) \n"
echo -e ${str}
echo -e ${str} >> ${filename}
${head} ./${application} -n ${default_size} -u ${all_updates} -z ${default_zipfian} -r ${rounds} >> ${filename}
echo "" >> ${filename}


##########################################
# Figure 2
##########################################
str="\n$other hash tables zipfian (figure 2)\n"
echo -e ${str}
echo -e ${str} >> ${filename}
for table in ${other_hash}
do
    ${head} ./${table} -n ${default_size} -u ${default_update} -z ${all_zipfians} -r ${rounds} >> ${filename}
    echo "" >> ${filename}
done

str="\n$other hash tables threads (z = 0) (figure 2)\n"
echo -e ${str}
echo -e ${str} >> ${filename}
for table in ${other_hash} bench_parlay_hash_lf_llsc bench_parlay_hash_seqlock
do
    ${head} ./${table} -p ${all_threads} -n ${default_size} -u ${default_update} -z 0 -r ${rounds} >> ${filename}
    echo "" >> ${filename}
done

echo "finished at: `date`"
