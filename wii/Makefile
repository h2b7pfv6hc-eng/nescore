#---------------------------------------------------------------------------------
# nescore for Wii - build with devkitPro (devkitPPC + libogc + libfat)
#   make            -> nescore.dol / nescore.elf
#   make dist       -> dist/apps/nescore/{boot.dol,meta.xml,icon.png}
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

TARGET   := nescore
BUILD    := build
SOURCES  := source ../src
INCLUDES := source ../src

CFLAGS   = -O3 -std=gnu99 -Wall -fno-strict-aliasing $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS)
LDFLAGS  = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS     := -lfat -lwiiuse -lbte -logc -lm
LIBDIRS  :=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

# the core is compiled as one unit (unity build) so the PPU/APU inline into the CPU
CFILES   := main.c
export OFILES   := $(CFILES:.c=.o) nescore.o
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) -L$(LIBOGC_LIB)
export UNITY    := $(CURDIR)/../unity/nescore.c
export LD       := $(CC)

.PHONY: $(BUILD) clean dist

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol dist

dist: $(BUILD)
	@mkdir -p dist/apps/nescore
	@cp $(OUTPUT).dol dist/apps/nescore/boot.dol
	@cp meta.xml icon.png dist/apps/nescore/

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

nescore.o: $(UNITY)
	@echo nescore core
	$(CC) -MMD -MP -MF $(DEPSDIR)/nescore.d $(CFLAGS) -c $< -o $@

-include $(DEPENDS)

endif
