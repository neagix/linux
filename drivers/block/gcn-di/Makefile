obj-$(CONFIG_GAMECUBE_DI) += gcn-di.o

$(obj)/gcn-di.o: $(obj)/drive_20010608.h \
			$(obj)/drive_20010831.h \
			$(obj)/drive_20020402.h \
			$(obj)/drive_20020823.h

#CONFIG_GAMECUBE_DI_BUILD_FIRMWARE=y

ifeq ($(CONFIG_GAMECUBE_DI_BUILD_FIRMWARE),y)

ASMN102 = mn10200-linux-as
LDMN102 = mn10200-linux-ld
OCMN102 = mn10200-linux-objcopy

quiet_cmd_build_difw = BLD FW	$@
cmd_build_difw = \
	$(CPP) -DDRIVE_MODEL=$(DRIVE_MODEL) $< > $(obj)/$(@F).s; \
	$(ASMN102) -o $(obj)/$(@F).o $(obj)/$(@F).s; \
	$(LDMN102) --section-start absolute=0 -Ttext=0x40d000 \
		-o $(obj)/$(@F).elf -e 0x40d000 $(obj)/$(@F).o; \
	$(OCMN102) -I elf32-mn10200 -O binary $(obj)/$(@F).elf \
		$(obj)/$(@F).bin; \
	(echo -n "static "; cat $(obj)/$(@F).bin | scripts/bin2c "$(subst .h,,$(@F))_firmware") > $@; \
	rm -f $(obj)/$(@F).o $(obj)/$(@F).elf $(obj)/$(@F).bin $(obj)/$(@F).s


targets += drive_20010608.h
$(obj)/drive_20010608.h: DRIVE_MODEL := 0x20010608
$(obj)/drive_20010608.h: $(src)/drive_all.S FORCE
	$(call if_changed,build_difw)

targets += drive_20010831.h
$(obj)/drive_20010831.h: DRIVE_MODEL := 0x20010831
$(obj)/drive_20010831.h: $(src)/drive_all.S FORCE
	$(call if_changed,build_difw)

targets += drive_20020402.h
$(obj)/drive_20020402.h: DRIVE_MODEL := 0x20020402
$(obj)/drive_20020402.h: $(src)/drive_all.S FORCE
	$(call if_changed,build_difw)

targets += drive_20020823.h
$(obj)/drive_20020823.h: DRIVE_MODEL := 0x20020823
$(obj)/drive_20020823.h: $(src)/drive_all.S FORCE
	$(call if_changed,build_difw)

endif


