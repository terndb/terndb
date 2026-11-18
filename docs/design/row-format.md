# The row format and the key encoding

`sh/sysl/terndb/row_format.sysl` implements `on-disk-format.md` §4.1–§4.5 and §4.7: the row header and
body, the record that shares the body's layout, type descriptors, the scalar encodings, relation,
record and union values, the in-place versus out-of-line decision and the 32-octet stub.
`sh/sysl/terndb/key_encoding.sysl` implements §4.6, the order-preserving key encoding, and §4.9, the
secondary-index entry. The store's rows are in this format, keyed by this encoding, in its B+trees
(`store.md`); in-memory mode uses the same row format as on-disk mode (§8.1 rule 2).

## What the code offers

- `encode_value` / `decode_value`: one non-null value of a type, exactly what a column stores and what
  an overflow chain holds (§4.7 rule 4).
- `encode_row`, `encode_row_with_stubs`, `decode_row_header`, `decode_row`: a row over its non-key
  columns. A column with a stub is stored out of line; `HAS_EXTERNAL` is the encoder's to set.
- `place_row`: the §4.7 decision for an entry with a key of a given length: `Inline`, the columns to
  move out of line, or `entry_too_large`. `encode_row` itself enforces no 4,000-octet limit.
- `column_descriptor`, `column_type`, `encode_descriptor`, `decode_descriptor`: §4.3.
- `encode_key`, `decode_key`, `push_key_component`, `key_able`: §4.6. Keys are decodable, since an
  index-only scan (§4.9 rule 3) reads values out of the index key.
- `index_entry`, `decode_index_entry`: §4.9.

Decoders raise `data_corrupted` (`D1001`) for anything that breaks the layout. Encoders raise
`type_mismatch` (`Q3001`) for a value that does not belong to its type, and for a type with no stored
form the code `ddl.md` refuses it with (`Q3014`, `Q3015`, `Q3016`, `Q6005`, `Q6011`).

## Deferred

- Overflow chains (§4.8): they need pages and `raw_crc`, which is CRC32C, a sysl gap
  (`docs/phase0/sysl-survey/`). `place_row` reports which columns must move; the caller writes the
  chain and passes the stub.
- Compression codecs (LZ4, Zstandard) for chains.
- Collated text keys (`key_spec` flag bit 2): v1 has no collations.
- Reading `key_spec` from the catalog and expression key components (§4.6 rule 5): the caller builds
  the `KeyColumn` list.

## Spec gaps

Places where `on-disk-format.md` is silent or ambiguous, and the reading the code takes. The spec text
is unchanged; items 1, 4 and 7 deserve a decision recorded in `docs/decisions.md`.

1. **The external bitmap is indexed by column number.** §4.2 says "bit i is 1 when variable column i
   is stored out of line" and also that the bitmap has the null bitmap's size. The code reads i as the
   column number among non-key columns (as in the null bitmap), which is what "same size" implies.
2. **Unused bitmap bits, a null fixed column, and `HAS_EXTERNAL`.** Bits past the last column in the
   null or external bitmap are written zero and a reader treats a set one as `data_corrupted`, as the
   wire decoder does for validity bitmaps. A null fixed column that is not zero-filled, `HAS_EXTERNAL`
   with no external bit set, an external bit on a fixed-width or null column are also corruption.
   Reserved header flag bits and the reserved octets of a relation value and a stub are ignored (§1
   rule 3).
3. **Placement.** Among columns of equal encoded length, the lower column number moves first. The
   external bitmap, which the first move adds, counts against the 4,000 octets, as does each 32-octet
   stub. A column longer than a `u16` offset can address is moved out of line rather than refused.
4. **A unique index with a null component.** §4.9 rule 1 keys a unique index by its columns alone,
   but `ddl.md` §2.5 rule 2 says a row with a null component never conflicts: two such rows would
   collide on one key. The code gives such an entry the non-unique form (index columns ‖ primary key,
   empty value); a reader tells the forms apart by whether a decoded index component is null.
5. **"The encoded primary key"** (§4.9 rule 1) is read as the primary key's §4.6 key, with each
   component in its own direction, both as a unique entry's value and as a non-unique entry's key
   suffix. Its octets are therefore exactly the clustered key's.
6. **Floats.** A row keeps a float's bits as given, NaN payload and −0 included; only keys
   canonicalize. A float32 value must be exactly representable as a float32 (`Q3001` otherwise). A
   key decoder refuses a non-canonical float key (a −0 or a NaN other than the canonical one).
7. **Wide tables can never store a row.** `ddl.md` §2.2 allows 65,535 columns and `CREATE TABLE`
   checks only that the fixed area plus key is at most 3,000 octets (§4.7 rule 2), but the null
   bitmap and the `u16` offset of every variable column are also always inline. With no key, at most
   1,872 variable-width columns fit even when all are null; a table with more always fails with
   `entry_too_large`. `CREATE TABLE` should count bitmap and offsets too.
8. **Descriptor nesting** is limited to 256 levels, the query tree's limit (`MaxTypeDepth`); deeper is
   `data_corrupted`.
9. **Decimal digits.** An unscaled value with more digits than the precision is refused when written
   (`Q3001`) and is `data_corrupted` when read, in a row and in a key.
10. **Relations as keys.** `query-tree.md` §2.5 rule 3 says relation values can be indexed, while §4.6
    rule 4 forbids them as key components. The code follows §4.6 rule 4.
11. **An unknown overflow codec** (3–255) in a stub is `data_corrupted`, and so is relation form 1,
    which v1 never writes.
