#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := subscribe_publish

#7/22/21 line 8 was commented out and replaced with line 9
# EXTRA_COMPONENT_DIRS := $(realpath ../..) 
EXTRA_COMPONENT_DIRS = $(IDF_PATH)/examples/common_components/qrcode

JUMPSTART_BOARD := board_esp32_devkitc.h
#EXTRA_COMPONENT_DIRS += $(PROJECT_PATH)/../components

include $(IDF_PATH)/make/project.mk

