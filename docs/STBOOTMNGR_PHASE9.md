# Starlix Phase 9 — RXFS Disk Discovery Diagnostics

This patch adds firmware console messages to the RXFS-first `stbootmngr.efi` path and a builder for a *new isolated VM test image*.

The candidate image has three partitions: ESP, custom RXFS GPT partition, and a copied legacy SSP fallback partition. Its RXFS filesystem is freshly formatted and intentionally does not contain `sys/BaseSystem.dmg`. Expected behavior is:

1. UEFI loads current BOOTX64.EFI and stbtmngr.efi from the isolated ESP.
2. The manager scans UEFI Disk I/O handles.
3. If the firmware exposes the RXFS GPT partition through a suitable Disk I/O handle, it logs `RXFS: Volume mounted read-only.`
4. BaseSystem open should then fail because the sample RXFS volume intentionally lacks `sys/BaseSystem.dmg`.
5. The manager reports and uses the legacy SSP fallback.

This verifies disk/partition discovery only, not APFS or a real boot. The source `main.cpp` boot-flow change itself is only diagnostics; no write-to-disk code and no ExitBootServices call is added. The builder writes only under `build/candidate_rxfs_phase9/run_<timestamp>/` and does not touch `image/`, the physical ESP/SSP devices, or E:.

Run after applying the patch and building EFI:

```bash
bash tools/build_rxfs_phase9_candidate.sh
```

Open the printed `.vmx` file in VMware Workstation. Read the UEFI console to check the messages. This candidate is temporary and retains an SSP partition only for fallback validation.
