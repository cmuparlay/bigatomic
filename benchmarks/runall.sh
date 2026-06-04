#!/bin/bash

# print various information about the machine/compiler/date at head of file
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
echo "NoTurbo:" `cat /sys/devices/system/cpu/intel_pstate/no_turbo` >> ${filename}
echo "MaxPerfPct" `cat /sys/devices/system/cpu/intel_pstate/max_perf_pct` >> ${filename}

# Experiments start here

head="numactl -i all"

llsc_schemes="seqlock wf_indirect wf_daniel lf_llsc"
cas_schemes="simplock std"

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

echo -e "\nbigatomic array geomean over schemes and sizes, with and without oversubscription\n" >> ${filename}

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

scheme=wf_ablation
block_size=4
command="./bench_hash_array_${block_size}_${scheme} -n ${some_sizes} -z ${some_zipfians} -u ${some_updates} -r ${rounds}"
echo ${command}
${head} ${command} >> ${filename}
echo "" >> ${filename}

for application in bench_parlay_hash
do
    echo -e "\n${application} geomean over schemes\n" >> ${filename}
    for scheme in ${llsc_schemes}
    do
        for p in ${some_threads}
        do
            command="./${application}_${scheme} -p ${p} -n ${some_sizes} -z ${some_zipfians} -u ${some_updates} -r ${rounds}"
            echo ${command}
            ${head} ${command} >> ${filename}
            echo "" >> ${filename}
        done
    done
done

application=bench_hash_array
for p in ${some_threads}
do
    str="\n${application} varying block size\n" 
    for block_size in ${some_block_sizes}
    do
        echo -e ${str}
        echo -e ${str} >> ${filename}
        for scheme in ${llsc_schemes} ${cas_schemes}
        do
            ${head} ./${application}_${block_size}_${scheme} -n ${default_size} -u ${default_update} -z ${default_zipfian} -r ${rounds} -p ${p} >> ${filename}
            echo "" >> ${filename}
        done
    done
done

for application in bench_hash_array_${default_block_size} bench_parlay_hash
do
    for p in ${some_threads}
    do
        str="\n${application} varying size\n" 
        echo -e ${str}
        echo -e ${str} >> ${filename}
        for scheme in ${all_schemes}
        do
            ${head} ./${application}_${scheme} -n ${all_sizes} -u ${default_update} -z ${default_zipfian} -r ${rounds} -p ${p} >> ${filename}
            echo "" >> ${filename}
        done

        str="\n${application} varying updates\n"
        echo -e ${str}
        echo -e ${str} >> ${filename}
        for scheme in ${all_schemes} 
        do
            ${head} ./${application}_${scheme} -n ${default_size} -u ${all_updates} -z ${default_zipfian} -r ${rounds} -p ${p} >> ${filename}
            echo "" >> ${filename}
        done

        str="\n${application} varying zipfians\n"
        echo -e ${str}
        echo -e ${str} >> ${filename}
        for scheme in ${all_schemes}
        do
            ${head} ./${application}_${scheme} -n ${default_size} -u ${default_update} -z ${all_zipfians} -r ${rounds} -p ${p}>> ${filename}
            echo "" >> ${filename}
        done
    done

    str="\n${application} varying threads\n"
    echo -e ${str}
    echo -e ${str} >> ${filename}
    for scheme in ${all_schemes}
    do
        ${head} ./${application}_${scheme} -n ${default_size} -u 5,20 -z ${default_zipfian} -p ${all_threads} -r ${rounds} >> ${filename}
        echo "" >> ${filename}
    done
done

application=bench_hash_array_4_wf_ablation
str="\n${application} varying updates\n"
echo -e ${str}
echo -e ${str} >> ${filename}
${head} ./${application} -n ${default_size} -u ${all_updates} -z ${default_zipfian} -r ${rounds} >> ${filename}
echo "" >> ${filename}

str="\nvarying threads for simplock and std\n"
echo -e ${str}
echo -e ${str} >> ${filename}
for scheme in ${cas_schemes}
do
    ${head} ./bench_hash_array_${default_block_size} -n ${default_size} -u 20 -z ${default_zipfian} -p ${threads} -r ${rounds} >> ${filename}
    echo "" >> ${filename}
done

str="\n$other hash tables geomean\n"
for table in ${other_hash}
do
    for p in ${some_threads}
    do
        command="./${table} -p ${p} -n ${some_sizes} -z ${some_zipfians} -u ${some_updates} -r ${rounds}"
        echo ${command}
        ${head} ${command} >> ${filename}
        echo "" >> ${filename}
    done
done

str="\n$other hash tables zipfian\n"
echo -e ${str}
echo -e ${str} >> ${filename}
for table in ${other_hash}
do
    ${head} ./${table} -n ${default_size} -u ${default_update} -z ${all_zipfians} -r ${rounds} >> ${filename}
    echo "" >> ${filename}
done

str="\n$other hash tables threads (z = ${default_zipfian})\n"
echo -e ${str}
echo -e ${str} >> ${filename}
for table in ${other_hash}
do
    ${head} ./${table} -p ${all_threads} -n ${default_size} -u ${default_update} -z ${default_zipfian} -r ${rounds} >> ${filename}
    echo "" >> ${filename}
done

str="\n$other hash tables threads (z = 0)\n"
echo -e ${str}
echo -e ${str} >> ${filename}
for table in ${other_hash} bench_parlay_hash_lf_llsc bench_parlay_hash_seqlock
do
    ${head} ./${table} -p ${all_threads} -n ${default_size} -u ${default_update} -z 0 -r ${rounds} >> ${filename}
    echo "" >> ${filename}
done
