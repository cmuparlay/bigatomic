#!/bin/bash

head="numactl -i all"

# print various imformation about the machine/compiler/date at head of file
procs=`nproc`
today=`date +"%m-%d-%Y"`
machine=`lscpu | grep 'Model name' | rev | cut -c 1-5 | rev`
machine_nowhite=${machine//[[:space:]]/}
filename=../../timings/${procs}-${machine}_${today}

echo ${filename}
> ${filename}
echo "Date:" ${today} >> ${filename}
echo `lscpu | grep 'Model name'` >> ${filename}
echo "Git branch:" `git rev-parse --short HEAD` >> ${filename}
echo "Compiler:" `g++ --version | head -1` >> ${filename}
echo "OS:" `lsb_release -a | grep 'Description' | cut -c 14-` >> ${filename}
echo "Hugepage:" `cat /sys/kernel/mm/transparent_hugepage/enabled` >> ${filename}

# Experiments start here

schemes="seqlock seqlocklock wf_indirect wf_hazard wf_daniel lf_lowmem"
slow_schemes="simplock std"

threads="8 16 32 64 96 192 384 512"

default_block_size=4
block_sizes="1 4 16"
block_sizes_more="1 2 4 8 16"

default_size=10000000
sizes="1000 10000 100000 1000000 10000000 100000000"

default_zipfian=.9
zipfians="0.0 0.5 0.8 0.9 .95 .99"

default_update=20
updates="0 5 10 20 50 100"

applications="bench_hash_array_${default_block_size} bench_parlay_hash bench_hash_list bench_leaf_tree"

echo -e "\nbigatomic array geomean over schemes and sizes\n" >> ${filename}

for scheme in ${schemes} 
do
    for block_size in ${block_sizes}
    do
        ${head} ./bench_hash_array_${block_size}_${scheme} >> ${filename}
    done
done

for scheme in ${schemes} ${slow_schemes}
do
    for block_size in ${block_sizes_more}
    do
        ${head} ./bench_hash_array_${block_size}_${scheme} -n ${default_size} -u ${default_update} -z ${default_zipfian} >> ${filename}>> ${filename}
    done
done

for scheme in ${schemes} 
do
    for block_size in ${block_sizes}
    do
        ${head} ./bench_hash_array_${block_size}_${scheme} >> ${filename}
    done
done

echo -e "\nparlay_hash geomean over schemes\n" >> ${filename}
for scheme in ${schemes}
do
    ${head} ./bench_parlay_hash_${scheme} >> ${filename}
done

echo -e "\nhash_list geomean over schemes\n" >> ${filename}
for scheme in ${schemes}
do
        ${head} ./bench_hash_list_${scheme} >> ${filename}
done

echo -e "\nleaf_tree geomean over schemes\n" >> ${filename}
for scheme in ${schemes}
do
        ${head} ./bench_leaf_tree_${scheme} >> ${filename}
done

echo -e "\nGo over applications varying various parameters\n" >> ${filename}
for application in ${applications}
do

    echo -e "\n${application} varying size\n" >> ${filename}
    for scheme in ${schemes}
    do
        for size in ${sizes}
        do
            ${head} ./${application}_${scheme} -n ${size} -u ${default_update} -z ${default_zipfian} >> ${filename}
        done
    done

    echo -e "\n${application} varying upsates\n" >> ${filename}
    for scheme in ${schemes}
    do
        for update in ${updates}
        do
            ${head} ./${application}_${scheme} -n ${default_size} -u ${update} -z ${default_zipfian} >> ${filename}
        done
    done

    echo -e "\n${application} varying zipfians\n" >> ${filename}
    for scheme in ${schemes}
    do
        for zipfian in ${zipfians}
        do
            ${head} ./${application}_${scheme} -n ${default_size} -u ${default_update} -z ${zipfian} >> ${filename}
        done
    done

    echo -e "\n${application} varying threads\n" >> ${filename}
    for scheme in ${schemes}
    do
        for thread in ${threads}
        do
            PARLAY_NUM_THREADS=${thread} ${head} ./${application}_${scheme} -n ${default_size} -u ${default_update} -z ${default_zipfian} >> ${filename}
        done
    done
done
