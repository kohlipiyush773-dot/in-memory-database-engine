#include "slotted_page.h"

void SlottedPage::Init() {
    std::memset(d_, 0, PAGE_SIZE);
    d_[OFF_TYPE] = (char)PT_HEAP;
    SetNextPage(INVALID_PAGE_ID);
    SetNumSlots(0);
    SetFreePtr((int)PAGE_SIZE);
}

int SlottedPage::LiveRecords() const {
    int n = 0;
    for (int s = 0; s < NumSlots(); ++s) if (SlotLen(s) > 0) ++n;
    return n;
}

int SlottedPage::FreeBytes() const {
    int used = HDR + SLOT_SZ * NumSlots();
    for (int s = 0; s < NumSlots(); ++s) used += SlotLen(s);
    return (int)PAGE_SIZE - used;
}

int SlottedPage::FindFreeSlot() const {
    for (int s = 0; s < NumSlots(); ++s) if (SlotLen(s) == 0) return s;
    return NumSlots();
}

bool SlottedPage::CanInsert(int len) const {
    if (len <= 0) return false;
    int slot = FindFreeSlot();
    int extra = (slot == NumSlots()) ? SLOT_SZ : 0;
    return FreeBytes() >= len + extra;
}

void SlottedPage::Compact() {
    char tmp[PAGE_SIZE];
    int ptr = (int)PAGE_SIZE;
    for (int s = 0; s < NumSlots(); ++s) {
        int len = SlotLen(s);
        if (len == 0) continue;
        int off = SlotOff(s);
        ptr -= len;
        std::memcpy(tmp + ptr, d_ + off, len);
        SetSlot(s, ptr, len);
    }
    std::memcpy(d_ + ptr, tmp + ptr, PAGE_SIZE - ptr);
    SetFreePtr(ptr);
}

bool SlottedPage::InsertAt(int slot, const char* data, int len) {
    int n = NumSlots();
    if (len <= 0 || slot < 0 || slot > n) return false;
    if (slot < n && SlotLen(slot) > 0) return false;           // occupied
    int extra = (slot == n) ? SLOT_SZ : 0;
    if (FreeBytes() < len + extra) return false;
    int contiguous = FreePtr() - (HDR + SLOT_SZ * n);
    if (contiguous < len + extra) Compact();
    if (slot == n) { SetNumSlots(n + 1); SetSlot(slot, 0, 0); }
    int ptr = FreePtr() - len;
    std::memcpy(d_ + ptr, data, len);
    SetFreePtr(ptr);
    SetSlot(slot, ptr, len);
    return true;
}

bool SlottedPage::Delete(int slot) {
    if (!SlotUsed(slot)) return false;
    SetSlot(slot, 0, 0);
    return true;
}

const char* SlottedPage::Get(int slot, int* len) const {
    if (!SlotUsed(slot)) return 0;
    *len = SlotLen(slot);
    return d_ + SlotOff(slot);
}
