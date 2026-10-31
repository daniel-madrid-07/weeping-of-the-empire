// totk_options_ui.h: the game's Options screen (System > Options) as totk_options_menu.cpp's hooks see it: the row list
// OptionsMgr::initialize builds, the two row controllers (a toggle, a selection), and the UI calls the hooks make. Field
// offsets and call addresses are 1.0.0's (main+...; another version goes through totk_version.h, as every guest call);
// the names are ours.
#pragma once

#include <cstddef>

#include "guest/guest.h"

namespace totk_options_ui {

// One row of the list (0x28 bytes): OptionsMgr::initialize makes one per entry of the game's option table; the screen
// clones a toggle or a selection part for each. Texts: the label "10%02d" (type), the choices "10%02d_%02d"
// (type, choice), the caption "%04d", all in LayoutMsg/SystemOption_00.
struct OptionEntry {
    s32 type;         // OptionType: the game's 0 .. 13, ours 50 and up; -1 a separator
    s32 buttonType;   // 0 toggle, 1 selection, 2 button, 3 separator
    u32 index;        // position in the list
    u32 _c;
    u32 choiceCount;  // a selection's number of choices
    u32 _14;
    u32 *choiceIds;   // the choices' ids (the game's rows: its ChoiceType values)
    u32 nameHash;     // the row's member of the save's option struct (0 for ours: no member)
    u32 visibleHash;  // 0: always shown, else a GameData flag that must be set
};
static_assert(offsetof(OptionEntry, choiceCount) == 0x10 && offsetof(OptionEntry, choiceIds) == 0x18 &&
                  offsetof(OptionEntry, nameHash) == 0x20 && sizeof(OptionEntry) == 0x28,
              "OptionEntry");

struct OptionsMgr {
    u32 count;
    u32 _4;
    OptionEntry *entries;
};
static_assert(offsetof(OptionsMgr, entries) == 0x8, "OptionsMgr");

// The screen part a row controller sits on; the button the cursor is on is looked up in its button group
struct OptionRowParts {
    u8 _0[0x30];
    void *buttonGroup;
};
static_assert(offsetof(OptionRowParts, buttonGroup) == 0x30, "OptionRowParts");

// A toggle row's controller (0x38 bytes)
struct OptionToggle {
    u8 _0[0x18];
    OptionRowParts *parts;
    void *layout;   // the row's layout
    u8 _28[4];
    s32 type;
    u32 nameHash;
    u32 index;
};
static_assert(offsetof(OptionToggle, parts) == 0x18 && offsetof(OptionToggle, type) == 0x2c &&
                  sizeof(OptionToggle) == 0x38,
              "OptionToggle");
constexpr size_t kToggleState = 0x89;   // a toggle button's on / off byte

// A selection row's controller (0x50 bytes): < choice >
struct OptionSelect {
    u8 _0[0x18];
    OptionRowParts *parts;
    void *layout;
    u8 _28[8];
    void *arrowNext;   // the > arrow
    void *arrowPrev;   // the < arrow
    s32 type;
    s32 choice;        // the shown choice, 0 .. choiceCount - 1
    u32 index;
    u32 nameHash;
};
static_assert(offsetof(OptionSelect, arrowNext) == 0x30 && offsetof(OptionSelect, type) == 0x40 &&
                  offsetof(OptionSelect, choice) == 0x44 && sizeof(OptionSelect) == 0x50,
              "OptionSelect");

// sead::FixedSafeString<32> after its formatting constructor, and eui's 0x18-byte MessageString
struct FixedSafeString32 {
    char *top;
    s32 bufferSize;
    u8 _c[4];
    char buffer[32];
};
static_assert(sizeof(FixedSafeString32) == 0x30, "FixedSafeString32");
struct MessageString {
    u8 _0[0x18];
};

// The OptionsMgr the open screen uses; null before there is one
inline OptionsMgr *options_mgr() {
    const u64 owner = **guest_mem::guest_data<u64 *>(0x1004558440);
    return *reinterpret_cast<OptionsMgr *const *>(static_cast<uintptr>(owner + 0x5a0));
}

namespace guest {
GUEST_FN(MessageStringCtor, 1000b906ec, void(MessageString *self));
GUEST_FN(FormatFixedSafeString32, 1000a206b0, void(char **self, const char *format, s32 value));
// the message `label` of `file` into msg (fallback: the label itself when there is none)
GUEST_FN(FindMessage, 1000b907a0,
                u32(MessageString *msg, const char *const *file, const FixedSafeString32 *label, bool fallback));
GUEST_FN(SetMessageString, 1000b90810,
                void(void *layout, const char *pane, const MessageString *msg, bool apply, void *args));
GUEST_FN(FindButton, 10019ebf28, void *(void *group, void *layout));
GUEST_FN(ButtonSetToggle, 1001a857a0, void(void *button, u32 on));
GUEST_FN(ArrowSetDisabled, 1001a85d64, void(void *arrow, bool disabled));
GUEST_FN(ArrowPlay, 1001a861f8, void(void *arrow));   // the arrow's pressed animation
GUEST_FN(FindEntry, 1001a85d24, OptionEntry *(OptionsMgr *mgr, s32 type));
GUEST_FN(SelectShowChoice, 1001a85d84, void(OptionSelect *self));   // "10%02d_%02d" into the choice pane
// OptionsMgr's entry array: count entries of 0x28 bytes from the heap, each a separator ({-1, 3, 0...}); count and
// entries are set only when the allocation succeeds
GUEST_FN(EntriesAlloc, 1001a067c8, void(OptionsMgr *mgr, u32 count, u64 heap));
// a {u32 count; u32 *data} array of count zeros (an entry's choices)
GUEST_FN(ChoicesAlloc, 1000f6105c, void(void *array, u32 count, u64 heap));
}  // namespace guest

}  // namespace totk_options_ui
