# Lisa ProFile parallel hard disk unit test.
# Exercises the device handshake/state machine driving the shared base+delta
# image subsystem (real image.c + storage.c; no emulator/ROM/MMU).

TEST_NAME    := lisa_profile
TEST_SRCS    := test.c
TEST_HARNESS := isolated
# checkpoint_stubs.c supplies the two checkpoint/restore symbols the ProFile now
# references (checkpoint_machine_dir, mac_checkpoint_restore_one_image) without
# dragging in checkpoint_machine.c / checkpoint_images.c — this suite never
# checkpoints (always inits with a NULL checkpoint).
EXTRA_SRCS   := ../../../../src/machines/lisa/lisa_profile.c \
                ../../../../src/core/storage/image.c \
                ../../../../src/core/storage/storage.c \
                checkpoint_stubs.c
# lisa_profile.h lives under machines/lisa; checkpoint_images.h under
# machines/runtime — neither is on common.mk's default include path.
EXTRA_CFLAGS  = -I$(EMU_ROOT)/machines/lisa -I$(EMU_ROOT)/machines/runtime

include ../../common.mk
