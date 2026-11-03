/* Do not edit; regenerate.*/
#pragma once

#define SAVEVER_REV_100 4637640u
#define SAVEVER_REV_LATEST 4843846u   /* the newest list the tables cover */

static const SaveKey drop_0[] = {
    {0, 0x0538d67au}, {0, 0x110192f4u}, {0, 0x1debe051u}, {0, 0x4d100147u},
    {0, 0x55baedecu}, {0, 0x5f20356fu}, {0, 0x5fbe303eu}, {0, 0x7105f81cu},
    {0, 0x76c41683u}, {0, 0x7f588407u}, {0, 0x8001b362u}, {0, 0x88f63656u},
    {0, 0x8d696023u}, {0, 0x8f71c191u}, {0, 0x99fea331u}, {0, 0x9d3aec10u},
    {0, 0x9f119a87u}, {0, 0xa69b583fu}, {0, 0xb75bfc0fu}, {0, 0xb866a1b0u},
    {0, 0xc033f4d1u}, {0, 0xd719817au}, {0, 0xd954c9a7u}, {0, 0xdd18bac2u},
    {0, 0xea96b6c6u}, {0, 0xff60e721u}, {1, 0x073a8127u}, {22, 0xa4512944u},
    {24, 0xcaf464f9u},
};
static const SaveKey drop_1[] = {
    {0, 0x0de0127cu}, {0, 0x82c839b9u}, {0, 0x83627d10u}, {0, 0xa9dc3920u},
    {0, 0xf5405ef1u}, {2, 0xe316d85cu}, {3, 0xe3cfa2abu}, {6, 0x7914fbaeu},
    {23, 0xc8e5018du},
};
static const SaveKey drop_2[] = {
    {0, 0},
};
static const SaveKey drop_5[] = {
    {0, 0},
};
static const SaveKey drop_6[] = {
    {0, 0},
};
static const uint64_t drop_b64[] = {
    0x01b3538b4c3a971eull, 0x10c13a11e074b643ull, 0x1d11c5a162341b0dull,
    0x4a0d55c7c33068f0ull, 0x716c3c1cbf022918ull, 0x7ae164061c847b73ull,
};
static const uint32_t enum_added_0[] = {0xa3b68ec6u, 0x33893cc9u, 0x13688702u, 0x51a5a672u, 0xca7373b7u};   /* 5 values */
static const uint32_t enum_added_1[] = {0x720dc4bdu, 0xeba9fcfbu, 0x8489e909u, 0x875bd462u, 0x27ee642au};   /* 5 values */
static const SaveEnumMap enum_maps[] = {
    {0, 6, 0x01d063dbu, 0x48db8265u, enum_added_0, 5},
    {0, 7, 0x8d4bc9a1u, 0xc525c3edu, enum_added_1, 5},
};
static const SaveBinRoom bin_rooms[] = {
    {2, 0x63696a32u, 10240},
};
static const SaveFileSpec file_specs[] = {
    {0, L"progress.sav", 2307552, 30693, 0x6f5fc4b582ebed9cull, drop_0, 29},
    {1, L"option.sav", 560, 33, 0x28851ef7d1e4f46bull, drop_1, 9},
    {2, L"caption.sav", 11380, 19, 0x2f40297ffe45a8d9ull, drop_2, 0},
    {5, L"footprint.sav", 614760, 6, 0x9d0603c9d250496bull, drop_5, 0},
    {6, L"direct_file_save_related.sav", 1152, 14, 0xbc909208cc48a710ull, drop_6, 0},
};
