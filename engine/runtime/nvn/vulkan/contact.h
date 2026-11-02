// contact.h: screen-space contact shadows (contact.c; TOTK_CONTACT_SHADOWS, off by default)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "vk.h"

typedef struct ContactPass {      // a render pass as it begins (exec.c begin_pass)
    TexBackend *color[8];         // the colour attachments' backends, NULL where none
    uint32_t color_nvn[8];        // their NVN formats
    int ncolor;
    uint32_t depth_nvn;           // the depth attachment's NVN format, 0 = none
    uint32_t w, h;                // the colour 0 attachment's host size
    uint64_t c4_addr, c4_size;    // the fragment stage's constant buffer 4 bound now (guest address = host pointer)
} ContactPass;

int contact_strength(void);                 // TOTK_CONTACT_SHADOWS 0..100 (0: off)
bool contact_wants_storage(VkFormat f);     // textures.c: a colour image the march writes (B10G11R11 while on)
bool contact_pass_begin(const ContactPass *p);   // true: call contact_flush before this pass begins
bool contact_flush(VkCommandBuffer cb);     // outside a pass: the pending march (also before dispatches and copies);
                                            // true: it wrote the scene colour (exec.c drops that image's feedback mirror)
void contact_forget(TexBackend *b);         // textures.c: b's image is destroyed, replaced or b goes to the recycle pool
