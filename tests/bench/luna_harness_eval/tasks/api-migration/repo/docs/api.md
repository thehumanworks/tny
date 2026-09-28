# Record access

Legacy callers use `RecordStore.lookup(scope, identifier, default)` to get a payload.
The lookup is not historical and cannot distinguish a null payload from a missing key.

Clients call `lookup` from their public reader.
