# Dark Souls III Save File Format Research

## Notes

* Only useful info for save backup/restore use are recorded.
* All fields are comparable to ELDEN RING's implementations (ersave.c)

## Location

* AppData\Roaming\DarkSoulsIII\<user_id>\DS30000.sl2, user_id is in hexadecimal form, which is different from ELDEN RING that is in decimal form.

## Main Save File (DS30000.sl2)

* BND4 file, header is the same as ELDEN RING's save
* 10 char slot + 1 summary slot + 1 regulation slot, the same as ELDEN RING's save

## Save Slot

* A 16-byte MD5 checksum of the encrypted data (including the IV)
* The 16-byte initialisation vector (IV) used for encryption
* The entry data, encrypted using 128-bit AES-CBC(PKCS7 padding) with the key `FD464D695E69A39A10E319A7ACE8B7FA`

## Char Slot

* Steam user id offset:
  * Read uint32 from offset 0x58 as N
  * (N + 0x6F) is the steam user id offset (8 bytes long, little-endian — uint64, the same as that in ELDEN RING. Note: an earlier revision of this doc said 16 bytes; that was an error.)

## Summary Slot

* Steam user id offset: 0x08
* active slot offset: 0x0FE8 (int as 4 bytes)
* available slots offset: 0x1098 (10 bytes for 10 slots)
* profile offset: 0x10A2
  * 10 profiles, each size is 0x22A

## Backup a char slot

* This is similar to ELDEN RING's implementation, but need decrypt first, then backup char slot, then relevant profile part of summary slot

## Restore a char slot

* This is similar to ELDEN RING's implementation, patch steam user id if not equal, then an encryption is needed(you can use old IV, or generate a new one, it is not important here) if data is changed, then replace the char slot and the whole summary slot with recalculated MD5 checksum.
