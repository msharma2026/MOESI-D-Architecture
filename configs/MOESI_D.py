# Copyright (c) 2019 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2006-2007 The Regents of The University of Michigan
# Copyright (c) 2009 Advanced Micro Devices, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import math
import os
from m5.util import fatal

import m5
from m5.defines import buildEnv
from m5.objects import *

from .Ruby import (
    create_directories,
    create_topology,
    send_evicts,
)


#
# Declare caches used by the protocol
#
class L1Cache(RubyCache):
    dataAccessLatency = 1
    tagAccessLatency = 1


class L2Cache(RubyCache):
    dataAccessLatency = 20
    tagAccessLatency = 20


def bounded_env(name, default, minimum=1, maximum=65535):
    value = int(os.environ.get(name, default))
    if not minimum <= value <= maximum:
        raise ValueError(f"{name} must be in [{minimum}, {maximum}]")
    return value


def flag_env(name, default="1"):
    return bool(bounded_env(name, default, 0, 1))


def define_options(parser):
    return


def create_system(
    options, full_system, system, dma_ports, bootmem, ruby_system, cpus
):
    if buildEnv["PROTOCOL"] != "MOESI_D":
        panic(
            "This script requires the MOESI_D protocol to be built."
        )

    # This artifact supports timing-CPU syscall-emulation experiments only.
    # Full-system, device DMA, cache checkpoint warmup, and persistence require
    # separate validation; do not silently advertise those as supported.
    if full_system or dma_ports or bootmem:
        raise ValueError("MOESI-D currently supports SE mode without device DMA")
    enabled = flag_env("DSTATE_ENABLED")
    persistence = flag_env("DSTATE_PERSISTENCE")
    force_nack = flag_env("DSTATE_FORCE_NACK", "0")
    buffer_size = bounded_env("DSTATE_BUFFER_SIZE", "32", 4, 4096)
    num_tbes = bounded_env("DSTATE_TBES", "16", 2, 4096)
    hot_threshold = bounded_env("DSTATE_THRESHOLD", "4")
    read_threshold = bounded_env("DSTATE_READ_DOWNGRADE", "3")
    exec_latency = bounded_env("DSTATE_EXEC_LATENCY", "42")
    # Bank executor ablation knobs. Defaults reproduce the conservative model:
    # one accepted op per bank, non-pipelined, same-line requests rejected.
    queue_depth = bounded_env("DSTATE_QUEUE_DEPTH", "1", 1, 64)
    # 0 selects "equal to the service latency", i.e. no pipelining.
    init_interval = bounded_env("DSTATE_INIT_INTERVAL", "0", 0, 65535) or exec_latency
    busy_stall = flag_env("DSTATE_BUSY_STALL", "0")
    # Back-pressure instead of NACK when the bank executor is full.
    queue_stall = flag_env("DSTATE_QUEUE_STALL", "0")
    # Far reads: loads of a retained line get a snapshot, no sharer is recorded.
    far_reads = flag_env("DSTATE_FAR_READS", "0")
    # Evict a victim for a delegated request to a line the home does not hold.
    evict_for_delegate = flag_env("DSTATE_EVICT_FOR_DELEGATE", "0")
    # Static placement (oracle runs): delegate only VIRTUAL addresses in [lo, hi) MB.
    range_lo = bounded_env("DSTATE_RANGE_LO_MB", "0", 0, 1 << 30)
    range_hi = bounded_env("DSTATE_RANGE_HI_MB", "0", 0, 1 << 30)
    # Per-core outstanding Ruby requests (gem5 default 16); applies to every mode.
    max_outstanding = bounded_env("DSTATE_MAX_OUTSTANDING", "16", 1, 1024)
    # Requester-side combining: queued same-word adds per issued request (1 = off).
    req_combine = bounded_env("DSTATE_REQ_COMBINE", "1", 1, 64)
    # Delta-line form once a combined request spans this many distinct words (0 = off).
    delta_min_words = bounded_env("DSTATE_DELTA_MIN_WORDS", "0", 0, 32)
    # Service-model and combining knobs (all default to the shipped behaviour).
    # 0 hit latency selects "equal to the service latency" (no hot-word benefit).
    hit_latency = bounded_env("DSTATE_HIT_LATENCY", "0", 0, 65535) or exec_latency
    hotwords = bounded_env("DSTATE_HOTWORDS", "0", 0, 1024)
    merge_limit = bounded_env("DSTATE_MERGE_LIMIT", "0", 0, 64)
    delegate_from_s = flag_env("DSTATE_DELEGATE_FROM_S", "0")
    # Knobs removed in round 8 (superseded or measured not to earn their cost); a
    # run that still sets one must not silently measure something else.
    for removed in ("DSTATE_MIN_WRITERS", "DSTATE_PROMOTE_WRITERS", "DSTATE_ADAPTIVE_GATE",
                    "DSTATE_MAX_TENURE", "DSTATE_TENURE_IDLE", "DSTATE_TENURE_PROBE"):
        if os.environ.get(removed) not in (None, "", "0"):
            fatal("%s was removed in round 8 (see docs/CHANGES.md; promotion is now "
                  "DSTATE_PROMOTE_CHANGES)", removed)
    # O3 only: no-return atomics bypass TSO's one-store-in-flight rule (Intel
    # RAO-INT's weakly ordered contract). Software fences where it publishes.
    relaxed_amo = flag_env("DSTATE_RELAXED_AMO", "0")
    # TSO same-line batching of no-return adds (O3 only; legal under TSO).
    tso_sameline = flag_env("DSTATE_TSO_SAMELINE", "0")
    # Admission gate on distinct writers, its epoch, promotion on writers, ACK value.
    writer_epoch = bounded_env("DSTATE_WRITER_EPOCH", "64", 1, 1 << 20)
    ack_value = flag_env("DSTATE_ACK_VALUE", "0")
    # Change-rate gate, its floor, time-based decay of the gate table, adaptation.
    min_change_pct = bounded_env("DSTATE_MIN_CHANGE_PCT", "0", 0, 100)
    min_changes = bounded_env("DSTATE_MIN_CHANGES", "2", 1, 1 << 20)
    writer_idle = bounded_env("DSTATE_WRITER_IDLE", "0", 0, 1 << 30)
    gate_table = bounded_env("DSTATE_GATE_TABLE", "1024", 1, 1 << 16)
    reject_as_getx = flag_env("DSTATE_REJECT_AS_GETX", "0")
    promote_changes = bounded_env("DSTATE_PROMOTE_CHANGES", "0", 0, 1 << 20)
    for cpu in cpus:
        try:
            cpu.relaxedNoReturnAtomics = relaxed_amo
            cpu.tsoSameLineAtomics = tso_sameline
        except AttributeError:
            if relaxed_amo:
                raise ValueError("DSTATE_RELAXED_AMO requires X86O3CPU")
            if tso_sameline:
                raise ValueError("DSTATE_TSO_SAMELINE requires X86O3CPU")

    cpu_sequencers = []

    #
    # The ruby network creation expects the list of nodes in the system to be
    # consistent with the NetDest list.  Therefore the l1 controller nodes must be
    # listed before the directory nodes and directory nodes before dma nodes, etc.
    #
    l1_cntrl_nodes = []
    l2_cntrl_nodes = []
    dma_cntrl_nodes = []

    #
    # Must create the individual controllers before the network to ensure the
    # controller constructors are called before the network constructor
    #
    block_size_bits = int(math.log(options.cacheline_size, 2))

    for i in range(options.num_cpus):
        #
        # First create the Ruby objects associated with this cpu
        #
        l1i_cache = L1Cache(
            size=options.l1i_size,
            assoc=options.l1i_assoc,
            start_index_bit=block_size_bits,
            is_icache=True,
        )
        l1d_cache = L1Cache(
            size=options.l1d_size,
            assoc=options.l1d_assoc,
            start_index_bit=block_size_bits,
            is_icache=False,
        )

        clk_domain = cpus[i].clk_domain

        l1_cntrl = MOESI_D_L1Cache_Controller(
            version=i,
            L1Icache=l1i_cache,
            L1Dcache=l1d_cache,
            send_evictions=send_evicts(options),
            d_state_enabled=enabled,
            d_state_delegate_from_s=delegate_from_s,
            number_of_TBEs=num_tbes,
            transitions_per_cycle=options.ports,
            clk_domain=clk_domain,
            ruby_system=ruby_system,
        )

        cpu_seq = RubySequencer(
            dstate_request_combine=req_combine,
            dstate_delta_min_words=delta_min_words,
            dstate_range_lo_mb=range_lo,
            dstate_range_hi_mb=range_hi,
            max_outstanding_requests=max_outstanding,
            version=i,
            dcache=l1d_cache,
            clk_domain=clk_domain,
            ruby_system=ruby_system,
        )

        l1_cntrl.sequencer = cpu_seq
        exec("ruby_system.l1_cntrl%d = l1_cntrl" % i)

        # Add controllers and sequencers to the appropriate lists
        cpu_sequencers.append(cpu_seq)
        l1_cntrl_nodes.append(l1_cntrl)

        # Connect the L1 controllers and the network
        l1_cntrl.mandatoryQueue = MessageBuffer(buffer_size=buffer_size)
        l1_cntrl.requestFromL1Cache = MessageBuffer(buffer_size=buffer_size)
        l1_cntrl.requestFromL1Cache.out_port = ruby_system.network.in_port
        l1_cntrl.responseFromL1Cache = MessageBuffer(buffer_size=buffer_size)
        l1_cntrl.responseFromL1Cache.out_port = ruby_system.network.in_port
        l1_cntrl.requestToL1Cache = MessageBuffer(buffer_size=buffer_size)
        l1_cntrl.requestToL1Cache.in_port = ruby_system.network.out_port
        l1_cntrl.responseToL1Cache = MessageBuffer(buffer_size=buffer_size)
        l1_cntrl.responseToL1Cache.in_port = ruby_system.network.out_port
        l1_cntrl.triggerQueue = MessageBuffer(ordered=True, buffer_size=buffer_size)

    # Create the L2s interleaved addr ranges
    l2_addr_ranges = []
    l2_bits = int(math.log(options.num_l2caches, 2))
    numa_bit = block_size_bits + l2_bits - 1
    sysranges = [] + system.mem_ranges
    if bootmem:
        sysranges.append(bootmem.range)
    for i in range(options.num_l2caches):
        ranges = []
        for r in sysranges:
            addr_range = AddrRange(
                r.start,
                size=r.size(),
                intlvHighBit=numa_bit,
                intlvBits=l2_bits,
                intlvMatch=i,
            )
            ranges.append(addr_range)
        l2_addr_ranges.append(ranges)

    for i in range(options.num_l2caches):
        #
        # First create the Ruby objects associated with this cpu
        #
        l2_cache = L2Cache(
            size=options.l2_size,
            assoc=options.l2_assoc,
            start_index_bit=block_size_bits + l2_bits,
        )

        l2_cntrl = MOESI_D_L2Cache_Controller(
            version=i,
            L2cache=l2_cache,
            transitions_per_cycle=options.ports,
            ruby_system=ruby_system,
            addr_ranges=l2_addr_ranges[i],
            d_state_enabled=enabled,
            d_state_persistence=persistence,
            d_state_force_nack=force_nack,
            d_state_hot_threshold=hot_threshold,
            d_state_read_downgrade_threshold=read_threshold,
            d_state_exec_latency=exec_latency,
            d_state_queue_depth=queue_depth,
            d_state_init_interval=init_interval,
            d_state_busy_stall=busy_stall,
            d_state_queue_stall=queue_stall,
            d_state_far_reads=far_reads,
            d_state_evict_for_delegate=evict_for_delegate,
            d_state_writer_epoch=writer_epoch,
            d_state_ack_value=ack_value,
            d_state_min_change_pct=min_change_pct,
            d_state_min_changes=min_changes,
            d_state_writer_idle=writer_idle,
            d_state_gate_table=gate_table,
            d_state_reject_as_getx=reject_as_getx,
            d_state_promote_changes=promote_changes,
            d_state_hit_latency=hit_latency,
            d_state_hotwords=hotwords,
            d_state_merge_limit=merge_limit,
            number_of_TBEs=num_tbes,
        )

        exec("ruby_system.l2_cntrl%d = l2_cntrl" % i)
        l2_cntrl_nodes.append(l2_cntrl)

        # Connect the L2 controllers and the network
        l2_cntrl.GlobalRequestFromL2Cache = MessageBuffer(buffer_size=buffer_size)
        l2_cntrl.GlobalRequestFromL2Cache.out_port = (
            ruby_system.network.in_port
        )
        l2_cntrl.L1RequestFromL2Cache = MessageBuffer(buffer_size=buffer_size)
        l2_cntrl.L1RequestFromL2Cache.out_port = ruby_system.network.in_port
        l2_cntrl.responseFromL2Cache = MessageBuffer(buffer_size=buffer_size)
        l2_cntrl.responseFromL2Cache.out_port = ruby_system.network.in_port

        l2_cntrl.GlobalRequestToL2Cache = MessageBuffer(buffer_size=buffer_size)
        l2_cntrl.GlobalRequestToL2Cache.in_port = ruby_system.network.out_port
        l2_cntrl.L1RequestToL2Cache = MessageBuffer(buffer_size=buffer_size)
        l2_cntrl.L1RequestToL2Cache.in_port = ruby_system.network.out_port
        l2_cntrl.responseToL2Cache = MessageBuffer(buffer_size=buffer_size)
        l2_cntrl.responseToL2Cache.in_port = ruby_system.network.out_port
        l2_cntrl.triggerQueue = MessageBuffer(ordered=True, buffer_size=buffer_size)

    # Run each of the ruby memory controllers at a ratio of the frequency of
    # the ruby system.
    # clk_divider value is a fix to pass regression.
    ruby_system.memctrl_clk_domain = DerivedClockDomain(
        clk_domain=ruby_system.clk_domain, clk_divider=3
    )

    mem_dir_cntrl_nodes, rom_dir_cntrl_node = create_directories(
        options, bootmem, ruby_system, system
    )
    dir_cntrl_nodes = mem_dir_cntrl_nodes[:]
    if rom_dir_cntrl_node is not None:
        dir_cntrl_nodes.append(rom_dir_cntrl_node)
    for dir_cntrl in dir_cntrl_nodes:
        # Connect the directory controllers and the network
        dir_cntrl.requestToDir = MessageBuffer(buffer_size=buffer_size)
        dir_cntrl.requestToDir.in_port = ruby_system.network.out_port
        dir_cntrl.responseToDir = MessageBuffer(buffer_size=buffer_size)
        dir_cntrl.responseToDir.in_port = ruby_system.network.out_port
        dir_cntrl.responseFromDir = MessageBuffer(buffer_size=buffer_size)
        dir_cntrl.responseFromDir.out_port = ruby_system.network.in_port
        dir_cntrl.forwardFromDir = MessageBuffer(buffer_size=buffer_size)
        dir_cntrl.forwardFromDir.out_port = ruby_system.network.in_port
        dir_cntrl.requestToMemory = MessageBuffer(buffer_size=buffer_size)
        dir_cntrl.responseFromMemory = MessageBuffer(buffer_size=buffer_size)
        dir_cntrl.triggerQueue = MessageBuffer(ordered=True, buffer_size=buffer_size)

    for i, dma_port in enumerate(dma_ports):
        #
        # Create the Ruby objects associated with the dma controller
        #
        dma_seq = DMASequencer(
            version=i, ruby_system=ruby_system, in_ports=dma_port
        )

        dma_cntrl = MOESI_D_DMA_Controller(
            version=i,
            dma_sequencer=dma_seq,
            transitions_per_cycle=options.ports,
            ruby_system=ruby_system,
        )

        exec("ruby_system.dma_cntrl%d = dma_cntrl" % i)
        dma_cntrl_nodes.append(dma_cntrl)

        # Connect the dma controller to the network
        dma_cntrl.mandatoryQueue = MessageBuffer(buffer_size=buffer_size)
        dma_cntrl.responseFromDir = MessageBuffer(buffer_size=buffer_size)
        dma_cntrl.responseFromDir.in_port = ruby_system.network.out_port
        dma_cntrl.reqToDir = MessageBuffer(buffer_size=buffer_size)
        dma_cntrl.reqToDir.out_port = ruby_system.network.in_port
        dma_cntrl.respToDir = MessageBuffer(buffer_size=buffer_size)
        dma_cntrl.respToDir.out_port = ruby_system.network.in_port
        dma_cntrl.triggerQueue = MessageBuffer(ordered=True, buffer_size=buffer_size)

    all_cntrls = (
        l1_cntrl_nodes + l2_cntrl_nodes + dir_cntrl_nodes + dma_cntrl_nodes
    )

    # Create the io controller and the sequencer
    if full_system:
        io_seq = DMASequencer(version=len(dma_ports), ruby_system=ruby_system)
        ruby_system._io_port = io_seq
        io_controller = MOESI_D_DMA_Controller(
            version=len(dma_ports),
            dma_sequencer=io_seq,
            ruby_system=ruby_system,
        )
        ruby_system.io_controller = io_controller

        # Connect the dma controller to the network
        io_controller.mandatoryQueue = MessageBuffer(buffer_size=buffer_size)
        io_controller.responseFromDir = MessageBuffer(buffer_size=buffer_size)
        io_controller.responseFromDir.in_port = ruby_system.network.out_port
        io_controller.reqToDir = MessageBuffer(buffer_size=buffer_size)
        io_controller.reqToDir.out_port = ruby_system.network.in_port
        io_controller.respToDir = MessageBuffer(buffer_size=buffer_size)
        io_controller.respToDir.out_port = ruby_system.network.in_port
        io_controller.triggerQueue = MessageBuffer(ordered=True, buffer_size=buffer_size)

        all_cntrls = all_cntrls + [io_controller]

    ruby_system.network.number_of_virtual_networks = 3
    topology = create_topology(all_cntrls, options)
    return (cpu_sequencers, mem_dir_cntrl_nodes, topology)
