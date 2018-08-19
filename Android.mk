ifeq ($(strip $(TARGET_USES_NQ_NFC)),true)
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
NFA := src/nfa
NFC := src/nfc
HAL := src/hal
UDRV := src/udrv

D_CFLAGS := -DANDROID -DBUILDCFG=1 \
    -Wno-deprecated-register \
    -Wno-unused-parameter \

#Enable NXP Specific
D_CFLAGS += -DNXP_EXTNS=TRUE
D_CFLAGS += -DNFC_NXP_STAT_DUAL_UICC_EXT_SWITCH=FALSE
D_CFLAGS += -DNFC_NXP_AID_MAX_SIZE_DYN=TRUE

#Enable HCE-F specific
D_CFLAGS += -DNXP_NFCC_HCE_F=TRUE

#variables for NFC_NXP_CHIP_TYPE
PN547C2 := 1
PN548C2 := 2
PN551   := 3
PN553   := 4
PN557   := 5

ifeq ($(PN547C2),1)
D_CFLAGS += -DPN547C2=1
endif
ifeq ($(PN548C2),2)
D_CFLAGS += -DPN548C2=2
endif
ifeq ($(PN551),3)
D_CFLAGS += -DPN551=3
endif
ifeq ($(PN553),4)
D_CFLAGS += -DPN553=4
endif
ifeq ($(PN557),5)
D_CFLAGS += -DPN557=5
endif

#### Select the JCOP OS Version ####
JCOP_VER_3_1 := 1
JCOP_VER_3_2 := 2
JCOP_VER_3_3 := 3
JCOP_VER_4_0 := 4

LOCAL_CFLAGS += -DJCOP_VER_3_1=$(JCOP_VER_3_1)
LOCAL_CFLAGS += -DJCOP_VER_3_2=$(JCOP_VER_3_2)
LOCAL_CFLAGS += -DJCOP_VER_3_3=$(JCOP_VER_3_3)
LOCAL_CFLAGS += -DJCOP_VER_4_0=$(JCOP_VER_4_0)

NFC_NXP_ESE:= TRUE
ifeq ($(NFC_NXP_ESE),TRUE)
LOCAL_CFLAGS += -DNFC_NXP_ESE=TRUE
LOCAL_CFLAGS += -DNFC_NXP_ESE_VER=$(JCOP_VER_4_0)
else
LOCAL_CFLAGS += -DNFC_NXP_ESE=FALSE
endif

ifeq ($(call is-board-platform-in-list,msm8909w msm8916 msm8994 msm8909 msm8996 msm8992 msm8952 msm8937 msm8953 msm8998),true)
D_CFLAGS += -DNQ_NFC_DUAL_UICC=FALSE
else
D_CFLAGS += -DNQ_NFC_DUAL_UICC=TRUE
endif

#### Select the CHIP ####
ifeq ($(strip $(NQ3XX_PRESENT)),true)
NXP_CHIP_TYPE := $(PN557)
else
NXP_CHIP_TYPE := $(PN548C2)
endif

ifeq ($(NXP_CHIP_TYPE),$(PN547C2))
D_CFLAGS += -DNFC_NXP_CHIP_TYPE=PN547C2
else ifeq ($(NXP_CHIP_TYPE),$(PN548C2))
D_CFLAGS += -DNFC_NXP_CHIP_TYPE=PN548C2
else ifeq ($(NXP_CHIP_TYPE),$(PN551))
D_CFLAGS += -DNFC_NXP_CHIP_TYPE=PN551
else ifeq ($(NXP_CHIP_TYPE),$(PN553))
D_CFLAGS += -DNFC_NXP_CHIP_TYPE=PN553
else ifeq ($(NXP_CHIP_TYPE),$(PN557))
D_CFLAGS += -DNFC_NXP_CHIP_TYPE=PN557
endif

#Gemalto SE support
D_CFLAGS += -DGEMALTO_SE_SUPPORT
D_CFLAGS += -DNXP_UICC_ENABLE
ifeq ($(NXP_CHIP_TYPE),$(PN553))
D_CFLAGS += -DJCOP_WA_ENABLE=FALSE
else
D_CFLAGS += -DJCOP_WA_ENABLE=TRUE
endif

#Routing Entries optimization
D_CFLAGS += -DNFC_NXP_LISTEN_ROUTE_TBL_OPTIMIZATION=TRUE
######################################
# Build shared library system/lib/libnfc-nci.so for stack code.

LOCAL_ARM_MODE := arm
ifeq (true,$(TARGET_IS_64_BIT))
LOCAL_MULTILIB := 64
else
LOCAL_MULTILIB := 32
endif
LOCAL_MODULE := libnqnfc-nci
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_OWNER := nxp
LOCAL_SHARED_LIBRARIES := libhardware_legacy libcutils liblog libdl libhardware libz
LOCAL_CFLAGS += $(D_CFLAGS)
LOCAL_C_INCLUDES := $(LOCAL_PATH)/src/include \
    $(LOCAL_PATH)/src/gki/ulinux \
    $(LOCAL_PATH)/src/gki/common \
    $(LOCAL_PATH)/$(NFA)/include \
    $(LOCAL_PATH)/$(NFA)/int \
    $(LOCAL_PATH)/$(NFC)/include \
    $(LOCAL_PATH)/$(NFC)/int \
    $(LOCAL_PATH)/src/hal/include \
    $(LOCAL_PATH)/src/hal/int \
    $(LOCAL_PATH)/$(HALIMPL)/include \
    system/libhwbinder/include \
    external/zlib

# Treble configuration
LOCAL_SHARED_LIBRARIES += libhidlbase libmetricslogger libhidltransport libhwbinder libutils android.hardware.nfc@1.0 vendor.nxp.hardware.nfc@1.0
LOCAL_SRC_FILES := \
    $(call all-c-files-under, $(NFA)/ce $(NFA)/dm $(NFA)/ee) \
    $(call all-c-files-under, $(NFA)/hci $(NFA)/int $(NFA)/p2p $(NFA)/rw $(NFA)/sys) \
    $(call all-c-files-under, $(NFC)/int $(NFC)/llcp $(NFC)/nci $(NFC)/ndef $(NFC)/nfc $(NFC)/tags) \
    $(call all-c-files-under, src/adaptation) \
    $(call all-cpp-files-under, src/adaptation) \
    $(call all-cpp-files-under, src/nfc/nfc) \
    $(call all-c-files-under, src/gki) \
    src/nfca_version.c
include $(BUILD_SHARED_LIBRARY)


######################################
include $(call all-makefiles-under,$(LOCAL_PATH))
endif
