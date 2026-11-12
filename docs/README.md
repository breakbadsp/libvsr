# VSR research

Start with the 2012 paper. Use the other papers to study its assumptions,
history, and production limits. Then read the [study notes](study-notes.md).
These notes contain an initial assessment, not a library design.

## Reading order

1. **Liskov and Cowling, 2012 — Viewstamped Replication Revisited.**
   [Local PDF](papers/vr-revisited-2012.pdf) · [MIT record](https://hdl.handle.net/1721.1/71763).
   Read §§2–4 first. Then study state transfer (§5.2), optimizations (§6),
   reconfiguration (§7), and correctness (§8). This is the main protocol reference.
2. **Schneider, revised January 1990 — Implementing Fault-tolerant Services
   using the State Machine Approach: A Tutorial.**
   [Local PDF](papers/state-machine-tutorial-1990.pdf) · [Cornell source](https://www.cs.cornell.edu/fbs/publications/86-800.pdf).
   Read for the application model, deterministic commands, and external outputs.
   This copy is TR 86-800, revised in 1990, rather than the journal edition.
3. **Oki and Liskov, 1988 — Viewstamped Replication: A New Primary Copy Method
   to Support Highly-Available Distributed Systems.**
   [Local PDF](papers/vr-original-1988.pdf) · [DOI](https://doi.org/10.1145/62546.62549).
   Read for the original viewstamp and transaction context. Keep this version
   separate from the revised protocol.
4. **van Renesse, Schiper, and Schneider, 2014 — Vive la Différence:
   Paxos vs. Viewstamped Replication vs. Zab.**
   [Local PDF](papers/paxos-vr-zab-2014.pdf) · [arXiv v3](https://arxiv.org/abs/1309.5671v3).
   Read after VSR. It uses refinement mappings to explain common structure and
   differences between protocols. This copy is v3; the first submission was in 2013.
5. **Alagappan et al., 2018 — Protocol-Aware Recovery for Consensus-Based Storage.**
   [Local PDF](papers/protocol-aware-recovery-2018.pdf) · [USENIX source](https://www.usenix.org/conference/fast18/presentation/alagappan).
   Read for storage corruption and recovery. This is supporting research,
   not another version of VSR.

## TigerBeetle references

The assessment uses commit `68958eb757747d0fc96a79d1f9c2fa6766b67b48`,
checked on 2026-11-07:

- [VSR internals](https://github.com/tigerbeetle/tigerbeetle/blob/68958eb757747d0fc96a79d1f9c2fa6766b67b48/docs/internals/vsr.md).
- [Deterministic simulation testing](https://github.com/tigerbeetle/tigerbeetle/blob/68958eb757747d0fc96a79d1f9c2fa6766b67b48/docs/internals/vopr.md).

These are implementation documents, not papers. Their details can differ from
the 2012 protocol. The assessment does not establish that TigerBeetle code can
be extracted as an independent library.

## Verify or restore the collection

Run from the repository root. Requirements: Python 3 and Poppler `pdfinfo`.
Restoration also requires `curl` and network access.

```sh
python3 docs/papers/check.py
python3 docs/papers/check.py --fetch
```

The first command verifies local PDFs without network access. The second also
restores missing files. Both check SHA-256, PDF signature, and page count.
Existing files are never replaced. A changed upstream file fails verification.

[manifest.json](papers/manifest.json) records titles, authors, versions, source
records, download URLs, retrieval date, page counts, and local checksums.
Some downloads use university mirrors of the original papers.
Checksums detect later changes; they do not prove publisher authenticity.
The papers retain their own copyright and permission notices.
