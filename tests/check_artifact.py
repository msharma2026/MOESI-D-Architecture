#!/usr/bin/env python3
"""Fast structural regression guards, NOT a coherence proof or simulation."""
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from export_patch import canonical_files


class ArtifactChecks(unittest.TestCase):
    def test_complete_patch_contains_every_canonical_file(self):
        patch = (ROOT / "gem5-moesi-d.patch").read_text()
        for source, dest in canonical_files():
            marker = f"diff --git a/{dest} b/{dest}\n"
            self.assertIn(marker, patch, dest)
            section = patch.split(marker, 1)[1].split("diff --git ", 1)[0]
            # Mirrored files are additions against the pinned gem5 release.
            self.assertIn("new file mode", section, dest)
            content = "\n".join(line[1:] for line in section.splitlines()
                                if line.startswith("+") and not line.startswith("+++")) + "\n"
            self.assertEqual(content, source.read_text(), str(source))

    def test_no_replacement_of_stock_locked_add(self):
        patch = (ROOT / "isa/stula-x86-isa.patch").read_text()
        removed = [line for line in patch.splitlines() if line.startswith("-") and not line.startswith("---")]
        self.assertFalse(any("macroop ADD_LOCKED" in line or "flags=(" in line for line in removed))
        self.assertIn("Inst::AADD(Mv, Gv)", patch)
        self.assertIn("dataSize != 4 && dataSize != 8", patch)
        self.assertNotIn("dataSize >= 8", patch)

    def test_terminal_completion_and_nack_contract(self):
        l1 = (ROOT / "protocol/MOESI_D-L1cache.sm").read_text()
        self.assertIn("transition(D_REQ, DState_NACK, D_RETRY)", l1)
        nack = l1.split("transition(D_REQ, DState_NACK, D_RETRY)", 1)[1].split("}", 1)[0]
        self.assertIn("d_scheduleFallback", nack)
        self.assertIn("n_popResponseQueue", nack)
        self.assertNotIn("b_issueDStateFallbackGETX", nack)
        self.assertIn("transition(D_RETRY, DState_Retry, IM_AMO)", l1)
        self.assertNotIn("deallocate", nack)
        issue = l1.split("transition(I, DStateReq_CPU, D_REQ)", 1)[1].split("}", 1)[0]
        self.assertNotIn("complete", issue.lower())
        self.assertIn("sequencer.atomicRemoteCallback(address)", l1)
        l2 = (ROOT / "protocol/MOESI_D-L2cache.sm").read_text()
        self.assertIn("d_apply; d_ack; d_finish", l2)
        self.assertIn("cache_entry.Sharers.count() == 0", l2)
        self.assertIn("d_state_force_nack", l2)
        self.assertIn("transition(DX_DRAIN, All_Acks, D_EXEC_D)", l2)

    def test_protocol_macro_reaches_the_cxx_build(self):
        # gem5's Kconfig build defines no generic PROTOCOL_<name> macro (only PROTOCOL_CHI), so
        # every `#if defined(PROTOCOL_MOESI_D)` in C++ silently compiles out unless the Ruby
        # SConscript adds the define. The first corrected build shipped without it: the
        # Sequencer atomic branch was absent from the binary, every AADD became an ordinary
        # store, and all oracles still passed. Guard both halves.
        ruby = (ROOT / "protocol/ruby-integration.patch").read_text()
        self.assertIn("CPPDEFINES=['PROTOCOL_MOESI_D']", ruby)
        self.assertIn("RUBY_PROTOCOL_MOESI_D", ruby)
        guards = ruby.count("defined(PROTOCOL_MOESI_D)")
        self.assertGreaterEqual(guards, 1)

    def test_store_class_micro_op_has_forwarding_guard(self):
        # The no-return add is a store-class micro-op (retires at commit like a
        # store) whose Request still carries ATOMIC_NO_RETURN_OP. The O3 LSQ
        # forwards a store's data to a younger same-address load only when the
        # instruction is not atomic; without the request-flag guard a load would
        # receive the add's delta. Both halves must ship together.
        isa = (ROOT / "isa/stula-x86-isa.patch").read_text()
        self.assertIn("['IsStore', 'IsInteger']", isa)
        self.assertNotIn("+                            ['IsAtomic', 'IsInteger'])", isa)
        cpu = (ROOT / "protocol/ruby-integration.patch").read_text()
        self.assertIn("src/cpu/o3/lsq_unit.cc", cpu)
        # the request-flag guard is computed once and applied to both forwarding checks
        self.assertIn("store_it->request()->mainReq()->isAtomic()", cpu)
        self.assertGreaterEqual(cpu.count("store_is_atomic"), 3)
        self.assertIn("relaxedNoReturnAtomics", cpu)
        self.assertIn("isAtomicNoReturn()", cpu)

    def test_no_ghost_directory_d_or_unsupported_operators(self):
        directory = (ROOT / "protocol/MOESI_D-dir.sm").read_text()
        self.assertNotIn("DSTATE", directory)
        msg = (ROOT / "protocol/MOESI_D-msg.sm").read_text()
        self.assertNotIn("PROMOTE_TO_D", msg)
        self.assertNotIn("IMIN", msg)
        self.assertNotIn("IMAX", msg)

    def test_bounded_buffers_and_same_isa_ablation(self):
        config = (ROOT / "configs/MOESI_D.py").read_text()
        self.assertNotIn("MessageBuffer()", config)
        for knob in ("DSTATE_ENABLED", "DSTATE_PERSISTENCE", "DSTATE_FORCE_NACK", "DSTATE_EXEC_LATENCY"):
            self.assertIn(knob, config)
        self.assertIn("transition(I, LocalAtomic_CPU, IM_AMO)",
                      (ROOT / "protocol/MOESI_D-L1cache.sm").read_text())


    def test_requester_combining_and_back_pressure_guards(self):
        """Round 3: the Sequencer combines only a consecutive run of same-word
        no-return adds, forgets the group when the L1 applies locally, and the
        L2 parks (never drops) a request that finds the executor full."""
        seq = (ROOT / "protocol/ruby-integration.patch").read_text(encoding="utf-8")
        for needle in ("Sequencer::issueFront",
                       "it->pkt->getAddr() != addr",
                       "it->pkt->getSize() != size",
                       "m_dstateCombined.erase(address);",
                       "dstate_request_combine"):
            self.assertIn(needle, seq)
        l2 = (ROOT / "protocol/MOESI_D-L2cache.sm").read_text(encoding="utf-8")
        self.assertIn("DState_Full", l2)
        self.assertIn("transition({I, M, D, ILX, ILOX, OLSX}, DState_Full)", l2)
        self.assertIn("void wakeUpAllBuffers();", l2)
        # both executor-release actions wake the parked requests
        self.assertEqual(l2.count("      wakeUpAllBuffers();"), 2)


if __name__ == "__main__":
    unittest.main()
