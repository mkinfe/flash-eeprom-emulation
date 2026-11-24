# Design notes

## Goals

1. **Atomic updates under power loss.** After a reset every id holds its last completed value.
2. **Flash rules.** NOR flash only clears bits; an erase sets a whole page to `0xFF`. On
   STM32L4/G4 each 8-byte double word carries ECC and may be programmed only once per erase.
3. **Endurance.** Erases are the limited resource, so they must be rare and spread evenly.
4. **Small and deterministic.** No heap, bounded stack, RAM proportional to `EE_MAX_KEYS`.

## Why a log of records

Rewriting a value in place is impossible on flash without erasing a page. Appending a new
record and remembering where the latest one is turns every update into "program erased units",
which flash does well. The cost is garbage collection when the page is full.

## Commit points

Every change has exactly one unit whose complete programming makes it visible:

| Change | Commit point |
|---|---|
| record (value or deletion) | its commit unit |
| page swap | the VALID unit of the new page |

A unit programmed only partly by a power cut has some bits still at 1, so it never equals the
expected pattern. That makes the commit decision exact.

### A bug the stress test found

The first version had no commit unit: a record was accepted when its data CRC matched. A 1 M
operation stress run failed after ~10,000 power cuts: a half-programmed data unit happened to
produce the same CRC-16 as the intended data, so a corrupted value was accepted. With a 16-bit
CRC that happens about once in 65,536 interrupted writes — rare, but certain over a product
lifetime. The fix was the commit unit, compared bit-exactly; the CRC stays to detect later bit
errors. Since the fix, 10 M operations with ~707,000 cuts pass.

## Why the header is written first

The scan needs each record's length to find the next record. If the header were written last,
a cut could leave programmed data behind an erased header — the next append would then program
those units a second time, which ECC flash forbids. With the header first:

- an erased header unit really is the end of the log;
- a damaged header (cut while programming it) ends the log; the rest of the page is not used
  and the next write performs a page swap;
- a valid header without its commit unit is skipped, and the log continues after it.

## Page states and generations

A page is valid only with a correct RECEIVE unit (magic + generation) and VALID unit
(magic + ~generation). After a swap both pages may be valid; the higher generation is newer.
The old page is erased lazily at the start of the next swap, so a swap costs one erase, and a
page that is already blank is not erased again.

## RAM index

`ee_init` scans the active page once and keeps `{id, length, offset}` for each live id.
Reads and the "unchanged value" check go straight to the record; a write updates the entry.
The index makes reads O(number of ids) without scanning flash.

## Verification after programming

Each record is read back (header, commit unit, data CRC) right after programming. A failure
that is not a power cut — a weak cell, for example — is reported as `EE_ERR_FLASH` and the
library continues on a fresh page, so a damaged area is never reused.

## Simulator

`sim/sim_flash.c` models the flash rules and counts any second program of a unit as a
violation. Power cuts are injected after a chosen number of operations: a cut program clears a
random subset of the bits that should be cleared; a cut erase sets a random subset of bits.
