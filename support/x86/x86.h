#ifndef X86_H
#define X86_H

void x86_init();
void x86_poll(int only_ide);
void x86_ide_set();

void x86_set_image(int num, char *filename);
// physcd: mount the physical usb cd-rom into the guest's cd slot
int x86_mount_phys_cd(void);
const char* x86_get_image_name(int num);
const char* x86_get_image_path(int num);

void x86_config_load();
void x86_config_save();
void x86_set_fdd_boot(uint32_t boot);

#endif
