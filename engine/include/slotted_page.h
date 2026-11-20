// slotted_page.h - DBMS record layout inside one 4 KB heap page (variable-length records).
//
//  | lsn(8) | type(1) | pad(3) | next_page(4) | num_slots(2) | free_ptr(2) | slot dir ... -> free <- ... records |
//  slot i = [offset(2) | length(2)] ; length 0 means "empty slot" (deleted).
//  Records grow from the END of the page towards the slot directory.
#pragma once
#include "common.h"

class SlottedPage {
public:
    enum { HDR = 20, SLOT_SZ = 4 };
    explicit SlottedPage(char* d) : d_(d) {}

    void Init();                                   // format as an empty heap page
    page_id_t NextPage() const { return RdI32(d_ + 12); }
    void SetNextPage(page_id_t p) { WrI32(d_ + 12, p); }
    int  NumSlots() const { return RdU16(d_ + 16); }
    bool SlotUsed(int s) const { return s >= 0 && s < NumSlots() && SlotLen(s) > 0; }
    int  FindFreeSlot() const;                     // lowest empty slot, else NumSlots() (new slot)
    int  FreeBytes() const;                        // usable free bytes (holes included)
    bool CanInsert(int len) const;
    bool InsertAt(int slot, const char* data, int len);   // false if impossible
    bool Delete(int slot);
    const char* Get(int slot, int* len) const;     // pointer inside the page or NULL
    int  LiveRecords() const;
    void Compact();                                // squeeze holes left by deletes
private:
    int  SlotOff(int s) const { return RdU16(d_ + HDR + s * SLOT_SZ); }
    int  SlotLen(int s) const { return RdU16(d_ + HDR + s * SLOT_SZ + 2); }
    void SetSlot(int s, int off, int len) { WrU16(d_ + HDR + s * SLOT_SZ, (uint16_t)off); WrU16(d_ + HDR + s * SLOT_SZ + 2, (uint16_t)len); }
    int  FreePtr() const { return RdU16(d_ + 18); }
    void SetFreePtr(int p) { WrU16(d_ + 18, (uint16_t)p); }
    void SetNumSlots(int n) { WrU16(d_ + 16, (uint16_t)n); }
    char* d_;
};
