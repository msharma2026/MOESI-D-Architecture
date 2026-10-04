#!/bin/bash
# CACTI 7 estimates (22 nm, ITRS-HP, 350 K) for the structures MOESI-D adds to an L2 bank,
# plus the bank's own 256 KB data array for scale. Output: TSV on stdout.
cd /home/ubuntu/cacti || exit 1
OUT=/home/ubuntu/cacti-moesi-d; mkdir -p $OUT
mk() { # name size block assoc type buswidth
  local name=$1 size=$2 block=$3 assoc=$4 type=$5 bus=$6
  sed -e "s/^-size (bytes) .*/-size (bytes) $size/" \
      -e "s/^-block size (bytes) .*/-block size (bytes) $block/" \
      -e "s/^-associativity .*/-associativity $assoc/" \
      -e "s/^-technology (u) .*/-technology (u) 0.022/" \
      -e "s/^-cache type .*/-cache type \"$type\"/" \
      -e "s|^-output/input bus width .*|-output/input bus width $bus|" \
      -e "s/^-Add ECC .*/-Add ECC - \"false\"/" \
      -e "s/^-Cache level .*/-Cache level (L2\/L3) - \"L2\"/" \
      -e "s/^-Core count .*/-Core count 4/" \
      -e "s/^-Print level .*/-Print level (DETAILED, CONCISE) - \"DETAILED\"/" \
      cache.cfg > $OUT/$name.cfg
  ./cacti -infile $OUT/$name.cfg > $OUT/$name.out 2>&1
  python3 - $name $OUT/$name.out <<'PY'
import re,sys
name,path=sys.argv[1],sys.argv[2]; t=open(path,errors='replace').read()
def g(pat):
    m=re.search(pat,t); return m.group(1) if m else 'n/a'
m=re.search(r'Cache height x width \(mm\):\s*([0-9.eE+-]+)\s*x\s*([0-9.eE+-]+)',t)
area=f"{float(m.group(1))*float(m.group(2)):.5f}" if m else g(r'Total area \(mm\^2\):\s*([0-9.eE+-]+)')
acc=g(r'Access time \(ns\):\s*([0-9.eE+-]+)')
rd=g(r'Total dynamic read energy per access \(nJ\):\s*([0-9.eE+-]+)') if 'Total dynamic read energy' in t else g(r'Dynamic read energy \(nJ\):\s*([0-9.eE+-]+)')
wr=g(r'Total dynamic write energy per access \(nJ\):\s*([0-9.eE+-]+)') if 'Total dynamic write energy' in t else g(r'Dynamic write energy \(nJ\):\s*([0-9.eE+-]+)')
lk=g(r'Total leakage power of a bank \(mW\):\s*([0-9.eE+-]+)') if 'Total leakage power of a bank' in t else g(r'Standby leakage per bank\(mW\):\s*([0-9.eE+-]+)')
err='ERROR' if ('ERROR' in t or 'error' in t.lower() and area=='n/a') else ''
print(f"{name}\t{area}\t{acc}\t{rd}\t{wr}\t{lk}\t{err}")
PY
}
echo -e "structure\tarea_mm2\taccess_ns\tread_nJ\twrite_nJ\tleak_mW\tnote"
mk admission_table_256x16B   4096   16 4  cache 128
mk admission_table_1024x16B  16384  16 4  cache 128
mk executor_queue_16x128B_4k 4096   128 1  ram  1024
mk hotword_buffer_2KB_16B_4w  2048   16 4  cache 128
mk merge_record_64x16B       1024   16 1  ram  128
mk l2_bank_256KB_128B_8way   262144 128 8 cache 1024
mk l1d_32KB_64B_8way         32768  64 8  cache 512
